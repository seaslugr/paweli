#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_internal.h"

static void compile_throw_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_block_stmt(compiler_t *compiler, const ast_node_t *block_node);
static type_t compile_condition(compiler_t *compiler, const ast_node_t *node, const char *context);
static void compile_while_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_for_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_break_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_continue_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_expr_stmt(compiler_t *compiler, const ast_node_t *node);

static void compile_throw_stmt(compiler_t *compiler, const ast_node_t *node) {
	type_t value_type;

	value_type = compile_expr_typed(compiler, node->as.throw_stmt.value, NULL);
	if (!type_equals(value_type, type_string())) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "throw expects a string, got %s", type_to_string(value_type));
	}

	compiler_emit_byte(compiler, OP_THROW, node->line, node->column);
}

void compile_statements(compiler_t *compiler, const ast_node_t *block_node) {
	size_t i;

	for (i = 0; i < block_node->as.block.statements.count; i++) {
		compile_statement(compiler, block_node->as.block.statements.items[i]);
	}
}

static void compile_block_stmt(compiler_t *compiler, const ast_node_t *block_node) {
	compiler_begin_scope(compiler);
	compile_statements(compiler, block_node);
	compiler_end_scope(compiler, block_node->line, block_node->column);
}

void compile_statement(compiler_t *compiler, const ast_node_t *node) {
	switch (node->kind) {
		case AST_VAR_DECL:
			compile_var_decl(compiler, node);
			break;
		case AST_FUNCTION_DECL:
			compile_nested_function_decl(compiler, node);
			break;
		case AST_EXPORT_STMT:
			compile_export_stmt(compiler, node);
			break;
		case AST_IF_STMT:
			compile_if_stmt(compiler, node);
			break;
		case AST_WHILE_STMT:
			compile_while_stmt(compiler, node);
			break;
		case AST_FOR_STMT:
			compile_for_stmt(compiler, node);
			break;
		case AST_FOREACH_STMT:
			compile_foreach_stmt(compiler, node);
			break;
		case AST_BREAK_STMT:
			compile_break_stmt(compiler, node);
			break;
		case AST_CONTINUE_STMT:
			compile_continue_stmt(compiler, node);
			break;
		case AST_RETURN_STMT:
			compile_return_stmt(compiler, node);
			break;
		case AST_EXPR_STMT:
			compile_expr_stmt(compiler, node);
			break;
		case AST_THROW_STMT:
			compile_throw_stmt(compiler, node);
			break;
		case AST_TRY_EXPR:
			// The parser returns a `try` statement as a bare AST_TRY_EXPR, with
			// no AST_EXPR_STMT around it. Compile it and discard the value.
			compile_expr_typed(compiler, node, NULL);
			compiler_emit_byte(compiler, OP_POP, node->line, node->column);
			break;
		case AST_BLOCK:
			compile_block_stmt(compiler, node);
			break;
		default:
			compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-statement", "expected a statement");
			break;
	}
}

void compile_var_decl(compiler_t *compiler, const ast_node_t *node) {
	type_t declared_type;
	type_t initializer_type;

	declared_type = compiler_resolve_type_ref(compiler, node->as.var_decl.type, node->line, node->column);
	initializer_type = compile_expr_typed(compiler, node->as.var_decl.initializer, &declared_type);

	if (!type_is_assignable(declared_type, initializer_type)) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot initialize a %s variable with a %s value", type_to_string(declared_type), type_to_string(initializer_type));
	}

	if (compiler_declare_local(compiler, node->as.var_decl.name, declared_type, node->line)) {
		compiler_mark_initialized(compiler);
		if (initializer_type.kind == TYPE_NULL) {
			value_type_t vtype;

			if (compiler_register_cacheable_value_type(declared_type, &vtype) && vtype != VAL_OBJ) {
				compiler_disqualify_local_reg_slot(compiler, compiler->local_count - 1);
			}
		}
	}
}

/* export is a compile-time declaration. It copies the signature of
   every overload of the named function, and the compiled function
   object, into the module's public interface. It emits no bytecode. It
   must be a direct statement in main(). It never runs, so
   `if (x) { export y; }` would export y either way, and is rejected. */
