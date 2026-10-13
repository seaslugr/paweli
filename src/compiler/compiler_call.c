#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_internal.h"

static type_t compile_value_call(compiler_t *compiler, const ast_node_t *node, type_t callee_type);
static type_t compile_bare_call(compiler_t *compiler, const ast_node_t *node, token_t name);
static type_t compile_stdlib_call(compiler_t *compiler, const ast_node_t *node, token_t member_name);
static type_t compile_module_call(compiler_t *compiler, const ast_node_t *node, token_t member_name, const module_unit_t *module);
static type_t compile_length_call(compiler_t *compiler, const ast_node_t *node);
static type_t compile_push_call(compiler_t *compiler, const ast_node_t *node);
static type_t compile_pop_call(compiler_t *compiler, const ast_node_t *node);
static type_t compile_sort_call(compiler_t *compiler, const ast_node_t *node);

type_t compile_call_expr(compiler_t *compiler, const ast_node_t *node) {
	const ast_node_t *callee;
	token_t name;
	int slot;
	int upvalue;

	callee = node->as.call_expr.callee;

	if (callee->kind == AST_IDENTIFIER_EXPR) {
		name = callee->as.identifier_expr.name;

		// A local or upvalue that holds a closure shadows a top-level function
		// or native of the same name. A local that is not a closure falls
		// through to compile_bare_call().
		slot = compiler_resolve_local(compiler, name);
		if (slot != -1) {
			if (compiler->locals[slot].type.kind == TYPE_FUNCTION) {
				compiler_emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)slot, node->line, node->column);
				return compile_value_call(compiler, node, compiler->locals[slot].type);
			}
		} else {
			upvalue = compiler_resolve_upvalue(compiler, name);
			if (upvalue != -1 && compiler->upvalues[upvalue].type.kind == TYPE_FUNCTION) {
				compiler_emit_bytes(compiler, OP_GET_UPVALUE, (uint8_t)upvalue, node->line, node->column);
				return compile_value_call(compiler, node, compiler->upvalues[upvalue].type);
			}
		}

		return compile_bare_call(compiler, node, name);
	}

	if (callee->kind == AST_MEMBER_EXPR) {
		const ast_node_t *object;

		// A struct field can hold a function value: `h.callback(4)`. A module,
		// stdlib, or struct namespace is never stored in a local or upvalue,
		// so an object that resolves as one is a struct instance.
		object = callee->as.member_expr.object;
		if (object->kind == AST_IDENTIFIER_EXPR) {
			token_t object_name = object->as.identifier_expr.name;

			if (compiler_resolve_local(compiler, object_name) != -1 || compiler_resolve_upvalue(compiler, object_name) != -1) {
				type_t member_type = compile_member_expr(compiler, callee);

				if (member_type.kind != TYPE_FUNCTION) {
					compiler_error(compiler, node->line, node->column, 0, "compiler::not-callable", "this expression cannot be called");
					return type_void();
				}
				return compile_value_call(compiler, node, member_type);
			}
		}

		return compile_member_call(compiler, node);
	}

	compiler_error(compiler, node->line, node->column, 0, "compiler::not-callable", "this expression cannot be called");
	return type_void();
}

/* Calls through a value already on the stack. A closure is a runtime
   value with no entry in any signature table, so its arguments are
   checked against callee_type.func_type. */
static type_t compile_value_call(compiler_t *compiler, const ast_node_t *node, type_t callee_type) {
	const func_type_t *ft;
	size_t arg_count;
	size_t i;
	type_t arg_type;
	type_t hint;
	const ast_node_t *arg_node;

	ft = callee_type.func_type;
	arg_count = node->as.call_expr.args.count;

	if (arg_count > UINT8_MAX) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::too-many-arguments", "a call can have at most 255 arguments");
		arg_count = 0;
	}

	if (arg_count != ft->param_count) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-count-mismatch", "expected %zu arguments but got %zu", ft->param_count, arg_count);
	}

	for (i = 0; i < arg_count; i++) {
		arg_node = node->as.call_expr.args.items[i];
		if (i < ft->param_count) {
			hint = ft->param_types[i];
			arg_type = compile_expr_typed(compiler, arg_node, &hint);
			if (!type_is_assignable(ft->param_types[i], arg_type)) {
				compiler_error(compiler, arg_node->line, arg_node->column, 0, "compiler::argument-type-mismatch", "argument %zu expects %s, got %s", i + 1, type_to_string(ft->param_types[i]), type_to_string(arg_type));
			}
		} else {
			compile_expr_typed(compiler, arg_node, NULL);
		}
	}

	compiler_emit_bytes(compiler, OP_CALL, (uint8_t)arg_count, node->line, node->column);

	return ft->has_return_type ? ft->return_type : type_void();
}

