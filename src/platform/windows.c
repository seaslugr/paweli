#define WIN32_LEAN_AND_MEAN

#include <direct.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "platform.h"

#define PATH_CAPACITY 4096
#define NULL_DEVICE "NUL"
// GetFinalPathNameByHandleA() answers with this prefix. Diagnostics
// print the canonical path, so the prefix is stripped.
#define EXTENDED_PATH_PREFIX "\\\\?\\"
#define EXTENDED_PATH_PREFIX_LENGTH 4
#define STDOUT_FD 1

static char *dup_path(const char *path, size_t length);

static char *dup_path(const char *path, size_t length) {
	char *out;

	out = malloc(length + 1);
	memcpy(out, path, length);
	out[length] = '\0';
	return out;
}

char *platform_self_exe_path() {
	char buf[PATH_CAPACITY];
	DWORD length;

	length = GetModuleFileNameA(NULL, buf, sizeof(buf));
	if (length == 0 || length >= sizeof(buf)) {
		return NULL;
	}

	return dup_path(buf, length);
}

bool platform_make_executable(const char *path) {
	(void)path;
	return true;
}

const char *platform_exe_suffix() {
	return ".exe";
}

bool platform_make_dir(const char *path) {
	return _mkdir(path) == 0 || errno == EEXIST;
}

char *platform_canonical_path(const char *path) {
	char buf[PATH_CAPACITY];
	HANDLE file;
	DWORD length;
	const char *start;

	file = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		return NULL;
	}

	length = GetFinalPathNameByHandleA(file, buf, sizeof(buf), FILE_NAME_NORMALIZED);
	CloseHandle(file);
	if (length == 0 || length >= sizeof(buf)) {
		return NULL;
	}

	start = buf;
	if (length > EXTENDED_PATH_PREFIX_LENGTH && strncmp(buf, EXTENDED_PATH_PREFIX, EXTENDED_PATH_PREFIX_LENGTH) == 0) {
		start += EXTENDED_PATH_PREFIX_LENGTH;
		length -= EXTENDED_PATH_PREFIX_LENGTH;
	}

	return dup_path(start, length);
}

bool platform_current_dir(char *buf, size_t capacity) {
	return _getcwd(buf, (int)capacity) != NULL;
}

bool platform_is_path_separator(char c) {
	return c == '/' || c == '\\';
}

const char *platform_last_path_separator(const char *path) {
	const char *last;

	last = NULL;
	for (const char *p = path; *p != '\0'; p++) {
		if (platform_is_path_separator(*p)) {
			last = p;
		}
	}
	return last;
}

/* A Windows console only reads ANSI escapes once virtual terminal
   processing is on. This turns it on. A console that refuses is
   reported as not a terminal, so callers fall back to plain output. */
bool platform_stream_is_tty(FILE *stream) {
	HANDLE handle;
	DWORD mode;
	int fd;

	fd = _fileno(stream);
	if (_isatty(fd) == 0) {
		return false;
	}

	handle = (HANDLE)_get_osfhandle(fd);
	if (handle == INVALID_HANDLE_VALUE || !GetConsoleMode(handle, &mode)) {
		return false;
	}
	if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
		return true;
	}

	return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

void platform_sleep_ms(long ms) {
	Sleep((DWORD)ms);
}

int platform_process_id() {
	return (int)GetCurrentProcessId();
}

int platform_stdout_suppress() {
	int null_fd;
	int saved;

	null_fd = _open(NULL_DEVICE, _O_WRONLY);
	if (null_fd == -1) {
		return PLATFORM_NO_SAVED_STDOUT;
	}

	saved = _dup(STDOUT_FD);
	_dup2(null_fd, STDOUT_FD);
	_close(null_fd);
	return saved;
}

void platform_stdout_restore(int saved) {
	if (saved == PLATFORM_NO_SAVED_STDOUT) {
		return;
	}
	_dup2(saved, STDOUT_FD);
	_close(saved);
}

bool platform_stdin_from_null() {
	return freopen(NULL_DEVICE, "r", stdin) != NULL;
}

void *platform_alloc_exec(const uint8_t *code, size_t length) {
	void *mem;
	DWORD old_protect;

	mem = VirtualAlloc(NULL, length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (mem == NULL) {
		return NULL;
	}

	memcpy(mem, code, length);

	if (!VirtualProtect(mem, length, PAGE_EXECUTE_READ, &old_protect)) {
		VirtualFree(mem, 0, MEM_RELEASE);
		return NULL;
	}

	FlushInstructionCache(GetCurrentProcess(), mem, length);
	return mem;
}