void compile_export_stmt(compiler_t *compiler, const ast_node_t *node) {
	overload_set_t *set;
	size_t i;
	obj_string_t *key;
	const signature_t *sig;
	struct_def_t *struct_def;

	if (!compiler->is_module_def) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::export-outside-module", "'export' can only be used in a file with a #moduledef directive");
		return;
	}

	if (!compiler->compiling_main || compiler->scope_depth != 1) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::export-must-be-unconditional", "'export' must be a direct statement in main(), not nested in a conditional or loop");
		return;
	}

	set = signature_table_find_set(compiler->file_signatures, node->as.export_stmt.name.start, node->as.export_stmt.name.length);
	if (set == NULL) {
		// The name is not a function, so it must be a struct declared in this
		// file. Exporting a struct only sets is_exported.
		struct_def = struct_registry_find(compiler->structs, node->as.export_stmt.name.start, node->as.export_stmt.name.length);
		if (struct_def == NULL) {
			compiler_error(compiler, node->as.export_stmt.name.line, node->as.export_stmt.name.column, (int)node->as.export_stmt.name.length, "compiler::undefined-variable", "cannot export undefined name '%.*s'", (int)node->as.export_stmt.name.length, node->as.export_stmt.name.start);
			return;
		}
		struct_def->is_exported = true;
		return;
	}

	for (i = 0; i < set->overload_count; i++) {
		signature_t *exported_sig;

		sig = &set->overloads[i];
		key = object_new_string(compiler->heap, compiler->strings, sig->mangled_name, strlen(sig->mangled_name));
		table_set(compiler->home_module->members, key, value_obj((obj_t *)sig->function));
		exported_sig = signature_table_add(compiler->out_exports, node->as.export_stmt.name.start, node->as.export_stmt.name.length, sig->param_types, sig->param_count, sig->return_type, sig->has_return_type);
		// @deprecated crosses the module boundary: an importer that calls a
		// deprecated export gets the warning too. deprecated_message points
		// into this file's source text, which module.c keeps alive for the
		// rest of the process (see vm_track_owned_source()).
		exported_sig->is_deprecated = sig->is_deprecated;
		exported_sig->deprecated_message = sig->deprecated_message;
		exported_sig->deprecated_message_length = sig->deprecated_message_length;
	}
}

static type_t compile_condition(compiler_t *compiler, const ast_node_t *node, const char *context) {
	type_t bool_hint;
	type_t actual;

	bool_hint = type_bool();
	actual = compile_expr_typed(compiler, node, &bool_hint);
	if (!type_equals(actual, type_bool())) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", context);
	}
	return actual;
}

void compile_if_stmt(compiler_t *compiler, const ast_node_t *node) {
	size_t then_jump;
	size_t else_jump;

	compile_condition(compiler, node->as.if_stmt.condition, "'if' condition must be bool");

	then_jump = compiler_emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
	compiler_emit_byte(compiler, OP_POP, node->line, node->column);
	compile_block_stmt(compiler, node->as.if_stmt.then_branch);

	else_jump = compiler_emit_jump(compiler, OP_JUMP, node->line, node->column);

	compiler_patch_jump(compiler, then_jump, node->line);
	compiler_emit_byte(compiler, OP_POP, node->line, node->column);

	if (node->as.if_stmt.else_branch != NULL) {
		if (node->as.if_stmt.else_branch->kind == AST_IF_STMT) {
			compile_if_stmt(compiler, node->as.if_stmt.else_branch);
		} else {
			compile_block_stmt(compiler, node->as.if_stmt.else_branch);
		}
	}

	compiler_patch_jump(compiler, else_jump, node->line);
}

static void compile_while_stmt(compiler_t *compiler, const ast_node_t *node) {
	size_t loop_start;
	size_t exit_jump;

	loop_start = compiler_current_chunk(compiler)->count;

	compile_condition(compiler, node->as.while_stmt.condition, "'while' condition must be bool");
	exit_jump = compiler_emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
	compiler_emit_byte(compiler, OP_POP, node->line, node->column);

	// A `while` has no scope beyond its body, so `break` and `continue`
	// pop the same locals.
	compiler_begin_loop(compiler, loop_start, compiler->scope_depth, compiler->scope_depth);
	compile_block_stmt(compiler, node->as.while_stmt.body);

	compiler_emit_loop(compiler, loop_start, node->line, node->column);

	compiler_patch_jump(compiler, exit_jump, node->line);
	compiler_emit_byte(compiler, OP_POP, node->line, node->column);

	compiler_end_loop(compiler, node->line);
}

