// Standalone harness for Phase (a) of the JIT: jit_compile_function()
// is not reachable from any .pw script yet (call_value() doesn't
// dispatch to compiled code until src/jit/jit_dispatch.c exists), so
// this drives it directly against a hand-built chunk_t, the same way
// scripts/run_tests.sh drives the real paweli binary against tests/*.pw.
// Uses plain if/return checks rather than assert(): NDEBUG (set by the
// Release config) compiles assert() away entirely, which would silently
// turn this into a no-op "test" under exactly the build this project's
// verification bar requires checking.
#include <stdio.h>

#include "compiler/chunk.h"
#include "jit/jit_compile.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/table.h"
#include "runtime/value.h"
#include "runtime/vm.h"

int main() {
	gc_heap_t heap;
	table_t strings;
	obj_function_t *function;
	size_t const_index;
	vm_t vm;
	value_t result;

	gc_heap_init(&heap);
	table_init(&strings);

	function = object_new_function(&heap);
	function->arity = 0;
	const_index = chunk_add_constant(&function->chunk, value_int(42));
	chunk_write(&function->chunk, OP_CONSTANT, 1, 1);
	chunk_write(&function->chunk, (uint8_t)const_index, 1, 1);
	chunk_write(&function->chunk, OP_RETURN, 1, 1);

	if (!jit_compile_function(function)) {
		fprintf(stderr, "jit_selftest: jit_compile_function failed\n");
		return 1;
	}

	vm_init(&vm, &heap, &strings, "<jit-selftest>", "", VM_DEFAULT_STACK_SIZE, VM_DEFAULT_MAX_FRAMES);

	// Mirrors call_value()'s actual invariant: the callee value itself
	// is left sitting on the stack (never popped) right below
	// frame->slots, so OP_RETURN's `frame->slots - 1` always lands on
	// a real, in-bounds slot to overwrite with the return value. Every
	// real call_value() caller (OP_CALL, vm_call_value()) guarantees
	// this; only vm_run()'s top-level frame doesn't, and that frame is
	// never JIT-dispatched (see src/jit/jit_compile.c's file comment).
	*vm.stack_top = value_obj((obj_t *)function);
	vm.stack_top++;

	vm.frame_count = 1;
	vm.frames[0].function = function;
	vm.frames[0].closure = NULL;
	vm.frames[0].ip = function->chunk.code;
	vm.frames[0].slots = vm.stack_top;

	if (!function->jit_entry(&vm)) {
		fprintf(stderr, "jit_selftest: jit_entry returned false\n");
		return 1;
	}

	if (vm.frame_count != 0) {
		fprintf(stderr, "jit_selftest: expected frame_count 0, got %d\n", vm.frame_count);
		return 1;
	}

	result = vm.stack_top[-1];
	if (!value_is_int(result) || value_as_int(result) != 42) {
		fprintf(stderr, "jit_selftest: expected int 42 on the stack, got something else\n");
		return 1;
	}

	vm_free(&vm);
	table_free(&strings);
	gc_free_all(&heap);

	printf("jit_selftest: OK\n");
	return 0;
}
