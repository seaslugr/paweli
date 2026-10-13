#pragma once

#include "runtime/object.h"

#define GC_INITIAL_THRESHOLD (1024 * 1024)
#define GC_HEAP_GROW_FACTOR 2

// How many bytes may be allocated between two safepoints before
// gc_heap_note_alloc() aborts. It is far above anything a correct run
// allocates in one window. It catches one bug: an allocating call in
// JIT code with no jit_emit_gc_check() before it, running in a loop.
#define GC_SAFEPOINT_STALENESS_LIMIT (16 * 1024 * 1024)

void gc_heap_init(gc_heap_t *heap);

/* Every heap allocation goes through here and never changes
   heap->bytes_allocated directly, so PAWELI_GC_SAFEPOINT_CHECK can
   track bytes_since_safepoint in one place. */
void gc_heap_note_alloc(gc_heap_t *heap, size_t size);

/* Resets bytes_since_safepoint. run() calls it at every safepoint,
   whether or not a collection runs. JIT code zeroes the field inline. */
void gc_note_safepoint(gc_heap_t *heap);

/* Runs one full mark-sweep cycle. It marks every value reachable from
   the roots (value stack, call frames, globals tables, open upvalues)
   and frees every heap object it did not reach. It also sweeps the
   string intern table, which holds weak references.

   Set PAWELI_GC_DEBUG to log the byte counts of each collection to
   stderr.

   The whole module depends on one rule: gc_collect() is only called at
   an instruction boundary. That is run()'s dispatch loop, or the same
   point in JIT code, right before a call that can allocate (see
   jit_emit_gc_check()). It is never called inside an object_new_*()
   function or a native. A new object is unreachable until the bytecode
   that asked for it stores it somewhere. A collection in between would
   free it. Keeping to these points avoids tracking temporary roots. */
void gc_collect(vm_t *vm);

/* Frees every object in the heap with no reachability check. Used at
   shutdown. */
void gc_free_all(gc_heap_t *heap);
