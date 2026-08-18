#define _POSIX_C_SOURCE 200809L
// realpath() is XSI (X/Open), not plain POSIX: glibc gates its
// declaration behind __USE_XOPEN_EXTENDED, which _POSIX_C_SOURCE alone
// does not set.
#define _XOPEN_SOURCE 700

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

static void resolver_push_in_progress(module_resolver_t *resolver, char *canonical_path);
static void resolver_pop_in_progress(module_resolver_t *resolver);
static void resolver_add_compiled(module_resolver_t *resolver, char *canonical_path, module_unit_t *unit);
static module_unit_t *resolver_find_compiled(const module_resolver_t *resolver, const char *canonical_path);
static bool resolver_is_in_progress(const module_resolver_t *resolver, const char *canonical_path);

static size_t append_str(char *buffer, size_t buffer_size, size_t offset, const char *text);
static void report_cycle(const module_resolver_t *resolver, const char *canonical_path, const char *importer_filename, const char *importer_source, int line, int column, const char *name, size_t name_length);

static module_unit_t *make_alias_unit(const module_unit_t *canonical, const char *name, size_t name_length);

/* name.pw, in the same directory as importer_path. No search path, no
   extension guessing: matches the project's minimal-scope philosophy.
   A name containing its own '/' (e.g. "sub/geometry") works too, purely
   as a byproduct of this being plain string concatenation. */
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

/* "sub/geometry" binds locally as "geometry", matching how the code
   that uses it writes `geometry.foo(...)`, not `sub/geometry.foo(...)`. */
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

void module_resolver_init(module_resolver_t *resolver) {
	resolver->compiled_paths = NULL;
	resolver->compiled_units = NULL;
	resolver->compiled_count = 0;
	resolver->compiled_capacity = 0;

	resolver->in_progress_paths = NULL;
	resolver->in_progress_count = 0;
	resolver->in_progress_capacity = 0;
}

void module_resolver_free(module_resolver_t *resolver) {
	size_t i;

	for (i = 0; i < resolver->compiled_count; i++) {
		free(resolver->compiled_paths[i]);
		module_unit_free(resolver->compiled_units[i]);
	}
	free(resolver->compiled_paths);
	free(resolver->compiled_units);

	// Should always be empty by the time this runs: every push is
	// matched by a pop, success or failure. Freed defensively anyway
	// rather than assumed, matching this codebase's general style.
	for (i = 0; i < resolver->in_progress_count; i++) {
		free(resolver->in_progress_paths[i]);
	}
	free(resolver->in_progress_paths);
}

static void resolver_push_in_progress(module_resolver_t *resolver, char *canonical_path) {
	if (resolver->in_progress_count == resolver->in_progress_capacity) {
		resolver->in_progress_capacity = resolver->in_progress_capacity < 8 ? 8 : resolver->in_progress_capacity * 2;
		resolver->in_progress_paths = realloc(resolver->in_progress_paths, resolver->in_progress_capacity * sizeof(char *));
	}
	resolver->in_progress_paths[resolver->in_progress_count] = canonical_path;
	resolver->in_progress_count++;
}

/* Does not free or return the popped path: the caller already holds the
   same pointer it originally passed to resolver_push_in_progress(), and
   decides its fate itself (resolver_add_compiled() on success, free() on
   failure). */
static void resolver_pop_in_progress(module_resolver_t *resolver) {
	resolver->in_progress_count--;
}

static void resolver_add_compiled(module_resolver_t *resolver, char *canonical_path, module_unit_t *unit) {
	if (resolver->compiled_count == resolver->compiled_capacity) {
		resolver->compiled_capacity = resolver->compiled_capacity < 8 ? 8 : resolver->compiled_capacity * 2;
		resolver->compiled_paths = realloc(resolver->compiled_paths, resolver->compiled_capacity * sizeof(char *));
		resolver->compiled_units = realloc(resolver->compiled_units, resolver->compiled_capacity * sizeof(module_unit_t *));
	}
	resolver->compiled_paths[resolver->compiled_count] = canonical_path;
	resolver->compiled_units[resolver->compiled_count] = unit;
	resolver->compiled_count++;
}

