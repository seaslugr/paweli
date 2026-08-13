#pragma once

#include "parser/ast.h"
#include "runtime/object.h"
#include "runtime/table.h"

/* Compiles a parsed program into a synthetic top-level "<script>" function.
   It defines every top-level fn as a global, then calls main() and
   returns. All heap objects created during compilation (strings, function
   objects) get linked into *objects, and all strings get interned into
   *strings. There is no GC yet, so the caller owns both and must free
   them once done. Returns NULL if compilation failed; errors are already
   printed to stderr. */
obj_function_t *compiler_compile(const ast_node_t *program, const char *filename, obj_t **objects, table_t *strings);
