#include <stdlib.h>

#include "compiler/chunk.h"
#include "gc.h"
#include "runtime/table.h"
#include "runtime/vm.h"

// The gray worklist for tracing: objects that are marked reachable but
// whose own children (a function's constants, a module's members, ...)
// have not been walked yet. An explicit stack instead of recursion, so
// a long chain of nested objects cannot overflow the C call stack.
typedef struct {
	obj_t **items;
	size_t count;
	size_t capacity;
} gray_stack_t;

static void gray_push(gray_stack_t *gray, obj_t *obj);
static void mark_object(gray_stack_t *gray, obj_t *obj);
static void mark_value(gray_stack_t *gray, value_t value);
static void mark_table(gray_stack_t *gray, table_t *table);
static void blacken_object(gray_stack_t *gray, obj_t *obj);
static void mark_roots(gray_stack_t *gray, vm_t *vm);
static void trace_references(gray_stack_t *gray);
static void sweep(gc_heap_t *heap);

static void gray_push(gray_stack_t *gray, obj_t *obj) {
	if (gray->count == gray->capacity) {
		gray->capacity = gray->capacity < 8 ? 8 : gray->capacity * 2;
		gray->items = realloc(gray->items, gray->capacity * sizeof(obj_t *));
	}
	gray->items[gray->count] = obj;
	gray->count++;
}

static void mark_object(gray_stack_t *gray, obj_t *obj) {
	if (obj == NULL || obj->is_marked) {
		return;
	}
	obj->is_marked = true;
	gray_push(gray, obj);
}

static void mark_value(gray_stack_t *gray, value_t value) {
	if (value_is_obj(value)) {
		mark_object(gray, value_as_obj(value));
	}
}

static void mark_table(gray_stack_t *gray, table_t *table) {
	size_t i;
	table_entry_t *entry;

	for (i = 0; i < table->capacity; i++) {
		entry = &table->entries[i];
		if (entry->key != NULL) {
			mark_object(gray, (obj_t *)entry->key);
			mark_value(gray, entry->value);
		}
	}
}

// Marks obj's own children gray. obj itself is already black by the time
// this runs (mark_object() marked it before pushing it onto the worklist).
static void blacken_object(gray_stack_t *gray, obj_t *obj) {
	obj_function_t *function;
	obj_module_t *module;
	obj_array_t *array;
	obj_struct_t *instance;
	obj_closure_t *closure;
	obj_upvalue_t *upvalue;
	size_t i;
	int k;

	switch (obj->type) {
		case OBJ_STRING:
		case OBJ_NATIVE:
			break;
		case OBJ_FUNCTION:
			function = (obj_function_t *)obj;
			mark_object(gray, (obj_t *)function->name);
			for (i = 0; i < function->chunk.constants_count; i++) {
				mark_value(gray, function->chunk.constants[i]);
			}
			break;
		case OBJ_MODULE:
			module = (obj_module_t *)obj;
			mark_object(gray, (obj_t *)module->name);
			mark_table(gray, module->members);
			break;
		case OBJ_ARRAY:
			array = (obj_array_t *)obj;
			for (i = 0; i < array->count; i++) {
				mark_value(gray, array->values[i]);
			}
			break;
		case OBJ_STRUCT:
			instance = (obj_struct_t *)obj;
			mark_object(gray, (obj_t *)instance->type_name);
			for (i = 0; i < instance->field_count; i++) {
				mark_value(gray, instance->fields[i]);
			}
			break;
		case OBJ_CLOSURE:
			closure = (obj_closure_t *)obj;
			mark_object(gray, (obj_t *)closure->function);
			for (k = 0; k < closure->upvalue_count; k++) {
				mark_object(gray, (obj_t *)closure->upvalues[k]);
			}
			break;
		case OBJ_UPVALUE:
			// While open, location points into the live value stack,
			// already covered by mark_roots()'s stack scan; closed is
			// only meaningful (non-null garbage) once location has been
			// repointed at it, so marking it unconditionally here is
			// always either correct or a harmless no-op.
			upvalue = (obj_upvalue_t *)obj;
			mark_value(gray, upvalue->closed);
			break;
	}
}

static void mark_roots(gray_stack_t *gray, vm_t *vm) {
	value_t *slot;
	int i;
	size_t j;
	obj_upvalue_t *upvalue;

	for (slot = vm->stack; slot < vm->stack_top; slot++) {
		mark_value(gray, *slot);
	}

	for (i = 0; i < vm->frame_count; i++) {
		mark_object(gray, (obj_t *)vm->frames[i].function);
		if (vm->frames[i].closure != NULL) {
			mark_object(gray, (obj_t *)vm->frames[i].closure);
		}
	}

	// Every loaded file's globals table is a permanent root, the same
	// way the single globals table was before modules existed: nothing
	// in it gets collected just because the running code has stopped
	// referencing it.
	for (j = 0; j < vm->file_globals_count; j++) {
		mark_table(gray, vm->file_globals[j]);
	}

	// An open upvalue must stay alive even once nothing but
	// vm->open_upvalues itself points at it: close_upvalues() (vm.c)
	// still has to walk the whole list when the frame that owns its
	// stack slot actually returns, whether or not the closure that
	// originally captured it is still reachable by then.
	for (upvalue = vm->open_upvalues; upvalue != NULL; upvalue = upvalue->next) {
		mark_object(gray, (obj_t *)upvalue);
	}
}

static void trace_references(gray_stack_t *gray) {
	obj_t *obj;

	while (gray->count > 0) {
		gray->count--;
		obj = gray->items[gray->count];
		blacken_object(gray, obj);
	}
}

static void sweep(gc_heap_t *heap) {
	obj_t *previous;
	obj_t *obj;
	obj_t *next;

	previous = NULL;
	obj = heap->objects;

	while (obj != NULL) {
		if (obj->is_marked) {
			obj->is_marked = false;
			previous = obj;
			obj = obj->next;
			continue;
		}

		next = obj->next;
		if (previous == NULL) {
			heap->objects = next;
		} else {
			previous->next = next;
		}
		object_free(heap, obj);
		obj = next;
	}
}

void gc_heap_init(gc_heap_t *heap) {
	heap->objects = NULL;
	heap->bytes_allocated = 0;
	heap->next_gc = GC_INITIAL_THRESHOLD;
}

void gc_collect(vm_t *vm) {
	gray_stack_t gray;
	size_t before;

	before = vm->heap->bytes_allocated;

	gray.items = NULL;
	gray.count = 0;
	gray.capacity = 0;

	mark_roots(&gray, vm);
	trace_references(&gray);
	free(gray.items);

	table_remove_white(vm->strings);
	sweep(vm->heap);

	if (getenv("PAWELI_GC_DEBUG") != NULL) {
		fprintf(stderr, "gc: %zu -> %zu bytes, next_gc was %zu\n", before, vm->heap->bytes_allocated, vm->heap->next_gc);
	}

	vm->heap->next_gc = vm->heap->bytes_allocated * GC_HEAP_GROW_FACTOR;
	if (vm->heap->next_gc < GC_INITIAL_THRESHOLD) {
		vm->heap->next_gc = GC_INITIAL_THRESHOLD;
	}
}

void gc_free_all(gc_heap_t *heap) {
	obj_t *obj;
	obj_t *next;

	obj = heap->objects;
	while (obj != NULL) {
		next = obj->next;
		object_free(heap, obj);
		obj = next;
	}
	heap->objects = NULL;
}
