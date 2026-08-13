#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "stdlib.h"
#include "string.h"

static bool stdlib_length(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool stdlib_to_upper(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool stdlib_concat(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
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

	if (!stdlib_check_arity(vm, "length", arg_count, 1)) {
		return false;
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

	result = object_new_string(vm->objects, vm->strings, buffer, string->length);
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

	result = object_new_string(vm->objects, vm->strings, buffer, a->length + b->length);
	free(buffer);

	*out_result = value_obj((obj_t *)result);
	return true;
}

void stdlib_register_string(vm_t *vm, obj_module_t *module) {
	stdlib_register_fn(vm, module, "length", stdlib_length);
	stdlib_register_fn(vm, module, "to_upper", stdlib_to_upper);
	stdlib_register_fn(vm, module, "concat", stdlib_concat);
}
