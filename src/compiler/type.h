#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "parser/ast.h"
#include "runtime/object.h"

typedef struct struct_def struct_def_t;
// Opaque here on purpose: signature.h depends on this header already, so
// struct_def_t can only ever hold a pointer to one, never the full
// definition, to avoid an #include cycle. See signature.h's own comment
// on signature_table_t's tag.
typedef struct signature_table signature_table_t;
// Opaque for the same reason: runtime/module.h depends on signature.h,
// which depends on this header, so type.h can only ever hold a pointer
// to one. type.c #includes runtime/module.h for the full definition.
typedef struct module_unit module_unit_t;

// TYPE_VOID is only a valid *return* type meaning "no value comes back";
// an expression can never actually have type TYPE_VOID. TYPE_NULL is the
// type of the `null` literal specifically: it unifies with nothing else,
// so `null` can only appear where an exact NULL match is expected (kept
// deliberately narrow rather than trying to model nullable types).
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
	// Number of trailing `[]` in the declared type: 0 for a plain
	// value, 1 for `int[]`, 2 for `int[][]`, and so on. Nested arrays
	// are represented by the depth count alone. `int[][]` is `int`
	// with array_depth 2, since the grammar has no grouping syntax
	// that would let `[]` bind to anything but the nearest preceding
	// type.
	size_t array_depth;
	// Valid only when kind == TYPE_STRUCT: which struct this is. Two
	// TYPE_STRUCT types are the same type iff this pointer matches; every
	// struct name resolves to exactly one struct_def_t per compile.
	const struct_def_t *struct_def;
	// Valid only when kind == TYPE_FUNCTION: the callable shape of a
	// nested fn's own local, just enough to type-check a call through
	// the name it (or an enclosing scope) bound it under. There is no
	// function-type syntax in the language, so this only ever comes from
	// compiling a nested `fn` statement, never from a var_decl.
	const func_type_t *func_type;
} type_t;

// Describes a nested fn's callable shape. Owned by whichever compiler_t
// compiled the `fn` statement that produced it (compiler.c tracks these
// for freeing once that compiler_t finishes), the same way struct_def_t
// belongs to a struct_registry_t.
struct func_type {
	type_t *param_types;
	size_t param_count;
	type_t return_type;
	bool has_return_type;
};

bool type_equals(type_t a, type_t b);
bool type_is_numeric(type_t type);

// Frees every func_type_t ever built by type_from_type_ref() while
// resolving a `fn(...) -> T` annotation (see its own comment on why
// these are process-lifetime instead of owned by a compiler_t). Call
// once, alongside compiler_shutdown(), after nothing will call
// type_from_type_ref() again.
void type_shutdown(void);

// True if a value of type `value` can go where `target` is expected:
// exact match, or value is TYPE_NULL (null is compatible with any
// declared type, since paweli has no separate nullable-type
// annotation). Used for var_decl initializers, assignment, return
// statements, and array element assignment.
bool type_is_assignable(type_t target, type_t value);

type_t type_int(void);
type_t type_float(void);
type_t type_string(void);
type_t type_bool(void);
type_t type_void(void);
type_t type_null(void);
type_t type_struct(const struct_def_t *def);
type_t type_function(const func_type_t *func_type);

// Same kind as element, with one more level of array. `int[][]` is
// type_array_of(type_array_of(type_int())), so element may itself
// already be an array type.
type_t type_array_of(type_t element);

// The element type of an array: same kind/struct/func, one fewer level
// of array. Total, not just for arrays: a non-array type comes back
// unchanged (depth stays 0), matching how `element.is_array = false`
// used to behave on a plain value. This keeps the one call site that
// computes it before knowing whether the value is an array or a string
// (compile_assign_expr's index target) from underflowing.
type_t type_element_of(type_t array);

// Allocates a process-lifetime, empty func_type_t (freed by
// type_shutdown()) for a caller building one directly: a nested fn's
// own self-type (compiler.c's compile_nested_function_decl(), built
// from its already-resolved param/return types, not from a
// `fn(...) -> T` type_ref_t), as opposed to type_from_type_ref()'s
// own internal allocation when parsing one out of a type annotation.
// The caller fills in every field itself.
func_type_t *type_new_func_type(void);

// Human-readable form ("int", "string[]", "Point", ...), used both in
// diagnostics and as a building block for mangled function names.
// Returns a pointer into a small internal rotating buffer set, valid
// until the next few calls; callers that need to keep more than one or
// two results around at once should copy them out.
const char *type_to_string(type_t type);

