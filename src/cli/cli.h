#pragma once

#include <stdbool.h>
#include <stdint.h>

// The CLI layer: parses argv into concrete values and dispatches into
// exec.h to do the actual work, then reports the result via
// output.h. main() (src/main.c) is just the top-level `argv[1]`
// subcommand switch calling into these.

/* `paweli <file>` (allow_project_fallback=false, called with the
   original, unshifted argc/argv) and `paweli run`/`paweli test`
   (allow_project_fallback=true, force_run_tests selects between them)
   all share this one implementation; see cli.c's own comment on it. */
int cli_run_command(int argc, char **argv, bool allow_project_fallback, bool force_run_tests);

/* `paweli build`. */
int cli_build_command(int argc, char **argv);

/* `paweli init`. */
int cli_init_command(int argc, char **argv);

/* A self-extracting executable's own invocation (./myprogram [flags]),
   dispatched directly from main() once paw_is_self_extracting() has
   confirmed this binary has an embedded program, bypassing every
   other command above entirely. */
int cli_run_embedded_command(const char *exe_path, uint64_t offset, uint64_t size, int argc, char **argv);
