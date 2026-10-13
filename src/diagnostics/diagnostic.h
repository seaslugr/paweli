#pragma once

typedef enum {
	DIAG_ERROR,
	DIAG_WARNING,
} diag_severity_t;

/* Reports one diagnostic. The lexer, parser, compiler, and VM all use
   it, so every stage prints the same format:

     [code] file:line:col: severity: message
      line | source text
           |        ^^^^^^^

   column <= 0 means no column is known. VM runtime errors only have a
   line. The column and the caret line are then left out.

   length is the number of source characters the caret underlines,
   starting at column. <= 1 draws one '^'. A length that runs past the
   end of the line is clamped.

   source is the whole file as null-terminated text. Pass NULL to skip
   the snippet.

   Output is colored when stderr is a terminal and NO_COLOR is unset. */
void diagnostic_emit(diag_severity_t severity, const char *code, const char *filename, const char *source, int line, int column, int length, const char *format, ...);
