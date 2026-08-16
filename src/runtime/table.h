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
	// Bumped every time `entries` is reallocated (adjust_capacity(),
	// table_remove_white()). The invariant a JIT-side cache of a raw
	// table_entry_t* (see jit_compile.c's inline-caching for
	// OP_GET_GLOBAL/OP_GET_PROPERTY) checks before trusting a
	// previously-resolved pointer is still valid. Deliberately bumped
	// unconditionally on *every* table_t's reallocation, not just the
	// ones a cache happens to be watching today. "generation changes
	// exactly when entries could have moved" is a simpler invariant to
	// state and verify than "only for tables the JIT currently caches".
	uint32_t generation;
} table_t;

void table_init(table_t *table);
void table_free(table_t *table);
bool table_get(table_t *table, obj_string_t *key, value_t *out_value);
// Returns the matched entry, or NULL if the key isn't present (or the
// table is empty). table_get() is now a thin wrapper over this.
// It is exposed separately so callers that need to cache the resolved
// location itself (the JIT's inline caching) have somewhere to get it
// from without re-deriving find_entry()'s logic themselves.
table_entry_t *table_get_entry(table_t *table, obj_string_t *key);
bool table_set(table_t *table, obj_string_t *key, value_t value);
bool table_delete(table_t *table, obj_string_t *key);

/* Looks up a string by content instead of by an existing key pointer.
   This is the one piece of interning machinery that cannot assume its
   key is already canonical, since deciding that is the whole point. */
obj_string_t *table_find_string(table_t *table, const char *chars, size_t length, uint32_t hash);

/* Removes every entry whose key is not marked. Used by gc_collect() on
   the string-intern table, which only holds weak references: an
   interned string with no other reachable reference should not be kept
   alive just for being interned. Must run after marking and before the
   object sweep, so the sweep is free to actually collect what this
   removes. */
void table_remove_white(table_t *table);
