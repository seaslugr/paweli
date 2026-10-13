#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cli/cli.h"
#include "cli/output.h"
#include "paw/paw.h"
#include "platform/platform.h"

#define SUBCOMMAND_MAX_NAMES 3

static int run_subcommand(int argc, char **argv) {
	return cli_run_command(argc, argv, true, false);
}

static int test_subcommand(int argc, char **argv) {
	return cli_run_command(argc, argv, true, true);
}

static int version_subcommand(int argc, char **argv) {
	(void)argc;
	(void)argv;
	print_version();
	return 0;
}

typedef struct {
	const char *names[SUBCOMMAND_MAX_NAMES];
	int (*handler)(int argc, char **argv);
} subcommand_t;

static const subcommand_t subcommands[] = {
	{{"build"}, cli_build_command},
	{{"init"}, cli_init_command},
	{{"run"}, run_subcommand},
	{{"test"}, test_subcommand},
	{{"version", "--version", "-v"}, version_subcommand},
};

static const subcommand_t *find_subcommand(const char *name) {
	for (size_t i = 0; i < sizeof(subcommands) / sizeof(subcommands[0]); i++) {
		for (size_t j = 0; j < SUBCOMMAND_MAX_NAMES && subcommands[i].names[j] != NULL; j++) {
			if (strcmp(name, subcommands[i].names[j]) == 0) {
				return &subcommands[i];
			}
		}
	}
	return NULL;
}

int main(int argc, char **argv) {
	char *self_exe_path;
	uint64_t payload_offset;
	uint64_t payload_size;
	int exit_code;
	const subcommand_t *subcommand;

	if (argc >= 2) {
		subcommand = find_subcommand(argv[1]);
		if (subcommand != NULL) {
			return subcommand->handler(argc - 1, argv + 1);
		}
	}

	self_exe_path = platform_self_exe_path();
	if (self_exe_path != NULL && paw_is_self_extracting(self_exe_path, &payload_offset, &payload_size)) {
		exit_code = cli_run_embedded_command(self_exe_path, payload_offset, payload_size, argc, argv);
		free(self_exe_path);
		return exit_code;
	}
	free(self_exe_path);

	return cli_run_command(argc, argv, false, false);
}
