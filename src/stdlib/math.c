#include <math.h>

#include "math.h"
#include "stdlib.h"

static bool stdlib_abs(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool stdlib_max(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool stdlib_min(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool stdlib_sqrt(vm_t *vm, int arg_count, value_t *args, value_t *out_result);

static bool stdlib_abs(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	int64_t n;

	if (!stdlib_check_arity(vm, "abs", arg_count, 1)) {
		return false;
	}
	if (!value_is_int(args[0])) {
		vm_runtime_error(vm, "vm::argument-type-mismatch", "abs expects an int argument");
		return false;
	}

	n = value_as_int(args[0]);
	*out_result = value_int(n < 0 ? -n : n);
	return true;
}

static bool stdlib_max(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	int64_t a;
	int64_t b;

	if (!stdlib_check_arity(vm, "max", arg_count, 2)) {
		return false;
	}
	if (!value_is_int(args[0]) || !value_is_int(args[1])) {
		vm_runtime_error(vm, "vm::argument-type-mismatch", "max expects two int arguments");
		return false;
	}

	a = value_as_int(args[0]);
	b = value_as_int(args[1]);
	*out_result = value_int(a > b ? a : b);
	return true;
}

static bool stdlib_min(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	int64_t a;
	int64_t b;

	if (!stdlib_check_arity(vm, "min", arg_count, 2)) {
		return false;
	}
	if (!value_is_int(args[0]) || !value_is_int(args[1])) {
		vm_runtime_error(vm, "vm::argument-type-mismatch", "min expects two int arguments");
		return false;
	}

	a = value_as_int(args[0]);
	b = value_as_int(args[1]);
	*out_result = value_int(a < b ? a : b);
	return true;
}

static bool stdlib_sqrt(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	double n;

	if (!stdlib_check_arity(vm, "sqrt", arg_count, 1)) {
		return false;
	}

	if (value_is_int(args[0])) {
		n = (double)value_as_int(args[0]);
	} else if (value_is_float(args[0])) {
		n = value_as_float(args[0]);
	} else {
		vm_runtime_error(vm, "vm::argument-type-mismatch", "sqrt expects a number argument");
		return false;
	}

	if (n < 0.0) {
		vm_runtime_error(vm, "vm::invalid-argument-value", "sqrt of a negative number");
		return false;
	}

	*out_result = value_float(sqrt(n));
	return true;
}

void stdlib_register_math(vm_t *vm, obj_module_t *module) {
	stdlib_register_fn(vm, module, "abs", stdlib_abs);
	stdlib_register_fn(vm, module, "max", stdlib_max);
	stdlib_register_fn(vm, module, "min", stdlib_min);
	stdlib_register_fn(vm, module, "sqrt", stdlib_sqrt);
}
