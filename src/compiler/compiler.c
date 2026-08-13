#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "diagnostics/diagnostic.h"

#define MAX_LOCALS 256

typedef struct {
	token_t name;
	int depth;
} local_t;

typedef struct {
	obj_function_t *function;
	obj_t **objects;
	table_t *strings;
	const char *filename;
	/* Whole file's null-terminated text, kept only to render source
	   snippets in diagnostics. compiler_t does not own it. */
	const char *source;
	bool had_error;

	local_t locals[MAX_LOCALS];
	int local_count;
	int scope_depth;
} compiler_t;

static chunk_t *current_chunk(compiler_t *compiler);
static void compiler_error(compiler_t *compiler, int line, int column, const char *code, const char *message);
static void emit_byte(compiler_t *compiler, uint8_t byte, int line, int column);
static void emit_bytes(compiler_t *compiler, uint8_t b1, uint8_t b2, int line, int column);
static size_t emit_jump(compiler_t *compiler, uint8_t instruction, int line, int column);
static void patch_jump(compiler_t *compiler, size_t offset, int line);
static void emit_loop(compiler_t *compiler, size_t loop_start, int line, int column);
static void emit_constant(compiler_t *compiler, value_t value, int line, int column);
static void emit_name_op_str(compiler_t *compiler, uint8_t opcode, const char *name, size_t length, int line, int column);
static void emit_name_op(compiler_t *compiler, uint8_t opcode, token_t name, int line, int column);
static int64_t parse_int_literal(token_t token);
static double parse_float_literal(token_t token);

static void begin_scope(compiler_t *compiler);
static void end_scope(compiler_t *compiler, int line, int column);
static bool identifiers_equal(token_t a, token_t b);
static bool add_local(compiler_t *compiler, token_t name, int line);
static bool declare_local(compiler_t *compiler, token_t name, int line);
static void mark_initialized(compiler_t *compiler);
static int resolve_local(compiler_t *compiler, token_t name);

static void compile_function_decl(const ast_node_t *fn_node, obj_t **objects, table_t *strings, const char *filename, const char *source, bool *had_error, obj_function_t **out_function);

static void compile_statements(compiler_t *compiler, const ast_node_t *block_node);
static void compile_block_stmt(compiler_t *compiler, const ast_node_t *block_node);
static void compile_statement(compiler_t *compiler, const ast_node_t *node);
static void compile_var_decl(compiler_t *compiler, const ast_node_t *node);
static void compile_if_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_while_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_for_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_return_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_expr_stmt(compiler_t *compiler, const ast_node_t *node);

static void compile_expr(compiler_t *compiler, const ast_node_t *node);
static void compile_assign_expr(compiler_t *compiler, const ast_node_t *node);
static void compile_binary_expr(compiler_t *compiler, const ast_node_t *node);
static void compile_logical_expr(compiler_t *compiler, const ast_node_t *node);
static void compile_unary_expr(compiler_t *compiler, const ast_node_t *node);
static void compile_call_expr(compiler_t *compiler, const ast_node_t *node);
static void compile_member_expr(compiler_t *compiler, const ast_node_t *node);
static void compile_identifier_expr(compiler_t *compiler, const ast_node_t *node);

static chunk_t *current_chunk(compiler_t *compiler) {
	return &compiler->function->chunk;
}

static void compiler_error(compiler_t *compiler, int line, int column, const char *code, const char *message) {
	diagnostic_emit(DIAG_ERROR, code, compiler->filename, compiler->source, line, column, "%s", message);
	compiler->had_error = true;
}

static void emit_byte(compiler_t *compiler, uint8_t byte, int line, int column) {
	chunk_write(current_chunk(compiler), byte, line, column);
}

static void emit_bytes(compiler_t *compiler, uint8_t b1, uint8_t b2, int line, int column) {
	emit_byte(compiler, b1, line, column);
	emit_byte(compiler, b2, line, column);
}

static size_t emit_jump(compiler_t *compiler, uint8_t instruction, int line, int column) {
	emit_byte(compiler, instruction, line, column);
	emit_byte(compiler, 0xff, line, column);
	emit_byte(compiler, 0xff, line, column);
	return current_chunk(compiler)->count - 2;
}

