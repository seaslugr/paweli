#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_internal.h"

void compile_function_decl(const ast_node_t *fn_node, const signature_t *sig, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, bool is_module_def, obj_module_t *home_module, signature_table_t *out_exports, signature_table_t *file_signatures, const module_unit_t *const *imported_modules, size_t imported_module_count, const struct_registry_t *structs, const global_registry_t *globals, bool is_struct_method, bool *had_error) {
	compiler_t compiler;
	size_t i;
	const ast_node_t *param;

	compiler.function = sig->function;
	compiler.heap = heap;
	compiler.strings = strings;
	compiler.filename = filename;
	compiler.source = source;
	compiler.had_error = false;
	compiler.is_module_def = is_module_def;
	// A struct function named "main" is not the entry point.
	// is_struct_method keeps `export` inside it from passing the check in
	// compile_export_stmt().
	compiler.compiling_main = !is_struct_method && fn_node->as.function_decl.name.length == 4 && memcmp(fn_node->as.function_decl.name.start, "main", 4) == 0;
	compiler.return_type = sig->return_type;
	compiler.has_return_type = sig->has_return_type;
	compiler.file_signatures = file_signatures;
	compiler.home_module = home_module;
	compiler.out_exports = out_exports;
	compiler.imported_modules = imported_modules;
	compiler.imported_module_count = imported_module_count;
	compiler.structs = structs;
	compiler.globals = globals;
	compiler.local_count = 0;
	compiler.scope_depth = 0;
	memset(compiler.local_reg_seen, 0, sizeof(compiler.local_reg_seen));
	memset(compiler.local_reg_eligible, 0, sizeof(compiler.local_reg_eligible));
	memset(compiler.local_reg_decl_end, 0, sizeof(compiler.local_reg_decl_end));
	memset(compiler.local_reg_dead_at, 0, sizeof(compiler.local_reg_dead_at));
	compiler.local_slot_high_water = 0;
	// A top-level fn has no enclosing scope and captures nothing.
	compiler.enclosing = NULL;
	compiler.upvalue_count = 0;
	compiler.loops = NULL;
	compiler.loop_count = 0;
	compiler.loop_capacity = 0;

	compiler_begin_scope(&compiler);

	for (i = 0; i < fn_node->as.function_decl.params.count; i++) {
		param = fn_node->as.function_decl.params.items[i];
		// Already validated during signature collection. NULL avoids a second
		// "unknown type" report.
		if (compiler_declare_local(&compiler, param->as.param.name, type_from_type_ref(param->as.param.type, compiler.structs, compiler.imported_modules, compiler.imported_module_count, NULL, NULL), param->line)) {
			compiler_mark_initialized(&compiler);
		}
	}

	compile_statements(&compiler, fn_node->as.function_decl.body);

	// An implicit `return null;` when the body has no return. Every call
	// then leaves exactly one value on the caller's stack. This is not
	// checked against a declared return type, which would need all-paths
	// analysis. Each explicit `return expr;` is checked in
	// compile_return_stmt().
	compiler_emit_byte(&compiler, OP_NULL, fn_node->line, fn_node->column);
	compiler_emit_byte(&compiler, OP_RETURN, fn_node->line, fn_node->column);

	compiler_finalize_local_reg_info(&compiler);

	*had_error = *had_error || compiler.had_error;
	free(compiler.loops);
}

/* Compiles every function in a struct and installs each into the
   struct's namespace object. A top-level fn gets an OP_CONSTANT and
   OP_DEFINE_GLOBAL pair for this. A struct function is written
   straight into namespace_obj->members, since nothing has to run. Each
   signature is looked up again from its parameter types. Call this
   after compiler_collect_struct_method_signatures() has run for every
   struct in the file, so a body can call any struct's function. */
