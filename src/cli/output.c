#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "output.h"
#include "project/manifest.h"
#include "runtime/vm.h"
#include "version.h"

static bool color_enabled(FILE *stream);
static void vprint_message(FILE *stream, const char *color, const char *prefix, const char *format, va_list args);

static bool color_enabled(FILE *stream) {
	return (getenv("NO_COLOR") == NULL) && isatty(fileno(stream));
}

/* Shared by print_error/print_warning/print_success: writes `prefix`
   in `color` (already bold, e.g. "\x1b[1;31m") followed by the
   formatted message and a newline, to `stream`. */
static void vprint_message(FILE *stream, const char *color, const char *prefix, const char *format, va_list args) {
	bool color_on;

	color_on = color_enabled(stream);
	fprintf(stream, "%s%s%s ", color_on ? color : "", prefix, color_on ? "\x1b[0m" : "");
	vfprintf(stream, format, args);
	fputc('\n', stream);
}

void print_error(const char *format, ...) {
	va_list args;

	va_start(args, format);
	vprint_message(stderr, "\x1b[1;31m", "ERROR:", format, args);
	va_end(args);
}

void print_warning(const char *format, ...) {
	va_list args;

	va_start(args, format);
	vprint_message(stderr, "\x1b[1;33m", "WARNING:", format, args);
	va_end(args);
}

void print_success(const char *format, ...) {
	va_list args;

	va_start(args, format);
	vprint_message(stdout, "\x1b[1;32m", "paweli:", format, args);
	va_end(args);
}

/* "paweli 0.1.0 (Debug)": bold+lime name, plain version, dim build type
   in parens. Colored the same way diagnostic_emit() decides color for
   stderr (NO_COLOR unset and the stream is a terminal), just checked
   against stdout instead, since --version prints there. */
void print_version(void) {
	bool color;
	const char *reset;
	const char *bold;
	const char *lime;
	const char *dim;

	color = color_enabled(stdout);
	reset = color ? "\x1b[0m" : "";
	bold = color ? "\x1b[1m" : "";
	lime = color ? "\x1b[38;5;149m" : "";
	dim = color ? "\x1b[2m" : "";

	printf("%s%spaweli%s %s %s(%s)%s\n", bold, lime, reset, PAWELI_VERSION_STRING, dim, PAWELI_BUILD_TYPE, reset);
}

/* Shown on a bare `paweli` invocation (no file, no subcommand): a
   git/cargo-style banner instead of a single usage line, since at this
   point the user has nothing else to go on. Colored the same way
   diagnostic_emit() decides color for stderr. */
void print_usage(void) {
	bool color;
	const char *reset;
	const char *bold;
	const char *lime;
	const char *gray;

	color = color_enabled(stderr);
	reset = color ? "\x1b[0m" : "";
	bold = color ? "\x1b[1m" : "";
	lime = color ? "\x1b[38;5;149m" : "";
	gray = color ? "\x1b[38;5;245m" : "";

	fprintf(stderr, "%s%spaweli%s %s\n", bold, lime, reset, PAWELI_VERSION_STRING);
	fprintf(stderr, "a small, statically-typed, C-like programming language\n");

	fprintf(stderr, "\n%susage:%s\n", bold, reset);
	fprintf(stderr, "    paweli [options] <file.pw|file.paw>\n");
	fprintf(stderr, "    paweli init [name]\n");
	fprintf(stderr, "    paweli run [file.pw] [options]\n");
	fprintf(stderr, "    paweli test [file.pw] [options]\n");
	fprintf(stderr, "    paweli build <file.pw> [output.paw] [options]\n");
	fprintf(stderr, "    paweli build <file.pw> --bin <name> [options]\n");
	fprintf(stderr, "    paweli build [options]  (reads %s in the current directory)\n", PROJECT_MANIFEST_FILENAME);
	fprintf(stderr, "    paweli --version\n");

	fprintf(stderr, "\n%sexamples:%s\n", bold, reset);
	fprintf(stderr, "    paweli hello.pw\n");
	fprintf(stderr, "    paweli build hello.pw --bin hello && ./hello\n");
	fprintf(stderr, "    mkdir myproject && cd myproject && paweli init && paweli run\n");

	fprintf(stderr, "\n%soptions:%s\n", bold, reset);
	fprintf(stderr, "    %s--dump-ast%s        print the parsed AST before compiling\n", bold, reset);
	fprintf(stderr, "    %s--dump-bytecode%s   print disassembled bytecode before running\n", bold, reset);
	fprintf(stderr, "    %s--jit-stats%s       print each function's JIT state after running\n", bold, reset);
	fprintf(stderr, "    %s--stack-size N%s    set the VM's value stack size (default %d)\n", bold, reset, VM_DEFAULT_STACK_SIZE);
	fprintf(stderr, "    %s--max-frames N%s    set the max call-frame depth (default %d)\n", bold, reset, VM_DEFAULT_MAX_FRAMES);

	fprintf(stderr, "\n%ssee book/src/cli/index.md or paweli(1) for the full reference.%s\n", gray, reset);
}
