// A standalone test of jit_compile_function(). It builds a chunk_t by
// hand and runs the compiled code directly, with no .pw script. It
// uses plain if/return checks. A Release build defines NDEBUG, which
// compiles assert() out and would make the test check nothing.
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
	chunk_write(&function->chunk, (uint8_t)((const_index >> 8) & 0xff), 1, 1);
	chunk_write(&function->chunk, (uint8_t)(const_index & 0xff), 1, 1);
	chunk_write(&function->chunk, OP_RETURN, 1, 1);

	if (!jit_compile_function(function)) {
		fprintf(stderr, "jit_selftest: jit_compile_function failed\n");
		return 1;
	}

	vm_init(&vm, &heap, &strings, "<jit-selftest>", "", VM_DEFAULT_STACK_SIZE, VM_DEFAULT_MAX_FRAMES);

	// The same layout call_value() sets up: the callee value stays on the
	// stack right below frame->slots, so the return value has a slot to
	// overwrite at frame->slots - 1.
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
