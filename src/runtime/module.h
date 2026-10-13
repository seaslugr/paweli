#pragma once

#include <stddef.h>

#include "compiler/signature.h"
#include "runtime/object.h"
#include "runtime/vm.h"

// The compiled result of one #module import. module_obj is the value
// the importer binds as a global under local_name. script defines the
// module's functions in the module's own globals table. Run it once
// with vm_run() before the importer's main(), so the module's internal
// calls resolve. Its exports are already in module_obj->members.
// exports is what the importer's compiler uses to type-check every
// `local_name.member(...)` call.
//
// The struct has a tag so compiler/type.h can declare it without
// including this header.
typedef struct module_unit {
	obj_module_t *module_obj;
	obj_function_t *script;
	signature_table_t exports;
	// Every struct the module's file declared, exported or not (see
	// struct_def_t.is_exported). It is the registry compiler_compile()
	// returned through out_structs, so an importer resolves the same
	// struct_def_t instances the exporting file made. The canonical unit
	// owns it.
	struct_registry_t *exported_structs;
	char *local_name;
	size_t local_name_length;
	// True for the one unit the resolver keeps per canonical file path. A
	// diamond import (A and B both import C) gives the second importer an
	// alias unit. An alias shares module_obj, script, exports, and
	// exported_structs with the canonical unit and has its own local_name.
	// Only the canonical unit frees the shared content.
	bool owns_content;

	// NULL for a canonical unit. For an alias, the canonical unit whose
	// content it shares. See module_unit_canonical().
	const struct module_unit *canonical_source;

	// This module's own #module imports: the canonical unit each one
	// resolved to, and the local name this module used for it. Kept so a
	// .paw file can rebuild the same binding graph (see
	// paw_write_payload()). Empty on an alias.
	struct {
		const struct module_unit *target;
		char *local_name;
		size_t local_name_length;
	} *own_imports;
	size_t own_import_count;
} module_unit_t;

/* Returns unit if it is canonical, or the canonical unit it aliases.
   Never NULL. */
const module_unit_t *module_unit_canonical(const module_unit_t *unit);

// Deduplicates module resolution and detects cycles across one compile
// session: the entry file and everything it imports. A canonical file
// path is compiled at most once. A diamond must give one struct_def_t
// per struct, since TYPE_STRUCT equality is pointer identity.
// in_progress is the cycle-detection stack: a path that is on it and
// not yet compiled means a #module cycle.
typedef struct {
	char **compiled_paths;
	module_unit_t **compiled_units;
	size_t compiled_count;
	size_t compiled_capacity;

	char **in_progress_paths;
	size_t in_progress_count;
	size_t in_progress_capacity;
} module_resolver_t;

void module_resolver_init(module_resolver_t *resolver);
/* Frees every canonical unit the resolver produced and its own
   bookkeeping. Call once, after the whole session has compiled or
   failed. */
void module_resolver_free(module_resolver_t *resolver);

/* Resolves, reads, parses, and compiles the module named by name (the
   #module string without quotes), relative to importer_path's
   directory. The importer_* arguments and line/column locate
   diagnostics that have no better position, such as a missing file.
   The module file's own parse and compile errors are reported as
   usual.

   Returns NULL if the file cannot be read, fails to parse or compile,
   has no #moduledef, declares a different name, or would close an
   import cycle. It allocates on vm->heap and registers a new globals
   table with vm_new_file_globals().

   The resolver is checked first: a file already compiled returns at
   once, and a cycle is rejected. The module's own imports are resolved
   through it before the module's body compiles. Pass the same resolver
   to every module_compile() call in one session. */
module_unit_t *module_compile(const char *name, size_t name_length, const char *importer_path, const char *importer_filename, const char *importer_source, int line, int column, vm_t *vm, module_resolver_t *resolver);

void module_unit_free(module_unit_t *unit);

/* Frees unit if it is an alias. Does nothing for a canonical unit,
   which module_resolver_free() frees, or for NULL. Use it to clean up
   an array of resolved imports once a compile step is done with it. */
void module_unit_free_if_alias(module_unit_t *unit);
