#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "stdlib.h"

static bool native_print(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_assert(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_test_progress(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_test_output_suppress_begin(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_test_output_suppress_end(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_test_report_pass(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_test_report_failure(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_test_report_summary(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool test_output_is_tty(void);
static void register_global(vm_t *vm, table_t *globals, const char *name, native_fn_t function);

static bool native_print(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	if (arg_count >= 1) {
		value_print(args[0], stdout);
	}
	fputs("\n", stdout);
	*out_result = value_null();
	return true;
}

/* assert(condition) or assert(condition, message). condition uses the
   same truthiness rules as if/while: only null and false are falsey.
   A failing assertion is a runtime error. That is what makes a test
   file self-checking. It exits nonzero exactly when an assertion turns
   out false, with no external expected-output file needed. */
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

/* Whether --run-tests' progress reporting should overwrite one line in
   place (a real terminal) or fall back to one static line per test (a
   pipe/redirect, or output captured to a file): the same call site that
   printed "running: NAME" per test before scripts/run_tests.sh existed,
   and still does when stdout isn't a tty, since that script greps
   captured output for exactly that line to catch a test category
   silently missing from main.pw. Checked once and cached: stdout either
   is or isn't a terminal for this process's whole lifetime. */
static bool test_output_is_tty(void) {
	static bool checked = false;
	static bool is_tty = false;

	if (!checked) {
		is_tty = isatty(fileno(stdout)) != 0;
		checked = true;
	}
	return is_tty;
}

/* Dev-only pacing knob, same idea as PAWELI_GC_STRESS/PAWELI_JIT_THRESHOLD
   elsewhere in this codebase: unset, --run-tests runs at full speed (the
   only thing that matters for CI/scripts/run_tests.sh); set to a
   millisecond count, native_test_report_pass()/_failure() below each
   pause that long right after printing, purely so a human watching a
   real terminal can actually see the live progress line move instead of
   the whole suite finishing before it's readable. */
static long test_step_delay_ms(void) {
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

static void test_step_sleep(void) {
	long ms;
	struct timespec ts;

	ms = test_step_delay_ms();
	if (ms <= 0) {
		return;
	}
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

/* fd 1's saved copy while a test's own stdout is redirected to
   /dev/null (native_test_output_suppress_begin()/_end() below), so it
   can be restored exactly. -1 whenever nothing is currently suppressed:
   both the initial state and, defensively, once restored. There is
   only ever one @test running at a time -- run_tests_mode never nests
   or parallelizes tests -- so one process-wide slot is enough. */
static int g_test_output_saved_fd = -1;

/* $test_output_suppress_begin()/$test_output_suppress_end(): bracket
   every @test call (see compiler.c's emit_test_runner()) so a test that
   prints -- e.g. tests/suite/basic.pw's test_print(), which exists
   purely to prove print() works at all -- doesn't interleave its output
   with the [OK]/[FAIL] report line or the live progress line's
   in-place \r redraw. Redirecting the real fd 1 (not just swapping
   `stdout`'s FILE*) is what makes this transparent to native_print()
   and anything else already holding a `stdout` reference. Unconditional
   in both tty and non-tty mode: a piped/logged run benefits from the
   quiet output exactly the same way a live terminal does. */
static bool native_test_output_suppress_begin(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	int devnull_fd;

	(void)args;
	if (!stdlib_check_arity(vm, "$test_output_suppress_begin", arg_count, 0)) {
		return false;
	}

	fflush(stdout);
	devnull_fd = open("/dev/null", O_WRONLY);
	if (devnull_fd != -1) {
		g_test_output_saved_fd = dup(STDOUT_FILENO);
		dup2(devnull_fd, STDOUT_FILENO);
		close(devnull_fd);
	}

	*out_result = value_null();
	return true;
}

static bool native_test_output_suppress_end(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	(void)args;
	if (!stdlib_check_arity(vm, "$test_output_suppress_end", arg_count, 0)) {
		return false;
	}

	fflush(stdout);
	if (g_test_output_saved_fd != -1) {
		dup2(g_test_output_saved_fd, STDOUT_FILENO);
		close(g_test_output_saved_fd);
		g_test_output_saved_fd = -1;
	}

	*out_result = value_null();
	return true;
}

/* $test_progress(current, total, name): a live "what's running right
   now" line, redrawn in place before each @test call -- only on a real
   terminal, where overwriting the same line means anything. On a
   pipe/redirect there is nothing useful to overwrite (and no reader
   watching it happen live anyway), so this is a no-op there; the
   permanent [OK]/[FAIL] line native_test_report_pass()/
   native_test_report_failure() print once the test actually finishes
   is the only per-test output in that mode, same as the "running: NAME"
   line scripts/run_tests.sh's pass-count check used to grep for -- that
   check now counts "[OK] "/"[FAIL] " instead, one of which every test
   is guaranteed to print in both modes. Never reachable from ordinary
   paweli source (an identifier can't start with '$', see
   synthetic_token()'s own comment in compiler.c), so this and its
   siblings below are registered as bare globals the same way
   print/assert are, without needing a native_signatures.c entry nobody
   could ever call anyway. */
static bool native_test_progress(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_string_t *name;

	if (!stdlib_check_arity(vm, "$test_progress", arg_count, 3)) {
		return false;
	}

	if (test_output_is_tty()) {
		name = (obj_string_t *)value_as_obj(args[2]);
		fprintf(stdout, "\r\x1b[2K\x1b[2m... [%lld/%lld] %.*s\x1b[0m", (long long)value_as_int(args[0]), (long long)value_as_int(args[1]), (int)name->length, name->chars);
		fflush(stdout);
		// The delay belongs here, not after the test's own [OK]/[FAIL]
		// line prints: a test itself runs in microseconds, so without
		// this the progress line drawn above would be overwritten by
		// the [OK]/[FAIL] line before any terminal ever gets a chance
		// to paint a frame with it visible, defeating the whole point
		// of test_step_delay_ms().
		test_step_sleep();
	}

	*out_result = value_null();
	return true;
}

/* $test_report_pass(name): the permanent "[OK] NAME" line a passing
   test leaves behind, overwriting whatever $test_progress() last drew
   on that same line (a real terminal) or just printed plain (a
   pipe/redirect, where $test_progress() never drew anything to begin
   with). */
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

/* $test_report_failure(name, message): same idea as
   native_test_report_pass(), but a failure also gets its caught
   message on an indented line right underneath. */
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

/* $test_report_summary(total, passed, failed): the run's one and only
   closing line, replacing whatever $test_progress() last drew on that
   same line (a real terminal; a plain trailing line on a pipe/
   redirect). Deliberately a single merged line -- "... [N/N] DONE" and
   the pass/fail counts used to print as two separate lines, which meant
   the progress line's own closing frame was immediately pushed up and
   out of "the last thing on screen" the moment the counts printed below
   it. Green when nothing failed, red otherwise; plain on a pipe/
   redirect same as everything else here. */
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