static type_t compile_bare_call(compiler_t *compiler, const ast_node_t *node, token_t name) {
	if (signature_table_find_set(compiler->file_signatures, name.start, name.length) != NULL) {
		return compiler_finish_resolved_call(compiler, compiler->file_signatures, name.start, name.length, name.line, name.column, node, OP_GET_GLOBAL, true, NULL, 0, NULL);
	}

	// print(x) accepts any type, which a fixed signature cannot say, so it
	// is handled here. This runs after the file's own table has no match,
	// so a user's `fn print(...)` shadows it.
	if (name.length == 5 && memcmp(name.start, "print", 5) == 0 && node->as.call_expr.args.count == 1) {
		compiler_emit_name_op_str(compiler, OP_GET_GLOBAL, "print", 5, node->line, node->column);
		compile_expr_typed(compiler, node->as.call_expr.args.items[0], NULL);
		compiler_emit_bytes(compiler, OP_CALL, 1, node->line, node->column);
		return type_void();
	}

	return compiler_finish_resolved_call(compiler, &compiler_native_globals, name.start, name.length, name.line, name.column, node, OP_GET_GLOBAL, false, NULL, 0, NULL);
}

type_t compile_member_call(compiler_t *compiler, const ast_node_t *node) {
	const ast_node_t *callee;
	const ast_node_t *object;
	token_t member_name;
	token_t object_name;
	struct_def_t *def;
	const module_unit_t *module;
	size_t m;
	size_t match_count;

	callee = node->as.call_expr.callee;
	object = callee->as.member_expr.object;
	member_name = callee->as.member_expr.name;

	if (object->kind != AST_IDENTIFIER_EXPR) {
		compiler_error(compiler, object->line, object->column, 0, "compiler::unsupported-member-access", "'.' access only works on modules");
		return type_void();
	}

	object_name = object->as.identifier_expr.name;

	if (object_name.length == 6 && memcmp(object_name.start, "stdlib", 6) == 0) {
		return compile_stdlib_call(compiler, node, member_name);
	}

	// A struct type name: `Point.name(args)`, the only way to call a
	// struct function. A local struct wins over an imported one of the
	// same name. Two imports with the same name and no local struct is an
	// ambiguity error.
	def = struct_registry_find(compiler->structs, object_name.start, object_name.length);
	if (def == NULL) {
		match_count = 0;
		for (m = 0; m < compiler->imported_module_count; m++) {
			struct_def_t *imported_def = struct_registry_find(compiler->imported_modules[m]->exported_structs, object_name.start, object_name.length);
			if (imported_def != NULL && imported_def->is_exported) {
				def = imported_def;
				match_count++;
			}
		}
		if (match_count > 1) {
			compiler_error(compiler, object_name.line, object_name.column, (int)object_name.length, "compiler::ambiguous-struct-name", "'%.*s' is exported by more than one imported module", (int)object_name.length, object_name.start);
			return type_void();
		}
	}
	if (def != NULL) {
		return compile_struct_call(compiler, node, def, member_name);
	}

	module = compiler_find_imported_module(compiler->imported_modules, compiler->imported_module_count, object_name.start, object_name.length);
	if (module != NULL) {
		return compile_module_call(compiler, node, member_name, module);
	}

	compiler_error(compiler, object_name.line, object_name.column, (int)object_name.length, "compiler::unknown-module", "'%.*s' is not a known module", (int)object_name.length, object_name.start);
	return type_void();
}

