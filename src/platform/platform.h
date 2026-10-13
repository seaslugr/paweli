#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Every OS-specific call in paweli goes through this header. posix.c
// implements it for Linux, windows.c for Windows. CMake compiles
// exactly one of them.

#define PLATFORM_NO_SAVED_STDOUT (-1)

/* Returns a malloc'd absolute path to the running executable, or NULL
   on failure. Caller owns the result. */
char *platform_self_exe_path();

/* Marks path executable. A no-op that returns true on Windows. */
bool platform_make_executable(const char *path);

/* The suffix an executable file needs: "" or ".exe". */
const char *platform_exe_suffix();

/* Creates one directory. Returns true if it was created or already
   exists. On failure errno holds the reason. */
bool platform_make_dir(const char *path);

/* Returns a malloc'd canonical absolute path for an existing file, or
   NULL if the file does not exist. Two spellings of the same file
   give the same string. Caller owns the result. */
char *platform_canonical_path(const char *path);

bool platform_current_dir(char *buf, size_t capacity);

bool platform_is_path_separator(char c);

/* Returns a pointer to the last path separator in path, or NULL if
   there is none. */
const char *platform_last_path_separator(const char *path);

/* True if stream is a terminal that accepts ANSI escape sequences. */
bool platform_stream_is_tty(FILE *stream);

void platform_sleep_ms(long ms);

int platform_process_id();

/* Points the real stdout at the null device. Returns a handle for
   platform_stdout_restore(), or PLATFORM_NO_SAVED_STDOUT on failure. */
int platform_stdout_suppress();
void platform_stdout_restore(int saved);

bool platform_stdin_from_null();

/* Copies length bytes of machine code into fresh page-aligned memory
   and makes it executable. The memory is writable while it is filled
   and read+execute after, never both at once. Returns NULL on
   failure. The mapping is never freed. */
void *platform_alloc_exec(const uint8_t *code, size_t length);
