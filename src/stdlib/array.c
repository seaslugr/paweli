#include <stdlib.h>

#include "array.h"
#include "stdlib.h"

static bool stdlib_push(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool stdlib_pop(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool stdlib_sort(vm_t *vm, int arg_count, value_t *args, value_t *out_result);
static bool check_array_arg(vm_t *vm, const char *fn_name, value_t value);
static bool array_merge_sort(vm_t *vm, value_t *values, value_t *buffer, size_t start, size_t end, value_t comparator);

static bool check_array_arg(vm_t *vm, const char *fn_name, value_t value) {
	if (!value_is_obj(value) || value_as_obj(value)->type != OBJ_ARRAY) {
		vm_runtime_error(vm, "vm::argument-type-mismatch", "%s expects an array argument", fn_name);
		return false;
	}
	return true;
}

static bool stdlib_push(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_array_t *array;

	if (!stdlib_check_arity(vm, "push", arg_count, 2)) {
		return false;
	}
	if (!check_array_arg(vm, "push", args[0])) {
		return false;
	}

	array = (obj_array_t *)value_as_obj(args[0]);
	object_array_push(vm->heap, array, args[1]);

	*out_result = value_null();
	return true;
}

static bool stdlib_pop(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_array_t *array;

	if (!stdlib_check_arity(vm, "pop", arg_count, 1)) {
		return false;
	}
	if (!check_array_arg(vm, "pop", args[0])) {
		return false;
	}

	array = (obj_array_t *)value_as_obj(args[0]);
	if (array->count == 0) {
		vm_runtime_error(vm, "vm::array-empty", "pop expects a non-empty array");
		return false;
	}

	array->count--;
	*out_result = array->values[array->count];
	return true;
}

/* A stable top-down merge sort of values[start, end). buffer is
   scratch space the size of the whole array. comparator is a paweli
   fn(T,T)->bool, called through vm_call_value(), so it can be any
   paweli function. qsort() cannot be used: its comparator has no way
   to receive the vm, and the variants that take a context argument are
   not portable.

   GC safety: buffer is malloc'd and is not a GC root. That is safe,
   because every value in it is a copy of a value still in values[],
   and the array is reachable from this native's args for the whole
   call. */
static bool array_merge_sort(vm_t *vm, value_t *values, value_t *buffer, size_t start, size_t end, value_t comparator) {
	size_t mid;
	size_t left;
	size_t right;
	size_t out;
	size_t i;
	value_t compare_args[2];
	value_t less_than;

	if (end - start < 2) {
		return true;
	}

	mid = start + (end - start) / 2;

	if (!array_merge_sort(vm, values, buffer, start, mid, comparator)) {
		return false;
	}
	if (!array_merge_sort(vm, values, buffer, mid, end, comparator)) {
		return false;
	}

	left = start;
	right = mid;
	out = start;

	while (left < mid && right < end) {
		// less_than(right, left): take from the right half only when it is
		// strictly less. A tie goes to the left half, which keeps the sort
		// stable.
		compare_args[0] = values[right];
		compare_args[1] = values[left];
		if (!vm_call_value(vm, comparator, 2, compare_args, &less_than)) {
			return false;
		}
		if (value_as_bool(less_than)) {
			buffer[out] = values[right];
			right++;
		} else {
			buffer[out] = values[left];
			left++;
		}
		out++;
	}
	while (left < mid) {
		buffer[out] = values[left];
		left++;
		out++;
	}
	while (right < end) {
		buffer[out] = values[right];
		right++;
		out++;
	}

	for (i = start; i < end; i++) {
		values[i] = buffer[i];
	}

	return true;
}

static bool stdlib_sort(vm_t *vm, int arg_count, value_t *args, value_t *out_result) {
	obj_array_t *array;
	value_t *buffer;
	bool ok;

	if (!stdlib_check_arity(vm, "sort", arg_count, 2)) {
		return false;
	}
	if (!check_array_arg(vm, "sort", args[0])) {
		return false;
	}

	array = (obj_array_t *)value_as_obj(args[0]);

	if (array->count < 2) {
		*out_result = value_null();
		return true;
	}

	buffer = malloc(array->count * sizeof(value_t));
	ok = array_merge_sort(vm, array->values, buffer, 0, array->count, args[1]);
	free(buffer);

	if (!ok) {
		return false;
	}

	*out_result = value_null();
	return true;
}

void stdlib_register_array(vm_t *vm, obj_module_t *module) {
	stdlib_register_fn(vm, module, "push", stdlib_push);
	stdlib_register_fn(vm, module, "pop", stdlib_pop);
	stdlib_register_fn(vm, module, "sort", stdlib_sort);
}
