#pragma once

#include "runtime/object.h"
#include "runtime/table.h"
#include "runtime/vm.h"
#include "stdlib/array.h"
#include "stdlib/convert.h"
#include "stdlib/io.h"
#include "stdlib/math.h"
#include "stdlib/string.h"

/* Registers the standard library in globals: the bare "print", and a
   "stdlib" module with the rest. Every loaded file gets both, with no
   #module. vm_new_file_globals() calls this once per file. */
void stdlib_install(vm_t *vm, table_t *globals);

/* Shared helpers for the natives in the other stdlib files. */
void stdlib_register_fn(vm_t *vm, obj_module_t *module, const char *name, native_fn_t function);
bool stdlib_check_arity(vm_t *vm, const char *fn_name, int arg_count, int expected);