static void patch_jump(compiler_t *compiler, size_t offset, int line) {
	size_t jump;

	jump = current_chunk(compiler)->count - offset - 2;

	if (jump > UINT16_MAX) {
		compiler_error(compiler, line, 0, "compiler::jump-too-large", "too much code to jump over");
		return;
	}

	current_chunk(compiler)->code[offset] = (uint8_t)((jump >> 8) & 0xff);
	current_chunk(compiler)->code[offset + 1] = (uint8_t)(jump & 0xff);
}

static void emit_loop(compiler_t *compiler, size_t loop_start, int line, int column) {
	size_t offset;
	size_t jump;

	emit_byte(compiler, OP_LOOP, line, column);

	offset = current_chunk(compiler)->count + 2;
	jump = offset - loop_start;

	if (jump > UINT16_MAX) {
		compiler_error(compiler, line, 0, "compiler::loop-too-large", "loop body too large");
		jump = 0;
	}

	emit_byte(compiler, (uint8_t)((jump >> 8) & 0xff), line, column);
	emit_byte(compiler, (uint8_t)(jump & 0xff), line, column);
}

static void emit_constant(compiler_t *compiler, value_t value, int line, int column) {
	size_t index;

	index = chunk_add_constant(current_chunk(compiler), value);
	if (index > UINT8_MAX) {
		compiler_error(compiler, line, 0, "compiler::too-many-constants", "too many constants in one function");
		return;
	}

	emit_bytes(compiler, OP_CONSTANT, (uint8_t)index, line, column);
}

static void emit_name_op_str(compiler_t *compiler, uint8_t opcode, const char *name, size_t length, int line, int column) {
	obj_string_t *string;
	size_t index;

	string = object_new_string(compiler->objects, compiler->strings, name, length);
	index = chunk_add_constant(current_chunk(compiler), value_obj((obj_t *)string));
	if (index > UINT8_MAX) {
		compiler_error(compiler, line, 0, "compiler::too-many-constants", "too many constants in one function");
		return;
	}

	emit_bytes(compiler, opcode, (uint8_t)index, line, column);
}

static void emit_name_op(compiler_t *compiler, uint8_t opcode, token_t name, int line, int column) {
	emit_name_op_str(compiler, opcode, name.start, name.length, line, column);
}

/* Token text is not null-terminated. The character right after it is not
   guaranteed to be non-numeric: strtod in particular would happily read
   past the token into whatever source text follows, for example an
   exponent suffix. Copy into a bounded buffer first, so parsing cannot
   run past the token's actual end. */
static int64_t parse_int_literal(token_t token) {
	char buffer[64];
	size_t length;

	length = token.length < sizeof(buffer) - 1 ? token.length : sizeof(buffer) - 1;
	memcpy(buffer, token.start, length);
	buffer[length] = '\0';

	return strtoll(buffer, NULL, 10);
}

static double parse_float_literal(token_t token) {
	char buffer[64];
	size_t length;

	length = token.length < sizeof(buffer) - 1 ? token.length : sizeof(buffer) - 1;
	memcpy(buffer, token.start, length);
	buffer[length] = '\0';

	return strtod(buffer, NULL);
}

static void begin_scope(compiler_t *compiler) {
	compiler->scope_depth++;
}

static void end_scope(compiler_t *compiler, int line, int column) {
	compiler->scope_depth--;

	while (compiler->local_count > 0 && compiler->locals[compiler->local_count - 1].depth > compiler->scope_depth) {
		emit_byte(compiler, OP_POP, line, column);
		compiler->local_count--;
	}
}

static bool identifiers_equal(token_t a, token_t b) {
	if (a.length != b.length) {
		return false;
	}
	return memcmp(a.start, b.start, a.length) == 0;
}

/* Returns false (and reports an error) without touching compiler state
   when the locals table is full, so a caller that skips mark_initialized()
   on failure never mistakes the previous local for the one that failed
   to get a slot. */
static bool add_local(compiler_t *compiler, token_t name, int line) {
	local_t *local;

	if (compiler->local_count >= MAX_LOCALS) {
		compiler_error(compiler, line, name.column, "compiler::too-many-locals", "too many local variables in one function");
		return false;
	}

	local = &compiler->locals[compiler->local_count];
	local->name = name;
	local->depth = -1;
	compiler->local_count++;
	return true;
}