static module_unit_t *resolver_find_compiled(const module_resolver_t *resolver, const char *canonical_path) {
	size_t i;

	for (i = 0; i < resolver->compiled_count; i++) {
		if (strcmp(resolver->compiled_paths[i], canonical_path) == 0) {
			return resolver->compiled_units[i];
		}
	}
	return NULL;
}

static bool resolver_is_in_progress(const module_resolver_t *resolver, const char *canonical_path) {
	size_t i;

	for (i = 0; i < resolver->in_progress_count; i++) {
		if (strcmp(resolver->in_progress_paths[i], canonical_path) == 0) {
			return true;
		}
	}
	return false;
}

/* Appends text at offset, clamped to never write past buffer_size, same
   pattern as parser/ast.c's type_ref_append(). Diagnostic-message-only,
   so silent truncation on a pathologically long import chain is an
   acceptable outcome. */
static size_t append_str(char *buffer, size_t buffer_size, size_t offset, const char *text) {
	int written;

	if (offset >= buffer_size) {
		return buffer_size;
	}
	written = snprintf(buffer + offset, buffer_size - offset, "%s", text);
	if (written <= 0) {
		return offset;
	}
	offset += (size_t)written;
	return offset > buffer_size ? buffer_size : offset;
}

static void report_cycle(const module_resolver_t *resolver, const char *canonical_path, const char *importer_filename, const char *importer_source, int line, int column, const char *name, size_t name_length) {
	char chain[2048];
	size_t offset;
	size_t i;

	offset = 0;
	for (i = 0; i < resolver->in_progress_count; i++) {
		if (i > 0) {
			offset = append_str(chain, sizeof(chain), offset, " -> ");
		}
		offset = append_str(chain, sizeof(chain), offset, resolver->in_progress_paths[i]);
	}
	offset = append_str(chain, sizeof(chain), offset, " -> ");
	append_str(chain, sizeof(chain), offset, canonical_path);

	diagnostic_emit(DIAG_ERROR, "compiler::circular-import", importer_filename, importer_source, line, column, (int)name_length, "'%.*s' would create a #module import cycle: %s", (int)name_length, name, chain);
}

/* A second (or third, ...) importer resolving a canonical file already
   memoized shares its module_obj/script/exports/exported_structs (same
   file, must stay the same struct_def_t pointers for TYPE_STRUCT
   equality to hold) but gets its own local_name: each importer's own
   #module "..." string decides its own binding name, independent of
   whatever name some other importer used. See module_unit_t.owns_content. */
static module_unit_t *make_alias_unit(const module_unit_t *canonical, const char *name, size_t name_length) {
	module_unit_t *unit;
	const char *local_name;
	size_t local_name_length;

	local_name = local_binding_name(name, name_length, &local_name_length);

	unit = malloc(sizeof(module_unit_t));
	unit->module_obj = canonical->module_obj;
	unit->script = canonical->script;
	unit->exports = canonical->exports;
	unit->exported_structs = canonical->exported_structs;
	unit->local_name = malloc(local_name_length);
	memcpy(unit->local_name, local_name, local_name_length);
	unit->local_name_length = local_name_length;
	unit->owns_content = false;
	unit->canonical_source = canonical;
	unit->own_imports = NULL;
	unit->own_import_count = 0;

	return unit;
}

const module_unit_t *module_unit_canonical(const module_unit_t *unit) {
	return unit->canonical_source != NULL ? unit->canonical_source : unit;
}

void module_unit_free_if_alias(module_unit_t *unit) {
	if (unit != NULL && !unit->owns_content) {
		module_unit_free(unit);
	}
}

