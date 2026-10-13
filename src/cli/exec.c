#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/chunk.h"
#include "compiler/compiler.h"
#include "exec.h"
#include "output.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "platform/platform.h"

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

/* Prints each function's JIT state. It walks the same object list as
   dump_all_bytecode(), but after the run, since the JIT state is only
   known once the program has run. Call it before gc_free_all() and
   vm_free() free the objects. */
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

		// Use the position of the string literal, so the diagnostic lands on
		// the module name.
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

	// A run does not need the import array. The bindings are already in
	// entry_globals.
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
		// A test run must not wait on input. A test that calls
		// stdlib.read_line() would block on the terminal when someone runs
		// `paweli test` by hand. So stdin is pointed at the null device. A
		// failure here is only a warning.
		if (run_tests && !platform_stdin_from_null()) {
			print_warning("could not redirect stdin to the null device for test mode");
		}

		// Each module's script defines its functions in its own globals table.
		// Run each once so those functions can call each other. A module's
		// main() is never called. resolver.compiled_units is in dependency
		// order and holds each module once.
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

/* The shared tail of exec_run_paw() and exec_run_embedded(), which
   only differ in where the paw_program_t came from. It dumps bytecode
   if asked, runs the module scripts and then the entry script, and
   cleans up. Takes ownership of vm, heap, strings, and program.
   program may be NULL if loading failed. */
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
		// Dependency order, once each. See module_scripts in paw_program_t.
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
