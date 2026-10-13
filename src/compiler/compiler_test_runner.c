#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_internal.h"

static bool resolve_test_function(const ast_node_t *fn_node, const signature_table_t *file_signatures, const signature_t **out_sig);
static void emit_test_progress(compiler_t *script, int current, int total, const char *name, size_t name_length, int line, int column);

/* True, with *out_sig set, if fn_node is a well-formed @test function.
   A malformed one was already reported by compiler_apply_decorators().
   Both passes of compiler_emit_test_runner() use this, so the count
   and the calls always agree. */
static bool resolve_test_function(const ast_node_t *fn_node, const signature_table_t *file_signatures, const signature_t **out_sig) {
	size_t d;
	const ast_node_t *decorator_node;
	token_t dname;
	token_t fname;

	for (d = 0; d < fn_node->as.function_decl.decorators.count; d++) {
		decorator_node = fn_node->as.function_decl.decorators.items[d];
		dname = decorator_node->as.decorator.name;
		if (dname.length == 4 && memcmp(dname.start, "test", 4) == 0) {
			fname = fn_node->as.function_decl.name;
			return signature_table_resolve(file_signatures, fname.start, fname.length, NULL, 0, out_sig) == SIGNATURE_MATCH_OK;
		}
	}
	return false;
}

/* Emits a `$test_progress(current, total, name)` call, the per-test
   status line. It is a native (see stdlib.c) because it needs a
   terminal check the language cannot express. current and total are
   constants: the runner is fully unrolled, one call per test. */
static void emit_test_progress(compiler_t *script, int current, int total, const char *name, size_t name_length, int line, int column) {
	obj_string_t *name_str;

	name_str = object_new_string(script->heap, script->strings, name, name_length);
	compiler_emit_name_op_str(script, OP_GET_GLOBAL, "$test_progress", 14, line, column);
	compiler_emit_constant(script, value_int(current), line, column);
	compiler_emit_constant(script, value_int(total), line, column);
	compiler_emit_constant(script, value_obj((obj_t *)name_str), line, column);
	compiler_emit_bytes(script, OP_CALL, 3, line, column);
	compiler_emit_byte(script, OP_POP, line, column);
}

/* Emits the test runner that replaces the call to main() in
   run_tests_mode. For each @test function it emits a progress update
   and a call wrapped in OP_CLOSURE and OP_TRY_CALL, as
   compile_try_expr() does for a source-level try/catch.

   Catching each failure lets every test run. The caught message is
   what vm_runtime_error() was given, with no file, line, or backtrace.
   OP_TRY_CALL has unwound the frames by then. The test's name is
   printed next to the message.

   After the last test the runner prints a summary line. If any test
   failed it throws a final error, so the run ends with a nonzero exit
   code like any uncaught error. */
