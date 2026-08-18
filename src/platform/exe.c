// readlink() and chmod() are POSIX, not ISO C11 (this project builds
// with CMAKE_C_EXTENSIONS OFF, i.e. strict -std=c11), so unistd.h/
// sys/stat.h don't declare them without an explicit feature-test
// macro, for the same reasoning as src/diagnostics/diagnostic.c's own
// isatty()/fileno() use. Linux-only for now (paweli only targets ELF
// binaries right now); every POSIX call in the whole codebase stays
// confined to this one file, diagnostic.c, and src/jit/jit_exec_mem.c
// (mmap()/mprotect() for JIT code memory), so a future Windows port
// only has three places to give a real implementation to.
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "exe.h"

char *platform_self_exe_path(void) {
	char *buf;
	ssize_t length;
	size_t capacity;

	capacity = 4096;
	buf = malloc(capacity);
	length = readlink("/proc/self/exe", buf, capacity - 1);
	if (length < 0) {
		free(buf);
		return NULL;
	}

	buf[length] = '\0';
	return buf;
}

bool platform_make_executable(const char *path) {
	struct stat info;

	if (stat(path, &info) != 0) {
		return false;
	}

	return chmod(path, info.st_mode | S_IXUSR | S_IXGRP | S_IXOTH) == 0;
}
