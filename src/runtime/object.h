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

// Every heap object is linked into heap->objects, an intrusive list
// that gc_collect() walks to sweep. bytes_allocated is roughly the
// live heap size. A collection runs once it passes next_gc, and next_gc
// grows after each collection.
typedef struct {
	obj_t *objects;
	size_t bytes_allocated;
	size_t next_gc;
	// For PAWELI_GC_SAFEPOINT_CHECK (see gc_heap_note_alloc()): the bytes
	// allocated since the last safepoint. Every safepoint resets it, in
	// run() and in JIT code. It stays 0 when the check is off.
	size_t bytes_since_safepoint;
	bool safepoint_check_enabled;
} gc_heap_t;

typedef struct {
	obj_t obj;
	size_t length;
	uint32_t hash;
	char *chars;
} obj_string_t;

// A named set of members: the built-in "stdlib" module, or the public
// interface of a file loaded with #module. members holds only what the
// file exported.
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

/* The native entry point of a JIT-compiled function. The callee finds
   its frame at &vm->frames[vm->frame_count - 1] and its arguments at
   frame->slots[0..arity), as the interpreter would. See jit_internal.h
   for the register and stack contract. */
typedef bool (*jit_entry_fn_t)(vm_t *vm);

// globals is the private global scope of the function's home file,
// shared by every function in that file and never NULL. module is the
// file's export table, set only for a #moduledef file. filename and
// source are borrowed and back the snippets in runtime errors.
//
// jit_hotness_count counts calls and OP_LOOP back-edges (see
// jit_dispatch.h). jit_entry stays NULL until compilation succeeds and
// is never cleared, since a compiled function is never recompiled.
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
	// The JIT's inline cache for OP_GET_GLOBAL and OP_GET_PROPERTY: one
	// cell per call site, allocated once by jit_compile_function(). It is
	// a jit_table_cache_t*, opaque here because this header cannot name
	// table_entry_t. NULL if the function has no such call site or is not
	// compiled.
	void *jit_table_cache;
	// The same for OP_CALL sites: a jit_call_cache_t*, also opaque here.
	void *jit_call_cache;
} obj_function_t;

/* A native returns true with its result in *out_result. On a type or
   arity error, as in stdlib.abs("x"), it records the error with
   vm_runtime_error() and returns false. */
typedef bool (*native_fn_t)(vm_t *vm, int arg_count, value_t *args, value_t *out_result);

typedef struct {
	obj_t obj;
	native_fn_t function;
	const char *name;
} obj_native_t;

// values grows by doubling as elements are pushed. count is the number
// of elements and capacity is the size of the buffer.
typedef struct {
	obj_t obj;
	value_t *values;
	size_t count;
	size_t capacity;
} obj_array_t;

// A struct instance. fields has one value_t per declared field, in
// declared order. OP_STRUCT sets it once and it is never resized.
// type_name is only used to name the type when printing.
typedef struct {
	obj_t obj;
	obj_string_t *type_name;
	value_t *fields;
	size_t field_count;
} obj_struct_t;

// A captured local, shared by the frame that owns it and every closure
// that captured it. While open, location points at the frame's stack
// slot. close_upvalues() copies the value into `closed` and points
// location there before the frame discards the slot, so a closure that
// outlives the call keeps the right storage. next links every open
// upvalue into vm->open_upvalues, deepest slot first.
typedef struct obj_upvalue {
	obj_t obj;
	value_t *location;
	value_t closed;
	struct obj_upvalue *next;
} obj_upvalue_t;

// A closure: a function plus the upvalues it captured, resolved once
// at OP_CLOSURE. upvalues has one entry per upvalue the body uses, in
// the order the compiler first resolved them. function is an ordinary
// obj_function_t with its own GC lifetime.
typedef struct {
	obj_t obj;
	obj_function_t *function;
	obj_upvalue_t **upvalues;
	int upvalue_count;
} obj_closure_t;

/* Every constructor links the new object into heap->objects and adds
   to heap->bytes_allocated. Only gc_collect() and gc_free_all() free
   objects.

   Strings are interned through *strings: the same content always gives
   the same obj_string_t pointer, so tables compare keys by pointer.
   The intern table holds weak references. gc_collect() removes an
   entry when nothing else reaches its string (see
   table_remove_white()). */
obj_string_t *object_new_string(gc_heap_t *heap, table_t *strings, const char *chars, size_t length);

/* Like object_new_string(), but decodes the backslash escapes
   (\n \t \r \\ \" \0) in raw first. raw does not include the quotes.
   The lexer leaves escapes undecoded for this function. */
obj_string_t *object_new_string_from_literal(gc_heap_t *heap, table_t *strings, const char *raw, size_t raw_length);

/* Concatenates a and b into an interned string. Used by `+` on two
   strings and by stdlib.concat. */
obj_string_t *object_new_string_concat(gc_heap_t *heap, table_t *strings, obj_string_t *a, obj_string_t *b);

obj_function_t *object_new_function(gc_heap_t *heap);
obj_native_t *object_new_native(gc_heap_t *heap, native_fn_t function, const char *name);
obj_module_t *object_new_module(gc_heap_t *heap, obj_string_t *name);
obj_array_t *object_new_array(gc_heap_t *heap);

/* The fields are allocated but not initialized. The caller fills every
   slot right away. */
obj_struct_t *object_new_struct(gc_heap_t *heap, obj_string_t *type_name, size_t field_count);

/* The upvalue entries are allocated but not initialized. The caller
   fills every slot right away. */
obj_closure_t *object_new_closure(gc_heap_t *heap, obj_function_t *function, int upvalue_count);

obj_upvalue_t *object_new_upvalue(gc_heap_t *heap, value_t *location);

/* Appends value to array and grows the buffer if needed. Any growth is
   added to heap->bytes_allocated. */
void object_array_push(gc_heap_t *heap, obj_array_t *array, value_t value);

/* Frees obj and subtracts its size from heap->bytes_allocated. Every
   free must go through here. A direct free() would leave
   bytes_allocated too high, and the GC would run less and less often. */
void object_free(gc_heap_t *heap, obj_t *obj);
void object_print(const obj_t *obj, FILE *out);
const char *object_type_name(obj_type_t type);
