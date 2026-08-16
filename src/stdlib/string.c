#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "stdlib.h"
#include "string.h"

static bool stdlib_length(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool stdlib_to_upper(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool stdlib_concat(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool stdlib_split(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool check_string_arg(vm_t *vm, const char *fn_name, value_t value);

static bool check_string_arg(vm_t *vm, const char *fn_name, value_t value) {
	if (!value_is_obj(value) || value_as_obj(value)->type != OBJ_STRING) {
		vm_runtime_error(vm, "vm::argument-type-mismatch", "%s expects a string argument", fn_name);
		return false;
	}
	return true;
}

static bool stdlib_length(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_string_t *string;
	obj_array_t *array;

	if (!stdlib_check_arity(vm, "length", arg_count, 1)) {
		return false;
	}

	if (value_is_obj(args[0]) && value_as_obj(args[0])->type == OBJ_ARRAY) {
		array = (obj_array_t *)value_as_obj(args[0]);
		*out_result = value_int((int64_t)array->count);
		return true;
	}

	if (!check_string_arg(vm, "length", args[0])) {
		return false;
	}

	string = (obj_string_t *)value_as_obj(args[0]);
	*out_result = value_int((int64_t)string->length);
	return true;
}

static bool stdlib_to_upper(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_string_t *string;
	char *buffer;
	size_t i;
	obj_string_t *result;

	if (!stdlib_check_arity(vm, "to_upper", arg_count, 1)) {
		return false;
	}
	if (!check_string_arg(vm, "to_upper", args[0])) {
		return false;
	}

	string = (obj_string_t *)value_as_obj(args[0]);
	buffer = malloc(string->length + 1);
	for (i = 0; i < string->length; i++) {
		buffer[i] = (char)toupper((unsigned char)string->chars[i]);
	}
	buffer[string->length] = '\0';

	result = object_new_string(vm->heap, vm->strings, buffer, string->length);
	free(buffer);

	*out_result = value_obj((obj_t *)result);
	return true;
}

static bool stdlib_concat(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_string_t *a;
	obj_string_t *b;
	char *buffer;
	obj_string_t *result;

	if (!stdlib_check_arity(vm, "concat", arg_count, 2)) {
		return false;
	}
	if (!check_string_arg(vm, "concat", args[0]) || !check_string_arg(vm, "concat", args[1])) {
		return false;
	}

	a = (obj_string_t *)value_as_obj(args[0]);
	b = (obj_string_t *)value_as_obj(args[1]);

	buffer = malloc(a->length + b->length + 1);
	memcpy(buffer, a->chars, a->length);
	memcpy(buffer + a->length, b->chars, b->length);
	buffer[a->length + b->length] = '\0';

	result = object_new_string(vm->heap, vm->strings, buffer, a->length + b->length);
	free(buffer);

	*out_result = value_obj((obj_t *)result);
	return true;
}

// Runs of the delimiter collapse: "3   4" split on " " gives ["3", "4"],
// not ["3", "", "", "4"]. Convenient for tokenizing free-form input like
// a calculator line, and there is no current use case that needs the
// empty-token form instead.
static bool stdlib_split(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_string_t *string;
	obj_string_t *delim;
	obj_array_t *result;
	obj_string_t *token;
	size_t i;
	size_t start;
	bool in_token;

	if (!stdlib_check_arity(vm, "split", arg_count, 2)) {
		return false;
	}
	if (!check_string_arg(vm, "split", args[0]) || !check_string_arg(vm, "split", args[1])) {
		return false;
	}

	string = (obj_string_t *)value_as_obj(args[0]);
	delim = (obj_string_t *)value_as_obj(args[1]);

	if (delim->length == 0) {
		vm_runtime_error(vm, "vm::argument-type-mismatch", "split delimiter must not be empty");
		return false;
	}

	result = object_new_array(vm->heap);

	i = 0;
	start = 0;
	in_token = false;
	while (i < string->length) {
		if (i + delim->length <= string->length && memcmp(string->chars + i, delim->chars, delim->length) == 0) {
			if (in_token) {
				token = object_new_string(vm->heap, vm->strings, string->chars + start, i - start);
				object_array_push(vm->heap, result, value_obj((obj_t *)token));
				in_token = false;
			}
			i += delim->length;
			continue;
		}
		if (!in_token) {
			start = i;
			in_token = true;
		}
		i++;
	}
	if (in_token) {
		token = object_new_string(vm->heap, vm->strings, string->chars + start, i - start);
		object_array_push(vm->heap, result, value_obj((obj_t *)token));
	}

	*out_result = value_obj((obj_t *)result);
	return true;
}

void stdlib_register_string(vm_t *vm, obj_module_t *module) {
	stdlib_register_fn(vm, module, "length", stdlib_length);
	stdlib_register_fn(vm, module, "to_upper", stdlib_to_upper);
	stdlib_register_fn(vm, module, "concat", stdlib_concat);
	stdlib_register_fn(vm, module, "split", stdlib_split);
}
