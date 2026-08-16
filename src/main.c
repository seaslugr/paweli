#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/chunk.h"
#include "compiler/compiler.h"
#include "compiler/paw.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "platform/exe.h"
#include "runtime/gc.h"
#include "runtime/module.h"
#include "runtime/object.h"
#include "runtime/table.h"
#include "runtime/vm.h"

static char *read_file(const char *path);
static void dump_all_bytecode(obj_t *objects);
static void dump_jit_stats(obj_t *objects);
static bool parse_long_flag_value(int argc, char **argv, int *i, const char *flag_name, long min_value, long *out_value);
static char *default_paw_output_path(const char *input_path);
static obj_function_t *compile_from_source(const char *path, const char *source, gc_heap_t *heap, table_t *strings, vm_t *vm, module_unit_t **out_module_unit, table_t **out_entry_globals, bool dump_ast, bool run_tests);
static int run_source(const char *path, char *source, bool dump_ast, bool dump_bytecode, bool jit_stats, bool run_tests, long stack_size, long max_frames);
static int run_paw_program(vm_t *vm, gc_heap_t *heap, table_t *strings, paw_program_t *program, bool dump_bytecode, bool jit_stats);
static int run_paw(const char *path, bool dump_bytecode, bool jit_stats, long stack_size, long max_frames);
static int run_embedded(const char *exe_path, uint64_t offset, uint64_t size, bool dump_bytecode, bool jit_stats, long stack_size, long max_frames);
static int run_embedded_command(const char *exe_path, uint64_t offset, uint64_t size, int argc, char **argv);
static int run_command(int argc, char **argv);
static int build_command(int argc, char **argv);

static char *read_file(const char *path) {
	FILE *file;
	long size;
	char *buffer;
	size_t bytes_read;

	file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "paweli: could not open '%s'\n", path);
		return NULL;
	}

	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);

	buffer = malloc((size_t)size + 1);
	bytes_read = fread(buffer, 1, (size_t)size, file);
	buffer[bytes_read] = '\0';

	fclose(file);
	return buffer;
}

static void dump_all_bytecode(obj_t *objects) {
	obj_t *obj;
	obj_function_t *function;

	for (obj = objects; obj != NULL; obj = obj->next) {
		if (obj->type == OBJ_FUNCTION) {
			function = (obj_function_t *)obj;
			chunk_disassemble(&function->chunk, function->name == NULL ? "<script>" : function->name->chars, stdout);
		}
	}
}

/* Walks the same intrusive object list dump_all_bytecode() does, but
   after the run finishes rather than before it starts: unlike a
   function's bytecode (fixed at compile time), its JIT state
   (src/jit/jit_dispatch.c) is only known once the program has actually
   run long enough to cross the hotness threshold for each function.
   Must be called before gc_free_all()/vm_free() free the very objects
   being walked. */
static void dump_jit_stats(obj_t *objects) {
	obj_t *obj;
	obj_function_t *function;
	const char *state_name;

	for (obj = objects; obj != NULL; obj = obj->next) {
		if (obj->type != OBJ_FUNCTION) {
			continue;
		}
		function = (obj_function_t *)obj;
		switch (function->jit_state) {
			case JIT_COMPILED:
				state_name = "compiled";
				break;
			case JIT_FAILED:
				state_name = "failed";
				break;
			default:
				state_name = "not compiled";
				break;
		}
		fprintf(stdout, "%-24s %-13s hotness=%u\n", function->name == NULL ? "<script>" : function->name->chars, state_name, function->jit_hotness_count);
	}
}

