#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "paw/paw.h"
#include "runtime/gc.h"
#include "runtime/module.h"
#include "runtime/object.h"
#include "runtime/table.h"
#include "runtime/vm.h"

// Compiles, runs, or serializes a paweli program from inputs that are
// already parsed: a path, a source buffer, resolved flags. Nothing
// here reads argv.

/* Reads the whole file into a NUL-terminated malloc'd buffer. Returns
   NULL on failure, with an error already printed. */
char *exec_read_file(const char *path);

/* Parses the entry file, compiles every #module it imports through
   resolver (which also resolves their imports), then compiles the
   entry file. Returns the entry script and fills *out_entry_globals.
   Used by exec_run_source() and by the build command.

   The caller creates resolver once per session and frees it with
   module_resolver_free() after everything has run. It owns every
   module_unit_t made here.

   *out_entry_imports receives the entry file's direct imports, a mix
   of canonical units and aliases. The caller frees this array: call
   module_unit_free_if_alias() on each entry, then free() the array. A
   run can free it at once. A build needs it for the .paw edge list.

   Returns NULL on failure, with an error already printed. */
obj_function_t *exec_compile_from_source(const char *path, const char *source, gc_heap_t *heap, table_t *strings, vm_t *vm, module_resolver_t *resolver, table_t **out_entry_globals, const module_unit_t ***out_entry_imports, size_t *out_entry_import_count, bool dump_ast, bool run_tests);

/* Parses, compiles, and runs source, the text of a .pw file. path is
   only for diagnostics. Takes ownership of source and always frees it.
   Returns the process exit code: 0 or 1. */
int exec_run_source(const char *path, char *source, bool dump_ast, bool dump_bytecode, bool jit_stats, bool run_tests, long stack_size, long max_frames);

/* Loads and runs a .paw file. Returns the process exit code. */
int exec_run_paw(const char *path, bool dump_bytecode, bool jit_stats, long stack_size, long max_frames);

/* Loads and runs the program embedded in a self-extracting executable.
   offset and size locate it in exe_path, as found by
   paw_is_self_extracting(). Returns the process exit code. */
int exec_run_embedded(const char *exe_path, uint64_t offset, uint64_t size, bool dump_bytecode, bool jit_stats, long stack_size, long max_frames);

/* The position of target in resolver->compiled_units, by pointer.
   target must be canonical (see module_unit_canonical()). Returns
   SIZE_MAX if it is not found, which a correct caller never sees. The
   build command uses it for the .paw edge list. */
size_t exec_find_module_index(const module_resolver_t *resolver, const module_unit_t *target);

/* Appends one edge to *edges, a growable array. *count and *capacity
   belong to the caller and are updated here. */
void exec_push_edge(paw_import_edge_t **edges, size_t *count, size_t *capacity, size_t importer_index, size_t target_index, const char *local_name, size_t local_name_length);
