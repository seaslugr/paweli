#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/chunk.h"
#include "compiler/compiler.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "runtime/object.h"
#include "runtime/table.h"
#include "runtime/vm.h"

static char *read_file(const char *path);
static void dump_all_bytecode(obj_t *objects);
static void free_objects(obj_t *objects);
static bool parse_long_flag_value(int argc, char **argv, int *i, const char *flag_name, long min_value, long *out_value);

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

static void free_objects(obj_t *objects) {
	obj_t *obj;
	obj_t *next;

	obj = objects;
	while (obj != NULL) {
		next = obj->next;
		object_free(obj);
		obj = next;
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

int main(int argc, char **argv) {
	bool dump_ast;
	bool dump_bytecode;
	const char *path;
	int i;
	char *source;
	parser_t parser;
	ast_node_t *program;
	obj_t *objects;
	table_t strings;
	obj_function_t *script;
	vm_t vm;
	vm_result_t result;
	int exit_code;
	long stack_size;
	long max_frames;

	dump_ast = false;
	dump_bytecode = false;
	path = NULL;
	stack_size = VM_DEFAULT_STACK_SIZE;
	max_frames = VM_DEFAULT_MAX_FRAMES;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--dump-ast") == 0) {
			dump_ast = true;
		} else if (strcmp(argv[i], "--dump-bytecode") == 0) {
			dump_bytecode = true;
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
		fprintf(stderr, "usage: paweli [--dump-ast] [--dump-bytecode] [--stack-size N] [--max-frames N] <file.pw>\n");
		return 1;
	}

	source = read_file(path);
	if (source == NULL) {
		return 1;
	}

	parser_init(&parser, source, path);
	program = parser_parse_program(&parser);

	if (dump_ast) {
		ast_dump(program, stdout);
	}

	if (parser.had_error) {
		fprintf(stderr, "paweli: failed to parse '%s'\n", path);
		ast_free(program);
		free(source);
		return 1;
	}

	objects = NULL;
	table_init(&strings);

	script = compiler_compile(program, path, source, &objects, &strings);

	if (dump_bytecode) {
		dump_all_bytecode(objects);
	}

	exit_code = 0;

	if (script == NULL) {
		fprintf(stderr, "paweli: failed to compile '%s'\n", path);
		exit_code = 1;
	} else {
		vm_init(&vm, &objects, &strings, path, source, (size_t)stack_size, (int)max_frames);
		result = vm_run(&vm, script);
		if (result != VM_OK) {
			exit_code = 1;
		}
		vm_free(&vm);
	}

	free_objects(objects);
	table_free(&strings);
	ast_free(program);
	free(source);

	return exit_code;
}
