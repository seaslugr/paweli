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
typedef struct {
	obj_module_t *module_obj;
	obj_function_t *script;
	signature_table_t exports;
	// Every struct this module's file declared, not just the exported
	// ones. See struct_def_t.is_exported (compiler/type.h) for how an
	// importer's resolve_type_ref() tells which are actually visible.
	// Owned by this module_unit_t (see module_unit_free()); the pointer
	// itself is what compiler_compile() hands back via its out_structs
	// parameter when this file compiled, so a struct_def_t an importer
	// resolves out of it is the exact same instance the exporting file's
	// own compile produced, which TYPE_STRUCT's pointer-identity equality
	// depends on.
	struct_registry_t *exported_structs;
	char *local_name;
	size_t local_name_length;
} module_unit_t;

/* Resolves, reads, parses, and fully compiles the module named by name
   (the dequoted #module "..." string), relative to importer_path's
   directory. importer_filename/importer_source/line/column are used to
   render diagnostics for failures that have no better location of their
   own (the module file could not be found at all, for instance); the
   module file's own parse/compile errors report through their usual
   mechanism and just cause this to return NULL.

   Fails (returns NULL) if the file cannot be read, fails to parse, fails
   to compile, has no #moduledef directive, or declares a name that does
   not match name. Reuses vm->heap/vm->strings and registers a fresh
   globals table via vm_new_file_globals(vm), same as any other loaded
   file. */
module_unit_t *module_compile(const char *name, size_t name_length, const char *importer_path, const char *importer_filename, const char *importer_source, int line, int column, vm_t *vm);

void module_unit_free(module_unit_t *unit);
