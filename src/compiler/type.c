#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/module.h"
#include "signature.h"
#include "type.h"

#define TYPE_STRING_BUFFERS 8
#define TYPE_STRING_BUFFER_SIZE 96

// Every func_type_t built while resolving a type_ref_t (a var_decl's
// declared type, a parameter, a struct field, a return type; anywhere
// a `fn(...) -> T` annotation can appear) is tracked here instead of by
// whichever compiler_t happened to be resolving it at the time. A
// per-compiler_t lifetime (which is all a nested fn's own self-type
// ever needed) stops being enough once a function-typed *exported
// function signature* can reference one: that has to survive into
// module_unit_t.exports, which outlives the compiling compiler_t
// entirely. Process-lifetime, freed once via type_shutdown() (called
// from compiler_shutdown()), matches how the native signature tables
// already handle the same "outlives any one compile" requirement.
typedef struct func_type_node {
	func_type_t *func_type;
	struct func_type_node *next;
} func_type_node_t;

static func_type_node_t *g_func_types = NULL;

static const char *type_kind_name(type_kind_t kind);
static func_type_t *alloc_func_type(void);
static size_t type_string_append(char *buffer, size_t buffer_size, size_t offset, const char *text, int length);
static size_t append_type_string(char *buffer, size_t buffer_size, size_t offset, type_t type);
static size_t append_func_type_string(char *buffer, size_t buffer_size, size_t offset, const func_type_t *ft);

static const char *type_kind_name(type_kind_t kind) {
	switch (kind) {
		case TYPE_INT: return "int";
		case TYPE_FLOAT: return "float";
		case TYPE_STRING: return "string";
		case TYPE_BOOL: return "bool";
		case TYPE_VOID: return "void";
		case TYPE_NULL: return "null";
		case TYPE_STRUCT: return "struct";
		case TYPE_FUNCTION: return "function";
	}
	return "?";
}

static func_type_t *alloc_func_type(void) {
	func_type_t *ft;
	func_type_node_t *node;

	ft = malloc(sizeof(func_type_t));
	node = malloc(sizeof(func_type_node_t));
	node->func_type = ft;
	node->next = g_func_types;
	g_func_types = node;

	return ft;
}

void type_shutdown(void) {
	func_type_node_t *node;
	func_type_node_t *next;

	node = g_func_types;
	while (node != NULL) {
		next = node->next;
		free(node->func_type->param_types);
		free(node->func_type);
		free(node);
		node = next;
	}
	g_func_types = NULL;
}

/* TYPE_STRUCT equality is nominal (pointer identity: two struct types are
   the same iff they came from the same declaration). TYPE_FUNCTION is
   structural instead: it matches "any function with this shape", since a
   function type is never declared anywhere, only ever spelled out
   wherever it's used (a param, a var_decl, a return type). */
static bool func_type_equals(const func_type_t *a, const func_type_t *b) {
	size_t i;

	if (a->param_count != b->param_count || a->has_return_type != b->has_return_type) {
		return false;
	}
	for (i = 0; i < a->param_count; i++) {
		if (!type_equals(a->param_types[i], b->param_types[i])) {
			return false;
		}
	}
	if (a->has_return_type && !type_equals(a->return_type, b->return_type)) {
		return false;
	}
	return true;
}

bool type_equals(type_t a, type_t b) {
	if (a.kind != b.kind || a.array_depth != b.array_depth) {
		return false;
	}
	if (a.kind == TYPE_STRUCT) {
		return a.struct_def == b.struct_def;
	}
	if (a.kind == TYPE_FUNCTION) {
		return func_type_equals(a.func_type, b.func_type);
	}
	return true;
}

bool type_is_numeric(type_t type) {
	return type.array_depth == 0 && (type.kind == TYPE_INT || type.kind == TYPE_FLOAT);
}

bool type_is_assignable(type_t target, type_t value) {
	if (value.kind == TYPE_NULL) {
		return true;
	}
	return type_equals(target, value);
}

type_t type_int(void) {
	type_t type;
	type.kind = TYPE_INT;
	type.array_depth = 0;
	type.struct_def = NULL;
	type.func_type = NULL;
	return type;
}

type_t type_float(void) {
	type_t type;
	type.kind = TYPE_FLOAT;
	type.array_depth = 0;
	type.struct_def = NULL;
	type.func_type = NULL;
	return type;
}

type_t type_string(void) {
	type_t type;
	type.kind = TYPE_STRING;
	type.array_depth = 0;
	type.struct_def = NULL;
	type.func_type = NULL;
	return type;
}

type_t type_bool(void) {
	type_t type;
	type.kind = TYPE_BOOL;
	type.array_depth = 0;
	type.struct_def = NULL;
	type.func_type = NULL;
	return type;
}

type_t type_void(void) {
	type_t type;
	type.kind = TYPE_VOID;
	type.array_depth = 0;
	type.struct_def = NULL;
	type.func_type = NULL;
	return type;
}

