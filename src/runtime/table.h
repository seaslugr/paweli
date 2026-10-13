#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/object.h"
#include "runtime/value.h"

typedef struct {
	obj_string_t *key;
	value_t value;
} table_entry_t;

typedef struct table {
	size_t count;
	size_t capacity;
	table_entry_t *entries;
	// Incremented every time `entries` is reallocated. A JIT inline cache
	// holds a raw table_entry_t* and compares this before it trusts the
	// pointer. It changes exactly when entries could have moved, for every
	// table.
	uint32_t generation;
} table_t;

void table_init(table_t *table);
void table_free(table_t *table);
bool table_get(table_t *table, obj_string_t *key, value_t *out_value);
/* Returns the entry for key, or NULL. The JIT's inline cache stores
   the entry pointer, so it needs this form. */
table_entry_t *table_get_entry(table_t *table, obj_string_t *key);
bool table_set(table_t *table, obj_string_t *key, value_t value);
bool table_delete(table_t *table, obj_string_t *key);

/* Looks up a string by content. Interning uses it to find out whether
   a string already has a canonical pointer. */
obj_string_t *table_find_string(table_t *table, const char *chars, size_t length, uint32_t hash);

/* Removes every entry whose key is not marked. gc_collect() uses it on
   the string intern table, which holds weak references. It must run
   after marking and before the sweep. */
void table_remove_white(table_t *table);
