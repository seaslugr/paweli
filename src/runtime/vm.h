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
	// NULL for a direct call to a bare (non-nested, no upvalues)
	// obj_function_t, e.g. every top-level fn. Non-NULL only when this
	// frame is running as a closure, set by call_value()'s OBJ_CLOSURE
	// case; OP_GET_UPVALUE/OP_SET_UPVALUE index closure->upvalues[] and
	// are only ever emitted inside a nested fn's own chunk, so this is
	// never dereferenced null.
	obj_closure_t *closure;
	uint8_t *ip;
	value_t *slots;
} call_frame_t;

typedef enum {
	VM_OK,
	VM_RUNTIME_ERROR,
} vm_result_t;

// One entry per loaded file (the entry script, plus one per module
// module_compile() has compiled). The vm owns and frees these: the
// entry file's own source is the one exception, still owned by main.c,
// matching vm_init()'s existing no-ownership convention for it, so it
// is never registered here.
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

	// One globals table per loaded file (see vm_new_file_globals()),
	// never freed mid-run, always a GC root: matches the pre-module
	// behavior where a single globals table was always fully marked.
	table_t **file_globals;
	size_t file_globals_count;
	size_t file_globals_capacity;

	vm_owned_source_t *owned_sources;
	size_t owned_sources_count;
	size_t owned_sources_capacity;

	// Every currently open upvalue, sorted by stack depth (deepest/most
	// recently pushed first). A permanent-until-closed GC root (see
	// gc.c's mark_roots()): close_upvalues() needs to walk this list at
	// the frame's actual return time regardless of whether any live
	// closure still references a given entry, so an upvalue cannot be
	// collected just because its closure already was.
	obj_upvalue_t *open_upvalues;

	gc_heap_t *heap;
	table_t *strings;

	// When true, gc_collect() runs before every instruction instead of
	// only once bytes_allocated crosses next_gc. Set from the
	// PAWELI_GC_STRESS environment variable (any non-empty value).
	// Deliberately aggressive: it is meant to turn a missed GC root into
	// an immediate, reproducible use-after-free instead of a rare one,
	// especially under ASan.
	bool gc_stress;

	const char *filename;
	/* Whole file's null-terminated text, kept only to render source
	   snippets in diagnostics. vm_t does not own it. */
	const char *source;

	// Set by vm_runtime_error() on every failure, whether or not
	// anything ends up catching it. Rendering the full diagnostic
	// (header, snippet, stack trace) is deferred until it's known for
	// certain nothing did; see vm_print_uncaught_error()'s own comment.
	// Only the plain message needs storing here: an uncaught error's
	// full rendering (source snippet, stack trace) still reads directly
	// from vm->frames, which stay exactly as they were at the moment of
	// failure. Nothing runs between a top-level vm_run() returning
	// VM_RUNTIME_ERROR and main.c calling vm_print_uncaught_error(), and
	// an OP_TRY_CALL that *did* catch something never reaches this path
	// at all (it handles the failure itself, not the caller).
	char error_code[64];
	char error_message[1024];
} vm_t;

/* Does not take ownership of heap, strings, or source. Same convention
   as the compiler: the caller frees them once done. Does NOT create the
   entry file's globals table or install the stdlib into it; the caller
   must do that itself via vm_new_file_globals() before compiling (see
   src/main.c), since the compiler needs that table to exist first.
   stack_size and max_frames configure the VM's value stack and
   call-frame depth. stack_size must be at least VM_MIN_STACK_SIZE. */
void vm_init(vm_t *vm, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, size_t stack_size, int max_frames);
void vm_free(vm_t *vm);
vm_result_t vm_run(vm_t *vm, obj_function_t *script);

/* Allocates a fresh, empty globals table, pre-populated with "print" and
   the "stdlib" module (same as every other file gets, no #module
   needed), and registers it as a permanent GC root. Returns it for the
   caller to pass into compiler_compile() as file_globals. Called once
   for the entry file (src/main.c) and once per loaded module
   (src/runtime/module.c); the vm owns the table either way and frees it
   in vm_free(). */
table_t *vm_new_file_globals(vm_t *vm);

/* Registers path/source as owned by the vm, freed in vm_free(). Used by
   module.c to keep a loaded module's source text alive for the rest of
   the run, since a runtime error from inside it needs to render a
   snippet from that exact source at any later point. */
void vm_track_owned_source(vm_t *vm, char *path, char *source);

/* Exposed so native functions (in src/stdlib) can report their own type
   or arity errors. They use the same mechanism the VM itself uses. code
   is a "vm::*" diagnostic code, see diagnostics/diagnostic.h. Records
   the message into vm->error_code/vm->error_message, but does NOT print
   anything itself. A try/catch (OP_TRY_CALL) might still catch this
   before it becomes a real, user-visible failure. Call
   vm_print_uncaught_error() to actually render and print it once it's
   known nothing did. */
void vm_runtime_error(vm_t *vm, const char *code, const char *format, ...);