type_t type_null(void) {
	type_t type;
	type.kind = TYPE_NULL;
	type.array_depth = 0;
	type.struct_def = NULL;
	type.func_type = NULL;
	return type;
}

type_t type_struct(const struct_def_t *def) {
	type_t type;
	type.kind = TYPE_STRUCT;
	type.array_depth = 0;
	type.struct_def = def;
	type.func_type = NULL;
	return type;
}

type_t type_function(const func_type_t *func_type) {
	type_t type;
	type.kind = TYPE_FUNCTION;
	type.array_depth = 0;
	type.struct_def = NULL;
	type.func_type = func_type;
	return type;
}

type_t type_array_of(type_t element) {
	element.array_depth++;
	return element;
}

type_t type_element_of(type_t array) {
	if (array.array_depth > 0) {
		array.array_depth--;
	}
	return array;
}

func_type_t *type_new_func_type(void) {
	return alloc_func_type();
}

/* Appends text (length characters, or the whole NUL-terminated string if
   length is negative; struct/field names are spans into source text,
   never NUL-terminated at their own end, so they always pass an explicit
   length; plain C-string literals pass -1 for convenience) at offset,
   clamped to never write past buffer_size. Returns the new offset, so a
   caller can thread a sequence of appends through it. This is a
   diagnostic-only formatting helper, so silent truncation on a
   pathologically long signature is an acceptable outcome; it does not
   need a real error path. */
static size_t type_string_append(char *buffer, size_t buffer_size, size_t offset, const char *text, int length) {
	int written;

	if (offset >= buffer_size) {
		return buffer_size;
	}
	written = snprintf(buffer + offset, buffer_size - offset, "%.*s", length, text);
	if (written <= 0) {
		return offset;
	}
	offset += (size_t)written;
	return offset > buffer_size ? buffer_size : offset;
}

static size_t append_type_string(char *buffer, size_t buffer_size, size_t offset, type_t type) {
	size_t i;

	if (type.kind == TYPE_STRUCT) {
		offset = type_string_append(buffer, buffer_size, offset, type.struct_def->name, (int)type.struct_def->name_length);
	} else if (type.kind == TYPE_FUNCTION) {
		offset = append_func_type_string(buffer, buffer_size, offset, type.func_type);
	} else {
		offset = type_string_append(buffer, buffer_size, offset, type_kind_name(type.kind), -1);
	}
	for (i = 0; i < type.array_depth; i++) {
		offset = type_string_append(buffer, buffer_size, offset, "[]", -1);
	}
	return offset;
}

static size_t append_func_type_string(char *buffer, size_t buffer_size, size_t offset, const func_type_t *ft) {
	size_t i;

	offset = type_string_append(buffer, buffer_size, offset, "fn(", -1);
	for (i = 0; i < ft->param_count; i++) {
		if (i > 0) {
			offset = type_string_append(buffer, buffer_size, offset, ",", -1);
		}
		offset = append_type_string(buffer, buffer_size, offset, ft->param_types[i]);
	}
	offset = type_string_append(buffer, buffer_size, offset, ")", -1);
	if (ft->has_return_type) {
		offset = type_string_append(buffer, buffer_size, offset, "->", -1);
		offset = append_type_string(buffer, buffer_size, offset, ft->return_type);
	}
	return offset;
}

const char *type_to_string(type_t type) {
	static char buffers[TYPE_STRING_BUFFERS][TYPE_STRING_BUFFER_SIZE];
	static int next_buffer = 0;
	char *buffer;

	buffer = buffers[next_buffer];
	next_buffer = (next_buffer + 1) % TYPE_STRING_BUFFERS;

	append_type_string(buffer, TYPE_STRING_BUFFER_SIZE, 0, type);

	return buffer;
}

void struct_registry_init(struct_registry_t *registry) {
	registry->defs = NULL;
	registry->count = 0;
	registry->capacity = 0;
}

void struct_registry_free(struct_registry_t *registry) {
	size_t i;

	for (i = 0; i < registry->count; i++) {
		free(registry->defs[i]->fields);
		// namespace_obj is deliberately left alone: it's a GC-heap object,
		// freed by the heap's own sweep/gc_free_all(), not owned by this
		// registry.
		signature_table_free(registry->defs[i]->methods);
		free(registry->defs[i]->methods);
		free(registry->defs[i]);
	}
	free(registry->defs);
	registry->defs = NULL;
	registry->count = 0;
	registry->capacity = 0;
}

struct_def_t *struct_registry_find(const struct_registry_t *registry, const char *name, size_t name_length) {
	size_t i;

	for (i = 0; i < registry->count; i++) {
		if (registry->defs[i]->name_length == name_length && memcmp(registry->defs[i]->name, name, name_length) == 0) {
			return registry->defs[i];
		}
	}
	return NULL;
}

