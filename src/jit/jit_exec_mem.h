#pragma once

#include <stddef.h>
#include <stdint.h>

/* Copies length bytes of code into fresh executable memory (see
   platform_alloc_exec()). The memory is never freed, since a compiled
   function is never recompiled. Returns NULL on failure.

   debug_name is only used when PAWELI_PERF_MAP is set. An entry is
   then appended to /tmp/perf-<pid>.map, the file `perf report` reads
   to name JIT code. May be NULL. This is a Linux profiling aid. */
void *jit_alloc_exec(const uint8_t *code, size_t length, const char *debug_name);