static type_t compile_stdlib_call(compiler_t *compiler, const ast_node_t *node, token_t member_name) {
	if (member_name.length == 6 && memcmp(member_name.start, "length", 6) == 0) {
		return compile_length_call(compiler, node);
	}
	if (member_name.length == 4 && memcmp(member_name.start, "push", 4) == 0) {
		return compile_push_call(compiler, node);
	}
	if (member_name.length == 3 && memcmp(member_name.start, "pop", 3) == 0) {
		return compile_pop_call(compiler, node);
	}
	if (member_name.length == 4 && memcmp(member_name.start, "sort", 4) == 0) {
		return compile_sort_call(compiler, node);
	}

	return compiler_finish_resolved_call(compiler, &compiler_stdlib_members, member_name.start, member_name.length, member_name.line, member_name.column, node, OP_GET_PROPERTY, false, "stdlib", 6, NULL);
}

static type_t compile_module_call(compiler_t *compiler, const ast_node_t *node, token_t member_name, const module_unit_t *module) {
	return compiler_finish_resolved_call(compiler, &module->exports, member_name.start, member_name.length, member_name.line, member_name.column, node, OP_GET_PROPERTY, true, module->local_name, module->local_name_length, NULL);
}

/* `Point.name(args)`. def is already resolved, so nothing is looked up
   by name at runtime. def->namespace_obj is pushed as a constant.
   def->methods belongs to this one struct, so the plain mangled name
   cannot collide with another struct's. */
type_t compile_struct_call(compiler_t *compiler, const ast_node_t *node, const struct_def_t *def, token_t member_name) {
	return compiler_finish_resolved_call(compiler, def->methods, member_name.start, member_name.length, member_name.line, member_name.column, node, OP_GET_PROPERTY, true, NULL, 0, (obj_t *)def->namespace_obj);
}

/* length, push, and pop work on an array of any element type, which a
   fixed signature cannot say ("(T[]) -> int"). They are resolved from
   the compiled type of the argument. The string form of length uses
   the ordinary signature table. */
static type_t compile_length_call(compiler_t *compiler, const ast_node_t *node) {
	type_t arg_type;

	if (node->as.call_expr.args.count != 1) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-count-mismatch", "length expects 1 argument");
		return type_int();
	}

	compiler_emit_name_op_str(compiler, OP_GET_GLOBAL, "stdlib", 6, node->line, node->column);
	compiler_emit_name_op_str(compiler, OP_GET_PROPERTY, "length", 6, node->line, node->column);
	arg_type = compile_expr_typed(compiler, node->as.call_expr.args.items[0], NULL);
	compiler_emit_bytes(compiler, OP_CALL, 1, node->line, node->column);

	if (arg_type.array_depth > 0 || type_equals(arg_type, type_string())) {
		return type_int();
	}

	compiler_error(compiler, node->line, node->column, 0, "compiler::argument-type-mismatch", "length expects a string or an array argument");
	return type_int();
}

static type_t compile_push_call(compiler_t *compiler, const ast_node_t *node) {
	type_t array_type;
	type_t value_type;
	type_t element_expected;

	if (node->as.call_expr.args.count != 2) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-count-mismatch", "push expects 2 arguments");
		return type_void();
	}

	compiler_emit_name_op_str(compiler, OP_GET_GLOBAL, "stdlib", 6, node->line, node->column);
	compiler_emit_name_op_str(compiler, OP_GET_PROPERTY, "push", 4, node->line, node->column);

	array_type = compile_expr_typed(compiler, node->as.call_expr.args.items[0], NULL);

	if (array_type.array_depth == 0) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-type-mismatch", "push expects an array as its first argument");
		compile_expr_typed(compiler, node->as.call_expr.args.items[1], NULL);
		compiler_emit_bytes(compiler, OP_CALL, 2, node->line, node->column);
		return type_void();
	}

	element_expected = type_element_of(array_type);
	value_type = compile_expr_typed(compiler, node->as.call_expr.args.items[1], &element_expected);

	if (!type_is_assignable(element_expected, value_type)) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-type-mismatch", "cannot push this value onto an array of a different element type");
	}

	compiler_emit_bytes(compiler, OP_CALL, 2, node->line, node->column);
	return type_void();
}

