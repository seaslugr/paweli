#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/object.h"
#include "runtime/vm.h"

// The result of loading a .paw container: an entry script ready to
// vm_run(), and (only when the program had a #module dependency) the
// module's own script plus enough to know it needs to run first. Mirrors
// what main.c already builds for a from-source compile (see
// compiler_compile()/module_compile()), so both paths run identically.
typedef struct {
	obj_function_t *entry_script;
	obj_function_t *module_script;
} paw_program_t;

/* True if path's first bytes are the .paw magic. Used by main.c to
   decide between this loader and the normal parse/compile path; a
   missing or unreadable file reads as "not a paw file", so the normal
   path can report the real error itself. */
bool paw_is_paw_file(const char *path);

/* Serializes the already-compiled program rooted at entry_script (and,
   if module_obj is non-NULL, the module it depends on) to path. Every
   argument mirrors what src/main.c already has in hand right after a
   normal compile: entry_filename/entry_source are the entry file's own
   path and source text; module_filename/module_source are the same for
   the module file, only read when module_obj is non-NULL. strip_source
   omits both source texts from the file, trading smaller output and a
   distributable artifact with no embedded source for runtime errors
   that can no longer render a source snippet.

   Returns false on failure (e.g. path could not be opened for writing),
   with an error already printed to stderr. */
bool paw_write_file(const char *path, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, obj_module_t *module_obj, obj_function_t *module_script, const char *module_local_name, size_t module_local_name_length, const char *module_filename, const char *module_source, bool strip_source);

/* Loads path, allocating a fresh obj_function_t/obj_string_t graph on
   vm->heap (interned through vm->strings) and a fresh per-file globals
   table for the entry file, and for the module file if one is present,
   both via vm_new_file_globals(vm), same as a from-source compile
   uses. Any embedded source text is registered with
   vm_track_owned_source() so runtime diagnostics can render it, and
   vm->filename/vm->source are pointed at the entry file's own (embedded
   or NULL). A module's local name, if any, is bound into the entry
   file's globals the same way main.c binds it for a from-source
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
bool paw_build_executable(const char *output_path, const char *stub_path, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, obj_module_t *module_obj, obj_function_t *module_script, const char *module_local_name, size_t module_local_name_length, const char *module_filename, const char *module_source, bool strip_source);
