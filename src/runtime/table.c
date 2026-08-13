#include <stdlib.h>
#include <string.h>

#include "table.h"

#define TABLE_MAX_LOAD 0.75

static table_entry_t *find_entry(table_entry_t *entries, size_t capacity, obj_string_t *key);
static void adjust_capacity(table_t *table, size_t capacity);

static table_entry_t *find_entry(table_entry_t *entries, size_t capacity, obj_string_t *key) {
	uint32_t index;
	table_entry_t *entry;
	table_entry_t *tombstone;

	index = key->hash % capacity;
	tombstone = NULL;

	for (;;) {
		entry = &entries[index];

		if (entry->key == NULL) {
			if (value_is_null(entry->value)) {
				return tombstone != NULL ? tombstone : entry;
			}
			if (tombstone == NULL) {
				tombstone = entry;
			}
		} else if (entry->key == key) {
			return entry;
		}

		index = (index + 1) % capacity;
	}
}

static void adjust_capacity(table_t *table, size_t capacity) {
	table_entry_t *entries;
	size_t i;
	table_entry_t *entry;
	table_entry_t *dest;

	entries = malloc(capacity * sizeof(table_entry_t));
	for (i = 0; i < capacity; i++) {
		entries[i].key = NULL;
		entries[i].value = value_null();
	}

	table->count = 0;
	for (i = 0; i < table->capacity; i++) {
		entry = &table->entries[i];
		if (entry->key == NULL) {
			continue;
		}
		dest = find_entry(entries, capacity, entry->key);
		dest->key = entry->key;
		dest->value = entry->value;
		table->count++;
	}

	free(table->entries);
	table->entries = entries;
	table->capacity = capacity;
}

void table_init(table_t *table) {
	table->count = 0;
	table->capacity = 0;
	table->entries = NULL;
}

void table_free(table_t *table) {
	free(table->entries);
	table_init(table);
}

bool table_get(table_t *table, obj_string_t *key, value_t *out_value) {
	table_entry_t *entry;

	if (table->count == 0) {
		return false;
	}

	entry = find_entry(table->entries, table->capacity, key);
	if (entry->key == NULL) {
		return false;
	}

	*out_value = entry->value;
	return true;
}

bool table_set(table_t *table, obj_string_t *key, value_t value) {
	table_entry_t *entry;
	bool is_new_key;

	if ((double)(table->count + 1) > (double)table->capacity * TABLE_MAX_LOAD) {
		adjust_capacity(table, table->capacity < 8 ? 8 : table->capacity * 2);
	}

	entry = find_entry(table->entries, table->capacity, key);
	is_new_key = entry->key == NULL;
	if (is_new_key && value_is_null(entry->value)) {
		table->count++;
	}

	entry->key = key;
	entry->value = value;
	return is_new_key;
}

bool table_delete(table_t *table, obj_string_t *key) {
	table_entry_t *entry;

	if (table->count == 0) {
		return false;
	}

	entry = find_entry(table->entries, table->capacity, key);
	if (entry->key == NULL) {
		return false;
	}

	entry->key = NULL;
	entry->value = value_bool(true);
	return true;
}

obj_string_t *table_find_string(table_t *table, const char *chars, size_t length, uint32_t hash) {
	uint32_t index;
	table_entry_t *entry;

	if (table->count == 0) {
		return NULL;
	}

	index = hash % table->capacity;

	for (;;) {
		entry = &table->entries[index];

		if (entry->key == NULL) {
			if (value_is_null(entry->value)) {
				return NULL;
			}
		} else if (entry->key->length == length && entry->key->hash == hash && memcmp(entry->key->chars, chars, length) == 0) {
			return entry->key;
		}

		index = (index + 1) % table->capacity;
	}
}