module_unit_t *module_compile(const char *name, size_t name_length, const char *importer_path, const char *importer_filename, const char *importer_source, int line, int column, vm_t *vm, module_resolver_t *resolver) {
	char *path;
	char *canonical_path;
	module_unit_t *memoized;
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
	size_t import_count;
	const module_unit_t **own_imports;
	size_t i;
	bool import_failed;

	path = resolve_module_path(importer_path, name, name_length);

	// realpath() requires the target to actually exist, so a NULL result
	// here means the same thing read_module_file()'s own fopen failure
	// used to mean: no such file. Canonicalizing up front (before ever
	// reading the file) is what makes the memo/cycle checks below
	// correct: two different #module strings that happen to resolve to
	// the same file must compare equal.
	canonical_path = realpath(path, NULL);
	if (canonical_path == NULL) {
		diagnostic_emit(DIAG_ERROR, "compiler::module-not-found", importer_filename, importer_source, line, column, (int)name_length, "could not find module '%.*s' (looked for '%s')", (int)name_length, name, path);
		free(path);
		return NULL;
	}

	// A diamond (this file and some sibling both import the same target,
	// directly or transitively): the target compiles once, memoized by
	// canonical path; every later importer gets a lightweight alias.
	memoized = resolver_find_compiled(resolver, canonical_path);
	if (memoized != NULL) {
		free(canonical_path);
		free(path);
		return make_alias_unit(memoized, name, name_length);
	}

	// A cycle: this exact canonical path is already mid-compile
	// somewhere up the current import chain.
	if (resolver_is_in_progress(resolver, canonical_path)) {
		report_cycle(resolver, canonical_path, importer_filename, importer_source, line, column, name, name_length);
		free(canonical_path);
		free(path);
		return NULL;
	}

	source = read_module_file(path);
	if (source == NULL) {
		// realpath() succeeded but the file vanished or became unreadable
		// before this fopen (a TOCTOU race); vanishingly rare, same
		// diagnostic as the ordinary not-found case above.
		diagnostic_emit(DIAG_ERROR, "compiler::module-not-found", importer_filename, importer_source, line, column, (int)name_length, "could not find module '%.*s' (looked for '%s')", (int)name_length, name, path);
		free(canonical_path);
		free(path);
		return NULL;
	}

	parser_init(&parser, source, path);
	program = parser_parse_program(&parser);

	if (parser.had_error) {
		diagnostic_emit(DIAG_ERROR, "compiler::module-parse-error", importer_filename, importer_source, line, column, (int)name_length, "failed to parse module '%.*s'", (int)name_length, name);
		ast_free(program);
		free(source);
		free(canonical_path);
		free(path);
		return NULL;
	}

	if (program->as.program.module_def == NULL) {
		diagnostic_emit(DIAG_ERROR, "compiler::module-missing-moduledef", importer_filename, importer_source, line, column, (int)name_length, "module '%.*s' has no #moduledef directive", (int)name_length, name);
		ast_free(program);
		free(source);
		free(canonical_path);
		free(path);
		return NULL;
	}

	def_name = program->as.program.module_def->as.module_def.name;
	if (def_name.length - 2 != name_length || memcmp(def_name.start + 1, name, name_length) != 0) {
		diagnostic_emit(DIAG_ERROR, "compiler::module-name-mismatch", importer_filename, importer_source, line, column, (int)name_length, "module file for '%.*s' declares itself as '%.*s'", (int)name_length, name, (int)(def_name.length - 2), def_name.start + 1);
		ast_free(program);
		free(source);
		free(canonical_path);
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
	unit->owns_content = true;
	unit->canonical_source = NULL;
	unit->own_imports = NULL;
	unit->own_import_count = 0;

	// Pushed before resolving this module's own imports (if it has any):
	// a cycle back through this exact file, however many hops deep,
	// needs this file already marked in-progress for
	// resolver_is_in_progress() to catch it.
	resolver_push_in_progress(resolver, canonical_path);

	import_count = program->as.program.imports.count;
	own_imports = import_count > 0 ? malloc(import_count * sizeof(module_unit_t *)) : NULL;
	import_failed = false;

	for (i = 0; i < import_count; i++) {
		const ast_node_t *import_node = program->as.program.imports.items[i];
		token_t import_name = import_node->as.module_import.name;

		// import_name's own line/column (the string literal), not
		// import_node's (the '#module' keyword one column earlier), so
		// diagnostics land on the module name itself.
		own_imports[i] = module_compile(import_name.start + 1, import_name.length - 2, path, path, source, import_name.line, import_name.column + 1, vm, resolver);
		if (own_imports[i] == NULL) {
			import_failed = true;
			// Only entries [0, i) were actually resolved; the cleanup
			// loop below must stop there, not at the original count.
			import_count = i;
			break;
		}
	}

	if (import_failed) {
		for (i = 0; i < import_count; i++) {
			module_unit_free_if_alias((module_unit_t *)own_imports[i]);
		}
		free(own_imports);
		resolver_pop_in_progress(resolver);
		free(canonical_path);

		diagnostic_emit(DIAG_ERROR, "compiler::module-compile-error", importer_filename, importer_source, line, column, (int)name_length, "failed to compile module '%.*s'", (int)name_length, name);
		ast_free(program);
		free(source);
		free(path);
		signature_table_free(&unit->exports);
		free(unit->local_name);
		free(unit);
		return NULL;
	}

	file_globals = vm_new_file_globals(vm);

	// Binds each of this module's own imports into ITS OWN file_globals,
	// the same way main.c binds the entry file's own imports into
	// entry_globals: compile_module_call() emits OP_GET_GLOBAL against
	// whichever frame is running, so a module that calls into its own
	// import at runtime needs that binding to exist in its own globals
	// table, not just to have type-checked correctly at compile time.
	for (i = 0; i < import_count; i++) {
		obj_string_t *import_local_name = object_new_string(vm->heap, vm->strings, own_imports[i]->local_name, own_imports[i]->local_name_length);
		table_set(file_globals, import_local_name, value_obj((obj_t *)own_imports[i]->module_obj));
	}

	// Recorded (target normalized to canonical, local_name copied
	// independently) before own_imports[] entries get freed below, for
	// paw_write_payload() to persist the same binding graph into a .paw
	// file's own edge list; see module_unit_t.own_imports.
	unit->own_import_count = import_count;
	unit->own_imports = import_count > 0 ? malloc(import_count * sizeof(*unit->own_imports)) : NULL;
	for (i = 0; i < import_count; i++) {
		unit->own_imports[i].target = module_unit_canonical(own_imports[i]);
		unit->own_imports[i].local_name = malloc(own_imports[i]->local_name_length);
		memcpy(unit->own_imports[i].local_name, own_imports[i]->local_name, own_imports[i]->local_name_length);
		unit->own_imports[i].local_name_length = own_imports[i]->local_name_length;
	}

	script = compiler_compile(program, path, source, vm->heap, vm->strings, file_globals, unit->module_obj, &unit->exports, own_imports, import_count, &unit->exported_structs, false);

	for (i = 0; i < import_count; i++) {
		module_unit_free_if_alias((module_unit_t *)own_imports[i]);
	}
	free(own_imports);

	ast_free(program);
	vm_track_owned_source(vm, path, source);

	if (script == NULL) {
		resolver_pop_in_progress(resolver);
		free(canonical_path);
		diagnostic_emit(DIAG_ERROR, "compiler::module-compile-error", importer_filename, importer_source, line, column, (int)name_length, "failed to compile module '%.*s'", (int)name_length, name);
		module_unit_free(unit);
		return NULL;
	}

	unit->script = script;

	resolver_pop_in_progress(resolver);
	resolver_add_compiled(resolver, canonical_path, unit);

	return unit;
}

void module_unit_free(module_unit_t *unit) {
	if (unit->owns_content) {
		size_t i;

		signature_table_free(&unit->exports);
		struct_registry_free(unit->exported_structs);
		free(unit->exported_structs);
		for (i = 0; i < unit->own_import_count; i++) {
			free(unit->own_imports[i].local_name);
		}
		free(unit->own_imports);
	}
	free(unit->local_name);
	free(unit);
}