/* A local only counts as a collision if it belongs to the current scope.
   Walking backwards, the first local from an outer, lower-depth scope
   means everything past it is legitimately shadowable.

   Returns false (and reports an error) on a duplicate name without adding
   a local. Callers must skip mark_initialized() when this returns false:
   otherwise, with no new local pushed, mark_initialized() would instead
   re-stamp the depth of whichever local already occupies the top slot. */
static bool declare_local(compiler_t *compiler, token_t name, int line) {
	int i;

	for (i = compiler->local_count - 1; i >= 0; i--) {
		if (compiler->locals[i].depth != -1 && compiler->locals[i].depth < compiler->scope_depth) {
			break;
		}
		if (identifiers_equal(name, compiler->locals[i].name)) {
			compiler_error(compiler, line, name.column, "compiler::duplicate-local", "a variable with this name already exists in this scope");
			return false;
		}
	}

	return add_local(compiler, name, line);
}

static void mark_initialized(compiler_t *compiler) {
	compiler->locals[compiler->local_count - 1].depth = compiler->scope_depth;
}

static int resolve_local(compiler_t *compiler, token_t name) {
	int i;

	for (i = compiler->local_count - 1; i >= 0; i--) {
		if (identifiers_equal(name, compiler->locals[i].name)) {
			if (compiler->locals[i].depth == -1) {
				compiler_error(compiler, name.line, name.column, "compiler::local-read-in-own-initializer", "cannot read a local variable in its own initializer");
			}
			return i;
		}
	}

	return -1;
}

static void compile_function_decl(const ast_node_t *fn_node, obj_t **objects, table_t *strings, const char *filename, const char *source, bool *had_error, obj_function_t **out_function) {
	compiler_t compiler;
	size_t i;
	const ast_node_t *param;

	compiler.function = object_new_function(objects);
	compiler.function->name = object_new_string(objects, strings, fn_node->as.function_decl.name.start, fn_node->as.function_decl.name.length);
	compiler.objects = objects;
	compiler.strings = strings;
	compiler.filename = filename;
	compiler.source = source;
	compiler.had_error = false;
	compiler.local_count = 0;
	compiler.scope_depth = 0;

	if (fn_node->as.function_decl.params.count > UINT8_MAX) {
		compiler_error(&compiler, fn_node->line, fn_node->column, "compiler::too-many-parameters", "a function can have at most 255 parameters");
	}
	compiler.function->arity = (int)fn_node->as.function_decl.params.count;

	begin_scope(&compiler);

	for (i = 0; i < fn_node->as.function_decl.params.count; i++) {
		param = fn_node->as.function_decl.params.items[i];
		if (declare_local(&compiler, param->as.param.name, param->line)) {
			mark_initialized(&compiler);
		}
	}

	compile_statements(&compiler, fn_node->as.function_decl.body);

	// Implicit `return null;` if the body has no explicit return.
	// This keeps the call convention uniform: every call leaves
	// exactly one value on the caller's stack.
	emit_byte(&compiler, OP_NULL, fn_node->line, fn_node->column);
	emit_byte(&compiler, OP_RETURN, fn_node->line, fn_node->column);

	*had_error = *had_error || compiler.had_error;
	*out_function = compiler.function;
}

static void compile_statements(compiler_t *compiler, const ast_node_t *block_node) {
	size_t i;

	for (i = 0; i < block_node->as.block.statements.count; i++) {
		compile_statement(compiler, block_node->as.block.statements.items[i]);
	}
}

static void compile_block_stmt(compiler_t *compiler, const ast_node_t *block_node) {
	begin_scope(compiler);
	compile_statements(compiler, block_node);
	end_scope(compiler, block_node->line, block_node->column);
}

static void compile_statement(compiler_t *compiler, const ast_node_t *node) {
	switch (node->kind) {
		case AST_VAR_DECL:
			compile_var_decl(compiler, node);
			break;
		case AST_EXPORT_STMT:
			// Module export semantics are not implemented yet. There is no
			// module system or runtime to export into. This case is
			// deliberately a no-op.
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
		case AST_RETURN_STMT:
			compile_return_stmt(compiler, node);
			break;
		case AST_EXPR_STMT:
			compile_expr_stmt(compiler, node);
			break;
		case AST_BLOCK:
			compile_block_stmt(compiler, node);
			break;
		default:
			compiler_error(compiler, node->line, node->column, "compiler::unsupported-statement", "expected a statement");
			break;
	}
}