void compiler_emit_test_runner(compiler_t *script, const ast_node_t *program, const signature_table_t *file_signatures) {
	size_t i;
	const ast_node_t *fn_node;
	token_t fname;
	const signature_t *test_sig;
	int total_tests;
	int test_index;
	int pass_slot;
	int fail_slot;
	int msg_slot;
	size_t try_call_offset;
	size_t end_jump_offset;
	size_t skip_throw_jump;
	compiler_t test_body;
	obj_string_t *failure_msg;

	total_tests = 0;
	for (i = 0; i < program->as.program.functions.count; i++) {
		if (resolve_test_function(program->as.program.functions.items[i], file_signatures, &test_sig)) {
			total_tests++;
		}
	}

	{
		char header[64];
		int header_len;

		header_len = snprintf(header, sizeof(header), "Found %d Tests, running now.", total_tests);
		compiler_emit_name_op_str(script, OP_GET_GLOBAL, "print", 5, program->line, program->column);
		compiler_emit_constant(script, value_obj((obj_t *)object_new_string(script->heap, script->strings, header, (size_t)(header_len > 0 ? header_len : 0))), program->line, program->column);
		compiler_emit_bytes(script, OP_CALL, 1, program->line, program->column);
		compiler_emit_byte(script, OP_POP, program->line, program->column);
	}

	// vm_run() pushes the script's function value before the first
	// instruction and points the outermost frame's slots at it. So runtime
	// slot 0 is taken. This placeholder local claims compile-time slot 0
	// with no push, so $test_pass_count below gets the slot its
	// OP_CONSTANT really pushes to.
	if (compiler_declare_local(script, compiler_synthetic_token("$test_script_self"), type_void(), program->line)) {
		compiler_mark_initialized(script);
	}

	compiler_emit_constant(script, value_int(0), program->line, program->column);
	if (compiler_declare_local(script, compiler_synthetic_token("$test_pass_count"), type_int(), program->line)) {
		compiler_mark_initialized(script);
	}
	pass_slot = script->local_count - 1;

	compiler_emit_constant(script, value_int(0), program->line, program->column);
	if (compiler_declare_local(script, compiler_synthetic_token("$test_fail_count"), type_int(), program->line)) {
		compiler_mark_initialized(script);
	}
	fail_slot = script->local_count - 1;

	test_index = 0;
	for (i = 0; i < program->as.program.functions.count; i++) {
		fn_node = program->as.program.functions.items[i];

		if (!resolve_test_function(fn_node, file_signatures, &test_sig)) {
			continue;
		}
		fname = fn_node->as.function_decl.name;
		test_index++;

		emit_test_progress(script, test_index, total_tests, fname.start, fname.length, fn_node->line, fn_node->column);

		// The try body: a closure with no parameters that calls the test and
		// discards the result. This is the setup compile_closure_body() does,
		// without an AST to compile from.
		test_body.function = object_new_function(script->heap);
		test_body.function->name = object_new_string(script->heap, script->strings, "<test>", 6);
		test_body.function->arity = 0;
		test_body.function->globals = script->function->globals;
		test_body.function->module = script->function->module;
		test_body.function->filename = script->filename;
		test_body.function->source = script->source;

		test_body.heap = script->heap;
		test_body.strings = script->strings;
		test_body.filename = script->filename;
		test_body.source = script->source;
		test_body.had_error = false;
		test_body.is_module_def = script->is_module_def;
		test_body.compiling_main = false;
		test_body.return_type = type_void();
		test_body.has_return_type = false;
		test_body.file_signatures = script->file_signatures;
		test_body.home_module = script->home_module;
		test_body.out_exports = script->out_exports;
		test_body.imported_modules = script->imported_modules;
		test_body.imported_module_count = script->imported_module_count;
		test_body.structs = script->structs;
		test_body.globals = script->globals;
		test_body.local_count = 0;
		test_body.scope_depth = 0;
		memset(test_body.local_reg_seen, 0, sizeof(test_body.local_reg_seen));
		memset(test_body.local_reg_eligible, 0, sizeof(test_body.local_reg_eligible));
		memset(test_body.local_reg_decl_end, 0, sizeof(test_body.local_reg_decl_end));
		memset(test_body.local_reg_dead_at, 0, sizeof(test_body.local_reg_dead_at));
		test_body.local_slot_high_water = 0;
		test_body.enclosing = script;
		test_body.upvalue_count = 0;
		test_body.loops = NULL;
		test_body.loop_count = 0;
		test_body.loop_capacity = 0;

		compiler_emit_name_op_str(&test_body, OP_GET_GLOBAL, test_sig->mangled_name, strlen(test_sig->mangled_name), fn_node->line, fn_node->column);
		compiler_emit_bytes(&test_body, OP_CALL, 0, fn_node->line, fn_node->column);
		compiler_emit_byte(&test_body, OP_POP, fn_node->line, fn_node->column);
		compiler_emit_byte(&test_body, OP_NULL, fn_node->line, fn_node->column);
		compiler_emit_byte(&test_body, OP_RETURN, fn_node->line, fn_node->column);

		compiler_finalize_local_reg_info(&test_body);
		script->had_error = script->had_error || test_body.had_error;
		free(test_body.loops);

		// A test that prints would mix its output into the [OK]/[FAIL] line
		// and the progress line. stdout is suppressed for the call and
		// restored before the report line prints.
		compiler_emit_name_op_str(script, OP_GET_GLOBAL, "$test_output_suppress_begin", 27, fn_node->line, fn_node->column);
		compiler_emit_bytes(script, OP_CALL, 0, fn_node->line, fn_node->column);
		compiler_emit_byte(script, OP_POP, fn_node->line, fn_node->column);

		compiler_emit_closure(script, test_body.function, &test_body, fn_node->line, fn_node->column);

		try_call_offset = compiler_emit_jump(script, OP_TRY_CALL, fn_node->line, fn_node->column);

		// Success: the try body's null return value is on top of the stack.
		compiler_emit_byte(script, OP_POP, fn_node->line, fn_node->column);
		compiler_emit_name_op_str(script, OP_GET_GLOBAL, "$test_output_suppress_end", 25, fn_node->line, fn_node->column);
		compiler_emit_bytes(script, OP_CALL, 0, fn_node->line, fn_node->column);
		compiler_emit_byte(script, OP_POP, fn_node->line, fn_node->column);
		compiler_emit_name_op_str(script, OP_GET_GLOBAL, "$test_report_pass", 17, fn_node->line, fn_node->column);
		compiler_emit_constant(script, value_obj((obj_t *)object_new_string(script->heap, script->strings, fname.start, fname.length)), fn_node->line, fn_node->column);
		compiler_emit_bytes(script, OP_CALL, 1, fn_node->line, fn_node->column);
		compiler_emit_byte(script, OP_POP, fn_node->line, fn_node->column);
		compiler_emit_local_increment(script, pass_slot, fn_node->line, fn_node->column);
		end_jump_offset = compiler_emit_jump(script, OP_JUMP, fn_node->line, fn_node->column);

		// Catch: OP_TRY_CALL jumped here with the failure message pushed.
		compiler_patch_jump(script, try_call_offset, fn_node->line);
		compiler_begin_scope(script);
		if (compiler_declare_local(script, compiler_synthetic_token("$test_msg"), type_string(), fn_node->line)) {
			compiler_mark_initialized(script);
		}
		msg_slot = script->local_count - 1;

		compiler_emit_name_op_str(script, OP_GET_GLOBAL, "$test_output_suppress_end", 25, fn_node->line, fn_node->column);
		compiler_emit_bytes(script, OP_CALL, 0, fn_node->line, fn_node->column);
		compiler_emit_byte(script, OP_POP, fn_node->line, fn_node->column);
		compiler_emit_name_op_str(script, OP_GET_GLOBAL, "$test_report_failure", 20, fn_node->line, fn_node->column);
		compiler_emit_constant(script, value_obj((obj_t *)object_new_string(script->heap, script->strings, fname.start, fname.length)), fn_node->line, fn_node->column);
		compiler_emit_bytes(script, OP_GET_LOCAL, (uint8_t)msg_slot, fn_node->line, fn_node->column);
		compiler_emit_bytes(script, OP_CALL, 2, fn_node->line, fn_node->column);
		compiler_emit_byte(script, OP_POP, fn_node->line, fn_node->column);

		compiler_emit_local_increment(script, fail_slot, fn_node->line, fn_node->column);
		compiler_end_scope(script, fn_node->line, fn_node->column);

		compiler_patch_jump(script, end_jump_offset, fn_node->line);
	}

	compiler_emit_name_op_str(script, OP_GET_GLOBAL, "$test_report_summary", 20, program->line, program->column);
	compiler_emit_constant(script, value_int(total_tests), program->line, program->column);
	compiler_emit_bytes(script, OP_GET_LOCAL, (uint8_t)pass_slot, program->line, program->column);
	compiler_emit_bytes(script, OP_GET_LOCAL, (uint8_t)fail_slot, program->line, program->column);
	compiler_emit_bytes(script, OP_CALL, 3, program->line, program->column);
	compiler_emit_byte(script, OP_POP, program->line, program->column);

	// if ($test_fail_count > 0) throw "...";
	compiler_emit_bytes(script, OP_GET_LOCAL, (uint8_t)fail_slot, program->line, program->column);
	compiler_emit_constant(script, value_int(0), program->line, program->column);
	compiler_emit_byte(script, OP_GREATER, program->line, program->column);
	skip_throw_jump = compiler_emit_jump(script, OP_JUMP_IF_FALSE, program->line, program->column);
	compiler_emit_byte(script, OP_POP, program->line, program->column);
	failure_msg = object_new_string(script->heap, script->strings, "one or more tests failed", 25);
	compiler_emit_constant(script, value_obj((obj_t *)failure_msg), program->line, program->column);
	compiler_emit_byte(script, OP_THROW, program->line, program->column);
	compiler_patch_jump(script, skip_throw_jump, program->line);
	compiler_emit_byte(script, OP_POP, program->line, program->column);
}
