#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cli/cli.h"
#include "cli/output.h"
#include "compiler/paw.h"
#include "platform/exe.h"

int main(int argc, char **argv) {
	char *self_exe_path;
	uint64_t payload_offset;
	uint64_t payload_size;
	int exit_code;

	if (argc >= 2 && strcmp(argv[1], "build") == 0) {
		return cli_build_command(argc - 1, argv + 1);
	}

	if (argc >= 2 && strcmp(argv[1], "init") == 0) {
		return cli_init_command(argc - 1, argv + 1);
	}

	if (argc >= 2 && strcmp(argv[1], "run") == 0) {
		return cli_run_command(argc - 1, argv + 1, true, false);
	}

	if (argc >= 2 && strcmp(argv[1], "test") == 0) {
		return cli_run_command(argc - 1, argv + 1, true, true);
	}

	if (argc >= 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
		print_version();
		return 0;
	}

	// Every plain invocation checks whether its own executable file has
	// a payload appended (see paw_is_self_extracting()) before falling
	// back to treating argv[1] as a .pw/.paw path. This is a few cheap
	// syscalls, and the only way a self-extracting binary built by
	// `paweli build --bin` can tell it's supposed to run its own
	// embedded program instead of expecting a file argument. An
	// ordinary paweli binary (nothing appended) always falls through
	// here: its own trailer check simply never matches.
	self_exe_path = platform_self_exe_path();
	if (self_exe_path != NULL && paw_is_self_extracting(self_exe_path, &payload_offset, &payload_size)) {
		exit_code = cli_run_embedded_command(self_exe_path, payload_offset, payload_size, argc, argv);
		free(self_exe_path);
		return exit_code;
	}
	free(self_exe_path);

	return cli_run_command(argc, argv, false, false);
}
