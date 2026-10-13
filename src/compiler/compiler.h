#pragma once

#include "parser/ast.h"
#include "runtime/module.h"
#include "runtime/object.h"
#include "runtime/table.h"

/* Compiles a parsed program into a top-level "<script>" function. The
   script defines every top-level fn as a global. For the entry file it
   then calls main(). Every heap object created goes into *heap, and
   every string is interned into *strings. The caller owns both and
   keeps them alive while the VM runs the result. Returns NULL on
   failure. Errors are already printed.

   Compilation has two passes. The first collects every top-level fn's
   signature, so overloads, forward references, and recursion work in
   any declaration order. The second compiles the bodies.

   file_globals is this file's private global scope. Every function
   compiled here is stamped with it. The caller owns it (see
   vm_new_file_globals()).

   home_module and out_exports are used only for a #moduledef file.
   home_module is the empty module object to fill. out_exports receives
   the signature of every exported overload, for the importer. Pass
   NULL for both for an ordinary file, where `export` is a compile
   error. A module's script never calls main(). Its exports are wired
   into home_module->members at compile time.

   imported_modules holds the compiled unit of every #module import in
   program. It is used to resolve `<local_name>.member(...)` calls and
   unqualified struct names that are not declared locally. A struct
   name exported by two imports is a compile error.

   out_structs is also only for a #moduledef file. On success it
   receives ownership of the file's whole struct registry, exported or
   not (see struct_def_t.is_exported). An exported signature's type_t
   points into the registry, and TYPE_STRUCT equality is pointer
   identity, so the registry must outlive every importer. Pass NULL
   otherwise, and the registry is freed before returning.

   run_tests_mode only matters for the entry file. When true, the
   script calls every @test function and skips main(). main() must
   still exist. */
obj_function_t *compiler_compile(const ast_node_t *program, const char *filename, const char *source, gc_heap_t *heap, table_t *strings, table_t *file_globals, obj_module_t *home_module, signature_table_t *out_exports, const module_unit_t *const *imported_modules, size_t imported_module_count, struct_registry_t **out_structs, bool run_tests_mode);

/* Frees the native signature tables (print, assert, stdlib.*), which
   compiler_compile() builds on first use. Call once, after the VM is
   done. */
void compiler_shutdown();
