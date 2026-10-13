#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "runtime/object.h"
#include "runtime/vm.h"

// What the writer needs for one module: the compiled content shared by
// every importer of the file. The entry file is passed separately.
// Which importer binds the module under which name is an edge (see
// paw_import_edge_t), since several files can import one module.
typedef struct {
	obj_module_t *module_obj;
	obj_function_t *script;
	const char *filename;
	const char *source;
} paw_module_info_t;

// One #module import edge. importer_index is 0 for the entry file, or
// 1 + an index into modules[] for a module that imports another. It
// binds modules[target_index] under local_name. local_name is a byte
// span with a length and is not NUL-terminated.
typedef struct {
	size_t importer_index;
	size_t target_index;
	const char *local_name;
	size_t local_name_length;
} paw_import_edge_t;

// The result of loading a .paw container: an entry script ready for
// vm_run(), and every module the program depends on. It matches what a
// compile from source builds, so both paths run the same way.
typedef struct {
	obj_function_t *entry_script;
	// In dependency order: a module that imports another comes after it.
	// Run module_scripts[0..module_count) in this order, once each, before
	// the entry script.
	obj_function_t **module_scripts;
	size_t module_count;
} paw_program_t;

/* True if the file starts with the .paw magic. A missing or unreadable
   file gives false, so the normal compile path reports the real error. */
bool paw_is_paw_file(const char *path);

/* Writes the compiled program rooted at entry_script to path, with its
   modules and the edges that record who imports whom under which name.
   entry_filename and entry_source are the entry file's path and text.
   strip_source leaves every source text out of the file. The file is
   smaller, and a runtime error then prints no source snippet.

   Returns false on failure, with an error already printed. */
bool paw_write_file(const char *path, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, const paw_module_info_t *modules, size_t module_count, const paw_import_edge_t *edges, size_t edge_count, bool strip_source);

/* Loads path. It allocates the functions and strings on vm->heap,
   interned through vm->strings, and makes a globals table for the
   entry file and for every module with vm_new_file_globals(). Embedded
   source text is handed to vm_track_owned_source(), and vm->filename
   and vm->source are pointed at the entry file's. Every import edge is
   rebuilt: each importer's globals table gets each target under its
   local name.

   Returns a paw_program_t to free with paw_program_free(), or NULL
   with an error already printed. */
paw_program_t *paw_load_file(const char *path, vm_t *vm);

/* Like paw_load_file(), but reads the payload from size bytes at
   offset inside exe_path. Used to run the program appended to a
   self-extracting executable (see paw_is_self_extracting()). */
paw_program_t *paw_load_embedded(const char *exe_path, uint64_t offset, uint64_t size, vm_t *vm);

void paw_program_free(paw_program_t *program);

/* True if exe_path ends with a valid paweli executable trailer (see
   paw_build_executable()). *out_offset and *out_size then give the
   byte range of the embedded payload. False for any other file,
   including a plain paweli binary. The trailer sits at a fixed
   distance from the end of the file. */
bool paw_is_self_extracting(const char *exe_path, uint64_t *out_offset, uint64_t *out_size);

/* Builds a self-extracting executable at output_path: a copy of
   stub_path, then the compiled program as paw_write_file() writes it,
   then a fixed-size trailer with the payload's offset and length.
   stub_path is the running paweli binary (see
   platform_self_exe_path()), which already knows how to find and run
   the trailer. The other arguments are those of paw_write_file().

   Returns false on failure, with an error already printed. */
bool paw_build_executable(const char *output_path, const char *stub_path, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, const paw_module_info_t *modules, size_t module_count, const paw_import_edge_t *edges, size_t edge_count, bool strip_source);
