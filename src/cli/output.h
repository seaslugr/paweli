#pragma once

// The CLI's terminal output: colored status lines and the two banners.
// commands.c and exec.c both report through these.

/* "ERROR: <message>" in bold red, to stderr. For a hard failure: bad
   arguments, a file that cannot be opened, a failed compile. */
void print_error(const char *format, ...);

/* "WARNING: <message>" in bold yellow, to stderr. For a problem that
   does not stop the command, such as a flag with no effect here. */
void print_warning(const char *format, ...);

/* "paweli: <message>" in bold green, to stdout. For a command that
   finished and produced something, such as a build's output file. */
void print_success(const char *format, ...);

/* "paweli 0.1.0 (Debug)" to stdout. */
void print_version();

/* The banner for a bare `paweli` with no file and no subcommand. */
void print_usage();
