#include <stdio.h>
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

obj_string_t *object_new_string(gc_heap_t *heap, table_t *strings, const char *chars, size_t length) {
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
	string->obj.is_marked = false;
	string->obj.next = heap->objects;
	heap->objects = &string->obj;

	string->length = length;
	string->chars = malloc(length + 1);
	memcpy(string->chars, chars, length);
	string->chars[length] = '\0';
	string->hash = hash;

	heap->bytes_allocated += sizeof(obj_string_t) + length + 1;

	table_set(strings, string, value_null());

	return string;
}

obj_string_t *object_new_string_from_literal(gc_heap_t *heap, table_t *strings, const char *raw, size_t raw_length) {
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

	string = object_new_string(heap, strings, buffer, out_len);
	free(buffer);
	return string;
}

obj_function_t *object_new_function(gc_heap_t *heap) {
	obj_function_t *function;

	function = malloc(sizeof(obj_function_t));
	function->obj.type = OBJ_FUNCTION;
	function->obj.is_marked = false;
	function->obj.next = heap->objects;
	heap->objects = &function->obj;

	function->arity = 0;
	function->jit_state = JIT_NOT_COMPILED;
	function->jit_hotness_count = 0;
	function->jit_entry = NULL;
	function->jit_table_cache = NULL;
	function->jit_call_cache = NULL;
	function->name = NULL;
	function->globals = NULL;
	function->module = NULL;
	function->filename = NULL;
	function->source = NULL;
	chunk_init(&function->chunk);

	heap->bytes_allocated += sizeof(obj_function_t);

	return function;
}

obj_native_t *object_new_native(gc_heap_t *heap, native_fn_t function, const char *name) {
	obj_native_t *native;

	native = malloc(sizeof(obj_native_t));
	native->obj.type = OBJ_NATIVE;
	native->obj.is_marked = false;
	native->obj.next = heap->objects;
	heap->objects = &native->obj;

	native->function = function;
	native->name = name;

	heap->bytes_allocated += sizeof(obj_native_t);

	return native;
}

obj_module_t *object_new_module(gc_heap_t *heap, obj_string_t *name) {
	obj_module_t *module;

	module = malloc(sizeof(obj_module_t));
	module->obj.type = OBJ_MODULE;
	module->obj.is_marked = false;
	module->obj.next = heap->objects;
	heap->objects = &module->obj;

	module->name = name;
	module->members = malloc(sizeof(table_t));
	table_init(module->members);

	heap->bytes_allocated += sizeof(obj_module_t) + sizeof(table_t);

	return module;
}

obj_array_t *object_new_array(gc_heap_t *heap) {
	obj_array_t *array;

	array = malloc(sizeof(obj_array_t));
	array->obj.type = OBJ_ARRAY;
	array->obj.is_marked = false;
	array->obj.next = heap->objects;
	heap->objects = &array->obj;

	array->values = NULL;
	array->count = 0;
	array->capacity = 0;

	heap->bytes_allocated += sizeof(obj_array_t);

	return array;
}

obj_struct_t *object_new_struct(gc_heap_t *heap, obj_string_t *type_name, size_t field_count) {
	obj_struct_t *instance;

	instance = malloc(sizeof(obj_struct_t));
	instance->obj.type = OBJ_STRUCT;
	instance->obj.is_marked = false;
	instance->obj.next = heap->objects;
	heap->objects = &instance->obj;

	instance->type_name = type_name;
	instance->fields = field_count > 0 ? malloc(field_count * sizeof(value_t)) : NULL;
	instance->field_count = field_count;

	heap->bytes_allocated += sizeof(obj_struct_t) + field_count * sizeof(value_t);

	return instance;
}

obj_closure_t *object_new_closure(gc_heap_t *heap, obj_function_t *function, int upvalue_count) {
	obj_closure_t *closure;
	obj_upvalue_t **upvalues;

	upvalues = upvalue_count > 0 ? malloc((size_t)upvalue_count * sizeof(obj_upvalue_t *)) : NULL;

	closure = malloc(sizeof(obj_closure_t));
	closure->obj.type = OBJ_CLOSURE;
	closure->obj.is_marked = false;
	closure->obj.next = heap->objects;
	heap->objects = &closure->obj;

	closure->function = function;
	closure->upvalues = upvalues;
	closure->upvalue_count = upvalue_count;

	heap->bytes_allocated += sizeof(obj_closure_t) + (size_t)upvalue_count * sizeof(obj_upvalue_t *);

	return closure;
}

obj_upvalue_t *object_new_upvalue(gc_heap_t *heap, value_t *location) {
	obj_upvalue_t *upvalue;

	upvalue = malloc(sizeof(obj_upvalue_t));
	upvalue->obj.type = OBJ_UPVALUE;
	upvalue->obj.is_marked = false;
	upvalue->obj.next = heap->objects;
	heap->objects = &upvalue->obj;

	upvalue->location = location;
	upvalue->closed = value_null();
	upvalue->next = NULL;

	heap->bytes_allocated += sizeof(obj_upvalue_t);

	return upvalue;
}

