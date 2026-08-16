#pragma once

#include "runtime/object.h"
#include "runtime/table.h"
#include "runtime/vm.h"
#include "stdlib/array.h"
#include "stdlib/convert.h"
#include "stdlib/io.h"
#include "stdlib/math.h"
#include "stdlib/string.h"

/* Registers everything the standard library provides into globals: bare
   "print", plus a "stdlib" module holding the rest. Both are always
   available in every loaded file, no #module needed. Called once per
   file via vm_new_file_globals(), not directly. */
void stdlib_install(vm_t *vm, table_t *globals);

/* Shared helpers for the native functions in math.c/string.c/convert.c. */
void stdlib_register_fn(vm_t *vm, obj_module_t *module, const char *name, native_fn_t function);
bool stdlib_check_arity(vm_t *vm, const char *fn_name, int arg_count, int expected);