static void compile_var_decl(compiler_t *compiler, const ast_node_t *node) {
	compile_expr(compiler, node->as.var_decl.initializer);
	if (declare_local(compiler, node->as.var_decl.name, node->line)) {
		mark_initialized(compiler);
	}
}

static void compile_if_stmt(compiler_t *compiler, const ast_node_t *node) {
	size_t then_jump;
	size_t else_jump;

	compile_expr(compiler, node->as.if_stmt.condition);

	then_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
	emit_byte(compiler, OP_POP, node->line, node->column);
	compile_block_stmt(compiler, node->as.if_stmt.then_branch);

	else_jump = emit_jump(compiler, OP_JUMP, node->line, node->column);

	patch_jump(compiler, then_jump, node->line);
	emit_byte(compiler, OP_POP, node->line, node->column);

	if (node->as.if_stmt.else_branch != NULL) {
		if (node->as.if_stmt.else_branch->kind == AST_IF_STMT) {
			compile_if_stmt(compiler, node->as.if_stmt.else_branch);
		} else {
			compile_block_stmt(compiler, node->as.if_stmt.else_branch);
		}
	}

	patch_jump(compiler, else_jump, node->line);
}

static void compile_while_stmt(compiler_t *compiler, const ast_node_t *node) {
	size_t loop_start;
	size_t exit_jump;

	loop_start = current_chunk(compiler)->count;

	compile_expr(compiler, node->as.while_stmt.condition);
	exit_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
	emit_byte(compiler, OP_POP, node->line, node->column);

	compile_block_stmt(compiler, node->as.while_stmt.body);

	emit_loop(compiler, loop_start, node->line, node->column);

	patch_jump(compiler, exit_jump, node->line);
	emit_byte(compiler, OP_POP, node->line, node->column);
}

static void compile_for_stmt(compiler_t *compiler, const ast_node_t *node) {
	size_t loop_start;
	bool has_exit_jump;
	size_t exit_jump;
	size_t body_jump;
	size_t increment_start;

	begin_scope(compiler);

	if (node->as.for_stmt.initializer != NULL) {
		compile_statement(compiler, node->as.for_stmt.initializer);
	}

	loop_start = current_chunk(compiler)->count;

	has_exit_jump = false;
	exit_jump = 0;
	if (node->as.for_stmt.condition != NULL) {
		compile_expr(compiler, node->as.for_stmt.condition);
		exit_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
		emit_byte(compiler, OP_POP, node->line, node->column);
		has_exit_jump = true;
	}

	if (node->as.for_stmt.increment != NULL) {
		body_jump = emit_jump(compiler, OP_JUMP, node->line, node->column);
		increment_start = current_chunk(compiler)->count;

		compile_expr(compiler, node->as.for_stmt.increment);
		emit_byte(compiler, OP_POP, node->line, node->column);

		emit_loop(compiler, loop_start, node->line, node->column);
		loop_start = increment_start;
		patch_jump(compiler, body_jump, node->line);
	}

	compile_block_stmt(compiler, node->as.for_stmt.body);

	emit_loop(compiler, loop_start, node->line, node->column);

	if (has_exit_jump) {
		patch_jump(compiler, exit_jump, node->line);
		emit_byte(compiler, OP_POP, node->line, node->column);
	}

	end_scope(compiler, node->line, node->column);
}

static void compile_return_stmt(compiler_t *compiler, const ast_node_t *node) {
	if (node->as.return_stmt.value != NULL) {
		compile_expr(compiler, node->as.return_stmt.value);
	} else {
		emit_byte(compiler, OP_NULL, node->line, node->column);
	}
	emit_byte(compiler, OP_RETURN, node->line, node->column);
}

static void compile_expr_stmt(compiler_t *compiler, const ast_node_t *node) {
	compile_expr(compiler, node->as.expr_stmt.expression);
	emit_byte(compiler, OP_POP, node->line, node->column);
}

