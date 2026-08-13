#include <stdlib.h>
#include <string.h>

#include "object.h"
#include "table.h"

static uint32_t hash_string(const char *chars, size_t length);

static uint32_t hash_string(const char *chars, size_t length) {
	uint32_t hash;
	size_t i;

	hash = 2166136261u;
	for (i = 0; i < length; i++) {
		hash ^= (uint8_t)chars[i];
		hash *= 16777619u;
	}
	return hash;
}

obj_string_t *object_new_string(obj_t **objects, table_t *strings, const char *chars, size_t length) {
	uint32_t hash;
	obj_string_t *interned;
	obj_string_t *string;

	hash = hash_string(chars, length);
	interned = table_find_string(strings, chars, length, hash);
	if (interned != NULL) {
		return interned;
	}

	string = malloc(sizeof(obj_string_t));
	string->obj.type = OBJ_STRING;
	string->obj.next = *objects;
	*objects = &string->obj;

	string->length = length;
	string->chars = malloc(length + 1);
	memcpy(string->chars, chars, length);
	string->chars[length] = '\0';
	string->hash = hash;

	table_set(strings, string, value_null());

	return string;
}

obj_string_t *object_new_string_from_literal(obj_t **objects, table_t *strings, const char *raw, size_t raw_length) {
	char *buffer;
	size_t out_len;
	size_t i;
	obj_string_t *string;

	buffer = malloc(raw_length + 1);
	out_len = 0;

	for (i = 0; i < raw_length; i++) {
		if (raw[i] == '\\' && i + 1 < raw_length) {
			i++;
			switch (raw[i]) {
				case 'n':
					buffer[out_len++] = '\n';
					break;
				case 't':
					buffer[out_len++] = '\t';
					break;
				case 'r':
					buffer[out_len++] = '\r';
					break;
				case '\\':
					buffer[out_len++] = '\\';
					break;
				case '"':
					buffer[out_len++] = '"';
					break;
				case '0':
					buffer[out_len++] = '\0';
					break;
				default:
					buffer[out_len++] = raw[i];
					break;
			}
		} else {
			buffer[out_len++] = raw[i];
		}
	}

	string = object_new_string(objects, strings, buffer, out_len);
	free(buffer);
	return string;
}

obj_function_t *object_new_function(obj_t **objects) {
	obj_function_t *function;

	function = malloc(sizeof(obj_function_t));
	function->obj.type = OBJ_FUNCTION;
	function->obj.next = *objects;
	*objects = &function->obj;

	function->arity = 0;
	function->name = NULL;
	chunk_init(&function->chunk);

	return function;
}

obj_native_t *object_new_native(obj_t **objects, native_fn_t function, const char *name) {
	obj_native_t *native;

	native = malloc(sizeof(obj_native_t));
	native->obj.type = OBJ_NATIVE;
	native->obj.next = *objects;
	*objects = &native->obj;

	native->function = function;
	native->name = name;

	return native;
}

obj_module_t *object_new_module(obj_t **objects, obj_string_t *name) {
	obj_module_t *module;

	module = malloc(sizeof(obj_module_t));
	module->obj.type = OBJ_MODULE;
	module->obj.next = *objects;
	*objects = &module->obj;

	module->name = name;
	module->members = malloc(sizeof(table_t));
	table_init(module->members);

	return module;
}

void object_free(obj_t *obj) {
	obj_string_t *string;
	obj_function_t *function;
	obj_native_t *native;
	obj_module_t *module;

	switch (obj->type) {
		case OBJ_STRING:
			string = (obj_string_t *)obj;
			free(string->chars);
			free(string);
			break;
		case OBJ_FUNCTION:
			function = (obj_function_t *)obj;
			chunk_free(&function->chunk);
			free(function);
			break;
		case OBJ_NATIVE:
			native = (obj_native_t *)obj;
			free(native);
			break;
		case OBJ_MODULE:
			module = (obj_module_t *)obj;
			table_free(module->members);
			free(module->members);
			free(module);
			break;
	}
}

void object_print(const obj_t *obj, FILE *out) {
	const obj_string_t *string;
	const obj_function_t *function;
	const obj_native_t *native;
	const obj_module_t *module;

	switch (obj->type) {
		case OBJ_STRING:
			string = (const obj_string_t *)obj;
			fprintf(out, "%.*s", (int)string->length, string->chars);
			break;
		case OBJ_FUNCTION:
			function = (const obj_function_t *)obj;
			if (function->name == NULL) {
				fputs("<script>", out);
			} else {
				fprintf(out, "<fn %.*s>", (int)function->name->length, function->name->chars);
			}
			break;
		case OBJ_NATIVE:
			native = (const obj_native_t *)obj;
			fprintf(out, "<native %s>", native->name);
			break;
		case OBJ_MODULE:
			module = (const obj_module_t *)obj;
			fprintf(out, "<module %.*s>", (int)module->name->length, module->name->chars);
			break;
	}
}

const char *object_type_name(obj_type_t type) {
	switch (type) {
		case OBJ_STRING: return "string";
		case OBJ_FUNCTION: return "function";
		case OBJ_NATIVE: return "native function";
		case OBJ_MODULE: return "module";
	}
	return "?";
}
