#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "parser/ast.h"
#include "runtime/object.h"

typedef struct struct_def struct_def_t;
// Opaque on purpose. signature.h includes this header, so type.h can
// only hold a pointer to a signature table.
typedef struct signature_table signature_table_t;
// Opaque for the same reason. runtime/module.h includes signature.h.
typedef struct module_unit module_unit_t;

// TYPE_VOID is only a return type: no value comes back. An expression
// never has it. TYPE_NULL is the type of the `null` literal.
typedef enum {
	TYPE_INT,
	TYPE_FLOAT,
	TYPE_STRING,
	TYPE_BOOL,
	TYPE_VOID,
	TYPE_NULL,
	TYPE_STRUCT,
	TYPE_FUNCTION,
} type_kind_t;

typedef struct func_type func_type_t;

typedef struct {
	type_kind_t kind;
	// The number of trailing `[]`: 0 for a plain value, 1 for `int[]`, 2
	// for `int[][]`. A nested array is the base type plus this depth.
	size_t array_depth;
	// Only valid when kind == TYPE_STRUCT. Two struct types are equal if
	// this pointer is equal. A struct name resolves to one struct_def_t
	// per compile.
	const struct_def_t *struct_def;
	// Only valid when kind == TYPE_FUNCTION: the parameter and return
	// types of the function value.
	const func_type_t *func_type;
} type_t;

// The callable shape of a function value. Every func_type_t lives for
// the whole process. type_shutdown() frees them.
struct func_type {
	type_t *param_types;
	size_t param_count;
	type_t return_type;
	bool has_return_type;
};

bool type_equals(type_t a, type_t b);
bool type_is_numeric(type_t type);

/* Frees every func_type_t. Call once, with compiler_shutdown(). */
void type_shutdown();

/* True if a value of type `value` can go where `target` is expected:
   an exact match, or value is TYPE_NULL. null fits any declared type,
   since paweli has no nullable annotation. */
bool type_is_assignable(type_t target, type_t value);

type_t type_int();
type_t type_float();
type_t type_string();
type_t type_bool();
type_t type_void();
type_t type_null();
type_t type_struct(const struct_def_t *def);
type_t type_function(const func_type_t *func_type);

/* The same type with one more level of array. element may already be
   an array. */
type_t type_array_of(type_t element);

/* The same type with one level of array removed. A non-array type
   comes back unchanged, so a caller may use this before it knows
   whether the value is an array or a string. */
type_t type_element_of(type_t array);

/* Allocates an empty func_type_t for a caller that builds one itself,
   as compile_nested_function_decl() does for a nested fn's own type.
   The caller fills in every field. type_shutdown() frees it. */
func_type_t *type_new_func_type();

/* A readable form ("int", "string[]", "Point"), used in diagnostics
   and in mangled function names. The result points into a small
   rotating set of buffers and stays valid for the next few calls. Copy
   it to keep it longer. */
const char *type_to_string(type_t type);

// One field of a struct declaration. default_value is the `= expr`
// from the declaration, or NULL if every literal must give the field.
typedef struct {
	const char *name;
	size_t name_length;
	type_t type;
	const ast_node_t *default_value;
} struct_field_def_t;

// One resolved `struct Name { ... }` declaration.
struct struct_def {
	const char *name;
	size_t name_length;
	struct_field_def_t *fields;
	size_t field_count;
	// Set by compile_export_stmt() when `export TypeName;` names this
	// struct. A struct that is not exported stays invisible to an
	// importer, although the importer receives the whole registry (see
	// module_unit_t.exported_structs).
	bool is_exported;

	// The struct's functions, called only as `TypeName.name(args)`. Each
	// struct has its own table, so two structs, or a struct and a
	// top-level fn, can share a function name. Heap-allocated because
	// this header only has an opaque declaration of signature_table_t.
	signature_table_t *methods;

	// The runtime object that `Point.name(...)` resolves against. It is an
	// obj_module_t: a name and a members table. Its members are this
	// struct's compiled functions (see compile_struct_functions()). It is
	// never bound to a user-visible name. A call site pushes it as a
	// constant. It is rooted under a reserved key in file_globals (see
	// register_structs()).
	obj_module_t *namespace_obj;
};

// Every struct declared in one file, in declaration order.
//
// A #moduledef file hands its whole registry, by reference, to
// module_unit_t.exported_structs (see out_structs in
// compiler_compile()). An importer then holds the same struct_def_t*
// the exporting file made. TYPE_STRUCT equality is pointer identity,
// so this is what makes a struct type the same on both sides of a
// #module boundary.
typedef struct {
	struct_def_t **defs;
	size_t count;
	size_t capacity;
} struct_registry_t;

void struct_registry_init(struct_registry_t *registry);
void struct_registry_free(struct_registry_t *registry);

/* Registers a new, empty struct_def_t and returns it for the caller to
   fill in. Returns NULL if name is already registered. heap and
   strings are used for the struct's namespace object and its display
   name. */
struct_def_t *struct_registry_add(struct_registry_t *registry, const char *name, size_t name_length, gc_heap_t *heap, table_t *strings);

struct_def_t *struct_registry_find(const struct_registry_t *registry, const char *name, size_t name_length);

/* The index of the named field in def->fields, or -1. */
int struct_def_find_field(const struct_def_t *def, const char *name, size_t name_length);

/* Converts a parsed type_ref_t into a type_t. For a struct name it
   looks in structs first, then in the exported structs of each
   imported module. If more than one import exports the name,
   *out_ambiguous is set. If nothing matches, *out_found is set to
   false. Both cases return TYPE_VOID. Either out pointer may be NULL.
   The caller reports the error, since it has the source position. */
type_t type_from_type_ref(type_ref_t type_ref, const struct_registry_t *structs, const module_unit_t *const *imported_modules, size_t imported_module_count, bool *out_found, bool *out_ambiguous);