static void compile_expr(compiler_t *compiler, const ast_node_t *node) {
	switch (node->kind) {
		case AST_ASSIGN_EXPR:
			compile_assign_expr(compiler, node);
			break;
		case AST_BINARY_EXPR:
			if (node->as.binary_expr.op == TOKEN_AMP_AMP || node->as.binary_expr.op == TOKEN_PIPE_PIPE) {
				compile_logical_expr(compiler, node);
			} else {
				compile_binary_expr(compiler, node);
			}
			break;
		case AST_UNARY_EXPR:
			compile_unary_expr(compiler, node);
			break;
		case AST_CALL_EXPR:
			compile_call_expr(compiler, node);
			break;
		case AST_MEMBER_EXPR:
			compile_member_expr(compiler, node);
			break;
		case AST_IDENTIFIER_EXPR:
			compile_identifier_expr(compiler, node);
			break;
		case AST_INT_LITERAL:
			emit_constant(compiler, value_int(parse_int_literal(node->as.literal.token)), node->line, node->column);
			break;
		case AST_FLOAT_LITERAL:
			emit_constant(compiler, value_float(parse_float_literal(node->as.literal.token)), node->line, node->column);
			break;
		case AST_STRING_LITERAL: {
			obj_string_t *string = object_new_string_from_literal(compiler->objects, compiler->strings, node->as.literal.token.start + 1, node->as.literal.token.length - 2);
			emit_constant(compiler, value_obj((obj_t *)string), node->line, node->column);
			break;
		}
		case AST_BOOL_LITERAL:
			emit_byte(compiler, node->as.literal.token.type == TOKEN_TRUE ? OP_TRUE : OP_FALSE, node->line, node->column);
			break;
		case AST_NULL_LITERAL:
			emit_byte(compiler, OP_NULL, node->line, node->column);
			break;
		default:
			compiler_error(compiler, node->line, node->column, "compiler::unsupported-expression", "expected an expression");
			break;
	}
}

static void compile_assign_expr(compiler_t *compiler, const ast_node_t *node) {
	int slot;

	compile_expr(compiler, node->as.assign_expr.value);

	if (node->as.assign_expr.target->kind == AST_IDENTIFIER_EXPR) {
		slot = resolve_local(compiler, node->as.assign_expr.target->as.identifier_expr.name);
		if (slot == -1) {
			compiler_error(compiler, node->line, node->column, "compiler::undeclared-assignment-target", "cannot assign to an undeclared or non-local variable");
			return;
		}
		emit_bytes(compiler, OP_SET_LOCAL, (uint8_t)slot, node->line, node->column);
		return;
	}

	compiler_error(compiler, node->line, node->column, "compiler::unsupported-member-assignment", "assignment to member expressions is not supported yet");
}

static void compile_binary_expr(compiler_t *compiler, const ast_node_t *node) {
	uint8_t opcode;

	compile_expr(compiler, node->as.binary_expr.left);
	compile_expr(compiler, node->as.binary_expr.right);

	switch (node->as.binary_expr.op) {
		case TOKEN_PLUS: opcode = OP_ADD; break;
		case TOKEN_MINUS: opcode = OP_SUBTRACT; break;
		case TOKEN_STAR: opcode = OP_MULTIPLY; break;
		case TOKEN_SLASH: opcode = OP_DIVIDE; break;
		case TOKEN_PERCENT: opcode = OP_MODULO; break;
		case TOKEN_EQUAL_EQUAL: opcode = OP_EQUAL; break;
		case TOKEN_BANG_EQUAL: opcode = OP_NOT_EQUAL; break;
		case TOKEN_LESS: opcode = OP_LESS; break;
		case TOKEN_LESS_EQUAL: opcode = OP_LESS_EQUAL; break;
		case TOKEN_GREATER: opcode = OP_GREATER; break;
		case TOKEN_GREATER_EQUAL: opcode = OP_GREATER_EQUAL; break;
		default:
			compiler_error(compiler, node->line, node->column, "compiler::unsupported-binary-operator", "unsupported binary operator");
			return;
	}

	emit_byte(compiler, opcode, node->line, node->column);
}

/* && and || short-circuit via jumps instead of compiling to a plain binary
   opcode. The right-hand side must not run at all once the left side
   already determines the result. */
