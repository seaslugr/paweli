#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cli.h"
#include "compiler/compiler.h"
#include "compiler/paw.h"
#include "exec.h"
#include "output.h"
#include "platform/exe.h"
#include "project/manifest.h"
#include "runtime/module.h"
#include "runtime/object.h"
#include "runtime/table.h"
#include "runtime/vm.h"

// Long-option codes for the two getopt_long() tables below. Values
// start past the ASCII range so they never collide with a short
// option (neither table below defines any, but getopt_long still
// wants distinct case labels).
typedef enum {
	OPT_DUMP_AST = 1000,
	OPT_DUMP_BYTECODE,
	OPT_JIT_STATS,
	OPT_RUN_TESTS,
	OPT_STACK_SIZE,
	OPT_MAX_FRAMES,
	OPT_BIN,
	OPT_STRIP_SOURCE,
} cli_long_opt_t;

// Shared by cli_run_command() and cli_run_embedded_command(): both
// accept the same run-time flags, they just react to --dump-ast and
// --run-tests differently (see each function).
static const struct option run_long_opts[] = {
	{"dump-ast", no_argument, NULL, OPT_DUMP_AST},
	{"dump-bytecode", no_argument, NULL, OPT_DUMP_BYTECODE},
	{"jit-stats", no_argument, NULL, OPT_JIT_STATS},
	{"run-tests", no_argument, NULL, OPT_RUN_TESTS},
	{"stack-size", required_argument, NULL, OPT_STACK_SIZE},
	{"max-frames", required_argument, NULL, OPT_MAX_FRAMES},
	{0, 0, 0, 0},
};

static const struct option build_long_opts[] = {
	{"bin", required_argument, NULL, OPT_BIN},
	{"strip-source", no_argument, NULL, OPT_STRIP_SOURCE},
	{"run-tests", no_argument, NULL, OPT_RUN_TESTS},
	{0, 0, 0, 0},
};

static bool parse_long_flag_value(const char *flag_name, const char *text, long min_value, long *out_value);
static char *default_paw_output_path(const char *input_path);
static bool ensure_project_build_dir(void);
static char *project_build_output_path(const char *name, const char *suffix);

static bool parse_long_flag_value(const char *flag_name, const char *text, long min_value, long *out_value) {
	long value = strtol(text, NULL, 10);

	if (value < min_value) {
		print_error("%s must be at least %ld", flag_name, min_value);
		return false;
	}

	*out_value = value;
	return true;
}

static char *default_paw_output_path(const char *input_path) {
	size_t length;
	size_t stem_length;
	char *out;

	length = strlen(input_path);
	stem_length = length;
	if (length >= 3 && strcmp(input_path + length - 3, ".pw") == 0) {
		stem_length = length - 3;
	}

	out = malloc(stem_length + 5);
	memcpy(out, input_path, stem_length);
	memcpy(out + stem_length, ".paw", 5);

	return out;
}

/* Creates the "build" directory a project-mode build() writes into,
   tolerating it already existing. Only ever called from
   cli_build_command() when reading its input from a manifest (see
   PROJECT_MANIFEST_FILENAME); a filename-argument build never touches
   this. */
static bool ensure_project_build_dir(void) {
	if (mkdir("build", 0755) == 0 || errno == EEXIST) {
		return true;
	}
	print_error("could not create 'build' directory: %s", strerror(errno));
	return false;
}

static char *project_build_output_path(const char *name, const char *suffix) {
	size_t length;
	char *out;

	length = strlen("build/") + strlen(name) + strlen(suffix) + 1;
	out = malloc(length);
	snprintf(out, length, "build/%s%s", name, suffix);
	return out;
}

/* Flag parsing for a self-extracting executable's own invocation
   (./myprogram [flags]): the same run flags cli_run_command() accepts,
   minus --dump-ast/test mode (which apply only to compiling from
   source, and to which program's tests run; both are already fixed by
   the time this executable was built) and minus a file argument
   (there is nothing to name; the program is this executable). Either
   of the first two is accepted anyway, with a warning, matching how
   cli_run_command() already treats them for a plain .paw file; anything
   else non-flag is a hard error, since there is no positional
   argument this command line could sensibly mean. */
