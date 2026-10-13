#include <stdio.h>
#include <stdlib.h>

#include "jit_exec_mem.h"
#include "platform/platform.h"

#define PERF_MAP_PATH_CAPACITY 64

static void write_perf_map_entry(void *mem, size_t length, const char *debug_name);

static void write_perf_map_entry(void *mem, size_t length, const char *debug_name) {
	static int checked_env = 0;
	static int enabled = 0;
	char path[PERF_MAP_PATH_CAPACITY];
	FILE *f;

	if (!checked_env) {
		enabled = getenv("PAWELI_PERF_MAP") != NULL;
		checked_env = 1;
	}
	if (!enabled || debug_name == NULL) {
		return;
	}

	snprintf(path, sizeof(path), "/tmp/perf-%d.map", platform_process_id());
	f = fopen(path, "a");
	if (f == NULL) {
		return;
	}
	fprintf(f, "%llx %llx jit:%s\n", (unsigned long long)(uintptr_t)mem, (unsigned long long)length, debug_name);
	fclose(f);
}

void *jit_alloc_exec(const uint8_t *code, size_t length, const char *debug_name) {
	void *mem;

	mem = platform_alloc_exec(code, length);
	if (mem != NULL) {
		write_perf_map_entry(mem, length, debug_name);
	}
	return mem;
}
