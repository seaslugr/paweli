#pragma once

#include <stdbool.h>

#include "runtime/object.h"

/* Translates function's bytecode chunk to native x86-64 machine code and
   installs it as function->jit_entry. Every opcode gets a real
   translation (see the per-opcode strategy in jit_compile.c's file
   comment) with no speculation and no deopt: once compiled, a function
   stays compiled forever. Returns false the first time it reaches an
   opcode this compiler does not yet handle, leaving function to keep
   interpreting forever (function->jit_state is set to JIT_FAILED so
   src/jit/jit_dispatch.c never retries it). This makes incomplete
   opcode coverage safe by construction while the JIT is built out
   phase by phase. */
bool jit_compile_function(obj_function_t *function);
