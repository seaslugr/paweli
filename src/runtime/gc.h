#pragma once

#include "runtime/object.h"

#define GC_INITIAL_THRESHOLD (1024 * 1024)
#define GC_HEAP_GROW_FACTOR 2

void gc_heap_init(gc_heap_t *heap);

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
