#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "compiler/chunk.h"
#include "runtime/value.h"

typedef struct table table_t;
typedef struct vm vm_t;

typedef enum {
	OBJ_STRING,
	OBJ_FUNCTION,
	OBJ_NATIVE,
	OBJ_MODULE,
	OBJ_ARRAY,
	OBJ_STRUCT,
	OBJ_CLOSURE,
	OBJ_UPVALUE,
} obj_type_t;

struct obj {
	obj_type_t type;
	bool is_marked;
	obj_t *next;
};

// Every heap object is linked into heap->objects, a simple intrusive
// linked list. gc_collect() (src/runtime/gc.c) walks it to sweep unmarked
// objects. bytes_allocated tracks roughly how much heap memory is live;
// gc.c grows next_gc after each collection and triggers the next one once
// bytes_allocated passes it.
typedef struct {
	obj_t *objects;
	size_t bytes_allocated;
	size_t next_gc;
} gc_heap_t;

typedef struct {
	obj_t obj;
	size_t length;
	uint32_t hash;
	char *chars;
} obj_string_t;

// A named bag of members: the auto-registered "stdlib" pseudo-module, or
// the public interface of a user file loaded via #module/#moduledef
// (src/runtime/module.c). members only holds what that file explicitly
// exported; everything else it declared stays private to it.
typedef struct {
	obj_t obj;
	obj_string_t *name;
	table_t *members;
} obj_module_t;

typedef enum {
	JIT_NOT_COMPILED,
	JIT_COMPILED,
	JIT_FAILED,
} jit_state_t;

// A JIT-compiled function's native entry point (src/jit/jit_compile.c).
// The callee finds its own already-pushed frame at
// &vm->frames[vm->frame_count - 1] and its arguments at
// frame->slots[0..arity), exactly as the interpreter would at that same
// call. See src/jit/jit_compile.c's own file comment for the full
// calling-convention contract (success/failure postconditions mirror
// OP_RETURN and a failed interpreted call respectively).
typedef bool (*jit_entry_fn_t)(vm_t *vm);

// globals is this function's home file's private global scope (every
// function declared in the same file shares one, set once by
// compiler_compile()). It is never NULL. module is that file's public
// export table, set only when the file was loaded as a #moduledef; NULL
// otherwise, which is what makes `export` a compile error outside one.
// filename/source back the diagnostic snippets vm_runtime_error() renders
// for a call into this function; both are borrowed, not owned, same as
// obj_module_t's members table. jit_state/jit_hotness_count/jit_entry
// are the JIT's own bookkeeping (src/jit/jit_dispatch.c increments
// jit_hotness_count both on every call and on every OP_LOOP back-edge
// run() executes for this function (a function called only once but
// looping millions of times inside that call is exactly as JIT-worthy
// as one called millions of times, so both signals feed the same
// counter), and drives the JIT_NOT_COMPILED -> JIT_COMPILED/JIT_FAILED
// transition; jit_entry stays NULL until compilation succeeds and is
// never cleared afterward, since a compiled function is never
// recompiled or invalidated). Compiling mid-loop can only ever benefit
// a *later* call to the same function, never the one currently
// interpreting the loop that triggered it. There is no
// on-stack-replacement here, deliberately, matching the rest of the
// JIT's no-speculation/no-deopt design.
typedef struct {
	obj_t obj;
	int arity;
	jit_state_t jit_state;
	uint32_t jit_hotness_count;
	jit_entry_fn_t jit_entry;
	chunk_t chunk;
	obj_string_t *name;
	table_t *globals;
	obj_module_t *module;
	const char *filename;
	const char *source;
	// One array element per static OP_GET_GLOBAL/OP_GET_PROPERTY call
	// site in this function's compiled body, allocated (once, at its
	// final size, never grown or freed) by jit_compile_function(). It is
	// jit_compile_function()'s inline-caching mechanism for those two
	// opcodes. Opaque
	// here (cast to jit_table_cache_t* only inside jit_compile.c) for
	// the same reason jit_entry_fn_t only describes the calling
	// convention and not the compiled code's own internals: object.h
	// forward-declares table_t (see below) specifically to avoid a
	// circular include with table.h, so it has no way to name
	// table_entry_t (an anonymous-struct typedef, not a tagged struct,
	// so it can't be forward-declared the way table_t itself is) even
	// if it wanted to spell out the cache cell's real layout here. NULL
	// until/unless jit_compile_function() actually allocates one (a
	// function with zero such call sites never does).
	void *jit_table_cache;
	// Same shape and lifetime as jit_table_cache above, one element per
	// static OP_CALL site instead. It is the JIT's inline caching for call
	// dispatch itself (jit_call_cache_t, private to jit_compile.c). Also
	// opaque here for the identical reason: no way to name
	// jit_call_cache_t's own field types cleanly without a circular
	// include, and object.h shouldn't need to know a compiled function's
	// internal codegen details regardless.
	void *jit_call_cache;
} obj_function_t;