int cli_run_embedded_command(const char *exe_path, uint64_t offset, uint64_t size, int argc, char **argv) {
	bool dump_bytecode = false;
	bool jit_stats = false;
	long stack_size = VM_DEFAULT_STACK_SIZE;
	long max_frames = VM_DEFAULT_MAX_FRAMES;
	int opt;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "", run_long_opts, NULL)) != -1) {
		switch (opt) {
		case OPT_DUMP_AST:
			print_warning("--dump-ast has no effect on this program (there is no source to parse)");
			break;
		case OPT_DUMP_BYTECODE:
			dump_bytecode = true;
			break;
		case OPT_JIT_STATS:
			jit_stats = true;
			break;
		case OPT_RUN_TESTS:
			print_warning("--run-tests has no effect on this program (its run mode was fixed when it was built)");
			break;
		case OPT_STACK_SIZE:
			if (!parse_long_flag_value("--stack-size", optarg, VM_MIN_STACK_SIZE, &stack_size)) {
				return 1;
			}
			break;
		case OPT_MAX_FRAMES:
			if (!parse_long_flag_value("--max-frames", optarg, 1, &max_frames)) {
				return 1;
			}
			break;
		default:
			return 1;
		}
	}

	if (optind < argc) {
		print_error("unexpected argument '%s'", argv[optind]);
		return 1;
	}

	return exec_run_embedded(exe_path, offset, size, dump_bytecode, jit_stats, stack_size, max_frames);
}

/* Shared by three entry points: the bare `paweli <file>` fallback
   (allow_project_fallback=false, force_run_tests=false, called with
   the original unshifted argc/argv, same as always), `paweli run`
   (allow_project_fallback=true, force_run_tests=false), and
   `paweli test` (allow_project_fallback=true, force_run_tests=true;
   its whole difference from `run` is this one bool, both otherwise
   identical). `--run-tests` is no longer a recognized flag anywhere:
   it always errors, directing to `paweli test`, the flag's full
   replacement for every case a run-time flag could express (baking
   test mode into a *compiled* artifact is a different, still-needed
   capability and stays exactly as `paweli build --run-tests` today). */
int cli_run_command(int argc, char **argv, bool allow_project_fallback, bool force_run_tests) {
	bool dump_ast = false;
	bool dump_bytecode = false;
	bool jit_stats = false;
	bool run_tests = force_run_tests;
	const char *path = NULL;
	int i;
	long stack_size = VM_DEFAULT_STACK_SIZE;
	long max_frames = VM_DEFAULT_MAX_FRAMES;
	char *source;
	bool from_manifest = false;
	project_manifest_t manifest;
	int result;
	int opt;

	optind = 1;
	while ((opt = getopt_long(argc, argv, "", run_long_opts, NULL)) != -1) {
		switch (opt) {
		case OPT_DUMP_AST:
			dump_ast = true;
			break;
		case OPT_DUMP_BYTECODE:
			dump_bytecode = true;
			break;
		case OPT_JIT_STATS:
			jit_stats = true;
			break;
		case OPT_RUN_TESTS:
			print_error("--run-tests was replaced by the 'paweli test' subcommand");
			return 1;
		case OPT_STACK_SIZE:
			if (!parse_long_flag_value("--stack-size", optarg, VM_MIN_STACK_SIZE, &stack_size)) {
				return 1;
			}
			break;
		case OPT_MAX_FRAMES:
			if (!parse_long_flag_value("--max-frames", optarg, 1, &max_frames)) {
				return 1;
			}
			break;
		default:
			return 1;
		}
	}

	// Same last-one-wins behavior a strcmp loop taking `path = argv[i]`
	// on every non-flag argument had: `paweli run a.pw b.pw` silently
	// runs b.pw. Not this refactor's problem to fix.
	for (i = optind; i < argc; i++) {
		path = argv[i];
	}

	if (path == NULL) {
		if (allow_project_fallback) {
			if (!project_manifest_exists(PROJECT_MANIFEST_FILENAME)) {
				fprintf(stderr, "usage: paweli %s <file.pw> [options]\n       paweli %s [options]  (reads %s in the current directory)\n", force_run_tests ? "test" : "run", force_run_tests ? "test" : "run", PROJECT_MANIFEST_FILENAME);
				return 1;
			}
			if (!project_manifest_read(PROJECT_MANIFEST_FILENAME, &manifest)) {
				return 1;
			}
			from_manifest = true;
			path = manifest.entry;
		} else if (argc == 1) {
			print_usage();
			return 1;
		} else {
			print_error("No file given");
			return 1;
		}
	}

	if (paw_is_paw_file(path)) {
		if (dump_ast) {
			print_warning("--dump-ast has no effect on a compiled .paw file (there is no source to parse)");
		}
		if (run_tests) {
			print_warning("'paweli test' has no effect on a compiled .paw file (its run mode was fixed by 'paweli build')");
		}
		result = exec_run_paw(path, dump_bytecode, jit_stats, stack_size, max_frames);
		if (from_manifest) {
			project_manifest_free(&manifest);
		}
		return result;
	}

	source = exec_read_file(path);
	if (source == NULL) {
		if (from_manifest) {
			project_manifest_free(&manifest);
		}
		return 1;
	}

	result = exec_run_source(path, source, dump_ast, dump_bytecode, jit_stats, run_tests, stack_size, max_frames);
	if (from_manifest) {
		project_manifest_free(&manifest);
	}
	return result;
}

