#include <stdio.h>
#include <string.h>

#include "stdlib.h"

static bool native_print(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool native_assert(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static void register_global(vm_t *vm, const char *name, native_fn_t function);

static bool native_print(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	if (arg_count >= 1) {
		value_print(args[0], stdout);
	}
	fputs("\n", stdout);
	*out_result = value_null();
	return true;
}

/* assert(condition) or assert(condition, message). condition uses the
   same truthiness rules as if/while: only null and false are falsey.
   A failing assertion is a runtime error. That is what makes a test
   file self-checking. It exits nonzero exactly when an assertion turns
   out false, with no external expected-output file needed. */
static bool native_assert(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_string_t *message;

	if (arg_count != 1 && arg_count != 2) {
		vm_runtime_error(vm, "vm::argument-count-mismatch", "assert expects 1 or 2 arguments but got %d", arg_count);
		return false;
	}

	if (value_is_falsey(args[0])) {
		if (arg_count == 2 && value_is_obj(args[1]) && value_as_obj(args[1])->type == OBJ_STRING) {
			message = (obj_string_t *)value_as_obj(args[1]);
			vm_runtime_error(vm, "vm::assertion-failed", "assertion failed: %.*s", (int)message->length, message->chars);
		} else {
			vm_runtime_error(vm, "vm::assertion-failed", "assertion failed");
		}
		return false;
	}

	*out_result = value_null();
	return true;
}

static void register_global(vm_t *vm, const char *name, native_fn_t function) {
	obj_string_t *key;
	obj_native_t *native;

	key = object_new_string(vm->objects, vm->strings, name, strlen(name));
	native = object_new_native(vm->objects, function, name);
	table_set(&vm->globals, key, value_obj((obj_t *)native));
}

bool stdlib_check_arity(vm_t *vm, const char *fn_name, int arg_count, int expected) {
	if (arg_count != expected) {
		vm_runtime_error(vm, "vm::argument-count-mismatch", "%s expects %d argument(s) but got %d", fn_name, expected, arg_count);
		return false;
	}
	return true;
}

void stdlib_register_fn(vm_t *vm, obj_module_t *module, const char *name, native_fn_t function) {
	obj_native_t *native;
	obj_string_t *key;

	native = object_new_native(vm->objects, function, name);
	key = object_new_string(vm->objects, vm->strings, name, strlen(name));
	table_set(module->members, key, value_obj((obj_t *)native));
}

void stdlib_install(vm_t *vm) {
	obj_string_t *stdlib_name;
	obj_module_t *module;

	register_global(vm, "print", native_print);
	register_global(vm, "assert", native_assert);

	stdlib_name = object_new_string(vm->objects, vm->strings, "stdlib", 6);
	module = object_new_module(vm->objects, stdlib_name);

	stdlib_register_math(vm, module);
	stdlib_register_string(vm, module);
	stdlib_register_convert(vm, module);

	table_set(&vm->globals, stdlib_name, value_obj((obj_t *)module));
}