/* Renders and prints the full diagnostic (header, source snippet, call
   stack) for vm->error_code/vm->error_message. This is exactly what
   vm_runtime_error() used to do unconditionally; it is now split out so a
   caller only reaches this once a top-level vm_run() has returned
   VM_RUNTIME_ERROR with nothing having caught it along the way. Reads
   vm->frames directly, which is safe: nothing runs between the failure
   and this call that would change them. */
void vm_print_uncaught_error(vm_t *vm);

/* Calls callee (an OBJ_FUNCTION/OBJ_CLOSURE/OBJ_NATIVE value, anything
   OP_CALL itself could call) with arg_count values from args,
   reentrantly: if callee is a paweli function/closure, this drives the
   VM's own instruction loop until that one call returns, so it works
   correctly no matter how deep the call nests or what it does (loops,
   further calls, its own natives). Exposed so a native (in src/stdlib)
   can call back into paweli code passed to it as a value, e.g. a sort
   comparator.

   On success, *out_result holds callee's return value (or null, if it
   had none) and this returns true. On failure (callee was not
   actually callable, its argument count didn't match, or running it
   hit a runtime error), this returns false with an error already
   reported via vm_runtime_error(), same as a failed OP_CALL; *out_result
   is left unset. Must only be called while vm is already mid-run (from
   inside a native function's own body), never before vm_run() or after
   it returns. */
bool vm_call_value(vm_t *vm, value_t callee, int arg_count, const value_t *args, value_t *out_result);

/* Closes every open upvalue pointing at slot `last` or deeper. The
   same operation OP_RETURN/OP_CLOSE_UPVALUE already perform inline in
   run(), exposed as a plain C function so JIT-generated code
   (src/jit/jit_compile.c) can call it directly for its own OP_RETURN
   translation. */
void vm_close_upvalues_from(vm_t *vm, value_t *last);

/* Public wrappers around vm.c's own binary_op()/bitwise_op() static
   helpers, for the same reason as vm_close_upvalues_from() above: pops
   two values off vm's stack, applies op (an OP_EQUAL..OP_MODULO or
   OP_BIT_AND..OP_SHIFT_RIGHT opcode byte), pushes the result. On
   failure (wrong operand types, division/modulo by zero, an
   out-of-range shift amount), reports the error via vm_runtime_error()
   and returns false without pushing anything, same as a failed
   interpreted OP_ADD/etc. */
bool vm_binary_op(vm_t *vm, uint8_t op);
bool vm_bitwise_op(vm_t *vm, uint8_t op);

/* The JIT translation of OP_CALL (src/jit/jit_compile.c): callee and
   its arg_count arguments must already be sitting on vm's stack, same
   precondition interpreted OP_CALL's own handler has. Reentrantly
   drives the callee to completion regardless of whether it's a native,
   an already-JIT-compiled paweli function/closure, or one that isn't
   (yet). See vm_call_value()'s own comment for the general pattern.
   On success, the result is left on vm's stack in place of the callee
   and its arguments, exactly like a successful interpreted OP_CALL
   leaves it; on failure, an error has already been reported via
   vm_runtime_error() and nothing on the stack is unwound, same as a
   failed interpreted OP_CALL. */
bool vm_jit_call(vm_t *vm, int arg_count);

/* Shared indexing logic behind OP_GET_INDEX/OP_SET_INDEX, extracted out
   of run()'s own switch cases so the interpreter and JIT-generated code
   (src/jit/jit_compile.c) share identical semantics. On success writes
   the result through out_result and returns true; on failure an error
   is already reported via vm_runtime_error() and this returns false,
   same convention as vm_binary_op()/vm_bitwise_op(). vm_get_index() can
   allocate (a single-character string result from indexing a string).
   It is a real GC safepoint; vm_set_index() never allocates. */
bool vm_get_index(vm_t *vm, value_t container, value_t index_value, value_t *out_result);
bool vm_set_index(vm_t *vm, value_t container, value_t index_value, value_t value, value_t *out_result);

/* Public wrapper around vm.c's own static capture_upvalue(), for
   JIT-generated code (see vm_make_closure() below). */
obj_upvalue_t *vm_capture_upvalue(vm_t *vm, value_t *local);

/* Shared behind OP_CLOSURE and its JIT translation. See vm.c's own
   comment on this function for the full contract. Never fails
   (is_local/index in descriptors are always compile-time-valid). */
value_t vm_make_closure(vm_t *vm, call_frame_t *frame, obj_function_t *function, const uint8_t *descriptors, uint8_t upvalue_count);

/* Shared behind OP_GET_PROPERTY and its JIT translation, same
   convention as vm_get_index()/vm_set_index() above. Never allocates. */
bool vm_get_property(vm_t *vm, obj_string_t *name, value_t object, value_t *out_result);

/* Shared behind OP_TRY_CALL and its JIT translation. See vm.c's own
   comment on this function for the full contract. Returns true on
   success (result pushed), false on failure (error message pushed in
   its place); the caller decides whether to fall through or jump to
   the catch handler based on that. Can allocate on the failure path
   (the error-message string). It is a real GC safepoint. */
bool vm_try_call(vm_t *vm);