struct_def_t *struct_registry_add(struct_registry_t *registry, const char *name, size_t name_length, gc_heap_t *heap, table_t *strings) {
	struct_def_t *def;
	obj_string_t *namespace_name;

	if (struct_registry_find(registry, name, name_length) != NULL) {
		return NULL;
	}

	def = malloc(sizeof(struct_def_t));
	def->name = name;
	def->name_length = name_length;
	def->fields = NULL;
	def->field_count = 0;
	def->is_exported = false;

	def->methods = malloc(sizeof(signature_table_t));
	signature_table_init(def->methods);

	namespace_name = object_new_string(heap, strings, name, name_length);
	def->namespace_obj = object_new_module(heap, namespace_name);

	if (registry->count == registry->capacity) {
		registry->capacity = registry->capacity < 8 ? 8 : registry->capacity * 2;
		registry->defs = realloc(registry->defs, registry->capacity * sizeof(struct_def_t *));
	}
	registry->defs[registry->count] = def;
	registry->count++;

	return def;
}

int struct_def_find_field(const struct_def_t *def, const char *name, size_t name_length) {
	size_t i;

	for (i = 0; i < def->field_count; i++) {
		if (def->fields[i].name_length == name_length && memcmp(def->fields[i].name, name, name_length) == 0) {
			return (int)i;
		}
	}
	return -1;
}

type_t type_from_type_ref(type_ref_t type_ref, const struct_registry_t *structs, const module_unit_t *const *imported_modules, size_t imported_module_count, bool *out_found, bool *out_ambiguous) {
	type_t type;
	struct_def_t *def;
	struct_def_t *match;
	func_type_t *ft;
	size_t i;
	size_t match_count;
	bool sub_found;
	bool sub_ambiguous;

	type.array_depth = type_ref.array_depth;
	type.struct_def = NULL;
	type.func_type = NULL;

	if (out_found != NULL) {
		*out_found = true;
	}
	if (out_ambiguous != NULL) {
		*out_ambiguous = false;
	}

	switch (type_ref.base_type) {
		case TOKEN_INT: type.kind = TYPE_INT; return type;
		case TOKEN_FLOAT: type.kind = TYPE_FLOAT; return type;
		case TOKEN_STRING: type.kind = TYPE_STRING; return type;
		case TOKEN_BOOL: type.kind = TYPE_BOOL; return type;
		case TOKEN_IDENTIFIER:
			def = struct_registry_find(structs, type_ref.struct_name.start, type_ref.struct_name.length);
			if (def != NULL) {
				type.kind = TYPE_STRUCT;
				type.struct_def = def;
				return type;
			}
			// A local match always wins outright (handled above); with
			// none, every imported module's exports is checked, since an
			// unqualified struct name is no longer guaranteed to come
			// from at most one import now that transitive/multi-import
			// resolution exists. Two different imports exporting the
			// same struct name is ambiguous, not "first one wins": there
			// is no principled ordering between two #module lines that
			// would make picking one silently correct.
			match_count = 0;
			match = NULL;
			for (i = 0; i < imported_module_count; i++) {
				struct_def_t *candidate = struct_registry_find(imported_modules[i]->exported_structs, type_ref.struct_name.start, type_ref.struct_name.length);
				if (candidate != NULL && candidate->is_exported) {
					match = candidate;
					match_count++;
				}
			}
			if (match_count > 1) {
				if (out_ambiguous != NULL) {
					*out_ambiguous = true;
				}
				break;
			}
			if (match != NULL) {
				type.kind = TYPE_STRUCT;
				type.struct_def = match;
				return type;
			}
			break;
		case TOKEN_FN:
			// Recursively resolved (a function type's own param/return
			// types can themselves be function types, e.g. a factory
			// returning a factory) against the same structs/
			// imported_modules, and tracked process-lifetime rather than
			// by any one compiler_t; see alloc_func_type()'s own
			// comment for why a nested fn's self-type never needed that,
			// but a function-typed annotation in general does.
			ft = alloc_func_type();
			ft->param_count = type_ref.function_ref->param_count;
			ft->param_types = ft->param_count > 0 ? malloc(ft->param_count * sizeof(type_t)) : NULL;
			ft->has_return_type = type_ref.function_ref->has_return_type;
			ft->return_type = type_void();

			for (i = 0; i < ft->param_count; i++) {
				ft->param_types[i] = type_from_type_ref(type_ref.function_ref->param_types[i], structs, imported_modules, imported_module_count, &sub_found, &sub_ambiguous);
				if (!sub_found && out_found != NULL) {
					*out_found = false;
				}
				if (sub_ambiguous && out_ambiguous != NULL) {
					*out_ambiguous = true;
				}
			}
			if (ft->has_return_type) {
				ft->return_type = type_from_type_ref(type_ref.function_ref->return_type, structs, imported_modules, imported_module_count, &sub_found, &sub_ambiguous);
				if (!sub_found && out_found != NULL) {
					*out_found = false;
				}
				if (sub_ambiguous && out_ambiguous != NULL) {
					*out_ambiguous = true;
				}
			}

			type.kind = TYPE_FUNCTION;
			type.func_type = ft;
			return type;
		default: break;
	}

	if (out_found != NULL) {
		*out_found = false;
	}
	type.kind = TYPE_VOID;
	return type;
}