// Natives report success or failure explicitly instead of just
// returning a value. They need a way to signal a type or arity
// error back to the VM, for example stdlib.abs("x"), without
// crashing or producing garbage. On success, the result goes in
// *out_result and the function returns true. On failure, it
// reports the error itself via vm_runtime_error and returns false.
typedef bool (*native_fn_t)(vm_t *vm, int arg_count, value_t *args, value_t *out_result);

typedef struct {
	obj_t obj;
	native_fn_t function;
	const char *name;
} obj_native_t;

// values grows with realloc as elements are pushed, doubling capacity like
// a chunk's bytecode buffer does. count is the number of live elements;
// capacity is how many the current buffer can hold.
typedef struct {
	obj_t obj;
	value_t *values;
	size_t count;
	size_t capacity;
} obj_array_t;

// A struct instance. fields is a fixed-size array, one value_t per field
// in the struct's declared order. It is set once at construction (OP_STRUCT)
// and never resized, unlike obj_array_t's values. field_count is that
// declared field count; type_name is only used to name the type in
// print() and object_type_name()-style diagnostics, matching how
// obj_module_t carries its own name.
typedef struct {
	obj_t obj;
	obj_string_t *type_name;
	value_t *fields;
	size_t field_count;
} obj_struct_t;

// A captured local, shared between the frame that owns it and every
// closure that closed over it. location points into a live frame's stack
// slot while open; vm.c's close_upvalues() (called from OP_RETURN and
// OP_CLOSE_UPVALUE) copies the value into `closed` and repoints location
// there once the owning frame is about to discard that slot, so a
// closure that outlives the call it was created in still sees (and can
// still mutate) the right storage. next threads every currently open
// upvalue into vm->open_upvalues, sorted by stack depth (deepest first);
// see vm.c's capture_upvalue()/close_upvalues().
typedef struct obj_upvalue {
	obj_t obj;
	value_t *location;
	value_t closed;
	struct obj_upvalue *next;
} obj_upvalue_t;

// A closure: a function plus the upvalues it captured from every
// enclosing scope, resolved once at OP_CLOSURE time. upvalues is a
// fixed-size array, one per upvalue the compiler determined this
// function's body actually references, in the order compiler.c's
// resolve_upvalue() first saw them. function is a plain, separately
// GC-tracked obj_function_t (the same object a bare, non-nested fn would
// be); a closure only ever adds the captured environment on top of it.
typedef struct {
	obj_t obj;
	obj_function_t *function;
	obj_upvalue_t **upvalues;
	int upvalue_count;
} obj_closure_t;

/* All object constructors link the new object into heap->objects and add
   to heap->bytes_allocated. Objects live until gc_collect() (or
   gc_free_all() at shutdown) frees them; nothing here frees on its own.

   Strings are interned through *strings. Identical content always
   returns the same obj_string_t pointer. That is what lets the globals
   table compare keys by pointer instead of content. The intern table
   only holds weak references: gc_collect() removes an entry whenever
   its string was not otherwise reachable, see table_remove_white(). */
obj_string_t *object_new_string(gc_heap_t *heap, table_t *strings, const char *chars, size_t length);

/* Like object_new_string, but decodes backslash escapes (\n \t \r \\ \" \0)
   in raw first. raw must not include the surrounding quotes. This is
   where the lexer's deferred escape decoding actually happens. */
obj_string_t *object_new_string_from_literal(gc_heap_t *heap, table_t *strings, const char *raw, size_t raw_length);

obj_function_t *object_new_function(gc_heap_t *heap);
obj_native_t *object_new_native(gc_heap_t *heap, native_fn_t function, const char *name);
obj_module_t *object_new_module(gc_heap_t *heap, obj_string_t *name);
obj_array_t *object_new_array(gc_heap_t *heap);

// field_count fields are allocated but left uninitialized; the caller
// (OP_STRUCT's handler in vm.c) fills every slot immediately afterward,
// so there is never an observable half-constructed instance.
obj_struct_t *object_new_struct(gc_heap_t *heap, obj_string_t *type_name, size_t field_count);

// upvalue_count entries are allocated but left uninitialized; the caller
// (OP_CLOSURE's handler in vm.c) fills every slot immediately afterward,
// same convention as object_new_struct's fields.
obj_closure_t *object_new_closure(gc_heap_t *heap, obj_function_t *function, int upvalue_count);

obj_upvalue_t *object_new_upvalue(gc_heap_t *heap, value_t *location);

/* Appends value to array, growing values with realloc if capacity is
   reached. Updates heap->bytes_allocated by the size of the growth, if
   any, same as a fresh object_new_*() call would. */
void object_array_push(gc_heap_t *heap, obj_array_t *array, value_t value);

/* Frees obj and subtracts its size back out of heap->bytes_allocated,
   undoing what the matching object_new_*() call added. Every free must
   go through here, direct free() calls elsewhere would leave
   bytes_allocated permanently overcounting and the GC would collect
   less and less often as a program runs. */
void object_free(gc_heap_t *heap, obj_t *obj);
void object_print(const obj_t *obj, FILE *out);
const char *object_type_name(obj_type_t type);