static void compile_for_stmt(compiler_t *compiler, const ast_node_t *node) {
	size_t loop_start;
	bool has_exit_jump;
	size_t exit_jump;
	size_t body_jump;
	size_t increment_start;

	compiler_begin_scope(compiler);

	if (node->as.for_stmt.initializer != NULL) {
		compile_statement(compiler, node->as.for_stmt.initializer);
	}

	loop_start = compiler_current_chunk(compiler)->count;

	has_exit_jump = false;
	exit_jump = 0;
	if (node->as.for_stmt.condition != NULL) {
		compile_condition(compiler, node->as.for_stmt.condition, "'for' condition must be bool");
		exit_jump = compiler_emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
		compiler_emit_byte(compiler, OP_POP, node->line, node->column);
		has_exit_jump = true;
	}

	if (node->as.for_stmt.increment != NULL) {
		body_jump = compiler_emit_jump(compiler, OP_JUMP, node->line, node->column);
		increment_start = compiler_current_chunk(compiler)->count;

		compile_expr_typed(compiler, node->as.for_stmt.increment, NULL);
		compiler_emit_byte(compiler, OP_POP, node->line, node->column);

		compiler_emit_loop(compiler, loop_start, node->line, node->column);
		loop_start = increment_start;
		compiler_patch_jump(compiler, body_jump, node->line);
	}

	// `continue` keeps the init-clause variable, since the increment and
	// condition need it. `break` leaves the loop and pops it too, one
	// scope shallower.
	compiler_begin_loop(compiler, loop_start, compiler->scope_depth, compiler->scope_depth - 1);
	compile_block_stmt(compiler, node->as.for_stmt.body);

	compiler_emit_loop(compiler, loop_start, node->line, node->column);

	if (has_exit_jump) {
		compiler_patch_jump(compiler, exit_jump, node->line);
		compiler_emit_byte(compiler, OP_POP, node->line, node->column);
	}

	// compiler_end_scope() must run before compiler_end_loop(). A break
	// has already popped down to the pre-loop depth, so its jump must land
	// after these pops. Otherwise the breaking path would pop twice.
	compiler_end_scope(compiler, node->line, node->column);
	compiler_end_loop(compiler, node->line);
}

/* `for (T name : array_expr) body`, over the elements of an array or
   the characters of a string. The parser cannot desugar it as it does
   the range form, since the element type is only known once array_expr
   is compiled. So this emits the bytecode of a three-clause `for` by
   hand, counting a hidden index over a hidden copy of the array or
   string. */
void compile_foreach_stmt(compiler_t *compiler, const ast_node_t *node) {
	type_t array_type;
	type_t declared_type;
	token_t array_token;
	token_t index_token;
	int array_slot;
	int index_slot;
	size_t loop_start;
	size_t exit_jump;
	size_t body_jump;
	size_t increment_start;

	compiler_begin_scope(compiler);

	array_token = compiler_synthetic_token("$for_array");
	array_type = compile_expr_typed(compiler, node->as.foreach_stmt.array_expr, NULL);
	if (compiler_declare_local(compiler, array_token, array_type, node->line)) {
		compiler_mark_initialized(compiler);
	}
	array_slot = compiler->local_count - 1;

	declared_type = compiler_resolve_type_ref(compiler, node->as.foreach_stmt.type, node->line, node->column);
	if (array_type.array_depth > 0) {
		type_t element_type = type_element_of(array_type);
		if (!type_equals(declared_type, element_type)) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "loop variable is declared %s but the array holds %s", type_to_string(declared_type), type_to_string(element_type));
		}
	} else if (type_equals(array_type, type_string())) {
		if (!type_equals(declared_type, type_string())) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "iterating a string yields string, but the loop variable is declared %s", type_to_string(declared_type));
		}
	} else {
		compiler_error(compiler, node->as.foreach_stmt.array_expr->line, node->as.foreach_stmt.array_expr->column, 0, "compiler::not-indexable", "'for (T x : ...)' requires an array or a string, got %s", type_to_string(array_type));
	}

	index_token = compiler_synthetic_token("$for_index");
	compiler_emit_constant(compiler, value_int(0), node->line, node->column);
	if (compiler_declare_local(compiler, index_token, type_int(), node->line)) {
		compiler_mark_initialized(compiler);
	}
	index_slot = compiler->local_count - 1;

	loop_start = compiler_current_chunk(compiler)->count;

	// Condition: $for_index < stdlib.length($for_array)
	compiler_emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)index_slot, node->line, node->column);
	compiler_emit_name_op_str(compiler, OP_GET_GLOBAL, "stdlib", 6, node->line, node->column);
	compiler_emit_name_op_str(compiler, OP_GET_PROPERTY, "length", 6, node->line, node->column);
	compiler_emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)array_slot, node->line, node->column);
	compiler_emit_bytes(compiler, OP_CALL, 1, node->line, node->column);
	compiler_emit_byte(compiler, OP_LESS, node->line, node->column);

	exit_jump = compiler_emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
	compiler_emit_byte(compiler, OP_POP, node->line, node->column);

	// Increment: $for_index = $for_index + 1
	body_jump = compiler_emit_jump(compiler, OP_JUMP, node->line, node->column);
	increment_start = compiler_current_chunk(compiler)->count;

	compiler_emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)index_slot, node->line, node->column);
	compiler_emit_constant(compiler, value_int(1), node->line, node->column);
	compiler_emit_byte(compiler, OP_ADD, node->line, node->column);
	compiler_emit_bytes(compiler, OP_SET_LOCAL, (uint8_t)index_slot, node->line, node->column);
	compiler_emit_byte(compiler, OP_POP, node->line, node->column);

	compiler_emit_loop(compiler, loop_start, node->line, node->column);
	loop_start = increment_start;
	compiler_patch_jump(compiler, body_jump, node->line);

	// `break` also pops $for_array and $for_index, one scope shallower
	// than `continue`, like the init variable in compile_for_stmt().
	compiler_begin_loop(compiler, loop_start, compiler->scope_depth, compiler->scope_depth - 1);

	compiler_begin_scope(compiler);
	compiler_emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)array_slot, node->line, node->column);
	compiler_emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)index_slot, node->line, node->column);
	compiler_emit_byte(compiler, OP_GET_INDEX, node->line, node->column);
	if (compiler_declare_local(compiler, node->as.foreach_stmt.name, declared_type, node->line)) {
		compiler_mark_initialized(compiler);
	}
	compile_statements(compiler, node->as.foreach_stmt.body);
	compiler_end_scope(compiler, node->line, node->column);

	compiler_emit_loop(compiler, loop_start, node->line, node->column);

	compiler_patch_jump(compiler, exit_jump, node->line);
	compiler_emit_byte(compiler, OP_POP, node->line, node->column);

	compiler_end_scope(compiler, node->line, node->column);
	compiler_end_loop(compiler, node->line);
}

