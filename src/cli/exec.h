#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compiler/paw.h"
#include "runtime/gc.h"
#include "runtime/module.h"
#include "runtime/object.h"
#include "runtime/table.h"
#include "runtime/vm.h"

// Given already-parsed inputs (a path, a source buffer, resolved
// flags), does the actual work of compiling, running, or serializing
// a paweli program. Nothing here touches argv; that's the rest of
// src/cli/'s job (cli.c/commands.c), which calls into this file once
// it has decided what to do. Kept as one file (not split per command)
// since compiling and running are already this tightly coupled
// everywhere else in the codebase (see exec_compile_from_source()'s
// own comment on why it's shared by both a plain run and a build).

/* Reads path whole into a NUL-terminated malloc'd buffer. Returns NULL
   on failure, with an error already printed to stderr. */
char *exec_read_file(const char *path);

/* Shared by exec_run_source() and src/cli/'s build command: parses
   path/source, resolves and compiles every #module import the entry
   file itself declares (zero or more, via resolver, which also
   transitively resolves each of those modules' own imports), then
   compiles the entry file itself. *out_entry_globals is populated and
   the compiled entry script is returned on success. resolver is the
   caller's, created once per whole compile session and freed once via
   module_resolver_free() after everything (including any module
   scripts) has finished running; it owns every module_unit_t this call
   (transitively) produces.

   *out_entry_imports and *out_entry_import_count are set to the entry
   file's own direct imports (zero or more; a possible mix of canonical
   units and aliases, same as module_compile()'s own own_imports array)
   on success, left NULL/0 on failure. Unlike everything resolver owns,
   this one array IS the caller's to free (module_unit_free_if_alias()
   each entry, then free() the array itself): a plain run doesn't need
   it beyond the runtime bindings already wired up here and can free it
   immediately, but a build needs it, still live, to build a .paw
   file's own edge list before it's safe to free.

   Returns NULL on failure, with an error already printed to stderr. */
obj_function_t *exec_compile_from_source(const char *path, const char *source, gc_heap_t *heap, table_t *strings, vm_t *vm, module_resolver_t *resolver, table_t **out_entry_globals, const module_unit_t ***out_entry_imports, size_t *out_entry_import_count, bool dump_ast, bool run_tests);

/* Parses, compiles, and runs source (a .pw file's contents; path is
   used for diagnostics only). Takes ownership of source (always
   freed, on every path). Returns the process exit code (0 success,
   1 failure), never exits or aborts itself. */
int exec_run_source(const char *path, char *source, bool dump_ast, bool dump_bytecode, bool jit_stats, bool run_tests, long stack_size, long max_frames);

/* Loads and runs a .paw container file. Returns the process exit
   code, same convention as exec_run_source(). */
int exec_run_paw(const char *path, bool dump_bytecode, bool jit_stats, long stack_size, long max_frames);

/* Loads and runs the program embedded in a self-extracting executable
   (offset/size locate it within exe_path, already validated by the
   caller via paw_is_self_extracting()). Returns the process exit
   code, same convention as exec_run_source(). */
int exec_run_embedded(const char *exe_path, uint64_t offset, uint64_t size, bool dump_bytecode, bool jit_stats, long stack_size, long max_frames);

/* Position of target within resolver->compiled_units, by pointer
   identity. target must already be canonicalized (module_unit_canonical())
   by the caller. Always found in practice when called correctly;
   SIZE_MAX is only a defensive fallback. Exported for src/cli/'s build
   command, which uses it (with exec_push_edge()) to build a .paw
   file's own module-import edge list. */
size_t exec_find_module_index(const module_resolver_t *resolver, const module_unit_t *target);

/* Appends one edge to *edges (a growable array; *count and *capacity
   are the caller's own bookkeeping, grown here as needed via
   realloc()). See exec_find_module_index()'s own comment on why this
   is exported. */
void exec_push_edge(paw_import_edge_t **edges, size_t *count, size_t *capacity, size_t importer_index, size_t target_index, const char *local_name, size_t local_name_length);