static void compile_logical_expr(compiler_t *compiler, const ast_node_t *node) {
	size_t jump;
	size_t end_jump;

	compile_expr(compiler, node->as.binary_expr.left);

	if (node->as.binary_expr.op == TOKEN_AMP_AMP) {
		jump = emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
		emit_byte(compiler, OP_POP, node->line, node->column);
		compile_expr(compiler, node->as.binary_expr.right);
		patch_jump(compiler, jump, node->line);
		return;
	}

	jump = emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
	end_jump = emit_jump(compiler, OP_JUMP, node->line, node->column);
	patch_jump(compiler, jump, node->line);
	emit_byte(compiler, OP_POP, node->line, node->column);
	compile_expr(compiler, node->as.binary_expr.right);
	patch_jump(compiler, end_jump, node->line);
}

static void compile_unary_expr(compiler_t *compiler, const ast_node_t *node) {
	compile_expr(compiler, node->as.unary_expr.operand);

	switch (node->as.unary_expr.op) {
		case TOKEN_MINUS:
			emit_byte(compiler, OP_NEGATE, node->line, node->column);
			break;
		case TOKEN_BANG:
			emit_byte(compiler, OP_NOT, node->line, node->column);
			break;
		default:
			compiler_error(compiler, node->line, node->column, "compiler::unsupported-unary-operator", "unsupported unary operator");
			break;
	}
}

static void compile_call_expr(compiler_t *compiler, const ast_node_t *node) {
	size_t i;
	size_t arg_count;

	compile_expr(compiler, node->as.call_expr.callee);

	arg_count = node->as.call_expr.args.count;
	if (arg_count > UINT8_MAX) {
		compiler_error(compiler, node->line, node->column, "compiler::too-many-arguments", "a call can have at most 255 arguments");
		arg_count = 0;
	}

	for (i = 0; i < arg_count; i++) {
		compile_expr(compiler, node->as.call_expr.args.items[i]);
	}

	emit_bytes(compiler, OP_CALL, (uint8_t)arg_count, node->line, node->column);
}

static void compile_member_expr(compiler_t *compiler, const ast_node_t *node) {
	compile_expr(compiler, node->as.member_expr.object);
	emit_name_op(compiler, OP_GET_PROPERTY, node->as.member_expr.name, node->line, node->column);
}

static void compile_identifier_expr(compiler_t *compiler, const ast_node_t *node) {
	int slot;

	slot = resolve_local(compiler, node->as.identifier_expr.name);
	if (slot != -1) {
		emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)slot, node->line, node->column);
		return;
	}

	emit_name_op(compiler, OP_GET_GLOBAL, node->as.identifier_expr.name, node->line, node->column);
}

obj_function_t *compiler_compile(const ast_node_t *program, const char *filename, const char *source, obj_t **objects, table_t *strings) {
	compiler_t script;
	size_t i;
	const ast_node_t *fn_node;
	bool had_error;
	bool found_main;
	obj_function_t *fn;

	had_error = false;
	found_main = false;

	script.function = object_new_function(objects);
	script.function->name = NULL;
	script.objects = objects;
	script.strings = strings;
	script.filename = filename;
	script.source = source;
	script.had_error = false;
	script.local_count = 0;
	script.scope_depth = 0;

	// program->as.program.directive (#module / #moduledef) is
	// intentionally ignored. The module system is not implemented yet.

	for (i = 0; i < program->as.program.functions.count; i++) {
		fn_node = program->as.program.functions.items[i];

		if (fn_node->as.function_decl.name.length == 4 && memcmp(fn_node->as.function_decl.name.start, "main", 4) == 0) {
			found_main = true;
		}

		compile_function_decl(fn_node, objects, strings, filename, source, &had_error, &fn);

		emit_constant(&script, value_obj((obj_t *)fn), fn_node->line, fn_node->column);
		emit_name_op(&script, OP_DEFINE_GLOBAL, fn_node->as.function_decl.name, fn_node->line, fn_node->column);
	}

	if (!found_main) {
		compiler_error(&script, program->line, program->column, "compiler::missing-main", "no 'main' function found");
	}

	emit_name_op_str(&script, OP_GET_GLOBAL, "main", 4, program->line, program->column);
	emit_bytes(&script, OP_CALL, 0, program->line, program->column);
	emit_byte(&script, OP_POP, program->line, program->column);

	emit_byte(&script, OP_NULL, program->line, program->column);
	emit_byte(&script, OP_RETURN, program->line, program->column);

	had_error = had_error || script.had_error;

	return had_error ? NULL : script.function;
}