static type_t compile_pop_call(compiler_t *compiler, const ast_node_t *node) {
	type_t array_type;

	if (node->as.call_expr.args.count != 1) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-count-mismatch", "pop expects 1 argument");
		return type_void();
	}

	compiler_emit_name_op_str(compiler, OP_GET_GLOBAL, "stdlib", 6, node->line, node->column);
	compiler_emit_name_op_str(compiler, OP_GET_PROPERTY, "pop", 3, node->line, node->column);

	array_type = compile_expr_typed(compiler, node->as.call_expr.args.items[0], NULL);
	compiler_emit_bytes(compiler, OP_CALL, 1, node->line, node->column);

	if (array_type.array_depth == 0) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-type-mismatch", "pop expects an array argument");
		return type_int();
	}

	return type_element_of(array_type);
}

/* stdlib.sort(T[] array, fn(T,T)->bool less_than) sorts in place. The
   comparator reports whether its first argument sorts before its
   second. Handled here because it is generic over T. The comparator's
   required type also depends on T, so it is built here and passed as
   the `expected` hint. A bare function name or a lambda then resolves
   against it. */
static type_t compile_sort_call(compiler_t *compiler, const ast_node_t *node) {
	type_t array_type;
	type_t element_type;
	type_t comparator_type;
	type_t expected;
	func_type_t *expected_ft;

	if (node->as.call_expr.args.count != 2) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-count-mismatch", "sort expects 2 arguments");
		return type_void();
	}

	compiler_emit_name_op_str(compiler, OP_GET_GLOBAL, "stdlib", 6, node->line, node->column);
	compiler_emit_name_op_str(compiler, OP_GET_PROPERTY, "sort", 4, node->line, node->column);

	array_type = compile_expr_typed(compiler, node->as.call_expr.args.items[0], NULL);

	if (array_type.array_depth == 0) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-type-mismatch", "sort expects an array as its first argument");
		compile_expr_typed(compiler, node->as.call_expr.args.items[1], NULL);
		compiler_emit_bytes(compiler, OP_CALL, 2, node->line, node->column);
		return type_void();
	}

	element_type = type_element_of(array_type);

	expected_ft = type_new_func_type();
	expected_ft->param_count = 2;
	expected_ft->param_types = malloc(2 * sizeof(type_t));
	expected_ft->param_types[0] = element_type;
	expected_ft->param_types[1] = element_type;
	expected_ft->has_return_type = true;
	expected_ft->return_type = type_bool();
	expected = type_function(expected_ft);

	comparator_type = compile_expr_typed(compiler, node->as.call_expr.args.items[1], &expected);
	if (!type_equals(expected, comparator_type)) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-type-mismatch", "sort's comparator must be %s, got %s", type_to_string(expected), type_to_string(comparator_type));
	}

	compiler_emit_bytes(compiler, OP_CALL, 2, node->line, node->column);
	return type_void();
}

/* Shared by every kind of call. Resolves name against table by
   argument count and types. The callee reference is reserved first and
   patched after the arguments are compiled (see
   compiler_patch_name_op()). use_mangled_name selects the name looked
   up at runtime: the mangled name for user functions, which can be
   overloaded, and the plain name for natives, which have one
   obj_native_t per name. */