static bool parse_long_flag_value(int argc, char **argv, int *i, const char *flag_name, long min_value, long *out_value) {
	long value;

	if (*i + 1 >= argc) {
		fprintf(stderr, "paweli: %s requires a value\n", flag_name);
		return false;
	}

	(*i)++;
	value = strtol(argv[*i], NULL, 10);

	if (value < min_value) {
		fprintf(stderr, "paweli: %s must be at least %ld\n", flag_name, min_value);
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

/* Shared by run_source() and build_command(): parses path/source, resolves
   and compiles a #module dependency if one is declared, then compiles the
   entry file itself. On success, *out_module_unit and *out_entry_globals
   are populated (the former NULL if there was no #module) and the
   compiled entry script is returned; the caller owns *out_module_unit
   (module_unit_free()) either way. Returns NULL on failure, with an error
   already printed to stderr and *out_module_unit left NULL. */
static obj_function_t *compile_from_source(const char *path, const char *source, gc_heap_t *heap, table_t *strings, vm_t *vm, module_unit_t **out_module_unit, table_t **out_entry_globals, bool dump_ast, bool run_tests) {
	parser_t parser;
	ast_node_t *program;
	module_unit_t *module_unit;
	table_t *entry_globals;
	obj_string_t *module_local_name;
	obj_function_t *script;

	*out_module_unit = NULL;

	parser_init(&parser, source, path);
	program = parser_parse_program(&parser);

	if (dump_ast) {
		ast_dump(program, stdout);
	}

	if (parser.had_error) {
		fprintf(stderr, "paweli: failed to parse '%s'\n", path);
		ast_free(program);
		return NULL;
	}

	module_unit = NULL;
	if (program->as.program.directive != NULL && program->as.program.directive->kind == AST_MODULE_IMPORT) {
		token_t import_name = program->as.program.directive->as.module_import.name;
		module_unit = module_compile(import_name.start + 1, import_name.length - 2, path, path, source, program->line, program->column, vm);
		if (module_unit == NULL) {
			fprintf(stderr, "paweli: failed to load a module required by '%s'\n", path);
			ast_free(program);
			return NULL;
		}
	}

	entry_globals = vm_new_file_globals(vm);
	*out_entry_globals = entry_globals;

	if (module_unit != NULL) {
		module_local_name = object_new_string(heap, strings, module_unit->local_name, module_unit->local_name_length);
		table_set(entry_globals, module_local_name, value_obj((obj_t *)module_unit->module_obj));
	}

	script = compiler_compile(program, path, source, heap, strings, entry_globals, NULL, NULL, module_unit, NULL, run_tests);
	ast_free(program);

	if (script == NULL) {
		fprintf(stderr, "paweli: failed to compile '%s'\n", path);
		if (module_unit != NULL) {
			module_unit_free(module_unit);
		}
		return NULL;
	}

	*out_module_unit = module_unit;
	return script;
}

static int run_source(const char *path, char *source, bool dump_ast, bool dump_bytecode, bool jit_stats, bool run_tests, long stack_size, long max_frames) {
	gc_heap_t heap;
	table_t strings;
	vm_t vm;
	module_unit_t *module_unit;
	table_t *entry_globals;
	obj_function_t *script;
	vm_result_t result;
	int exit_code;

	gc_heap_init(&heap);
	table_init(&strings);
	vm_init(&vm, &heap, &strings, path, source, (size_t)stack_size, (int)max_frames);

	script = compile_from_source(path, source, &heap, &strings, &vm, &module_unit, &entry_globals, dump_ast, run_tests);

	if (dump_bytecode && script != NULL) {
		dump_all_bytecode(heap.objects);
	}

	exit_code = 0;

	if (script == NULL) {
		exit_code = 1;
	} else {
		// The module's own script only defines its functions as globals
		// in its own private table (its exports are already wired into
		// module_unit->module_obj->members by the compiler); it needs to
		// run once so its functions can call each other later, but it
		// never calls its own main().
		if (module_unit != NULL) {
			result = vm_run(&vm, module_unit->script);
			if (result != VM_OK) {
				vm_print_uncaught_error(&vm);
				exit_code = 1;
			}
		}

		if (exit_code == 0) {
			result = vm_run(&vm, script);
			if (result != VM_OK) {
				vm_print_uncaught_error(&vm);
				exit_code = 1;
			}
		}
	}

	if (jit_stats) {
		dump_jit_stats(heap.objects);
	}

	if (module_unit != NULL) {
		module_unit_free(module_unit);
	}

	vm_free(&vm);
	compiler_shutdown();
	gc_free_all(&heap);
	table_free(&strings);
	free(source);

	return exit_code;
}

/* Shared tail for run_paw() and run_embedded(): both have, by this
   point, only differed in *how* they got a paw_program_t (a whole
   file vs. a byte range inside a larger executable). Everything
   after that (optionally dumping bytecode, running the module script
   before the entry script, and cleanup) is identical. Takes ownership
   of vm/heap/strings (already initialized by the caller) and program
   (may be NULL, meaning loading already failed). */
static int run_paw_program(vm_t *vm, gc_heap_t *heap, table_t *strings, paw_program_t *program, bool dump_bytecode, bool jit_stats) {
	vm_result_t result;
	int exit_code;

	if (dump_bytecode && program != NULL) {
		dump_all_bytecode(heap->objects);
	}

	exit_code = 0;

	if (program == NULL) {
		exit_code = 1;
	} else {
		if (program->module_script != NULL) {
			result = vm_run(vm, program->module_script);
			if (result != VM_OK) {
				vm_print_uncaught_error(vm);
				exit_code = 1;
			}
		}

		if (exit_code == 0) {
			result = vm_run(vm, program->entry_script);
			if (result != VM_OK) {
				vm_print_uncaught_error(vm);
				exit_code = 1;
			}
		}

		if (jit_stats) {
			dump_jit_stats(heap->objects);
		}

		paw_program_free(program);
	}

	vm_free(vm);
	gc_free_all(heap);
	table_free(strings);

	return exit_code;
}

static int run_paw(const char *path, bool dump_bytecode, bool jit_stats, long stack_size, long max_frames) {
	gc_heap_t heap;
	table_t strings;
	vm_t vm;
	paw_program_t *program;

	gc_heap_init(&heap);
	table_init(&strings);
	vm_init(&vm, &heap, &strings, path, NULL, (size_t)stack_size, (int)max_frames);

	program = paw_load_file(path, &vm);

	return run_paw_program(&vm, &heap, &strings, program, dump_bytecode, jit_stats);
}

static int run_embedded(const char *exe_path, uint64_t offset, uint64_t size, bool dump_bytecode, bool jit_stats, long stack_size, long max_frames) {
	gc_heap_t heap;
	table_t strings;
	vm_t vm;
	paw_program_t *program;

	gc_heap_init(&heap);
	table_init(&strings);
	vm_init(&vm, &heap, &strings, exe_path, NULL, (size_t)stack_size, (int)max_frames);

	program = paw_load_embedded(exe_path, offset, size, &vm);

	return run_paw_program(&vm, &heap, &strings, program, dump_bytecode, jit_stats);
}

/* Flag parsing for a self-extracting executable's own invocation
   (./myprogram [flags]): the same run flags run_command() accepts,
   minus --dump-ast/--run-tests (which apply only to compiling from
   source, and to which program's tests run; both are already fixed by
   the time this executable was built) and minus a file argument
   (there is nothing to name; the program is this executable). Either
   of the first two is accepted anyway, with a warning, matching how
   run_command() already treats them for a plain .paw file; anything
   else non-flag is a hard error, since there is no positional
   argument this command line could sensibly mean. */
static int run_embedded_command(const char *exe_path, uint64_t offset, uint64_t size, int argc, char **argv) {
	bool dump_bytecode;
	bool jit_stats;
	long stack_size;
	long max_frames;
	int i;

	dump_bytecode = false;
	jit_stats = false;
	stack_size = VM_DEFAULT_STACK_SIZE;
	max_frames = VM_DEFAULT_MAX_FRAMES;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--dump-ast") == 0) {
			fprintf(stderr, "paweli: --dump-ast has no effect on this program (there is no source to parse)\n");
		} else if (strcmp(argv[i], "--dump-bytecode") == 0) {
			dump_bytecode = true;
		} else if (strcmp(argv[i], "--jit-stats") == 0) {
			jit_stats = true;
		} else if (strcmp(argv[i], "--run-tests") == 0) {
			fprintf(stderr, "paweli: --run-tests has no effect on this program (its run mode was fixed when it was built)\n");
		} else if (strcmp(argv[i], "--stack-size") == 0) {
			if (!parse_long_flag_value(argc, argv, &i, "--stack-size", VM_MIN_STACK_SIZE, &stack_size)) {
				return 1;
			}
		} else if (strcmp(argv[i], "--max-frames") == 0) {
			if (!parse_long_flag_value(argc, argv, &i, "--max-frames", 1, &max_frames)) {
				return 1;
			}
		} else {
			fprintf(stderr, "paweli: unexpected argument '%s'\n", argv[i]);
			return 1;
		}
	}

	return run_embedded(exe_path, offset, size, dump_bytecode, jit_stats, stack_size, max_frames);
}