void compile_struct_functions(const ast_node_t *struct_node, struct_def_t *def, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, bool is_module_def, obj_module_t *home_module, signature_table_t *out_exports, signature_table_t *file_signatures, const module_unit_t *const *imported_modules, size_t imported_module_count, const struct_registry_t *structs, const global_registry_t *globals, bool *had_error) {
	size_t i;
	size_t p;
	const ast_node_t *fn_node;
	const ast_node_t *param_node;
	type_t *param_types;
	size_t param_count;
	const signature_t *sig;
	signature_match_result_t match;
	obj_string_t *key;

	for (i = 0; i < struct_node->as.struct_decl.functions.count; i++) {
		fn_node = struct_node->as.struct_decl.functions.items[i];

		param_count = fn_node->as.function_decl.params.count;
		param_types = param_count > 0 ? malloc(param_count * sizeof(type_t)) : NULL;
		for (p = 0; p < param_count; p++) {
			param_node = fn_node->as.function_decl.params.items[p];
			param_types[p] = type_from_type_ref(param_node->as.param.type, structs, imported_modules, imported_module_count, NULL, NULL);
		}

		match = signature_table_resolve(def->methods, fn_node->as.function_decl.name.start, fn_node->as.function_decl.name.length, param_types, param_count, &sig);
		free(param_types);

		if (match != SIGNATURE_MATCH_OK) {
			// A duplicate declaration, already reported. Nothing to compile.
			continue;
		}

		compile_function_decl(fn_node, sig, heap, strings, filename, source, is_module_def, home_module, out_exports, file_signatures, imported_modules, imported_module_count, structs, globals, true, had_error);

		key = object_new_string(heap, strings, sig->mangled_name, strlen(sig->mangled_name));
		table_set(def->namespace_obj->members, key, value_obj((obj_t *)sig->function));
	}
}

/* Emits the OP_CLOSURE that creates inner's compiled function value in
   the enclosing function's chunk. The descriptor bytes after the
   opcode come from inner->upvalues. */
void compiler_emit_closure(compiler_t *compiler, obj_function_t *function, compiler_t *inner, int line, int column) {
	size_t const_index;
	int i;

	const_index = chunk_add_constant(compiler_current_chunk(compiler), value_obj((obj_t *)function));
	if (const_index > UINT16_MAX) {
		compiler_error(compiler, line, column, 0, "compiler::too-many-constants", "too many constants in one function");
		return;
	}
	if (inner->upvalue_count > UINT8_MAX) {
		compiler_error(compiler, line, column, 0, "compiler::too-many-upvalues", "too many upvalues captured in one function");
		return;
	}

	compiler_emit_byte(compiler, OP_CLOSURE, line, column);
	compiler_emit_bytes(compiler, (uint8_t)((const_index >> 8) & 0xff), (uint8_t)(const_index & 0xff), line, column);
	compiler_emit_byte(compiler, (uint8_t)inner->upvalue_count, line, column);
	for (i = 0; i < inner->upvalue_count; i++) {
		compiler_emit_byte(compiler, inner->upvalues[i].is_local ? 1 : 0, line, column);
		compiler_emit_byte(compiler, inner->upvalues[i].index, line, column);
	}
}

/* Shared by compile_nested_function_decl(), compile_lambda_expr(), and
   compile_try_expr(). Builds a compiler_t for the body, compiles it,
   and emits the OP_CLOSURE for its value. The caller resolves the
   parameter and return types first. function_name is only the label in
   a stack trace. params and body are passed separately because a try
   block has a body and no parameters. */
void compile_closure_body(compiler_t *compiler, const ast_list_t *params, ast_node_t *body, type_t *param_types, size_t param_count, type_t return_type, bool has_return_type, token_t function_name, int line, int column) {
	compiler_t inner;
	size_t i;
	const ast_node_t *param;

	inner.function = object_new_function(compiler->heap);
	inner.function->name = object_new_string(compiler->heap, compiler->strings, function_name.start, function_name.length);
	inner.function->arity = (int)param_count;
	inner.function->globals = compiler->function->globals;
	inner.function->module = compiler->function->module;
	inner.function->filename = compiler->filename;
	inner.function->source = compiler->source;

	inner.heap = compiler->heap;
	inner.strings = compiler->strings;
	inner.filename = compiler->filename;
	inner.source = compiler->source;
	inner.had_error = false;
	inner.is_module_def = compiler->is_module_def;
	inner.compiling_main = false;
	inner.return_type = return_type;
	inner.has_return_type = has_return_type;
	inner.file_signatures = compiler->file_signatures;
	inner.home_module = compiler->home_module;
	inner.out_exports = compiler->out_exports;
	inner.imported_modules = compiler->imported_modules;
	inner.imported_module_count = compiler->imported_module_count;
	inner.structs = compiler->structs;
	inner.globals = compiler->globals;
	inner.local_count = 0;
	inner.scope_depth = 0;
	memset(inner.local_reg_seen, 0, sizeof(inner.local_reg_seen));
	memset(inner.local_reg_eligible, 0, sizeof(inner.local_reg_eligible));
	memset(inner.local_reg_decl_end, 0, sizeof(inner.local_reg_decl_end));
	memset(inner.local_reg_dead_at, 0, sizeof(inner.local_reg_dead_at));
	inner.local_slot_high_water = 0;
	inner.enclosing = compiler;
	inner.upvalue_count = 0;
	inner.loops = NULL;
	inner.loop_count = 0;
	inner.loop_capacity = 0;

	compiler_begin_scope(&inner);

	for (i = 0; i < param_count; i++) {
		param = params->items[i];
		if (compiler_declare_local(&inner, param->as.param.name, param_types[i], param->line)) {
			compiler_mark_initialized(&inner);
		}
	}

	compile_statements(&inner, body);

	compiler_emit_byte(&inner, OP_NULL, line, column);
	compiler_emit_byte(&inner, OP_RETURN, line, column);

	compiler_finalize_local_reg_info(&inner);

	compiler->had_error = compiler->had_error || inner.had_error;
	free(inner.loops);

	compiler_emit_closure(compiler, inner.function, &inner, line, column);
}

