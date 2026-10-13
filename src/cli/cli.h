#pragma once

#include <stdbool.h>
#include <stdint.h>

// The CLI layer. It parses argv into values, calls exec.h to do the
// work, and reports through output.h. main() only picks the
// subcommand.

/* `paweli <file>`, `paweli run`, and `paweli test`. The bare form
   passes allow_project_fallback false and the original argc/argv. The
   two subcommands pass true, and force_run_tests selects `test`. */
int cli_run_command(int argc, char **argv, bool allow_project_fallback, bool force_run_tests);

/* `paweli build`. */
int cli_build_command(int argc, char **argv);

/* `paweli init`. */
int cli_init_command(int argc, char **argv);

/* Runs the program embedded in a self-extracting executable
   (./myprogram [flags]). main() calls it once
   paw_is_self_extracting() has found a payload, before any other
   command. */
int cli_run_embedded_command(const char *exe_path, uint64_t offset, uint64_t size, int argc, char **argv);
