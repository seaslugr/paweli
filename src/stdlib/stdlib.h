#pragma once

#include "runtime/object.h"
#include "runtime/vm.h"
#include "stdlib/convert.h"
#include "stdlib/math.h"
#include "stdlib/string.h"

/* Registers everything the standard library provides into vm's globals:
   bare "print", plus a "stdlib" module holding the rest. Both are always
   available. No #module is needed, same as any other builtin. */
void stdlib_install(vm_t *vm);

/* Shared helpers for the native functions in math.c/string.c/convert.c. */
void stdlib_register_fn(vm_t *vm, obj_module_t *module, const char *name, native_fn_t function);
bool stdlib_check_arity(vm_t *vm, const char *fn_name, int arg_count, int expected);
