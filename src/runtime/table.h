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
} table_t;

void table_init(table_t *table);
void table_free(table_t *table);
bool table_get(table_t *table, obj_string_t *key, value_t *out_value);
bool table_set(table_t *table, obj_string_t *key, value_t value);
bool table_delete(table_t *table, obj_string_t *key);

/* Looks up a string by content instead of by an existing key pointer.
   This is the one piece of interning machinery that cannot assume its
   key is already canonical, since deciding that is the whole point. */
obj_string_t *table_find_string(table_t *table, const char *chars, size_t length, uint32_t hash);