static int run_command(int argc, char **argv) {
	bool dump_ast;
	bool dump_bytecode;
	bool jit_stats;
	bool run_tests;
	const char *path;
	int i;
	long stack_size;
	long max_frames;
	char *source;

	dump_ast = false;
	dump_bytecode = false;
	jit_stats = false;
	run_tests = false;
	path = NULL;
	stack_size = VM_DEFAULT_STACK_SIZE;
	max_frames = VM_DEFAULT_MAX_FRAMES;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--dump-ast") == 0) {
			dump_ast = true;
		} else if (strcmp(argv[i], "--dump-bytecode") == 0) {
			dump_bytecode = true;
		} else if (strcmp(argv[i], "--jit-stats") == 0) {
			jit_stats = true;
		} else if (strcmp(argv[i], "--run-tests") == 0) {
			run_tests = true;
		} else if (strcmp(argv[i], "--stack-size") == 0) {
			if (!parse_long_flag_value(argc, argv, &i, "--stack-size", VM_MIN_STACK_SIZE, &stack_size)) {
				return 1;
			}
		} else if (strcmp(argv[i], "--max-frames") == 0) {
			if (!parse_long_flag_value(argc, argv, &i, "--max-frames", 1, &max_frames)) {
				return 1;
			}
		} else {
			path = argv[i];
		}
	}

	if (path == NULL) {
		fprintf(stderr, "usage: paweli [--dump-ast] [--dump-bytecode] [--jit-stats] [--run-tests] [--stack-size N] [--max-frames N] <file.pw|file.paw>\n");
		return 1;
	}

	if (paw_is_paw_file(path)) {
		if (dump_ast) {
			fprintf(stderr, "paweli: --dump-ast has no effect on a compiled .paw file (there is no source to parse)\n");
		}
		if (run_tests) {
			fprintf(stderr, "paweli: --run-tests has no effect on a compiled .paw file (its run mode was fixed by 'paweli build')\n");
		}
		return run_paw(path, dump_bytecode, jit_stats, stack_size, max_frames);
	}

	source = read_file(path);
	if (source == NULL) {
		return 1;
	}

	return run_source(path, source, dump_ast, dump_bytecode, jit_stats, run_tests, stack_size, max_frames);
}

