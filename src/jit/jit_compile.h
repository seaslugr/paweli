#pragma once

#include <stdbool.h>

#include "runtime/object.h"

/* Translates the function's bytecode to x86-64 machine code and
   installs it as function->jit_entry. There is no speculation and no
   deoptimization: a compiled function stays compiled. Returns false at
   the first opcode with no translation and sets function->jit_state to
   JIT_FAILED, so the function stays interpreted and is never retried. */
bool jit_compile_function(obj_function_t *function);
