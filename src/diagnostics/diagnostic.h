#pragma once

typedef enum {
	DIAG_ERROR,
	DIAG_WARNING,
} diag_severity_t;

/* Shared reporter used by the lexer, parser, compiler, and VM so every
   stage prints diagnostics in one format:

       [code] file:line:col: severity: message
         line | source text
              |      ^

   column <= 0 means "no column available" (e.g. VM runtime errors, which
   only have a line number from the bytecode's line table); that stage and
   the caret line are then omitted: "[code] file:line: severity: message".

   source is the whole file's null-terminated text, used to render the
   snippet; pass NULL to skip the snippet entirely (no source available,
   e.g. an error found before the file was read).

   Output is colored (bold severity, cyan code, green caret) when stderr
   is a terminal and the NO_COLOR environment variable is unset; otherwise
   it prints as plain text. */
void diagnostic_emit(diag_severity_t severity, const char *code, const char *filename, const char *source, int line, int column, const char *format, ...);