static int build_command(int argc, char **argv) {
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
	module_unit_t *module_unit;
	table_t *entry_globals;
	obj_function_t *script;
	const char *module_filename;
	const char *module_source;
	char *self_exe_path;
	bool ok;

	output_path = NULL;
	owned_output_path = NULL;
	want_exe = false;
	strip_source = false;
	run_tests = false;
	positional_count = 0;
	positional[0] = NULL;
	positional[1] = NULL;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--bin") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "paweli: --bin requires a value\n");
				return 1;
			}
			i++;
			want_exe = true;
			output_path = argv[i];
		} else if (strcmp(argv[i], "--strip-source") == 0) {
			strip_source = true;
		} else if (strcmp(argv[i], "--run-tests") == 0) {
			run_tests = true;
		} else {
			if (positional_count >= 2) {
				fprintf(stderr, "paweli: unexpected argument '%s'\n", argv[i]);
				return 1;
			}
			positional[positional_count++] = argv[i];
		}
	}

	input_path = positional[0];
	if (input_path == NULL) {
		fprintf(stderr, "usage: paweli build <file.pw> [output.paw] [--strip-source] [--run-tests]\n       paweli build <file.pw> --bin <name> [--strip-source] [--run-tests]\n");
		return 1;
	}

	if (want_exe) {
		if (positional_count > 1) {
			fprintf(stderr, "paweli: unexpected argument '%s' (the executable's name comes from --bin, not a second argument)\n", positional[1]);
			return 1;
		}
	} else if (positional_count > 1) {
		output_path = positional[1];
	}

	if (!want_exe && output_path == NULL) {
		owned_output_path = default_paw_output_path(input_path);
		output_path = owned_output_path;
	}

	source = read_file(input_path);
	if (source == NULL) {
		free(owned_output_path);
		return 1;
	}

	gc_heap_init(&heap);
	table_init(&strings);
	vm_init(&vm, &heap, &strings, input_path, source, VM_DEFAULT_STACK_SIZE, VM_DEFAULT_MAX_FRAMES);

	script = compile_from_source(input_path, source, &heap, &strings, &vm, &module_unit, &entry_globals, false, run_tests);

	ok = script != NULL;
	if (ok) {
		module_filename = NULL;
		module_source = NULL;
		if (module_unit != NULL) {
			// module_compile() already tracked the module file's own
			// path/source as this vm's one and only owned source (a
			// #moduledef file can never itself have a #module import, so
			// there is exactly one).
			module_filename = vm.owned_sources[0].path;
			module_source = vm.owned_sources[0].source;
		}

		if (want_exe) {
			self_exe_path = platform_self_exe_path();
			if (self_exe_path == NULL) {
				fprintf(stderr, "paweli: could not determine this build's own executable path; --bin needs to copy it as the new executable's runtime stub, and that isn't supported on this platform yet\n");
				ok = false;
			} else {
				ok = paw_build_executable(output_path, self_exe_path, script, input_path, source, module_unit != NULL ? module_unit->module_obj : NULL, module_unit != NULL ? module_unit->script : NULL, module_unit != NULL ? module_unit->local_name : NULL, module_unit != NULL ? module_unit->local_name_length : 0, module_filename, module_source, strip_source);
				free(self_exe_path);
				if (ok) {
					printf("paweli: wrote executable '%s'\n", output_path);
				}
			}
		} else {
			ok = paw_write_file(output_path, script, input_path, source, module_unit != NULL ? module_unit->module_obj : NULL, module_unit != NULL ? module_unit->script : NULL, module_unit != NULL ? module_unit->local_name : NULL, module_unit != NULL ? module_unit->local_name_length : 0, module_filename, module_source, strip_source);
			if (ok) {
				printf("paweli: wrote '%s'\n", output_path);
			}
		}
	}

	if (module_unit != NULL) {
		module_unit_free(module_unit);
	}

	vm_free(&vm);
	compiler_shutdown();
	gc_free_all(&heap);
	table_free(&strings);
	free(source);
	free(owned_output_path);

	return ok ? 0 : 1;
}

int main(int argc, char **argv) {
	char *self_exe_path;
	uint64_t payload_offset;
	uint64_t payload_size;
	int exit_code;

	if (argc >= 2 && strcmp(argv[1], "build") == 0) {
		return build_command(argc - 1, argv + 1);
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
		exit_code = run_embedded_command(self_exe_path, payload_offset, payload_size, argc, argv);
		free(self_exe_path);
		return exit_code;
	}
	free(self_exe_path);

	return run_command(argc, argv);
}
