#include <stdint.h>
#include <stdlib.h>

#include "jit_compile.h"
#include "jit_dispatch.h"

static uint32_t jit_hotness_threshold();

static uint32_t jit_hotness_threshold() {
	static bool cached;
	static uint32_t value;
	const char *env;

	if (!cached) {
		env = getenv("PAWELI_JIT_THRESHOLD");
		value = (env != NULL && env[0] != '\0') ? (uint32_t)strtoul(env, NULL, 10) : JIT_DEFAULT_HOTNESS_THRESHOLD;
		cached = true;
	}
	return value;
}

bool jit_maybe_compile(obj_function_t *function) {
	if (function->jit_entry != NULL) {
		return true;
	}
	if (function->jit_state == JIT_FAILED) {
		return false;
	}

	function->jit_hotness_count++;
	if (function->jit_hotness_count < jit_hotness_threshold()) {
		return false;
	}

	return jit_compile_function(function);
}

void jit_note_loop_iteration(obj_function_t *function) {
	if (function->jit_entry != NULL || function->jit_state == JIT_FAILED) {
		return;
	}

	function->jit_hotness_count++;
	if (function->jit_hotness_count < jit_hotness_threshold()) {
		return;
	}

	jit_compile_function(function);
}