int cli_build_command(int argc, char **argv) {
	const char *input_path;
	const char *output_path;
	char *owned_output_path;
	bool want_exe;
	bool strip_source;
	bool run_tests;
	int i;
	int positional_count;
	const char *positional[2];
	char *source;
	gc_heap_t heap;
	table_t strings;
	vm_t vm;
	module_resolver_t resolver;
	table_t *entry_globals;
	const module_unit_t **entry_imports;
	size_t entry_import_count;
	obj_function_t *script;
	paw_module_info_t *module_infos;
	paw_import_edge_t *edges;
	size_t edge_count;
	size_t edge_capacity;
	char *self_exe_path;
	bool ok;
	size_t module_index;
	size_t j;
	bool from_manifest;
	project_manifest_t manifest;

	output_path = NULL;
	owned_output_path = NULL;
	want_exe = false;
	strip_source = false;
	run_tests = false;
	positional_count = 0;
	positional[0] = NULL;
	positional[1] = NULL;
	from_manifest = false;

	{
		int opt;

		optind = 1;
		while ((opt = getopt_long(argc, argv, "", build_long_opts, NULL)) != -1) {
			switch (opt) {
			case OPT_BIN:
				want_exe = true;
				output_path = optarg;
				break;
			case OPT_STRIP_SOURCE:
				strip_source = true;
				break;
			case OPT_RUN_TESTS:
				run_tests = true;
				break;
			default:
				return 1;
			}
		}

		for (i = optind; i < argc; i++) {
			if (positional_count >= 2) {
				print_error("unexpected argument '%s'", argv[i]);
				return 1;
			}
			positional[positional_count++] = argv[i];
		}
	}

	input_path = positional[0];
	if (input_path == NULL) {
		// No filename argument: fall back to project mode, reading the
		// entry point from PROJECT_MANIFEST_FILENAME in the current
		// directory instead. project_manifest_exists() (silent) is
		// checked first so "no file arg, no project here either" still
		// prints the plain usage message below, rather than a confusing
		// "could not open project.pawproj" for what's just an ordinary
		// missing-argument mistake; project_manifest_read() prints its
		// own diagnostic if the manifest exists but is actually broken.
		if (!project_manifest_exists(PROJECT_MANIFEST_FILENAME)) {
			fprintf(stderr, "usage: paweli build <file.pw> [output.paw] [--strip-source] [--run-tests]\n       paweli build <file.pw> --bin <name> [--strip-source] [--run-tests]\n       paweli build [--bin <name>] [--strip-source] [--run-tests]  (reads " PROJECT_MANIFEST_FILENAME " in the current directory)\n");
			return 1;
		}
		if (!project_manifest_read(PROJECT_MANIFEST_FILENAME, &manifest)) {
			return 1;
		}
		from_manifest = true;
		input_path = manifest.entry;
	}

	if (want_exe) {
		if (positional_count > 1) {
			print_error("unexpected argument '%s' (the executable's name comes from --bin, not a second argument)", positional[1]);
			if (from_manifest) {
				project_manifest_free(&manifest);
			}
			return 1;
		}
	} else if (positional_count > 1) {
		output_path = positional[1];
	}

	// Project-mode output always lands under build/, named after the
	// manifest (or, for --bin, the name given to it), instead of next to
	// the source the way a filename-argument build always has. A
	// filename-argument build's output location is completely
	// unaffected: this only ever runs when from_manifest is true.
	if (from_manifest) {
		if (!ensure_project_build_dir()) {
			project_manifest_free(&manifest);
			return 1;
		}
		owned_output_path = want_exe ? project_build_output_path(output_path, "") : project_build_output_path(manifest.name, ".paw");
		output_path = owned_output_path;
	} else if (!want_exe && output_path == NULL) {
		owned_output_path = default_paw_output_path(input_path);
		output_path = owned_output_path;
	}

	source = exec_read_file(input_path);
	if (source == NULL) {
		free(owned_output_path);
		if (from_manifest) {
			project_manifest_free(&manifest);
		}
		return 1;
	}

	gc_heap_init(&heap);
	table_init(&strings);
	vm_init(&vm, &heap, &strings, input_path, source, VM_DEFAULT_STACK_SIZE, VM_DEFAULT_MAX_FRAMES);
	module_resolver_init(&resolver);

	script = exec_compile_from_source(input_path, source, &heap, &strings, &vm, &resolver, &entry_globals, &entry_imports, &entry_import_count, false, run_tests);

	ok = script != NULL;
	if (ok) {
		// resolver.compiled_units is every module the program
		// transitively depends on, already deduplicated (a diamond
		// import appears once) and in dependency order; vm.owned_sources
		// holds exactly the matching filename/source for each one, in
		// the same order, since module_compile() calls
		// vm_track_owned_source() exactly once per canonical module,
		// right where it also adds that module to the resolver (see
		// module.c), and never for an alias.
		module_infos = resolver.compiled_count > 0 ? malloc(resolver.compiled_count * sizeof(paw_module_info_t)) : NULL;
		for (module_index = 0; module_index < resolver.compiled_count; module_index++) {
			module_infos[module_index].module_obj = resolver.compiled_units[module_index]->module_obj;
			module_infos[module_index].script = resolver.compiled_units[module_index]->script;
			module_infos[module_index].filename = vm.owned_sources[module_index].path;
			module_infos[module_index].source = vm.owned_sources[module_index].source;
		}

		// The whole #module import graph, as a flat edge list: the entry
		// file's own direct imports (importer_index 0), then every
		// module's own imports (importer_index = 1 + its own position in
		// resolver.compiled_units). module_unit_canonical() normalizes an
		// edge target that happens to be an alias (an importer used a
		// different name/path string than whichever importer first
		// resolved that file) back to the one canonical unit that
		// actually has a slot in module_infos.
		edges = NULL;
		edge_count = 0;
		edge_capacity = 0;
		for (module_index = 0; module_index < entry_import_count; module_index++) {
			const module_unit_t *canonical_target = module_unit_canonical(entry_imports[module_index]);
			exec_push_edge(&edges, &edge_count, &edge_capacity, 0, exec_find_module_index(&resolver, canonical_target), entry_imports[module_index]->local_name, entry_imports[module_index]->local_name_length);
		}
		for (module_index = 0; module_index < resolver.compiled_count; module_index++) {
			const module_unit_t *importer = resolver.compiled_units[module_index];

			for (j = 0; j < importer->own_import_count; j++) {
				exec_push_edge(&edges, &edge_count, &edge_capacity, module_index + 1, exec_find_module_index(&resolver, importer->own_imports[j].target), importer->own_imports[j].local_name, importer->own_imports[j].local_name_length);
			}
		}

		if (want_exe) {
			self_exe_path = platform_self_exe_path();
			if (self_exe_path == NULL) {
				print_error("could not determine this build's own executable path; --bin needs to copy it as the new executable's runtime stub, and that isn't supported on this platform yet");
				ok = false;
			} else {
				ok = paw_build_executable(output_path, self_exe_path, script, input_path, source, module_infos, resolver.compiled_count, edges, edge_count, strip_source);
				free(self_exe_path);
				if (ok) {
					print_success("wrote executable '%s'", output_path);
				}
			}
		} else {
			ok = paw_write_file(output_path, script, input_path, source, module_infos, resolver.compiled_count, edges, edge_count, strip_source);
			if (ok) {
				print_success("wrote '%s'", output_path);
			}
		}

		free(edges);
		free(module_infos);
	}

	for (module_index = 0; module_index < entry_import_count; module_index++) {
		module_unit_free_if_alias((module_unit_t *)entry_imports[module_index]);
	}
	free(entry_imports);

	module_resolver_free(&resolver);

	vm_free(&vm);
	compiler_shutdown();
	gc_free_all(&heap);
	table_free(&strings);
	free(source);
	free(owned_output_path);
	if (from_manifest) {
		project_manifest_free(&manifest);
	}

	return ok ? 0 : 1;
}

/* paweli init [name]: with no name, scaffolds PROJECT_MANIFEST_FILENAME
   and a starter main.pw directly in the current directory. With a
   name, creates a new directory of that name first (tolerating it
   already existing, same as ensure_project_build_dir() does for build/) and
   scaffolds inside that instead, so `paweli init myapp` needs no
   separate mkdir/cd step. See project_init() (src/project/manifest.c)
   for exactly what gets written. */
int cli_init_command(int argc, char **argv) {
	const char *name;
	int i;

	name = NULL;
	for (i = 1; i < argc; i++) {
		if (name != NULL) {
			print_error("unexpected argument '%s'", argv[i]);
			return 1;
		}
		name = argv[i];
	}

	if (name != NULL) {
		if (mkdir(name, 0755) != 0 && errno != EEXIST) {
			print_error("could not create directory '%s': %s", name, strerror(errno));
			return 1;
		}
		if (!project_init(name, name)) {
			return 1;
		}
		print_success("created '%s/%s'", name, PROJECT_MANIFEST_FILENAME);
	} else {
		if (!project_init(".", NULL)) {
			return 1;
		}
		print_success("created '%s'", PROJECT_MANIFEST_FILENAME);
	}

	return 0;
}
