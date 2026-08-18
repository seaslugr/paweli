#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/chunk.h"
#include "compiler/compiler.h"
#include "exec.h"
#include "output.h"
#include "parser/ast.h"
#include "parser/parser.h"

static void dump_all_bytecode(obj_t *objects);
static void dump_jit_stats(obj_t *objects);
static int run_paw_program(vm_t *vm, gc_heap_t *heap, table_t *strings, paw_program_t *program, bool dump_bytecode, bool jit_stats);

char *exec_read_file(const char *path) {
	FILE *file;
	long size;
	char *buffer;
	size_t bytes_read;

	file = fopen(path, "rb");
	if (file == NULL) {
		print_error("could not open '%s'", path);
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

obj_function_t *exec_compile_from_source(const char *path, const char *source, gc_heap_t *heap, table_t *strings, vm_t *vm, module_resolver_t *resolver, table_t **out_entry_globals, const module_unit_t ***out_entry_imports, size_t *out_entry_import_count, bool dump_ast, bool run_tests) {
	parser_t parser;
	ast_node_t *program;
	const module_unit_t **entry_imports;
	size_t entry_import_count;
	size_t i;
	table_t *entry_globals;
	obj_function_t *script;

	*out_entry_imports = NULL;
	*out_entry_import_count = 0;

	parser_init(&parser, source, path);
	program = parser_parse_program(&parser);

	if (dump_ast) {
		ast_dump(program, stdout);
	}

	if (parser.had_error) {
		print_error("failed to parse '%s'", path);
		ast_free(program);
		return NULL;
	}

	entry_import_count = program->as.program.imports.count;
	entry_imports = entry_import_count > 0 ? malloc(entry_import_count * sizeof(module_unit_t *)) : NULL;

	for (i = 0; i < entry_import_count; i++) {
		const ast_node_t *import_node = program->as.program.imports.items[i];
		token_t import_name = import_node->as.module_import.name;

		// import_name's own line/column (the string literal), not
		// import_node's (the '#module' keyword one column earlier), so
		// diagnostics land on the module name itself.
		entry_imports[i] = module_compile(import_name.start + 1, import_name.length - 2, path, path, source, import_name.line, import_name.column + 1, vm, resolver);
		if (entry_imports[i] == NULL) {
			print_error("failed to load a module required by '%s'", path);
			break;
		}
	}

	if (i < entry_import_count) {
		size_t j;

		for (j = 0; j < i; j++) {
			module_unit_free_if_alias((module_unit_t *)entry_imports[j]);
		}
		free(entry_imports);
		ast_free(program);
		return NULL;
	}

	entry_globals = vm_new_file_globals(vm);
	*out_entry_globals = entry_globals;

	for (i = 0; i < entry_import_count; i++) {
		obj_string_t *module_local_name = object_new_string(heap, strings, entry_imports[i]->local_name, entry_imports[i]->local_name_length);
		table_set(entry_globals, module_local_name, value_obj((obj_t *)entry_imports[i]->module_obj));
	}

	script = compiler_compile(program, path, source, heap, strings, entry_globals, NULL, NULL, entry_imports, entry_import_count, NULL, run_tests);
	ast_free(program);

	if (script == NULL) {
		for (i = 0; i < entry_import_count; i++) {
			module_unit_free_if_alias((module_unit_t *)entry_imports[i]);
		}
		free(entry_imports);
		print_error("failed to compile '%s'", path);
		return NULL;
	}

	*out_entry_imports = entry_imports;
	*out_entry_import_count = entry_import_count;
	return script;
}

int exec_run_source(const char *path, char *source, bool dump_ast, bool dump_bytecode, bool jit_stats, bool run_tests, long stack_size, long max_frames) {
	gc_heap_t heap;
	table_t strings;
	vm_t vm;
	module_resolver_t resolver;
	table_t *entry_globals;
	const module_unit_t **entry_imports;
	size_t entry_import_count;
	obj_function_t *script;
	vm_result_t result;
	int exit_code;
	size_t i;

	gc_heap_init(&heap);
	table_init(&strings);
	vm_init(&vm, &heap, &strings, path, source, (size_t)stack_size, (int)max_frames);
	module_resolver_init(&resolver);

	script = exec_compile_from_source(path, source, &heap, &strings, &vm, &resolver, &entry_globals, &entry_imports, &entry_import_count, dump_ast, run_tests);

	// Not needed beyond the runtime bindings exec_compile_from_source()
	// already wired into entry_globals; free immediately (a plain run
	// doesn't serialize anything, unlike a build).
	for (i = 0; i < entry_import_count; i++) {
		module_unit_free_if_alias((module_unit_t *)entry_imports[i]);
	}
	free(entry_imports);

	if (dump_bytecode && script != NULL) {
		dump_all_bytecode(heap.objects);
	}

	exit_code = 0;

	if (script == NULL) {
		exit_code = 1;
	} else {
		// --run-tests is meant to be a hermetic, unattended run: a test
		// exercising stdlib.read_line() on empty/closed stdin (see
		// tests/suite/io.pw) would otherwise block on this process's
		// real stdin whenever someone runs `paweli test` by hand from a
		// terminal instead of through scripts/run_tests.sh's explicit
		// `< /dev/null`. Redirecting here makes that the default instead
		// of a footgun. A failed freopen() just leaves stdin as it was;
		// nothing about running the tests depends on this succeeding.
		if (run_tests && freopen("/dev/null", "r", stdin) == NULL) {
			print_warning("could not redirect stdin to /dev/null for test mode");
		}

		// Every distinct module's own script only defines its functions
		// as globals in its own private table (its exports are already
		// wired into its module_obj->members by the compiler); each
		// needs to run once so its functions can call each other later,
		// none of them ever calls its own main(). resolver.compiled_units
		// is already in dependency order (module_compile()'s recursion
		// finishes a module's own imports before the module itself, so
		// a plain append-on-finish list is already topologically
		// sorted) and already deduplicated (a diamond-imported module
		// appears once, not once per importer), so running it in that
		// order, once each, is exactly right.
		for (i = 0; i < resolver.compiled_count && exit_code == 0; i++) {
			result = vm_run(&vm, resolver.compiled_units[i]->script);
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

	module_resolver_free(&resolver);

	vm_free(&vm);
	compiler_shutdown();
	gc_free_all(&heap);
	table_free(&strings);
	free(source);

	return exit_code;
}

/* Shared tail for exec_run_paw() and exec_run_embedded(): both
   have, by this point, only differed in *how* they got a
   paw_program_t (a whole file vs. a byte range inside a larger
   executable). Everything after that (optionally dumping bytecode,
   running the module script before the entry script, and cleanup) is
   identical. Takes ownership of vm/heap/strings (already initialized
   by the caller) and program (may be NULL, meaning loading already
   failed). */
static int run_paw_program(vm_t *vm, gc_heap_t *heap, table_t *strings, paw_program_t *program, bool dump_bytecode, bool jit_stats) {
	vm_result_t result;
	int exit_code;
	size_t i;

	if (dump_bytecode && program != NULL) {
		dump_all_bytecode(heap->objects);
	}

	exit_code = 0;

	if (program == NULL) {
		exit_code = 1;
	} else {
		// Same dependency-order, run-once-each guarantee as
		// exec_run_source()'s own loop over resolver.compiled_units;
		// see paw_program_t's own comment on module_scripts.
		for (i = 0; i < program->module_count && exit_code == 0; i++) {
			result = vm_run(vm, program->module_scripts[i]);
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

int exec_run_paw(const char *path, bool dump_bytecode, bool jit_stats, long stack_size, long max_frames) {
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

int exec_run_embedded(const char *exe_path, uint64_t offset, uint64_t size, bool dump_bytecode, bool jit_stats, long stack_size, long max_frames) {
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

size_t exec_find_module_index(const module_resolver_t *resolver, const module_unit_t *target) {
	size_t i;

	for (i = 0; i < resolver->compiled_count; i++) {
		if (resolver->compiled_units[i] == target) {
			return i;
		}
	}
	return SIZE_MAX;
}

void exec_push_edge(paw_import_edge_t **edges, size_t *count, size_t *capacity, size_t importer_index, size_t target_index, const char *local_name, size_t local_name_length) {
	if (*count == *capacity) {
		*capacity = *capacity < 8 ? 8 : *capacity * 2;
		*edges = realloc(*edges, *capacity * sizeof(paw_import_edge_t));
	}
	(*edges)[*count].importer_index = importer_index;
	(*edges)[*count].target_index = target_index;
	(*edges)[*count].local_name = local_name;
	(*edges)[*count].local_name_length = local_name_length;
	(*count)++;
}
