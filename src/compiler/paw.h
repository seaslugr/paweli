#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/object.h"
#include "runtime/vm.h"

// One module's worth of serialization info, write side: the compiled
// content shared by every importer of this one file (the entry file
// itself is passed separately, since there's always exactly one of
// those but zero or more modules). Which importer binds this module
// under what local name is edge information, not a property of the
// module itself now that one module can be imported by more than one
// file; see paw_import_edge_t.
typedef struct {
	obj_module_t *module_obj;
	obj_function_t *script;
	const char *filename;
	const char *source;
} paw_module_info_t;

// One #module import edge: importer_index (0 for the entry file, or
// 1 + a modules[] index for a module importing another module) binds
// modules[target_index] under local_name, exactly the runtime binding
// module_compile()/compile_from_source() (main.c) set up for a
// from-source compile. local_name is a raw byte span (module_unit_t's
// own local_name/local_name_length), not a NUL-terminated C string,
// unlike filename/source elsewhere in this file.
typedef struct {
	size_t importer_index;
	size_t target_index;
	const char *local_name;
	size_t local_name_length;
} paw_import_edge_t;

// The result of loading a .paw container: an entry script ready to
// vm_run(), and every module the program transitively depends on (each
// needs vm_run() once, before the entry script; see module_scripts'
// ordering note below). Mirrors what main.c already builds for a
// from-source compile (see compiler_compile()/module_compile()), so both
// paths run identically.
typedef struct {
	obj_function_t *entry_script;
	// Dependency order (a module that itself imports another always
	// comes after that other one), the same order module_resolver_t's
	// own compiled_units ends up in from a from-source compile: running
	// module_scripts[0..module_count) in this order, once each, is
	// exactly right, matching run_source()'s (main.c) own loop over
	// resolver.compiled_units.
	obj_function_t **module_scripts;
	size_t module_count;
} paw_program_t;

/* True if path's first bytes are the .paw magic. Used by main.c to
   decide between this loader and the normal parse/compile path; a
   missing or unreadable file reads as "not a paw file", so the normal
   path can report the real error itself. */
bool paw_is_paw_file(const char *path);

/* Serializes the already-compiled program rooted at entry_script (and
   every module in modules, module_count of them, zero or more, plus
   edges/edge_count recording who imports whom under what name) to path.
   entry_filename/entry_source are the entry file's own path and source
   text, same as src/main.c already has in hand right after a normal
   compile. strip_source omits every source text (entry and modules)
   from the file, trading smaller output and a distributable artifact
   with no embedded source for runtime errors that can no longer render
   a source snippet.

   Returns false on failure (e.g. path could not be opened for writing),
   with an error already printed to stderr. */
bool paw_write_file(const char *path, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, const paw_module_info_t *modules, size_t module_count, const paw_import_edge_t *edges, size_t edge_count, bool strip_source);

/* Loads path, allocating a fresh obj_function_t/obj_string_t graph on
   vm->heap (interned through vm->strings) and a fresh per-file globals
   table for the entry file and for every module, all via
   vm_new_file_globals(vm), same as a from-source compile uses. Any
   embedded source text is registered with vm_track_owned_source() so
   runtime diagnostics can render it, and vm->filename/vm->source are
   pointed at the entry file's own (embedded or NULL). Every import edge
   is rebuilt exactly (each importer's own globals table gets each of
   its targets bound under its own local name), the same graph
   compile_from_source()/module_compile() build for a from-source
   compile.

   Returns a heap-allocated paw_program_t on success (free with
   paw_program_free()), or NULL on failure with an error already
   printed to stderr. */
paw_program_t *paw_load_file(const char *path, vm_t *vm);

/* Like paw_load_file(), but reads the payload from a size-byte range
   starting at offset within exe_path instead of treating the whole
   file as the payload. Used to run a self-extracting executable's own
   appended program; see paw_is_self_extracting(). */
paw_program_t *paw_load_embedded(const char *exe_path, uint64_t offset, uint64_t size, vm_t *vm);

void paw_program_free(paw_program_t *program);

/* True if exe_path ends with a valid paweli executable trailer (see
   paw_build_executable()), in which case *out_offset and *out_size are
   set to the byte range of its embedded .paw payload. False for any other
   file, including a perfectly ordinary paweli binary with nothing
   appended. There is no ambiguity to worry about, since the trailer
   sits at a fixed distance from the end of the file and a plain
   binary's own last 24 bytes will essentially never happen to match
   the magic by chance. */
bool paw_is_self_extracting(const char *exe_path, uint64_t *out_offset, uint64_t *out_size);

/* Builds a self-extracting native executable at output_path: a copy of
   stub_path (in practice, always the currently running paweli binary
   itself; see platform_self_exe_path(), since it already contains
   everything needed to detect and run the trailer this appends)
   followed by the compiled program, exactly as paw_write_file() would
   write it, followed by a small fixed-size trailer recording where
   that payload starts and how long it is. Every other argument is the
   same as paw_write_file()'s.

   Returns false on failure (stub_path couldn't be read, output_path
   couldn't be written, or the result couldn't be marked executable),
   with an error already printed to stderr. */
bool paw_build_executable(const char *output_path, const char *stub_path, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, const paw_module_info_t *modules, size_t module_count, const paw_import_edge_t *edges, size_t edge_count, bool strip_source);
