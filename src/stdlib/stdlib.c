#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
#include "stdlib.h"

static bool native_print(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_assert(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_test_progress(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_test_output_suppress_begin(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_test_output_suppress_end(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_test_report_pass(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_test_report_failure(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_test_report_summary(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool test_output_is_tty();
static void register_global(vm_t *vm, table_t *globals, const char *name, native_fn_t function);

static bool native_print(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	if (arg_count >= 1) {
		value_print(args[0], stdout);
	}
	fputs("\n", stdout);
	*out_result = value_null();
	return true;
}

/* assert(condition) or assert(condition, message). Only null and false
   are falsey. A failed assertion is a runtime error, so a test file
   exits nonzero exactly when an assertion fails. */
static bool native_assert(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_string_t *message;

	if (arg_count != 1 && arg_count != 2) {
		vm_runtime_error(vm, "vm::argument-count-mismatch", "assert expects 1 or 2 arguments but got %d", arg_count);
		return false;
	}

	if (value_is_falsey(args[0])) {
		if (arg_count == 2 && value_is_obj(args[1]) && value_as_obj(args[1])->type == OBJ_STRING) {
			message = (obj_string_t *)value_as_obj(args[1]);
			vm_runtime_error(vm, "vm::assertion-failed", "assertion failed: %.*s", (int)message->length, message->chars);
		} else {
			vm_runtime_error(vm, "vm::assertion-failed", "assertion failed");
		}
		return false;
	}

	*out_result = value_null();
	return true;
}

/* True if test progress can redraw one line in place. On a pipe or
   file each test gets its own plain line. scripts/run_tests.sh counts
   those lines. Checked once and cached. */
static bool test_output_is_tty() {
	static bool checked = false;
	static bool is_tty = false;

	if (!checked) {
		is_tty = platform_stream_is_tty(stdout);
		checked = true;
	}
	return is_tty;
}

/* A development knob. With PAWELI_TEST_STEP_DELAY_MS set to a number
   of milliseconds, the runner pauses that long per test, so a person
   can watch the progress line. Unset, tests run at full speed. */
static long test_step_delay_ms() {
	static bool checked = false;
	static long ms = 0;
	const char *env;

	if (!checked) {
		env = getenv("PAWELI_TEST_STEP_DELAY_MS");
		ms = (env != NULL && env[0] != '\0') ? strtol(env, NULL, 10) : 0;
		checked = true;
	}
	return ms;
}

static void test_step_sleep() {
	long ms = test_step_delay_ms();

	if (ms > 0) {
		platform_sleep_ms(ms);
	}
}

// The saved stdout while a test's output is suppressed, or
// PLATFORM_NO_SAVED_STDOUT. Only one @test runs at a time, so one slot
// is enough.
static int g_test_output_saved_fd = PLATFORM_NO_SAVED_STDOUT;

/* $test_output_suppress_begin() and $test_output_suppress_end() go
   around every @test call (see compiler_emit_test_runner()). A test
   that prints would mix its output into the [OK]/[FAIL] line and the
   progress line. The real stdout is redirected, so print() and
   anything else that holds `stdout` are covered. */
static bool native_test_output_suppress_begin(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	(void)args;
	if (!stdlib_check_arity(vm, "$test_output_suppress_begin", arg_count, 0)) {
		return false;
	}

	fflush(stdout);
	g_test_output_saved_fd = platform_stdout_suppress();

	*out_result = value_null();
	return true;
}

static bool native_test_output_suppress_end(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	(void)args;
	if (!stdlib_check_arity(vm, "$test_output_suppress_end", arg_count, 0)) {
		return false;
	}

	fflush(stdout);
	platform_stdout_restore(g_test_output_saved_fd);
	g_test_output_saved_fd = PLATFORM_NO_SAVED_STDOUT;

	*out_result = value_null();
	return true;
}

/* $test_progress(current, total, name) draws the "running now" line
   before each @test call. It only draws on a terminal, where the line
   can be redrawn in place. On a pipe it does nothing, and the
   [OK]/[FAIL] line is the only output per test. paweli source cannot
   call it, since an identifier cannot start with '$'. It is registered
   as a bare global like print. */
static bool native_test_progress(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_string_t *name;

	if (!stdlib_check_arity(vm, "$test_progress", arg_count, 3)) {
		return false;
	}

	if (test_output_is_tty()) {
		name = (obj_string_t *)value_as_obj(args[2]);
		fprintf(stdout, "\r\x1b[2K\x1b[2m... [%lld/%lld] %.*s\x1b[0m", (long long)value_as_int(args[0]), (long long)value_as_int(args[1]), (int)name->length, name->chars);
		fflush(stdout);
		// The delay goes here, before the test runs. A test takes
		// microseconds, so the [OK]/[FAIL] line would replace the progress
		// line before the terminal ever showed it.
		test_step_sleep();
	}

	*out_result = value_null();
	return true;
}

/* $test_report_pass(name) prints the permanent "[OK] NAME" line. On a
   terminal it replaces the progress line. */
static bool native_test_report_pass(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_string_t *name;

	if (!stdlib_check_arity(vm, "$test_report_pass", arg_count, 1)) {
		return false;
	}
	name = (obj_string_t *)value_as_obj(args[0]);

	if (test_output_is_tty()) {
		fprintf(stdout, "\r\x1b[2K\x1b[32m[OK]\x1b[0m %.*s\n", (int)name->length, name->chars);
	} else {
		fprintf(stdout, "[OK] %.*s\n", (int)name->length, name->chars);
	}
	fflush(stdout);

	*out_result = value_null();
	return true;
}

/* $test_report_failure(name, message) prints "[FAIL] NAME" with the
   caught message indented on the next line. */
static bool native_test_report_failure(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_string_t *name;
	obj_string_t *message;

	if (!stdlib_check_arity(vm, "$test_report_failure", arg_count, 2)) {
		return false;
	}
	name = (obj_string_t *)value_as_obj(args[0]);
	message = (obj_string_t *)value_as_obj(args[1]);

	if (test_output_is_tty()) {
		fprintf(stdout, "\r\x1b[2K\x1b[31m[FAIL]\x1b[0m %.*s\n  %.*s\n", (int)name->length, name->chars, (int)message->length, message->chars);
	} else {
		fprintf(stdout, "[FAIL] %.*s\n  %.*s\n", (int)name->length, name->chars, (int)message->length, message->chars);
	}
	fflush(stdout);

	*out_result = value_null();
	return true;
}

/* $test_report_summary(total, passed, failed) prints the one closing
   line of the run, with the pass and fail counts. On a terminal it
   replaces the progress line. It is green when nothing failed and red
   otherwise. */
static bool native_test_report_summary(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	int64_t total;
	int64_t passed;
	int64_t failed;

	if (!stdlib_check_arity(vm, "$test_report_summary", arg_count, 3)) {
		return false;
	}
	total = value_as_int(args[0]);
	passed = value_as_int(args[1]);
	failed = value_as_int(args[2]);

	if (test_output_is_tty()) {
		fprintf(stdout, "\r\x1b[2K\x1b[2m... [%lld/%lld] DONE\x1b[0m %s-- %lld passed, %lld failed\x1b[0m\n", (long long)total, (long long)total, failed > 0 ? "\x1b[31m" : "\x1b[32m", (long long)passed, (long long)failed);
	} else {
		fprintf(stdout, "... [%lld/%lld] DONE -- %lld passed, %lld failed\n", (long long)total, (long long)total, (long long)passed, (long long)failed);
	}

	*out_result = value_null();
	return true;
}

static void register_global(vm_t *vm, table_t *globals, const char *name, native_fn_t function) {
	obj_string_t *key;
	obj_native_t *native;

	key = object_new_string(vm->heap, vm->strings, name, strlen(name));
	native = object_new_native(vm->heap, function, name);
	table_set(globals, key, value_obj((obj_t *)native));
}

bool stdlib_check_arity(vm_t *vm, const char *fn_name, int arg_count, int expected) {
	if (arg_count != expected) {
		vm_runtime_error(vm, "vm::argument-count-mismatch", "%s expects %d argument(s) but got %d", fn_name, expected, arg_count);
		return false;
	}
	return true;
}

void stdlib_register_fn(vm_t *vm, obj_module_t *module, const char *name, native_fn_t function) {
	obj_native_t *native;
	obj_string_t *key;

	native = object_new_native(vm->heap, function, name);
	key = object_new_string(vm->heap, vm->strings, name, strlen(name));
	table_set(module->members, key, value_obj((obj_t *)native));
}

void stdlib_install(vm_t *vm, table_t *globals) {
	obj_string_t *stdlib_name;
	obj_module_t *module;

	register_global(vm, globals, "print", native_print);
	register_global(vm, globals, "assert", native_assert);
	register_global(vm, globals, "$test_progress", native_test_progress);
	register_global(vm, globals, "$test_output_suppress_begin", native_test_output_suppress_begin);
	register_global(vm, globals, "$test_output_suppress_end", native_test_output_suppress_end);
	register_global(vm, globals, "$test_report_pass", native_test_report_pass);
	register_global(vm, globals, "$test_report_failure", native_test_report_failure);
	register_global(vm, globals, "$test_report_summary", native_test_report_summary);

	stdlib_name = object_new_string(vm->heap, vm->strings, "stdlib", 6);
	module = object_new_module(vm->heap, stdlib_name);

	stdlib_register_math(vm, module);
	stdlib_register_string(vm, module);
	stdlib_register_convert(vm, module);
	stdlib_register_array(vm, module);
	stdlib_register_io(vm, module);

	table_set(globals, stdlib_name, value_obj((obj_t *)module));
}