/* `fn name(...) { ... }` as a statement inside a function. The name is
   declared as a local in the enclosing scope before the body is
   compiled, so a call to itself resolves as an upvalue. That is what
   makes self-recursion work. Nested fn names are not hoisted, so a
   call to a sibling declared later is an ordinary undefined-name
   error.

   OP_CLOSURE is emitted even if compiler_declare_local() failed. A
   duplicate name is already a compile error, so that bytecode never
   runs. */
void compile_nested_function_decl(compiler_t *compiler, const ast_node_t *node) {
	func_type_t *ft;
	type_t fn_type;
	size_t i;
	const ast_node_t *param;
	type_t *param_types;
	size_t param_count;
	type_t return_type;
	bool has_return_type;
	token_t name;

	name = node->as.function_decl.name;
	param_count = node->as.function_decl.params.count;

	param_types = param_count > 0 ? malloc(param_count * sizeof(type_t)) : NULL;
	for (i = 0; i < param_count; i++) {
		param = node->as.function_decl.params.items[i];
		param_types[i] = compiler_resolve_type_ref(compiler, param->as.param.type, param->line, param->column);
	}

	if (param_count > UINT8_MAX) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::too-many-parameters", "a function can have at most 255 parameters");
	}

	has_return_type = node->as.function_decl.has_return_type;
	return_type = has_return_type ? compiler_resolve_type_ref(compiler, node->as.function_decl.return_type, node->line, node->column) : type_void();

	ft = type_new_func_type();
	ft->param_types = param_types;
	ft->param_count = param_count;
	ft->return_type = return_type;
	ft->has_return_type = has_return_type;

	fn_type = type_function(ft);

	{
		int slot = -1;

		if (compiler_declare_local(compiler, name, fn_type, node->line)) {
			slot = compiler->local_count - 1;
			compiler_mark_initialized(compiler);
		}

		// The local was declared before its OP_CLOSURE was emitted, so
		// compiler_note_local_reg_slot() recorded decl_end one instruction too
		// early. The JIT would load the register from whatever was on the
		// stack before the closure existed. Set it again now that OP_CLOSURE
		// is in the chunk.
		compile_closure_body(compiler, &node->as.function_decl.params, node->as.function_decl.body, param_types, param_count, return_type, has_return_type, name, node->line, node->column);

		if (slot >= 0 && compiler->local_reg_eligible[slot]) {
			compiler->local_reg_decl_end[slot] = compiler_current_chunk(compiler)->count;
		}
	}
}

/* `fn(TYPE NAME, ...) -> TYPE { ... }` as an expression. The parser
   gives it an AST_FUNCTION_DECL node with an empty name. There is no
   name to declare, so a lambda cannot call itself. "<lambda>" is only
   its stack-trace label. Returns the TYPE_FUNCTION of the expression. */
