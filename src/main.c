#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cli/cli.h"
#include "cli/output.h"
#include "compiler/paw.h"
#include "platform/exe.h"

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
	const char *names[2];
	int (*handler)(int argc, char **argv);
} subcommand_t;

static const subcommand_t subcommands[] = {
	{{"build", NULL}, cli_build_command},
	{{"init", NULL}, cli_init_command},
	{{"run", NULL}, run_subcommand},
	{{"test", NULL}, test_subcommand},
	{{"version", "-v"}, version_subcommand},
};

static const subcommand_t *find_subcommand(const char *name) {
	size_t i;

	for (i = 0; i < sizeof(subcommands) / sizeof(subcommands[0]); i++) {
		if (strcmp(name, subcommands[i].names[0]) == 0) {
			return &subcommands[i];
		}
		if (subcommands[i].names[1] != NULL && strcmp(name, subcommands[i].names[1]) == 0) {
			return &subcommands[i];
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
