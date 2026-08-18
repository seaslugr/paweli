#pragma once

#include <stdbool.h>

#include "runtime/object.h"

// Threshold, in combined calls + loop back-edges (see
// jit_note_loop_iteration() below), before a function gets
// JIT-compiled. Overridable via the PAWELI_JIT_THRESHOLD env var
// (mirrors vm.c's own PAWELI_GC_STRESS convention); 0 forces every
// function to compile on its very first call, for testing the JIT path
// itself.
//
// Default 1: a function is compiled on its very first call, so even a
// called-exactly-once function whose single call loops for the whole
// program runs natively. That is the common scripting shape (main() called
// once, doing all the work). This was chosen deliberately after
// measuring: with a higher default threshold, jit_maybe_compile() also
// triggers via OP_LOOP back-edge counting, but without
// on-stack-replacement (there is none, matching the no-speculation/
// no-deopt design) the *currently running* call keeps interpreting, so
// a once-called function got compiled and never benefited on the call
// that mattered. Eager-compile-on-first-call removes that entire class
// of misses at the cost of compiling cold functions too. This is measured
// negligible for real programs, since jit_compile_function() needs no
// allocation and pays only microsecond-scale time per function.
#define JIT_DEFAULT_HOTNESS_THRESHOLD 1

/* Called from call_value() on every call to a bare/closure paweli
   function. Increments function->jit_hotness_count and, once it
   crosses the hotness threshold, compiles function synchronously via
   jit_compile_function(). Compiling a chunk_t that's already fully
   built cannot itself allocate on paweli's heap, error, or otherwise
   need to run reentrantly, so this is safe to call directly from the
   middle of call_value(). A function that previously failed to compile
   (JIT_FAILED) is never retried. Returns true if function->jit_entry is
   set on return (whether just compiled or already had been from an
   earlier call), false otherwise. The caller uses this to decide
   whether to dispatch to jit_entry or push an interpreted frame. */
bool jit_maybe_compile(obj_function_t *function);

/* Called from run()'s own OP_LOOP handler (vm.c) on every back-edge.
   a function called only once but looping millions of times inside
   that single call is exactly as JIT-worthy as one called millions of
   times, and a pure call-count heuristic misses it entirely. Shares
   jit_hotness_count and the same threshold with jit_maybe_compile()
   above. Deliberately returns nothing: compiling mid-loop can only
   speed up a *later* call to this same function, never the call
   currently interpreting the loop that triggered it. There is no
   on-stack-replacement here, matching the rest of the JIT's
   no-speculation/no-deopt design (see jit_compile.c's file comment),
   so run()'s own OP_LOOP handler has nothing to branch on. */
void jit_note_loop_iteration(obj_function_t *function);
