#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "jit_internal.h"

bool jit_helper_equals(const value_t *a, const value_t *b) {
	return value_equals(*a, *b);
}

bool jit_helper_not_equals(const value_t *a, const value_t *b) {
	return !value_equals(*a, *b);
}

void jit_helper_error_negate_non_numeric(vm_t *vm) {
	vm_runtime_error(vm, "vm::non-numeric-operand", "operand of unary '-' must be a number");
}

void jit_helper_error_bit_not_non_int(vm_t *vm) {
	vm_runtime_error(vm, "vm::bitwise-type-mismatch", "operand of unary '~' must be an int");
}

/* Does the real lookup and refreshes the cache cell. The fast path in
   jit_emit_op_get_global() calls this on a first run or a generation
   mismatch. The cache only saves time. The result never depends on it. */
bool jit_helper_get_global_cached(vm_t *vm, table_t *globals, obj_string_t *name, value_t *out, jit_table_cache_t *cache) {
	table_entry_t *entry;

	entry = table_get_entry(globals, name);
	if (entry == NULL) {
		vm_runtime_error(vm, "vm::undefined-variable", "undefined variable '%.*s'", (int)name->length, name->chars);
		return false;
	}
	*out = entry->value;
	cache->entry = entry;
	cache->generation = globals->generation;
	return true;
}

/* The write side of jit_helper_get_global_cached(). It looks the entry
   up with table_get_entry() and never grows the table. The compiler
   only emits OP_SET_GLOBAL for a declared global, and OP_DEFINE_GLOBAL
   creates every entry before any function body runs (see chunk.h). The
   NULL branch should be unreachable. It reports a runtime error so a
   future bug fails loudly. */
bool jit_helper_set_global_cached(vm_t *vm, table_t *globals, obj_string_t *name, const value_t *value, jit_table_cache_t *cache) {
	table_entry_t *entry;

	entry = table_get_entry(globals, name);
	if (entry == NULL) {
		vm_runtime_error(vm, "vm::undefined-variable", "undefined variable '%.*s'", (int)name->length, name->chars);
		return false;
	}
	entry->value = *value;
	cache->entry = entry;
	cache->generation = globals->generation;
	return true;
}

/* Calls through vm_jit_call(), then refreshes the call cache. The
   callee is read before the call, because the call replaces it on the
   stack with the result. It sits arg_count slots below the top.

   The callee cannot be freed between the read and the cache write. It
   is on the stack for the whole call. */
bool jit_helper_call_cached(vm_t *vm, int arg_count, jit_call_cache_t *cache) {
	value_t callee_value;
	obj_function_t *function;
	obj_closure_t *closure;

	callee_value = vm->stack_top[-1 - arg_count];
	function = NULL;
	closure = NULL;
	if (value_is_obj(callee_value)) {
		obj_t *obj = value_as_obj(callee_value);
		if (obj->type == OBJ_FUNCTION) {
			function = (obj_function_t *)obj;
		} else if (obj->type == OBJ_CLOSURE) {
			closure = (obj_closure_t *)obj;
			function = closure->function;
		}
	}

	if (!vm_jit_call(vm, arg_count)) {
		return false;
	}

	// Cache only a JIT_COMPILED callee. jit_state never goes back, so the
	// cell needs no later check.
	if (function != NULL && function->jit_state == JIT_COMPILED) {
		cache->callee = value_as_obj(callee_value);
		cache->function = function;
		cache->closure = closure;
	}
	return true;
}

void jit_helper_error_stack_overflow(vm_t *vm) {
	vm_runtime_error(vm, "vm::stack-overflow", "stack overflow");
}

/* Pointer-taking wrappers around vm_get_index() and vm_set_index().
   The values already sit on the stack. See jit_helper_array_push(). */
bool jit_helper_get_index(vm_t *vm, const value_t *container, const value_t *index_value, value_t *out) {
	return vm_get_index(vm, *container, *index_value, out);
}

bool jit_helper_set_index(vm_t *vm, const value_t *container, const value_t *index_value, const value_t *value, value_t *out) {
	return vm_set_index(vm, *container, *index_value, *value, out);
}

/* Pointer-taking wrappers for the two runtime functions that pass or
   return a value_t by value. System V and Win64 pass a 16-byte struct
   in different ways. A pointer is the same on both. */
void jit_helper_array_push(gc_heap_t *heap, obj_array_t *array, const value_t *value) {
	object_array_push(heap, array, *value);
}

void jit_helper_make_closure(vm_t *vm, call_frame_t *frame, obj_function_t *function, const uint8_t *descriptors, uint8_t upvalue_count, value_t *out) {
	*out = vm_make_closure(vm, frame, function, descriptors, upvalue_count);
}

void jit_helper_throw(vm_t *vm, obj_string_t *thrown) {
	vm_runtime_error(vm, "user::thrown", "%.*s", (int)thrown->length, thrown->chars);
}

/* Like jit_helper_get_global_cached(), with a receiver. The table is
   the members of whichever module is on the stack, so the cell records
   that module and the fast path compares it. This repeats the logic
   and error messages of vm_get_property(), which returns only the
   value and not the entry the cache needs. */
bool jit_helper_get_property_cached(vm_t *vm, obj_string_t *name, const value_t *object, value_t *out, jit_table_cache_t *cache) {
	obj_module_t *module;
	table_entry_t *entry;

	if (!value_is_obj(*object) || value_as_obj(*object)->type != OBJ_MODULE) {
		vm_runtime_error(vm, "vm::unsupported-member-access", "'.' access only works on modules right now");
		return false;
	}

	module = (obj_module_t *)value_as_obj(*object);
	entry = table_get_entry(module->members, name);
	if (entry == NULL) {
		vm_runtime_error(vm, "vm::undefined-module-member", "module '%.*s' has no member '%.*s'", (int)module->name->length, module->name->chars, (int)name->length, name->chars);
		return false;
	}

	*out = entry->value;
	cache->receiver = module;
	cache->generation = module->members->generation;
	cache->entry = entry;
	return true;
}
