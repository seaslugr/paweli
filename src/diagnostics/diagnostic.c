/* fileno() and isatty() are POSIX, not ISO C11 (this project builds with
   CMAKE_C_EXTENSIONS OFF, i.e. strict -std=c11), so stdio.h/unistd.h don't
   declare them without an explicit feature-test macro. */
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "diagnostic.h"

#define COLOR_RESET "\x1b[0m"
#define COLOR_BOLD "\x1b[1m"
#define COLOR_RED "\x1b[1;31m"
#define COLOR_YELLOW "\x1b[1;33m"
#define COLOR_CYAN "\x1b[36m"
#define COLOR_GREEN "\x1b[1;32m"

static bool color_enabled(void) {
	static int cached = -1;

	if (cached == -1) {
		cached = (getenv("NO_COLOR") == NULL) && isatty(fileno(stderr)) ? 1 : 0;
	}
	return cached != 0;
}

static const char *severity_name(diag_severity_t severity) {
	switch (severity) {
		case DIAG_ERROR: return "error";
		case DIAG_WARNING: return "warning";
	}
	return "error";
}

static const char *severity_color(diag_severity_t severity) {
	switch (severity) {
		case DIAG_ERROR: return COLOR_RED;
		case DIAG_WARNING: return COLOR_YELLOW;
	}
	return COLOR_RED;
}

/* Finds the 1-indexed `line`'s text within `source` (the whole file's
   null-terminated text). out_start and out_length describe that line's
   text, not including its trailing newline. Returns false if `line` doesn't
   exist in source (out of range, or source is NULL). */
static bool find_source_line(const char *source, int line, const char **out_start, size_t *out_length) {
	int current_line;
	const char *p;
	const char *line_start;

	if (source == NULL || line < 1) {
		return false;
	}

	current_line = 1;
	line_start = source;
	for (p = source; *p != '\0' && current_line < line; p++) {
		if (*p == '\n') {
			current_line++;
			line_start = p + 1;
		}
	}

	if (current_line != line) {
		return false;
	}

	for (p = line_start; *p != '\0' && *p != '\n'; p++) {
	}

	*out_start = line_start;
	*out_length = (size_t)(p - line_start);
	return true;
}

/* Diagnostics anchored at end-of-file land one line past the final
   newline, at column 1 (the lexer just keeps counting lines as it skips
   trailing whitespace). That "line" is a phantom: it has no content, so
   rendering it as a snippet just shows an empty line with a floating
   caret and no context. This walks back to the nearest real line (one
   that has content, or at least isn't sitting right at the end of the
   buffer with nothing after it) and repoints line/column just past its
   last character instead. A genuinely blank line the user wrote is left
   alone: it's followed by more source rather than sitting at the very
   end of the buffer, so it doesn't match the "phantom" check below. */
static void clamp_to_source(const char *source, int *line, int *column) {
	const char *line_start;
	size_t line_length;

	if (!find_source_line(source, *line, &line_start, &line_length)) {
		return;
	}

	if (line_length != 0 || *line_start != '\0') {
		return;
	}

	while (*line > 1) {
		*line -= 1;
		if (!find_source_line(source, *line, &line_start, &line_length)) {
			return;
		}
		if (line_length > 0) {
			*column = (int)line_length + 1;
			return;
		}
	}
}

/* Prints the offending source line and, when column is known, a caret
   line under it. Tabs in the source line are reproduced (not replaced by
   spaces) in the padding before the caret, so the caret lines up
   correctly whatever width the terminal expands tabs to. */
static void print_snippet(const char *source, int line, int column, bool color) {
	const char *line_start;
	size_t line_length;
	char line_number[16];
	size_t i;
	size_t caret_index;

	if (!find_source_line(source, line, &line_start, &line_length)) {
		return;
	}

	snprintf(line_number, sizeof(line_number), "%d", line);
	fprintf(stderr, " %s | %.*s\n", line_number, (int)line_length, line_start);

	if (column <= 0) {
		return;
	}

	fprintf(stderr, " %*s | ", (int)strlen(line_number), "");

	caret_index = (size_t)(column - 1);
	for (i = 0; i < caret_index && i < line_length; i++) {
		fputc(line_start[i] == '\t' ? '\t' : ' ', stderr);
	}

	if (color) {
		fprintf(stderr, "%s^%s\n", COLOR_GREEN, COLOR_RESET);
	} else {
		fputs("^\n", stderr);
	}
}

void diagnostic_emit(diag_severity_t severity, const char *code, const char *filename, const char *source, int line, int column, const char *format, ...) {
	va_list args;
	bool color;
	const char *reset;
	const char *bold;
	const char *cyan;

	color = color_enabled();
	reset = color ? COLOR_RESET : "";
	bold = color ? COLOR_BOLD : "";
	cyan = color ? COLOR_CYAN : "";

	if (column > 0) {
		clamp_to_source(source, &line, &column);
	}

	fprintf(stderr, "%s[%s]%s ", cyan, code, reset);

	if (column > 0) {
		fprintf(stderr, "%s%s:%d:%d:%s ", bold, filename, line, column, reset);
	} else {
		fprintf(stderr, "%s%s:%d:%s ", bold, filename, line, reset);
	}

	fprintf(stderr, "%s%s:%s ", color ? severity_color(severity) : "", severity_name(severity), reset);

	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	fputs("\n", stderr);

	print_snippet(source, line, column, color);
}
