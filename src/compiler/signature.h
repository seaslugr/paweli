#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "runtime/object.h"
#include "type.h"

// One callable signature: one overload of a name. mangled_name is the
// name the runtime uses, such as "add(int,int)". signature_table_add()
// builds it once. The table owns the signature.
//
// function is NULL until the signature-collection pass attaches the
// obj_function_t. It is used to copy an exported overload into its
// module's members table.
typedef struct {
	type_t *param_types;
	size_t param_count;
	type_t return_type;
	bool has_return_type;
	char *mangled_name;
	obj_function_t *function;

	// Set when the `fn` declaration has a @deprecated decorator.
	// deprecated_message points into the source text of the declaring
	// file and is not owned. It is NULL when the decorator has no
	// argument. compile_export_stmt() copies both fields across a #module
	// boundary, so an importer's call sites warn too.
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

// Every name declared at the top level of one file, or exported from
// one module. The struct has a tag so compiler/type.h can declare it
// without including this header.
typedef struct signature_table {
	overload_set_t *sets;
	size_t set_count;
	size_t set_capacity;
} signature_table_t;

typedef enum {
	SIGNATURE_MATCH_OK,
	SIGNATURE_MATCH_NOT_FOUND, // no overload set under this name
	SIGNATURE_MATCH_NO_OVERLOAD, // the name exists, but no overload fits these argument types
} signature_match_result_t;

void signature_table_init(signature_table_t *table);
void signature_table_free(signature_table_t *table);

/* Registers a new overload under name. Returns NULL and changes
   nothing if an overload with the same parameter types already exists.
   A different return type alone does not make a new overload. On
   success returns the new signature_t*, owned by the table. name is
   copied. */
signature_t *signature_table_add(signature_table_t *table, const char *name, size_t name_length, const type_t *param_types, size_t param_count, type_t return_type, bool has_return_type);

/* Finds the overload under name whose parameter types exactly match
   arg_types (same count, same types in order). Writes it to
   *out_signature on SIGNATURE_MATCH_OK; *out_signature is unspecified
   otherwise. */
signature_match_result_t signature_table_resolve(const signature_table_t *table, const char *name, size_t name_length, const type_t *arg_types, size_t arg_count, const signature_t **out_signature);

/* Returns the overload set under this name, or NULL. Used to tell an
   unknown name from a known name with the wrong arguments. */
overload_set_t *signature_table_find_set(const signature_table_t *table, const char *name, size_t name_length);
