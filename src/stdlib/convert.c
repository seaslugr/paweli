#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "convert.h"
#include "stdlib.h"

static bool stdlib_to_string(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool stdlib_to_int(vm_t *vm, int arg_count, value_t *args, value_t *out_result);

static bool stdlib_to_string(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	char buffer[32];
	int written;
	obj_string_t *result;

	if (!stdlib_check_arity(vm, "to_string", arg_count, 1)) {
		return false;
	}

	if (value_is_int(args[0])) {
		written = snprintf(buffer, sizeof(buffer), "%lld", (long long)value_as_int(args[0]));
	} else if (value_is_float(args[0])) {
		written = snprintf(buffer, sizeof(buffer), "%g", value_as_float(args[0]));
	} else if (value_is_bool(args[0])) {
		written = snprintf(buffer, sizeof(buffer), "%s", value_as_bool(args[0]) ? "true" : "false");
	} else {
		vm_runtime_error(vm, "vm::argument-type-mismatch", "to_string expects an int, float, or bool argument");
		return false;
	}

	result = object_new_string(vm->objects, vm->strings, buffer, (size_t)written);
	*out_result = value_obj((obj_t *)result);
	return true;
}

static bool stdlib_to_int(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_string_t *string;
	char buffer[64];
	size_t length;

	if (!stdlib_check_arity(vm, "to_int", arg_count, 1)) {
		return false;
	}
	if (!value_is_obj(args[0]) || value_as_obj(args[0])->type != OBJ_STRING) {
		vm_runtime_error(vm, "vm::argument-type-mismatch", "to_int expects a string argument");
		return false;
	}

	string = (obj_string_t *)value_as_obj(args[0]);
	length = string->length < sizeof(buffer) - 1 ? string->length : sizeof(buffer) - 1;
	memcpy(buffer, string->chars, length);
	buffer[length] = '\0';

	*out_result = value_int(strtoll(buffer, NULL, 10));
	return true;
}

void stdlib_register_convert(vm_t *vm, obj_module_t *module) {
	stdlib_register_fn(vm, module, "to_string", stdlib_to_string);
	stdlib_register_fn(vm, module, "to_int", stdlib_to_int);
}
