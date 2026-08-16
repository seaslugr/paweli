#pragma once

#include <stddef.h>
#include <stdint.h>

/* Copies length bytes from code into a fresh page-rounded mapping and
   makes it executable (RW while writing, then RX; never RWX at any
   point a stray jump could land on it). The mapping is never freed:
   paweli functions are never recompiled or invalidated once JIT'd (see
   docs/design.md's JIT decision log entry), so there is no code cache
   to evict. Returns NULL on failure.

   debug_name is used only when the PAWELI_PERF_MAP environment
   variable is set, in which case an entry is appended to
   /tmp/perf-<pid>.map (perf's documented convention for symbolizing
   dynamically-generated code) so `perf report` shows real paweli
   function names instead of bare addresses inside JIT'd code. May be
   NULL. TEMPORARY profiling aid, not part of the permanent JIT
   surface. */
void *jit_alloc_exec(const uint8_t *code, size_t length, const char *debug_name);
