#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/compiler.h"
#include "diagnostics/diagnostic.h"
#include "module.h"
#include "parser/ast.h"
#include "parser/parser.h"

static char *resolve_module_path(const char *importer_path, const char *name, size_t name_length);
static char *read_module_file(const char *path);
static const char *local_binding_name(const char *name, size_t name_length, size_t *out_length);

// name.pw, in the same directory as importer_path. No search path, no
// extension guessing: matches the project's minimal-scope philosophy.
// A name containing its own '/' (e.g. "sub/geometry") works too, purely
// as a byproduct of this being plain string concatenation.
static char *resolve_module_path(const char *importer_path, const char *name, size_t name_length) {
	const char *slash;
	size_t dir_length;
	char *path;

	slash = strrchr(importer_path, '/');
	dir_length = slash != NULL ? (size_t)(slash - importer_path + 1) : 0;

	path = malloc(dir_length + name_length + 4);
	memcpy(path, importer_path, dir_length);
	memcpy(path + dir_length, name, name_length);
	memcpy(path + dir_length + name_length, ".pw", 4);

	return path;
}

static char *read_module_file(const char *path) {
	FILE *file;
	long size;
	char *buffer;
	size_t bytes_read;

	file = fopen(path, "rb");
	if (file == NULL) {
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

// "sub/geometry" binds locally as "geometry", matching how the code
// that uses it writes `geometry.foo(...)`, not `sub/geometry.foo(...)`.
static const char *local_binding_name(const char *name, size_t name_length, size_t *out_length) {
	size_t i;

	for (i = name_length; i > 0; i--) {
		if (name[i - 1] == '/') {
			*out_length = name_length - i;
			return name + i;
		}
	}

	*out_length = name_length;
	return name;
}

module_unit_t *module_compile(const char *name, size_t name_length, const char *importer_path, const char *importer_filename, const char *importer_source, int line, int column, vm_t *vm) {
	char *path;
	char *source;
	parser_t parser;
	ast_node_t *program;
	token_t def_name;
	obj_string_t *module_name;
	table_t *file_globals;
	module_unit_t *unit;
	obj_function_t *script;
	const char *local_name;
	size_t local_name_length;

	path = resolve_module_path(importer_path, name, name_length);

	source = read_module_file(path);
	if (source == NULL) {
		diagnostic_emit(DIAG_ERROR, "compiler::module-not-found", importer_filename, importer_source, line, column, "could not find module '%.*s' (looked for '%s')", (int)name_length, name, path);
		free(path);
		return NULL;
	}

	parser_init(&parser, source, path);
	program = parser_parse_program(&parser);

	if (parser.had_error) {
		diagnostic_emit(DIAG_ERROR, "compiler::module-parse-error", importer_filename, importer_source, line, column, "failed to parse module '%.*s'", (int)name_length, name);
		ast_free(program);
		free(source);
		free(path);
		return NULL;
	}

	if (program->as.program.directive == NULL || program->as.program.directive->kind != AST_MODULE_DEF) {
		diagnostic_emit(DIAG_ERROR, "compiler::module-missing-moduledef", importer_filename, importer_source, line, column, "module '%.*s' has no #moduledef directive", (int)name_length, name);
		ast_free(program);
		free(source);
		free(path);
		return NULL;
	}

	def_name = program->as.program.directive->as.module_def.name;
	if (def_name.length - 2 != name_length || memcmp(def_name.start + 1, name, name_length) != 0) {
		diagnostic_emit(DIAG_ERROR, "compiler::module-name-mismatch", importer_filename, importer_source, line, column, "module file for '%.*s' declares itself as '%.*s'", (int)name_length, name, (int)(def_name.length - 2), def_name.start + 1);
		ast_free(program);
		free(source);
		free(path);
		return NULL;
	}

	local_name = local_binding_name(name, name_length, &local_name_length);

	unit = malloc(sizeof(module_unit_t));
	module_name = object_new_string(vm->heap, vm->strings, name, name_length);
	unit->module_obj = object_new_module(vm->heap, module_name);
	signature_table_init(&unit->exports);
	unit->local_name = malloc(local_name_length);
	memcpy(unit->local_name, local_name, local_name_length);
	unit->local_name_length = local_name_length;

	file_globals = vm_new_file_globals(vm);

	script = compiler_compile(program, path, source, vm->heap, vm->strings, file_globals, unit->module_obj, &unit->exports, NULL, &unit->exported_structs, false);

	ast_free(program);
	vm_track_owned_source(vm, path, source);

	if (script == NULL) {
		diagnostic_emit(DIAG_ERROR, "compiler::module-compile-error", importer_filename, importer_source, line, column, "failed to compile module '%.*s'", (int)name_length, name);
		signature_table_free(&unit->exports);
		struct_registry_free(unit->exported_structs);
		free(unit->exported_structs);
		free(unit->local_name);
		free(unit);
		return NULL;
	}

	unit->script = script;
	return unit;
}

void module_unit_free(module_unit_t *unit) {
	signature_table_free(&unit->exports);
	struct_registry_free(unit->exported_structs);
	free(unit->exported_structs);
	free(unit->local_name);
	free(unit);
}