static void compile_break_stmt(compiler_t *compiler, const ast_node_t *node) {
	loop_context_t *loop;
	size_t jump;

	if (compiler->loop_count == 0) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::break-outside-loop", "'break' can only be used inside a loop");
		return;
	}

	loop = &compiler->loops[compiler->loop_count - 1];
	compiler_emit_pop_to_depth(compiler, loop->break_depth, node->line, node->column);
	jump = compiler_emit_jump(compiler, OP_JUMP, node->line, node->column);

	if (loop->break_jump_count == loop->break_jump_capacity) {
		loop->break_jump_capacity = loop->break_jump_capacity < 4 ? 4 : loop->break_jump_capacity * 2;
		loop->break_jumps = realloc(loop->break_jumps, loop->break_jump_capacity * sizeof(size_t));
	}
	loop->break_jumps[loop->break_jump_count++] = jump;
}

static void compile_continue_stmt(compiler_t *compiler, const ast_node_t *node) {
	loop_context_t *loop;

	if (compiler->loop_count == 0) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::continue-outside-loop", "'continue' can only be used inside a loop");
		return;
	}

	loop = &compiler->loops[compiler->loop_count - 1];
	compiler_emit_pop_to_depth(compiler, loop->continue_depth, node->line, node->column);
	compiler_emit_loop(compiler, loop->continue_target, node->line, node->column);
}

void compile_return_stmt(compiler_t *compiler, const ast_node_t *node) {
	type_t value_type;

	if (node->as.return_stmt.value != NULL) {
		if (!compiler->has_return_type) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::return-value-without-type", "cannot return a value from a function with no declared '-> type'");
			compile_expr_typed(compiler, node->as.return_stmt.value, NULL);
		} else {
			value_type = compile_expr_typed(compiler, node->as.return_stmt.value, &compiler->return_type);
			if (!type_is_assignable(compiler->return_type, value_type)) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "returned a %s value but this function is declared to return %s", type_to_string(value_type), type_to_string(compiler->return_type));
			}
		}
	} else {
		compiler_emit_byte(compiler, OP_NULL, node->line, node->column);
	}
	compiler_emit_byte(compiler, OP_RETURN, node->line, node->column);
}

static void compile_expr_stmt(compiler_t *compiler, const ast_node_t *node) {
	compile_expr_typed(compiler, node->as.expr_stmt.expression, NULL);
	compiler_emit_byte(compiler, OP_POP, node->line, node->column);
}

/* counter += 1 for one of the two tally locals declared by
   compiler_emit_test_runner(). */
void compiler_emit_local_increment(compiler_t *script, int slot, int line, int column) {
	compiler_emit_bytes(script, OP_GET_LOCAL, (uint8_t)slot, line, column);
	compiler_emit_constant(script, value_int(1), line, column);
	compiler_emit_byte(script, OP_ADD, line, column);
	compiler_emit_bytes(script, OP_SET_LOCAL, (uint8_t)slot, line, column);
	compiler_emit_byte(script, OP_POP, line, column);
}
