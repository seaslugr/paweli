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
} obj_type_t;

struct obj {
	obj_type_t type;
	obj_t *next;
};

typedef struct {
	obj_t obj;
	size_t length;
	uint32_t hash;
	char *chars;
} obj_string_t;

typedef struct {
	obj_t obj;
	int arity;
	chunk_t chunk;
	obj_string_t *name;
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

// A named bag of members. Currently only used for the auto-registered
// "stdlib" pseudo-module. There are no file-backed user modules yet,
// see docs/design.md.
typedef struct {
	obj_t obj;
	obj_string_t *name;
	table_t *members;
} obj_module_t;

/* All object constructors link the new object into *objects, a simple
   intrusive linked list. There is no GC yet. This list exists so
   whoever owns it can walk it and free everything in one place once
   done.

   Strings are interned through *strings. Identical content always
   returns the same obj_string_t pointer. That is what lets the globals
   table compare keys by pointer instead of content. */
obj_string_t *object_new_string(obj_t **objects, table_t *strings, const char *chars, size_t length);

/* Like object_new_string, but decodes backslash escapes (\n \t \r \\ \" \0)
   in raw first. raw must not include the surrounding quotes. This is
   where the lexer's deferred escape decoding actually happens. */
obj_string_t *object_new_string_from_literal(obj_t **objects, table_t *strings, const char *raw, size_t raw_length);

obj_function_t *object_new_function(obj_t **objects);
obj_native_t *object_new_native(obj_t **objects, native_fn_t function, const char *name);
obj_module_t *object_new_module(obj_t **objects, obj_string_t *name);

void object_free(obj_t *obj);
void object_print(const obj_t *obj, FILE *out);
const char *object_type_name(obj_type_t type);
