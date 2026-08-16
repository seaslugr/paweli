/* mmap()/mprotect() are POSIX, not ISO C11 (this project builds with
   CMAKE_C_EXTENSIONS OFF, i.e. strict -std=c11), so sys/mman.h doesn't
   declare them without an explicit feature-test macro, for the same
   reasoning as src/platform/exe.c's own readlink()/chmod() use. This is
   the third and last file in the tree allowed to touch raw POSIX APIs,
   alongside exe.c and src/diagnostics/diagnostic.c's isatty()/fileno().
   A future Windows port has exactly these three places to give a
   real implementation to. Linux-only for now (paweli only targets ELF
   right now). */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "jit_exec_mem.h"

static size_t round_up_to_page(size_t n) {
	size_t page_size;

	page_size = (size_t)sysconf(_SC_PAGESIZE);
	return (n + page_size - 1) / page_size * page_size;
}

static void write_perf_map_entry(void *mem, size_t length, const char *debug_name) {
	static int checked_env = 0;
	static int enabled = 0;
	char path[64];
	FILE *f;

	if (!checked_env) {
		enabled = getenv("PAWELI_PERF_MAP") != NULL;
		checked_env = 1;
	}
	if (!enabled || debug_name == NULL) {
		return;
	}

	snprintf(path, sizeof(path), "/tmp/perf-%d.map", (int)getpid());
	f = fopen(path, "a");
	if (f == NULL) {
		return;
	}
	fprintf(f, "%llx %llx jit:%s\n", (unsigned long long)(uintptr_t)mem, (unsigned long long)length, debug_name);
	fclose(f);
}

void *jit_alloc_exec(const uint8_t *code, size_t length, const char *debug_name) {
	size_t mapped_size;
	void *mem;

	mapped_size = round_up_to_page(length);
	mem = mmap(NULL, mapped_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		return NULL;
	}

	memcpy(mem, code, length);

	if (mprotect(mem, mapped_size, PROT_READ | PROT_EXEC) != 0) {
		munmap(mem, mapped_size);
		return NULL;
	}

	write_perf_map_entry(mem, length, debug_name);

	return mem;
}
