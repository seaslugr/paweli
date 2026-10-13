#pragma once

#include "runtime/object.h"

#define GC_INITIAL_THRESHOLD (1024 * 1024)
#define GC_HEAP_GROW_FACTOR 2

// How much can accumulate in bytes_since_safepoint (gc_heap_t, object.h)
// before gc_heap_note_alloc() concludes that safepoints have stopped
// running somewhere on the current code path and aborts. Deliberately
// far above any single legitimate allocation this codebase ever makes
// in one safepoint-to-safepoint window (no bulk data structures), so it
// only trips on the specific bug class it exists to catch: an
// allocating call reachable from JIT-generated code with no paired
// emit_gc_check(), left allocating in an unbounded loop with nothing
// else in the same function ever reaching a safepoint.
#define GC_SAFEPOINT_STALENESS_LIMIT (16 * 1024 * 1024)

void gc_heap_init(gc_heap_t *heap);

// Every heap allocation must funnel through here instead of touching
// heap->bytes_allocated directly (see object.c), so PAWELI_GC_SAFEPOINT_CHECK
// has one place to also track bytes_since_safepoint.
void gc_heap_note_alloc(gc_heap_t *heap, size_t size);

// Called from every sanctioned GC safepoint (vm.c's run() loop; the
// equivalent point in JIT-generated code is emitted directly by
// emit_gc_check() in src/jit/jit_compile.c rather than calling this,
// since it's cheaper to zero one field in place than to emit a real
// call). Resets bytes_since_safepoint regardless of whether a
// collection actually ran this time -- reaching the check at all is
// what counts, not the outcome.
void gc_note_safepoint(gc_heap_t *heap);

/* Runs one full mark-sweep cycle on vm: traces every value reachable from
   its roots (value stack, call frames, globals table), then frees every
   heap object that was not reached. Also sweeps vm's string-intern table
   (table_remove_white()), since that table only holds weak references.

   Set PAWELI_GC_DEBUG to any non-empty value to log each collection's
   bytes_allocated before/after and the previous next_gc threshold to
   stderr. Off by default; costs one getenv() check per collection when
   unset, nothing per instruction (unlike PAWELI_GC_STRESS in vm.c).

   Safety invariant this whole module depends on: gc_collect() is only
   ever called from a bytecode instruction boundary: vm.c's own
   instruction-dispatch loop, or (see src/jit/jit_compile.c's
   emit_gc_check()) the exact equivalent point in JIT-generated native
   code, immediately before a call that's about to allocate; never
   from inside an object_new_*() call or a native function body. A
   freshly allocated object is not reachable from any root until the
   bytecode that requested it stores it somewhere (a push, a
   table_set, ...); if collection could run in the middle of that, the
   object could be swept before anything points at it. Keeping every
   call to gc_collect() at one of these well-known points sidesteps
   that instead of tracking temporary roots. */
void gc_collect(vm_t *vm);

/* Unconditionally frees every object in the heap, no reachability check.
   Used at program shutdown once nothing needs to run anymore. */
void gc_free_all(gc_heap_t *heap);
