#pragma once

#include "parser/ast.h"
#include "runtime/module.h"
#include "runtime/object.h"
#include "runtime/table.h"

/* Compiles a parsed program into a synthetic top-level "<script>"
   function. It defines every top-level fn as a global, then (for the
   entry file only, see below) calls main() and returns. All heap
   objects created during compilation (strings, function objects) get
   linked into *heap, and all strings get interned into *strings; both
   stay owned by the caller, who must keep them alive (and eventually
   free them) for as long as the VM runs on the result. Returns NULL if
   compilation failed; errors are already printed to stderr.

   Compilation is two-pass: every top-level fn's signature (and an
   allocated, still-empty obj_function_t for it) is collected first, so
   overload resolution, forward references, and recursion all see every
   sibling function regardless of declaration order, before any body is
   actually compiled.

   file_globals is this file's own private global scope: every function
   this call compiles, including the returned script itself, gets
   stamped with it, so OP_GET_GLOBAL/OP_DEFINE_GLOBAL resolve names
   against it rather than some other loaded file's globals. The caller
   owns it (see vm_new_file_globals()).

   home_module/out_exports are used together, only when program has a
   #moduledef directive: home_module is the (already allocated, empty)
   module object to populate, and out_exports is initialized and filled
   with the type signature of every exported overload, for
   module_compile()'s caller to hand to whatever file imports this
   module. Passing NULL for both compiles program as an ordinary
   (non-module) file; `export` is then a compile error wherever it
   appears. compiler_compile() itself does not call main() for a module
   file; the compiled script only defines its own globals, since a
   module's exports are already fully wired into home_module->members by
   the time this returns, with no runtime step needed to populate them.

   imported_modules/imported_module_count cover every #module import
   program itself has (zero or more): each entry is the already-compiled
   unit for one import (see module_compile()), consulted to type-check
   and resolve every `<local_name>.member(...)` call in program, and (via
   its own exported_structs) every unqualified struct-typed name that
   isn't declared locally. Two different imports exporting the same
   struct name unqualified is an ambiguous-reference compile error, not
   first-one-wins.

   out_structs, like out_exports, is only meaningful when program has a
   #moduledef directive: on success it receives ownership of this file's
   whole struct registry (every struct it declared, not just the
   exported ones; see struct_def_t.is_exported), for module_compile()'s
   caller to hand to whatever file imports this module as its
   exported_structs. This registry is normally freed before returning;
   passing a non-NULL out_structs is what keeps it alive instead, since
   an exported struct-typed signature's type_t holds a raw pointer into
   it that has to stay valid (and stay the *same* pointer, since
   TYPE_STRUCT equality is pointer identity) for as long as any importer
   might resolve against it. Pass NULL when compiling an ordinary
   (non-module) file, or when program has no #module directive; there is
   nothing else useful to do with this file's own local struct registry
   once compilation finishes in that case.

   run_tests_mode only matters for the entry file (pass false for a
   #module dependency's own compile, see module.c): when true, the
   compiled script's top-level code calls every @test-tagged top-level
   function instead of main(); main() is still required to exist
   either way, just not called in this mode. */
obj_function_t *compiler_compile(const ast_node_t *program, const char *filename, const char *source, gc_heap_t *heap, table_t *strings, table_t *file_globals, obj_module_t *home_module, signature_table_t *out_exports, const module_unit_t *const *imported_modules, size_t imported_module_count, struct_registry_t **out_structs, bool run_tests_mode);

/* Frees the process-lifetime native signature tables (print/assert/
   stdlib.*), built lazily on first use by compiler_compile(). Call once,
   after the VM has finished running and nothing will call
   compiler_compile() again. */
void compiler_shutdown(void);
