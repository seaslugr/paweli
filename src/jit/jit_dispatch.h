#pragma once

#include <stdbool.h>

#include "runtime/object.h"

// The number of calls plus loop back-edges before a function is
// JIT-compiled. PAWELI_JIT_THRESHOLD overrides it. The tests set it to
// 0 to force every function through the JIT.
//
// The default is 1: compile on the first call. There is no on-stack
// replacement, so a function compiled in the middle of a call keeps
// interpreting for that call. A program whose main() is called once
// and loops for the whole run would then never use its compiled code.
// Compiling cold functions too costs microseconds per function.
#define JIT_DEFAULT_HOTNESS_THRESHOLD 1

/* Called from call_value() on every call to a paweli function or
   closure. Adds to function->jit_hotness_count and compiles the
   function once the count reaches the threshold. Compiling cannot
   allocate on the paweli heap or fail with a runtime error, so it is
   safe in the middle of call_value(). A JIT_FAILED function is never
   retried. Returns true if function->jit_entry is set on return. */
bool jit_maybe_compile(obj_function_t *function);

/* Called from the OP_LOOP handler in run() on every back-edge. A
   function called once that loops a million times is as hot as one
   called a million times. It shares the counter and threshold with
   jit_maybe_compile(). It returns nothing: with no on-stack
   replacement, a compile here only speeds up a later call. */
void jit_note_loop_iteration(obj_function_t *function);