type_t compiler_finish_resolved_call(compiler_t *compiler, const signature_table_t *table, const char *name, size_t name_length, int name_line, int name_column, const ast_node_t *call_node, uint8_t patch_opcode, bool use_mangled_name, const char *prefix_name, size_t prefix_name_length, obj_t *prefix_constant) {
	overload_set_t *set;
	size_t arg_count;
	size_t i;
	type_t *arg_types;
	const type_t *sole_candidate_params;
	bool have_sole_candidate;
	size_t patch_offset;
	const signature_t *resolved;
	signature_match_result_t match;
	const ast_node_t *arg_node;
	type_t hint;

	set = signature_table_find_set(table, name, name_length);
	arg_count = call_node->as.call_expr.args.count;

	if (arg_count > UINT8_MAX) {
		compiler_error(compiler, call_node->line, call_node->column, 0, "compiler::too-many-arguments", "a call can have at most 255 arguments");
		arg_count = 0;
	}

	have_sole_candidate = false;
	sole_candidate_params = NULL;
	if (set != NULL) {
		for (i = 0; i < set->overload_count; i++) {
			if (set->overloads[i].param_count == arg_count) {
				if (have_sole_candidate) {
					have_sole_candidate = false;
					break;
				}
				have_sole_candidate = true;
				sole_candidate_params = set->overloads[i].param_types;
			}
		}
	}

	if (prefix_name != NULL) {
		compiler_emit_name_op_str(compiler, OP_GET_GLOBAL, prefix_name, prefix_name_length, call_node->line, call_node->column);
	} else if (prefix_constant != NULL) {
		// A struct function call. The namespace object is known at compile
		// time, so it is pushed as a constant. A module is looked up by name,
		// because it may live in another file's globals table.
		compiler_emit_constant(compiler, value_obj(prefix_constant), call_node->line, call_node->column);
	}

	patch_offset = compiler_current_chunk(compiler)->count;
	compiler_emit_byte(compiler, patch_opcode, call_node->line, call_node->column);
	compiler_emit_bytes(compiler, 0xff, 0xff, call_node->line, call_node->column);

	arg_types = arg_count > 0 ? malloc(arg_count * sizeof(type_t)) : NULL;
	for (i = 0; i < arg_count; i++) {
		arg_node = call_node->as.call_expr.args.items[i];
		if (have_sole_candidate) {
			hint = sole_candidate_params[i];
			arg_types[i] = compile_expr_typed(compiler, arg_node, &hint);
		} else {
			arg_types[i] = compile_expr_typed(compiler, arg_node, NULL);
		}
	}

	if (set == NULL) {
		compiler_error(compiler, name_line, name_column, (int)name_length, "compiler::undefined-function", "undefined function '%.*s'", (int)name_length, name);
		free(arg_types);
		return type_void();
	}

	match = signature_table_resolve(table, name, name_length, arg_types, arg_count, &resolved);

	if (match != SIGNATURE_MATCH_OK) {
		char arg_list[256];
		size_t pos;

		pos = 0;
		for (i = 0; i < arg_count && pos < sizeof(arg_list) - 16; i++) {
			pos += (size_t)snprintf(arg_list + pos, sizeof(arg_list) - pos, "%s%s", i > 0 ? ", " : "", type_to_string(arg_types[i]));
		}
		arg_list[pos] = '\0';

		compiler_error(compiler, name_line, name_column, (int)name_length, "compiler::no-matching-overload", "no overload of '%.*s' takes (%s)", (int)name_length, name, arg_list);
		free(arg_types);
		return type_void();
	}
	free(arg_types);

	if (resolved->is_deprecated) {
		if (resolved->deprecated_message_length > 0) {
			compiler_warning(compiler, name_line, name_column, (int)name_length, "compiler::deprecated-call", "'%.*s' is deprecated: %.*s", (int)name_length, name, (int)resolved->deprecated_message_length, resolved->deprecated_message);
		} else {
			compiler_warning(compiler, name_line, name_column, (int)name_length, "compiler::deprecated-call", "'%.*s' is deprecated", (int)name_length, name);
		}
	}

	if (use_mangled_name) {
		compiler_patch_name_op(compiler, patch_offset, resolved->mangled_name, strlen(resolved->mangled_name), call_node->line, call_node->column);
	} else {
		compiler_patch_name_op(compiler, patch_offset, name, name_length, call_node->line, call_node->column);
	}

	compiler_emit_bytes(compiler, OP_CALL, (uint8_t)arg_count, call_node->line, call_node->column);

	return resolved->has_return_type ? resolved->return_type : type_void();
}
