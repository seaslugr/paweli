#pragma once

#include <stdbool.h>

#include "runtime/object.h"
#include "runtime/table.h"
#include "runtime/value.h"

#define VM_DEFAULT_STACK_SIZE 4096
#define VM_DEFAULT_MAX_FRAMES 256
#define VM_MIN_STACK_SIZE 16

typedef struct {
	obj_function_t *function;
	// NULL for a call to a plain function, such as every top-level fn.
	// Non-NULL when the frame runs a closure. OP_GET_UPVALUE and
	// OP_SET_UPVALUE only appear in a nested fn's chunk, so they never see
	// NULL here.
	obj_closure_t *closure;
	uint8_t *ip;
	value_t *slots;
} call_frame_t;

typedef enum {
	VM_OK,
	VM_RUNTIME_ERROR,
} vm_result_t;

// One entry per loaded module file. The vm owns and frees these. The
// entry file's source is owned by the caller and is not listed.
typedef struct {
	char *path;
	char *source;
} vm_owned_source_t;

typedef struct vm {
	call_frame_t *frames;
	int frame_count;
	int max_frames;

	value_t *stack;
	value_t *stack_top;
	size_t stack_size;

	// One globals table per loaded file (see vm_new_file_globals()). Every
	// table is a GC root and lives until vm_free().
	table_t **file_globals;
	size_t file_globals_count;
	size_t file_globals_capacity;

	vm_owned_source_t *owned_sources;
	size_t owned_sources_count;
	size_t owned_sources_capacity;

	// Every open upvalue, deepest stack slot first. It is a GC root.
	// close_upvalues() walks it when a frame returns, so an upvalue must
	// stay alive even if its closure is gone.
	obj_upvalue_t *open_upvalues;

	gc_heap_t *heap;
	table_t *strings;

	// When true, gc_collect() runs before every instruction. Set from
	// PAWELI_GC_STRESS. It turns a missed GC root into an immediate
	// use-after-free that ASan can report.
	bool gc_stress;

	const char *filename;
	// The whole file as null-terminated text, for diagnostic snippets.
	// Not owned.
	const char *source;

	// Set by vm_runtime_error() on every failure, caught or not. Only the
	// message is stored. vm_print_uncaught_error() renders the snippet and
	// stack trace later from vm->frames, which stay as they were at the
	// failure.
	char error_code[64];
	char error_message[1024];
} vm_t;

/* Does not take ownership of heap, strings, or source. The caller
   frees them. It does not create the entry file's globals table. Call
   vm_new_file_globals() for that before compiling, since the compiler
   needs the table. stack_size must be at least VM_MIN_STACK_SIZE. */
void vm_init(vm_t *vm, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, size_t stack_size, int max_frames);
void vm_free(vm_t *vm);
vm_result_t vm_run(vm_t *vm, obj_function_t *script);

/* Allocates an empty globals table, adds "print" and the "stdlib"
   module, and registers it as a GC root. Pass the result to
   compiler_compile() as file_globals. Called once for the entry file
   and once per module. The vm owns the table and frees it in
   vm_free(). */
table_t *vm_new_file_globals(vm_t *vm);

/* Hands path and source to the vm, which frees them in vm_free().
   module.c uses it to keep a module's source alive, since a runtime
   error can need a snippet from it at any time. */
void vm_track_owned_source(vm_t *vm, char *path, char *source);

/* Records a runtime error in vm->error_code and vm->error_message.
   code is a "vm::*" diagnostic code. Natives in src/stdlib use it for
   their own type and arity errors. It prints nothing: an OP_TRY_CALL
   may still catch the error. Call vm_print_uncaught_error() once it is
   known that nothing did. */
void vm_runtime_error(vm_t *vm, const char *code, const char *format, ...);

/* Prints the full diagnostic for the recorded error: header, source
   snippet, and call stack. Call it only after a top-level vm_run()
   returned VM_RUNTIME_ERROR. It reads vm->frames, which nothing has
   changed since the failure. */
void vm_print_uncaught_error(vm_t *vm);

/* Calls callee with arg_count values from args. callee can be anything
   OP_CALL can call. For a paweli function or closure this runs the
   instruction loop until that call returns. A native uses it to call
   back into paweli code, as stdlib.sort does with its comparator.

   On success *out_result holds the return value and this returns true.
   On failure it returns false with the error already recorded, and
   *out_result is not set. Call it only from inside a native, while the
   vm is running. */
bool vm_call_value(vm_t *vm, value_t callee, int arg_count, const value_t *args, value_t *out_result);

/* Closes every open upvalue that points at slot `last` or above it.
   OP_RETURN and OP_CLOSE_UPVALUE do this inline in run(). This form is
   for JIT code. */
void vm_close_upvalues_from(vm_t *vm, value_t *last);

/* Wrappers around binary_op() and bitwise_op() for JIT code. Each pops
   two values, applies the opcode, and pushes the result. On failure
   (wrong operand types, division by zero, a shift amount out of range)
   it records the error and returns false with nothing pushed. */
bool vm_binary_op(vm_t *vm, uint8_t op);
bool vm_bitwise_op(vm_t *vm, uint8_t op);

/* The JIT's OP_CALL. The callee and its arg_count arguments must be on
   the stack. It runs the callee to completion, whether it is a native,
   a compiled function, or an interpreted one. On success the result
   replaces the callee and arguments on the stack. On failure the error
   is recorded and the stack is left as it is. */
bool vm_jit_call(vm_t *vm, int arg_count);

/* The indexing logic behind OP_GET_INDEX and OP_SET_INDEX, shared by
   the interpreter and JIT code. On success the result goes to
   out_result. On failure the error is recorded and the function
   returns false. vm_get_index() can allocate, since indexing a string
   builds a string, so it is a GC safepoint. vm_set_index() never
   allocates. */
bool vm_get_index(vm_t *vm, value_t container, value_t index_value, value_t *out_result);
bool vm_set_index(vm_t *vm, value_t container, value_t index_value, value_t value, value_t *out_result);

/* A wrapper around capture_upvalue() for JIT code. */
obj_upvalue_t *vm_capture_upvalue(vm_t *vm, value_t *local);

/* The logic behind OP_CLOSURE, shared with JIT code. Never fails. */
value_t vm_make_closure(vm_t *vm, call_frame_t *frame, obj_function_t *function, const uint8_t *descriptors, uint8_t upvalue_count);

/* The logic behind OP_GET_PROPERTY, shared with JIT code. Never
   allocates. */
bool vm_get_property(vm_t *vm, obj_string_t *name, value_t object, value_t *out_result);

/* The logic behind OP_TRY_CALL, shared with JIT code. Returns true
   with the result pushed, or false with the error message pushed. The
   failure path allocates the message string, so this is a GC
   safepoint. */
bool vm_try_call(vm_t *vm);
