#pragma once

// The CLI's terminal-facing presentation: colored status lines and the
// two banners (--version, no-arguments usage). No argv parsing and no
// program-execution logic lives here; see cli.h and exec.h for those.
// Shared by both cli.c/commands.c (which call these directly) and
// exec.c (which reports its own compile/run failures the same way,
// exactly as this codebase's other layers already report through a
// shared, layer-crossing reporter; see diagnostic.h's own doc comment
// for the precedent this follows).

/* "ERROR: <message>" in bold red, to stderr. For hard failures: bad
   arguments, a file that can't be opened, a compile that didn't
   succeed. */
void print_error(const char *format, ...);

/* "WARNING: <message>" in bold yellow, to stderr. For a problem that
   doesn't stop the command, e.g. a flag that has no effect in this
   context. */
void print_warning(const char *format, ...);

/* "paweli: <message>" in bold green, to stdout. For a command that
   completed and produced something worth naming, e.g. build's output
   file. */
void print_success(const char *format, ...);

/* "paweli 0.1.0 (Debug)" to stdout. Used by `paweli --version`/`-v`. */
void print_version(void);

/* Shown on a bare `paweli` invocation (no file, no subcommand): a
   git/cargo-style banner instead of a single usage line. */
void print_usage(void);
