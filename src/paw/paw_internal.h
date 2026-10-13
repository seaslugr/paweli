#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paw.h"
#include "compiler/chunk.h"
#include "platform/platform.h"
#include "runtime/gc.h"
#include "runtime/table.h"
#include "runtime/value.h"

#define PAW_MAGIC "PAWC"
#define PAW_MAGIC_LENGTH 4
// Format versions. A reader rejects a file with another version.
// v2: function bodies carry local_reg_eligible, local_reg_types, and
//     local_reg_decl_end. Without them a loaded function gets no JIT
//     register allocation.
// v3: function bodies carry local_reg_dead_at. The JIT needs it to
//     know when a VAL_OBJ register local's slot stops being its own
//     (see jit_emit_gc_check()). Reading 0 for it would be unsafe.
// v4: one optional module became module_count modules, and a
//     function's home file became a u32 index into
//     [entry, module 0, module 1, ...].
#define PAW_VERSION 4

#define PAW_FLAG_HAS_SOURCE 0x01

#define PAW_CONST_NULL 0
#define PAW_CONST_BOOL 1
#define PAW_CONST_INT 2
#define PAW_CONST_FLOAT 3
#define PAW_CONST_STRING 4
#define PAW_CONST_FUNCTION 5
#define PAW_CONST_MODULE 6

#define PAW_NO_STRING UINT32_MAX

// A self-extracting executable is a copy of the paweli binary, then a
// .paw payload, then this fixed-size trailer. The trailer is found by
// its offset from the end of the file, so the size of the copied
// binary does not need to be known.
#define PAW_EXE_MAGIC "PAWELIEX"
#define PAW_EXE_MAGIC_LENGTH 8
#define PAW_EXE_TRAILER_SIZE (PAW_EXE_MAGIC_LENGTH + 8 + 8)

// Every string, function, and module object reachable from the
// program's roots gets a stable id the first time it is seen. The
// roots are the entry script, each module's script and module_obj, and
// any struct namespace object embedded as a constant. The three arrays
// are searched linearly by pointer. Programs are small enough that a
// hash table is not needed.
//
// file_globals holds each loaded file's globals table, entry first. A
// function's home file is found by comparing its ->globals against
// these. A struct's functions are only reachable through a namespace
// object, so there is no other way to tell which file they belong to.
typedef struct {
	FILE *out;
	bool ok;

	// [0] is the entry file's globals. [1..file_globals_count) are the
	// modules, in the order of the modules array given to
	// paw_write_payload(). A function's home file is its index here.
	table_t **file_globals;
	size_t file_globals_count;

	obj_string_t **strings;
	size_t strings_count;
	size_t strings_capacity;

	obj_function_t **functions;
	uint32_t *function_home;
	size_t functions_count;
	size_t functions_capacity;

	obj_module_t **modules;
	size_t modules_count;
	size_t modules_capacity;
} paw_writer_t;

typedef struct {
	const uint8_t *data;
	size_t size;
	size_t pos;
	bool ok;
} paw_reader_t;

bool paw_writer_write_function_body(paw_writer_t *w, size_t index);
bool paw_write_payload(FILE *out, obj_function_t *entry_script, const char *entry_filename, const char *entry_source, const paw_module_info_t *modules, size_t module_count, const paw_import_edge_t *edges, size_t edge_count, bool strip_source);