// One field of a struct declaration: its name, resolved type, and
// (optionally) the default-value expression from `= expr` in the
// declaration. default_value is NULL when the field is required at
// every construction site.
typedef struct {
	const char *name;
	size_t name_length;
	type_t type;
	const ast_node_t *default_value;
} struct_field_def_t;

// A single `struct Name { ... }` declaration, resolved: field types are
// already type_t (which, for a struct-typed field, points at another
// struct_def_t declared earlier in the same file; structs may only
// reference already-declared structs, so there is no forward reference,
// self-reference, or cross-module struct support).
struct struct_def {
	const char *name;
	size_t name_length;
	struct_field_def_t *fields;
	size_t field_count;
	// Set by compiler.c's compile_export_stmt() when this file has a
	// #moduledef directive and `export TypeName;` names this struct.
	// False for every struct in an ordinary (non-module) file, and for
	// any struct in a module file that was never exported; those stay
	// invisible to an importer even though the whole registry they live
	// in gets handed to it, see module_unit_t.exported_structs.
	bool is_exported;

	// Struct functions (`fn` declared inside the struct body), called
	// only as `TypeName.name(args)`; see compiler.c's compile_struct_call().
	// A separate table per struct, not merged into any file's top-level
	// signatures: two different structs (or a struct and a top-level fn)
	// can freely share a function name with zero collision, and struct
	// functions get overloading for free since signature_table_t already
	// supports it. Heap-allocated (not embedded) purely to keep this
	// header's only dependency on signature.h an opaque forward
	// declaration; see signature_table_t's own comment.
	signature_table_t *methods;

	// The runtime object `Point.name(...)` calls actually resolve
	// against: reuses obj_module_t as-is (already exactly "name + a
	// members table", nothing new needed) rather than inventing a
	// dedicated object type. Its members table is populated with this
	// struct's compiled functions once their bodies compile (see
	// compile_struct_functions()); never bound under a user-visible
	// name (unlike a real module). A call site pushes it directly as
	// a compile-time constant, since the compiler already holds this
	// exact struct_def_t* when resolving `Point.name(...)`. Rooted for
	// GC purposes under a reserved key in file_globals (see
	// register_structs()), since nothing else references it.
	obj_module_t *namespace_obj;
};

// All the structs declared in one file, in declaration order.
//
// Normally file-local, but a #moduledef file's whole registry is handed,
// by reference (not copied), to module_unit_t.exported_structs once that
// file finishes compiling (see compiler_compile()'s out_structs
// parameter), so an importer's struct_def_t* for an exported type is
// the exact same pointer the exporting file's own compile produced.
// TYPE_STRUCT equality is raw pointer identity (see type_t above), so
// this is the only way a struct type can mean the same thing on both
// sides of a #module boundary.
typedef struct {
	struct_def_t **defs;
	size_t count;
	size_t capacity;
} struct_registry_t;

void struct_registry_init(struct_registry_t *registry);
void struct_registry_free(struct_registry_t *registry);

// Registers a new, empty struct_def_t under `name` and returns it so the
// caller can fill in its fields once they're resolved. Returns NULL if
// `name` is already registered (a duplicate struct declaration). heap/
// strings are used to allocate the struct's runtime namespace object
// (see struct_def_t.namespace_obj) and its display name.
struct_def_t *struct_registry_add(struct_registry_t *registry, const char *name, size_t name_length, gc_heap_t *heap, table_t *strings);

struct_def_t *struct_registry_find(const struct_registry_t *registry, const char *name, size_t name_length);

// Index of the named field within def->fields, or -1 if def has no such
// field.
int struct_def_find_field(const struct_def_t *def, const char *name, size_t name_length);

// Converts a parsed `type_ref_t` (the AST-level syntax annotation, e.g.
// from a param or var_decl) into the compiler's internal type_t. structs
// is consulted first when type_ref names a struct (base_type ==
// TOKEN_IDENTIFIER); if nothing there matches, each of imported_modules'
// exported_structs is checked next, accepting only an entry with
// is_exported set. If more than one imported module exports a struct by
// this name, that's ambiguous: *out_ambiguous (if non-NULL) is set to
// true and a dummy TYPE_VOID returned, same as the not-found case,
// which the caller distinguishes to report the right diagnostic. Exactly
// one match resolves normally. *out_found (if non-NULL) is set to false,
// and a dummy TYPE_VOID returned, when type_ref names an identifier that
// matches nothing at all; the caller decides how to report either case
// (it has the source position and compiler context this function
// doesn't).
type_t type_from_type_ref(type_ref_t type_ref, const struct_registry_t *structs, const module_unit_t *const *imported_modules, size_t imported_module_count, bool *out_found, bool *out_ambiguous);