void object_array_push(gc_heap_t *heap, obj_array_t *array, value_t value) {
	if (array->count + 1 > array->capacity) {
		size_t old_capacity;

		old_capacity = array->capacity;
		array->capacity = old_capacity < 8 ? 8 : old_capacity * 2;
		array->values = realloc(array->values, array->capacity * sizeof(value_t));
		heap->bytes_allocated += (array->capacity - old_capacity) * sizeof(value_t);
	}

	array->values[array->count++] = value;
}

void object_free(gc_heap_t *heap, obj_t *obj) {
	obj_string_t *string;
	obj_function_t *function;
	obj_native_t *native;
	obj_module_t *module;
	obj_array_t *array;
	obj_struct_t *instance;
	obj_closure_t *closure;
	obj_upvalue_t *upvalue;

	switch (obj->type) {
		case OBJ_STRING:
			string = (obj_string_t *)obj;
			heap->bytes_allocated -= sizeof(obj_string_t) + string->length + 1;
			free(string->chars);
			free(string);
			break;
		case OBJ_FUNCTION:
			function = (obj_function_t *)obj;
			heap->bytes_allocated -= sizeof(obj_function_t);
			chunk_free(&function->chunk);
			// Unlike jit_entry's executable mapping (mmap, never
			// unmapped even here. See jit_exec_mem.h's own comment;
			// invisible to ASan/LeakSanitizer either way, since that's
			// a different allocator than the one they track), the JIT's
			// inline-caching buffer (jit_table_cache_t*, see
			// jit_compile.c's own comment on the type) is a plain
			// calloc(), fully within malloc-family tracking. Once
			// `function` itself is freed below, nothing else in the
			// process holds this pointer, so it has to be freed here
			// explicitly or it becomes a genuine, ASan-visible leak.
			// Safe regardless of GC timing: nothing can still be
			// executing this function's compiled code or consulting
			// its cache once the function object itself is being
			// destroyed.
			free(function->jit_table_cache);
			// Same reasoning as jit_table_cache above. jit_call_cache
			// (jit_call_cache_t*, jit_compile.c's own inline caching for
			// OP_CALL dispatch) is also a plain calloc().
			free(function->jit_call_cache);
			free(function);
			break;
		case OBJ_NATIVE:
			native = (obj_native_t *)obj;
			heap->bytes_allocated -= sizeof(obj_native_t);
			free(native);
			break;
		case OBJ_MODULE:
			module = (obj_module_t *)obj;
			heap->bytes_allocated -= sizeof(obj_module_t) + sizeof(table_t);
			table_free(module->members);
			free(module->members);
			free(module);
			break;
		case OBJ_ARRAY:
			array = (obj_array_t *)obj;
			heap->bytes_allocated -= sizeof(obj_array_t) + array->capacity * sizeof(value_t);
			free(array->values);
			free(array);
			break;
		case OBJ_STRUCT:
			instance = (obj_struct_t *)obj;
			heap->bytes_allocated -= sizeof(obj_struct_t) + instance->field_count * sizeof(value_t);
			free(instance->fields);
			free(instance);
			break;
		case OBJ_CLOSURE:
			// Does not free closure->function: that obj_function_t is a
			// separate, independently GC-tracked heap object (the same
			// kind a non-nested fn's global binding points at), not owned
			// by the closure that wraps it.
			closure = (obj_closure_t *)obj;
			heap->bytes_allocated -= sizeof(obj_closure_t) + (size_t)closure->upvalue_count * sizeof(obj_upvalue_t *);
			free(closure->upvalues);
			free(closure);
			break;
		case OBJ_UPVALUE:
			upvalue = (obj_upvalue_t *)obj;
			heap->bytes_allocated -= sizeof(obj_upvalue_t);
			free(upvalue);
			break;
	}
}

void object_print(const obj_t *obj, FILE *out) {
	const obj_string_t *string;
	const obj_function_t *function;
	const obj_native_t *native;
	const obj_module_t *module;
	const obj_array_t *array;
	const obj_struct_t *instance;
	const obj_closure_t *closure;
	size_t i;

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
		case OBJ_ARRAY:
			array = (const obj_array_t *)obj;
			fputc('[', out);
			for (i = 0; i < array->count; i++) {
				if (i > 0) {
					fputs(", ", out);
				}
				value_print(array->values[i], out);
			}
			fputc(']', out);
			break;
		case OBJ_STRUCT:
			instance = (const obj_struct_t *)obj;
			fprintf(out, "<%.*s>", (int)instance->type_name->length, instance->type_name->chars);
			break;
		case OBJ_CLOSURE:
			closure = (const obj_closure_t *)obj;
			if (closure->function->name == NULL) {
				fputs("<script>", out);
			} else {
				fprintf(out, "<fn %.*s>", (int)closure->function->name->length, closure->function->name->chars);
			}
			break;
		case OBJ_UPVALUE:
			fputs("<upvalue>", out);
			break;
	}
}

const char *object_type_name(obj_type_t type) {
	switch (type) {
		case OBJ_STRING: return "string";
		case OBJ_FUNCTION: return "function";
		case OBJ_NATIVE: return "native function";
		case OBJ_MODULE: return "module";
		case OBJ_ARRAY: return "array";
		case OBJ_STRUCT: return "struct";
		case OBJ_CLOSURE: return "function";
		case OBJ_UPVALUE: return "upvalue";
	}
	return "?";
}
