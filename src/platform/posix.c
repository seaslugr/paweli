// The POSIX and XSI calls below are not ISO C11. This project builds
// with CMAKE_C_EXTENSIONS OFF, so the headers only declare them when
// these feature-test macros are set. realpath() needs _XOPEN_SOURCE.
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"

#define SELF_EXE_PATH_CAPACITY 4096
#define NULL_DEVICE "/dev/null"

char *platform_self_exe_path() {
	char *buf;
	ssize_t length;

	buf = malloc(SELF_EXE_PATH_CAPACITY);
	length = readlink("/proc/self/exe", buf, SELF_EXE_PATH_CAPACITY - 1);
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

const char *platform_exe_suffix() {
	return "";
}

bool platform_make_dir(const char *path) {
	return mkdir(path, 0755) == 0 || errno == EEXIST;
}

char *platform_canonical_path(const char *path) {
	return realpath(path, NULL);
}

bool platform_current_dir(char *buf, size_t capacity) {
	return getcwd(buf, capacity) != NULL;
}

bool platform_is_path_separator(char c) {
	return c == '/';
}

const char *platform_last_path_separator(const char *path) {
	return strrchr(path, '/');
}

bool platform_stream_is_tty(FILE *stream) {
	return isatty(fileno(stream)) != 0;
}

void platform_sleep_ms(long ms) {
	struct timespec ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

int platform_process_id() {
	return (int)getpid();
}

int platform_stdout_suppress() {
	int null_fd;
	int saved;

	null_fd = open(NULL_DEVICE, O_WRONLY);
	if (null_fd == -1) {
		return PLATFORM_NO_SAVED_STDOUT;
	}

	saved = dup(STDOUT_FILENO);
	dup2(null_fd, STDOUT_FILENO);
	close(null_fd);
	return saved;
}

void platform_stdout_restore(int saved) {
	if (saved == PLATFORM_NO_SAVED_STDOUT) {
		return;
	}
	dup2(saved, STDOUT_FILENO);
	close(saved);
}

bool platform_stdin_from_null() {
	return freopen(NULL_DEVICE, "r", stdin) != NULL;
}

void *platform_alloc_exec(const uint8_t *code, size_t length) {
	size_t page_size;
	size_t mapped_size;
	void *mem;

	page_size = (size_t)sysconf(_SC_PAGESIZE);
	mapped_size = (length + page_size - 1) / page_size * page_size;

	mem = mmap(NULL, mapped_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) {
		return NULL;
	}

	memcpy(mem, code, length);

	if (mprotect(mem, mapped_size, PROT_READ | PROT_EXEC) != 0) {
		munmap(mem, mapped_size);
		return NULL;
	}

	return mem;
}
