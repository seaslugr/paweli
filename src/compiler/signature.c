#include <stdlib.h>
#include <string.h>

#include "signature.h"

static overload_set_t *find_or_create_set(signature_table_t *table, const char *name, size_t name_length);
static bool signature_types_match(const signature_t *signature, const type_t *arg_types, size_t arg_count);
static char *build_mangled_name(const char *name, size_t name_length, const type_t *param_types, size_t param_count);

void signature_table_init(signature_table_t *table) {
	table->sets = NULL;
	table->set_count = 0;
	table->set_capacity = 0;
}

void signature_table_free(signature_table_t *table) {
	size_t i;
	size_t j;

	for (i = 0; i < table->set_count; i++) {
		for (j = 0; j < table->sets[i].overload_count; j++) {
			free(table->sets[i].overloads[j].param_types);
			free(table->sets[i].overloads[j].mangled_name);
		}
		free(table->sets[i].overloads);
		free((char *)table->sets[i].name);
	}
	free(table->sets);

	signature_table_init(table);
}

overload_set_t *signature_table_find_set(const signature_table_t *table, const char *name, size_t name_length) {
	size_t i;

	for (i = 0; i < table->set_count; i++) {
		if (table->sets[i].name_length == name_length && memcmp(table->sets[i].name, name, name_length) == 0) {
			return &table->sets[i];
		}
	}

	return NULL;
}

static overload_set_t *find_or_create_set(signature_table_t *table, const char *name, size_t name_length) {
	overload_set_t *set;
	char *owned_name;

	set = signature_table_find_set(table, name, name_length);
	if (set != NULL) {
		return set;
	}

	if (table->set_count == table->set_capacity) {
		table->set_capacity = table->set_capacity < 4 ? 4 : table->set_capacity * 2;
		table->sets = realloc(table->sets, table->set_capacity * sizeof(overload_set_t));
	}

	owned_name = malloc(name_length);
	memcpy(owned_name, name, name_length);

	set = &table->sets[table->set_count];
	set->name = owned_name;
	set->name_length = name_length;
	set->overloads = NULL;
	set->overload_count = 0;
	set->overload_capacity = 0;
	table->set_count++;

	return set;
}

static bool signature_types_match(const signature_t *signature, const type_t *arg_types, size_t arg_count) {
	size_t i;

	if (signature->param_count != arg_count) {
		return false;
	}

	for (i = 0; i < arg_count; i++) {
		// null is compatible with any parameter type, the same way it is
		// compatible with any declared variable type: there is no
		// nullable-type annotation in paweli, so this is the only way a
		// literal `null` argument can ever match anything.
		if (arg_types[i].kind == TYPE_NULL) {
			continue;
		}
		if (!type_equals(signature->param_types[i], arg_types[i])) {
			return false;
		}
	}

	return true;
}

static char *build_mangled_name(const char *name, size_t name_length, const type_t *param_types, size_t param_count) {
	size_t total;
	size_t i;
	size_t type_len;
	char *buffer;
	size_t pos;
	const char *type_str;

	total = name_length + 2 + 1;
	for (i = 0; i < param_count; i++) {
		total += strlen(type_to_string(param_types[i]));
		if (i > 0) {
			total += 1;
		}
	}

	buffer = malloc(total);
	pos = 0;
	memcpy(buffer + pos, name, name_length);
	pos += name_length;
	buffer[pos++] = '(';

	for (i = 0; i < param_count; i++) {
		if (i > 0) {
			buffer[pos++] = ',';
		}
		type_str = type_to_string(param_types[i]);
		type_len = strlen(type_str);
		memcpy(buffer + pos, type_str, type_len);
		pos += type_len;
	}

	buffer[pos++] = ')';
	buffer[pos] = '\0';

	return buffer;
}

signature_t *signature_table_add(signature_table_t *table, const char *name, size_t name_length, const type_t *param_types, size_t param_count, type_t return_type, bool has_return_type) {
	overload_set_t *set;
	signature_t *signature;
	const signature_t *existing;
	type_t *owned_param_types;

	set = find_or_create_set(table, name, name_length);

	if (signature_table_resolve(table, name, name_length, param_types, param_count, &existing) == SIGNATURE_MATCH_OK) {
		return NULL;
	}

	if (set->overload_count == set->overload_capacity) {
		set->overload_capacity = set->overload_capacity < 2 ? 2 : set->overload_capacity * 2;
		set->overloads = realloc(set->overloads, set->overload_capacity * sizeof(signature_t));
	}

	owned_param_types = NULL;
	if (param_count > 0) {
		owned_param_types = malloc(param_count * sizeof(type_t));
		memcpy(owned_param_types, param_types, param_count * sizeof(type_t));
	}

	signature = &set->overloads[set->overload_count];
	signature->param_types = owned_param_types;
	signature->param_count = param_count;
	signature->return_type = return_type;
	signature->has_return_type = has_return_type;
	signature->mangled_name = build_mangled_name(name, name_length, param_types, param_count);
	signature->function = NULL;
	signature->is_deprecated = false;
	signature->deprecated_message = NULL;
	signature->deprecated_message_length = 0;
	set->overload_count++;

	return signature;
}

signature_match_result_t signature_table_resolve(const signature_table_t *table, const char *name, size_t name_length, const type_t *arg_types, size_t arg_count, const signature_t **out_signature) {
	overload_set_t *set;
	size_t i;

	set = signature_table_find_set(table, name, name_length);
	if (set == NULL) {
		return SIGNATURE_MATCH_NOT_FOUND;
	}

	for (i = 0; i < set->overload_count; i++) {
		if (signature_types_match(&set->overloads[i], arg_types, arg_count)) {
			*out_signature = &set->overloads[i];
			return SIGNATURE_MATCH_OK;
		}
	}

	return SIGNATURE_MATCH_NO_OVERLOAD;
}