type_t compile_lambda_expr(compiler_t *compiler, const ast_node_t *node) {
	func_type_t *ft;
	size_t i;
	const ast_node_t *param;
	type_t *param_types;
	size_t param_count;
	type_t return_type;
	bool has_return_type;
	token_t lambda_name;

	param_count = node->as.function_decl.params.count;

	param_types = param_count > 0 ? malloc(param_count * sizeof(type_t)) : NULL;
	for (i = 0; i < param_count; i++) {
		param = node->as.function_decl.params.items[i];
		param_types[i] = compiler_resolve_type_ref(compiler, param->as.param.type, param->line, param->column);
	}

	if (param_count > UINT8_MAX) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::too-many-parameters", "a function can have at most 255 parameters");
	}

	has_return_type = node->as.function_decl.has_return_type;
	return_type = has_return_type ? compiler_resolve_type_ref(compiler, node->as.function_decl.return_type, node->line, node->column) : type_void();

	ft = type_new_func_type();
	ft->param_types = param_types;
	ft->param_count = param_count;
	ft->return_type = return_type;
	ft->has_return_type = has_return_type;

	lambda_name = (token_t){.type = TOKEN_FN, .start = "<lambda>", .length = 8, .line = node->line, .column = node->column, .error_code = NULL};
	compile_closure_body(compiler, &node->as.function_decl.params, node->as.function_decl.body, param_types, param_count, return_type, has_return_type, lambda_name, node->line, node->column);

	return type_function(ft);
}

/* `expr catch(TYPE fallback)`, or `try { ... } catch(...) { ... }`.
   Both forms parse to the same AST_TRY_EXPR.

   The try block compiles as an anonymous closure with no parameters,
   called through OP_TRY_CALL. run() reports a failure by returning out
   of the whole run() call, so it cannot jump to a catch label in the
   same call. OP_TRY_CALL limits the failure to one reentrant call. See
   vm_try_call().

   The bytecode has the shape of an if/else:

     OP_CLOSURE <try-body>
     OP_TRY_CALL <catch>      success: push the result, fall through
     OP_JUMP <end>            success only: skip the catch region
     <catch>:
     bind or pop the pushed message
     catch_body, if any
     fallback_expr, or OP_NULL
     <end>:

   fallback_expr is compiled after the catch binding's scope closes, so
   it cannot use the caught message. Code that needs the message
   belongs in catch_body. */
type_t compile_try_expr(compiler_t *compiler, const ast_node_t *node) {
	type_t binding_type;
	type_t fallback_type;
	type_t fallback_actual_type;
	type_t try_return_type;
	bool try_has_return_type;
	size_t try_call_offset;
	size_t end_jump_offset;
	token_t try_name;

	try_has_return_type = node->as.try_expr.has_fallback;
	try_return_type = type_void();
	fallback_type = type_void();

	if (node->as.try_expr.has_fallback) {
		fallback_type = compiler_resolve_type_ref(compiler, node->as.try_expr.fallback_type, node->line, node->column);
		try_return_type = fallback_type;
	}

	try_name = (token_t){.type = TOKEN_TRY, .start = "<try>", .length = 5, .line = node->line, .column = node->column, .error_code = NULL};
	compile_closure_body(compiler, NULL, node->as.try_expr.try_body, NULL, 0, try_return_type, try_has_return_type, try_name, node->line, node->column);

	try_call_offset = compiler_emit_jump(compiler, OP_TRY_CALL, node->line, node->column);
	end_jump_offset = compiler_emit_jump(compiler, OP_JUMP, node->line, node->column);
	compiler_patch_jump(compiler, try_call_offset, node->line);

	compiler_begin_scope(compiler);
	if (node->as.try_expr.has_binding) {
		binding_type = compiler_resolve_type_ref(compiler, node->as.try_expr.binding_type, node->line, node->column);
		if (!type_equals(binding_type, type_string())) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::catch-binding-must-be-string", "a caught error is always a string; declare the binding as 'string'");
		}
		if (compiler_declare_local(compiler, node->as.try_expr.binding_name, type_string(), node->line)) {
			compiler_mark_initialized(compiler);
		}
	} else {
		compiler_emit_byte(compiler, OP_POP, node->line, node->column);
	}

	if (node->as.try_expr.catch_body != NULL) {
		compile_statement(compiler, node->as.try_expr.catch_body);
	}

	compiler_end_scope(compiler, node->line, node->column);

	if (node->as.try_expr.has_fallback) {
		fallback_actual_type = compile_expr_typed(compiler, node->as.try_expr.fallback_expr, &fallback_type);
		if (!type_is_assignable(fallback_type, fallback_actual_type)) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "catch's fallback must be %s, got %s", type_to_string(fallback_type), type_to_string(fallback_actual_type));
		}
	} else {
		compiler_emit_byte(compiler, OP_NULL, node->line, node->column);
	}

	compiler_patch_jump(compiler, end_jump_offset, node->line);

	return try_has_return_type ? fallback_type : type_void();
}
