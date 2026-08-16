#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "runtime/object.h"
#include "type.h"

// A single callable signature: one specific overload of a name.
// mangled_name is what the runtime actually calls it, e.g.
// "add(int,int)", built once by signature_table_add() and reused at
// every call site that resolves to this overload, so name mangling only
// ever happens in one place. Owned by the signature_table_t it lives in.
//
// function is NULL until the caller (compiler.c's signature-collection
// pass) attaches the actual obj_function_t this signature belongs to,
// once it exists; used only when copying an exported overload's runtime
// value into its module's members table.
typedef struct {
	type_t *param_types;
	size_t param_count;
	type_t return_type;
	bool has_return_type;
	char *mangled_name;
	obj_function_t *function;

	// Set by compiler.c when this overload's `fn` declaration carries a
	// `@deprecated` decorator. deprecated_message is a borrowed pointer
	// into whichever file's source text declared it (never owned here,
	// never decoded; a diagnostic string only), NULL when the
	// decorator had no `("...")` argument. Propagated across a #module
	// boundary by compile_export_stmt(), so an imported deprecated
	// overload still warns at the importer's own call sites.
	bool is_deprecated;
	const char *deprecated_message;
	size_t deprecated_message_length;
} signature_t;

// Every fn declared under one source name, e.g. all three `add` overloads.
typedef struct {
	const char *name;
	size_t name_length;
	signature_t *overloads;
	size_t overload_count;
	size_t overload_capacity;
} overload_set_t;

// Every name declared at the top level of one file, or exported from one
// module. Tagged (not anonymous) so compiler/type.h, which this header
// already depends on, can forward-declare it and hold a signature_table_t*
// (struct_def_t.methods) without an #include cycle back here.
typedef struct signature_table {
	overload_set_t *sets;
	size_t set_count;
	size_t set_capacity;
} signature_table_t;

typedef enum {
	SIGNATURE_MATCH_OK,
	SIGNATURE_MATCH_NOT_FOUND,   // no overload set exists under this name at all
	SIGNATURE_MATCH_NO_OVERLOAD, // the name exists, but no overload fits these arg types
} signature_match_result_t;

void signature_table_init(signature_table_t *table);
void signature_table_free(signature_table_t *table);

/* Registers a new overload under name. Returns NULL and leaves the table
   unchanged if a signature with the exact same parameter types already
   exists under this name; return-type-only differences do not count as
   a distinct overload, matching C#. On success, returns the new
   signature_t* (still owned by the table). name is copied in, so the
   caller's buffer does not need to outlive the call. */
signature_t *signature_table_add(signature_table_t *table, const char *name, size_t name_length, const type_t *param_types, size_t param_count, type_t return_type, bool has_return_type);

/* Finds the overload under name whose parameter types exactly match
   arg_types (same count, same types in order). Writes it to
   *out_signature on SIGNATURE_MATCH_OK; *out_signature is unspecified
   otherwise. */
signature_match_result_t signature_table_resolve(const signature_table_t *table, const char *name, size_t name_length, const type_t *arg_types, size_t arg_count, const signature_t **out_signature);

/* True if there is at least one overload under this name, regardless of
   arity. Used for the "a bare reference to an overloaded name is only
   valid as a call's own callee" rule, and to tell "unknown name" apart
   from "known name, wrong arguments" in diagnostics. */
overload_set_t *signature_table_find_set(const signature_table_t *table, const char *name, size_t name_length);
