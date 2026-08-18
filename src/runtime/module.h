#pragma once

#include <stddef.h>

#include "compiler/signature.h"
#include "runtime/object.h"
#include "runtime/vm.h"

// The fully compiled result of a #module import, produced entirely at
// compile time. module_obj is the runtime value the importer binds as a
// global under local_name; script defines the module's own functions as
// globals in its own private table and must be vm_run() once before the
// importer's own main() (its exports are already wired into
// module_obj->members by the time compiler_compile() returns it, so
// running script is only needed for the module's *internal*
// function-to-function calls to resolve). exports is what the
// importer's own compiler consults to type-check and overload-resolve
// every `local_name.member(...)` call.
//
// Tagged (struct module_unit, not anonymous) so compiler/type.h can
// forward-declare it opaquely, the same reason signature_table_t is
// tagged (see signature.h's comment on that).
typedef struct module_unit {
	obj_module_t *module_obj;
	obj_function_t *script;
	signature_table_t exports;
	// Every struct this module's file declared, not just the exported
	// ones. See struct_def_t.is_exported (compiler/type.h) for how an
	// importer's resolve_type_ref() tells which are actually visible.
	// Owned by the *canonical* module_unit_t for this file (see
	// owns_content below); the pointer itself is what compiler_compile()
	// hands back via its out_structs parameter when this file compiled,
	// so a struct_def_t an importer resolves out of it is the exact same
	// instance the exporting file's own compile produced, which
	// TYPE_STRUCT's pointer-identity equality depends on.
	struct_registry_t *exported_structs;
	char *local_name;
	size_t local_name_length;
	// True only for the one module_unit_t module_resolver_t memoizes per
	// canonical file path (the first importer to resolve it). A diamond
	// import (A and B both import C) produces a second, lightweight
	// module_unit_t for the second importer, sharing module_obj/script/
	// exports/exported_structs with the canonical one (same file, must
	// stay the same struct_def_t pointers) but carrying its own
	// local_name (each importer's own #module "..." string decides its
	// own binding name, independent of any other importer's). Only the
	// canonical unit's module_unit_free() frees exports/exported_structs/
	// own_imports; an alias's module_unit_free() frees only its own
	// local_name and itself. See module_resolver_t.
	bool owns_content;

	// NULL for a canonical unit. Non-NULL for an alias: points at the
	// real canonical unit whose content this one shares. See
	// module_unit_canonical().
	const struct module_unit *canonical_source;

	// This module's own #module imports, zero or more: for each one,
	// which module it resolved to (always normalized to a canonical
	// unit already, see module_compile()'s own comment on this array;
	// pass every target through module_unit_canonical() anyway if
	// you're not 100% sure) and the local name this module used for it,
	// as written in this module's own #module line. Persisted so a .paw
	// file can rebuild the exact same runtime binding graph
	// module_compile()/compiler_compile() already rely on for a
	// from-source compile; see paw_write_payload()'s own edges
	// parameter. Always empty (own_imports NULL, own_import_count 0) on
	// an alias; owned by this unit only when owns_content is true.
	struct {
		const struct module_unit *target;
		char *local_name;
		size_t local_name_length;
	} *own_imports;
	size_t own_import_count;
} module_unit_t;

/* unit itself if it's already canonical (canonical_source NULL), or the
   canonical unit it's an alias of otherwise. Never NULL. */
const module_unit_t *module_unit_canonical(const module_unit_t *unit);

// Deduplicates and cycle-checks module resolution across one whole
// compile session (the entry file plus everything it transitively
// imports). A canonical (realpath()'d) file path is compiled at most
// once: a diamond (A and B both import C) must produce exactly one
// struct_def_t/signature_table_t for C, not two, since TYPE_STRUCT
// equality is pointer identity. in_progress doubles as the cycle-
// detection stack: a canonical path already mid-compile (on
// in_progress, not yet in compiled) means a #module cycle.
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
// Frees every canonical module_unit_t this resolver ever produced (see
// module_unit_t.owns_content) plus its own bookkeeping. Call once, after
// the entry file and everything it transitively imports has finished
// compiling, success or failure.
void module_resolver_free(module_resolver_t *resolver);

/* Resolves, reads, parses, and fully compiles the module named by name
   (the dequoted #module "..." string), relative to importer_path's
   directory. importer_filename/importer_source/line/column are used to
   render diagnostics for failures that have no better location of their
   own (the module file could not be found at all, for instance); the
   module file's own parse/compile errors report through their usual
   mechanism and just cause this to return NULL.

   Fails (returns NULL) if the file cannot be read, fails to parse, fails
   to compile, has no #moduledef directive, declares a name that does not
   match name, or would close a #module import cycle. Reuses
   vm->heap/vm->strings and registers a fresh globals table via
   vm_new_file_globals(vm), same as any other loaded file.

   resolver is consulted first (memoized canonical-path hit returns
   immediately, a cycle is rejected immediately) and is how this module's
   own #module imports, if it has any, get resolved recursively before
   its body compiles: the caller must pass the same resolver instance
   into every module_compile() call for one whole session, including
   this function's own recursive calls into itself. */
module_unit_t *module_compile(const char *name, size_t name_length, const char *importer_path, const char *importer_filename, const char *importer_source, int line, int column, vm_t *vm, module_resolver_t *resolver);

void module_unit_free(module_unit_t *unit);

/* Frees unit only if it's an alias (module_unit_t.owns_content false);
   a no-op for a canonical unit (owned by whichever module_resolver_t
   produced it, freed later by module_resolver_free()) or for NULL. Used
   anywhere an array of resolved imports needs cleanup right after a
   compile step has finished consuming it: canonical units in that array
   stay alive for module_resolver_free() to find later, aliases don't,
   since they're never referenced anywhere else. */
void module_unit_free_if_alias(module_unit_t *unit);
