#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_internal.h"

// The signature of every native: print and assert as bare globals, and
// every stdlib.* member. Built once and reused for every file compiled
// in this process. compiler_shutdown() frees them.
signature_table_t compiler_native_globals;
signature_table_t compiler_stdlib_members;
static bool g_native_signatures_ready = false;

static void ensure_native_signatures();
static void root_struct_namespace(compiler_t *compiler, const struct_def_t *def);
static void register_structs(compiler_t *script_compiler, const ast_node_t *program, struct_registry_t *structs, const module_unit_t *const *imported_modules, size_t imported_module_count);
static void global_registry_init(global_registry_t *registry);
static void global_registry_free(global_registry_t *registry);
static bool global_registry_add(global_registry_t *registry, const char *name, size_t name_length, type_t type, int line, int column);
static void register_globals(compiler_t *script_compiler, const ast_node_t *program, global_registry_t *globals);

static void ensure_native_signatures() {
	if (g_native_signatures_ready) {
		return;
	}
	signature_table_init(&compiler_native_globals);
	signature_table_init(&compiler_stdlib_members);
	native_signatures_init(&compiler_native_globals, &compiler_stdlib_members);
	g_native_signatures_ready = true;
}

void compiler_shutdown() {
	type_shutdown();

	if (!g_native_signatures_ready) {
		return;
	}
	signature_table_free(&compiler_native_globals);
	signature_table_free(&compiler_stdlib_members);
	g_native_signatures_ready = false;
}

/* Roots def->namespace_obj in this file's globals table, which is a GC
   root, under a reserved key. Identifiers cannot contain '<' or '>',
   so the key cannot collide with real source. Nothing looks the key
   up. compile_struct_call() pushes namespace_obj as a constant. The
   entry only keeps the object alive. */
static void root_struct_namespace(compiler_t *compiler, const struct_def_t *def) {
	char key_buf[128];
	int key_len;
	obj_string_t *key;

	key_len = snprintf(key_buf, sizeof(key_buf), "<struct %.*s>", (int)def->name_length, def->name);
	key = object_new_string(compiler->heap, compiler->strings, key_buf, (size_t)(key_len > 0 ? key_len : 0));
	table_set(compiler->function->globals, key, value_obj((obj_t *)def->namespace_obj));
}

/* Registers every top-level `struct Name { ... }` and resolves its
   field types. It runs before compiler_collect_signatures(), since a
   parameter or return type may name a struct.

   A field's type may name its own struct or one registered earlier.
   struct_registry_add() inserts the struct before its fields are
   resolved, so self-reference works. A struct declared later in the
   file cannot be named. That keeps name resolution to one pass from
   top to bottom. */
static void register_structs(compiler_t *script_compiler, const ast_node_t *program, struct_registry_t *structs, const module_unit_t *const *imported_modules, size_t imported_module_count) {
	size_t i;
	size_t f;
	size_t m;
	const ast_node_t *struct_node;
	const ast_node_t *field_node;
	struct_def_t *def;
	struct_def_t *imported_def;
	struct_field_def_t *fields;
	size_t field_count;

	for (i = 0; i < program->as.program.structs.count; i++) {
		struct_node = program->as.program.structs.items[i];

		def = struct_registry_add(structs, struct_node->as.struct_decl.name.start, struct_node->as.struct_decl.name.length, script_compiler->heap, script_compiler->strings);
		if (def == NULL) {
			compiler_error(script_compiler, struct_node->as.struct_decl.name.line, struct_node->as.struct_decl.name.column, (int)struct_node->as.struct_decl.name.length, "compiler::duplicate-struct", "'%.*s' is already declared as a struct", (int)struct_node->as.struct_decl.name.length, struct_node->as.struct_decl.name.start);
			continue;
		}
		root_struct_namespace(script_compiler, def);

		// An unqualified struct name resolves locally first (see
		// compiler_resolve_type_ref()). A local struct with the name of an
		// imported one would hide it with no way to reach it, so it is
		// rejected, like two local structs with one name.
		for (m = 0; m < imported_module_count; m++) {
			imported_def = struct_registry_find(imported_modules[m]->exported_structs, struct_node->as.struct_decl.name.start, struct_node->as.struct_decl.name.length);
			if (imported_def != NULL && imported_def->is_exported) {
				compiler_error(script_compiler, struct_node->as.struct_decl.name.line, struct_node->as.struct_decl.name.column, (int)struct_node->as.struct_decl.name.length, "compiler::duplicate-struct", "'%.*s' is already declared as a struct", (int)struct_node->as.struct_decl.name.length, struct_node->as.struct_decl.name.start);
				break;
			}
		}

		field_count = struct_node->as.struct_decl.fields.count;
		fields = field_count > 0 ? malloc(field_count * sizeof(struct_field_def_t)) : NULL;

		for (f = 0; f < field_count; f++) {
			field_node = struct_node->as.struct_decl.fields.items[f];

			fields[f].name = field_node->as.param.name.start;
			fields[f].name_length = field_node->as.param.name.length;
			fields[f].type = compiler_resolve_type_ref(script_compiler, field_node->as.param.type, field_node->line, field_node->column);
			fields[f].default_value = field_node->as.param.default_value;
		}

		def->fields = fields;
		def->field_count = field_count;
	}
}

static void global_registry_init(global_registry_t *registry) {
	registry->defs = NULL;
	registry->count = 0;
	registry->capacity = 0;
}

static void global_registry_free(global_registry_t *registry) {
	free(registry->defs);
	registry->defs = NULL;
	registry->count = 0;
	registry->capacity = 0;
}

const global_def_t *compiler_global_registry_find(const global_registry_t *registry, const char *name, size_t name_length) {
	size_t i;

	for (i = 0; i < registry->count; i++) {
		if (registry->defs[i].name_length == name_length && memcmp(registry->defs[i].name, name, name_length) == 0) {
			return &registry->defs[i];
		}
	}
	return NULL;
}

/* Returns false and adds nothing if name is already registered. The
   caller reports the error. */
static bool global_registry_add(global_registry_t *registry, const char *name, size_t name_length, type_t type, int line, int column) {
	global_def_t *def;

	if (compiler_global_registry_find(registry, name, name_length) != NULL) {
		return false;
	}

	if (registry->count == registry->capacity) {
		registry->capacity = registry->capacity == 0 ? 8 : registry->capacity * 2;
		registry->defs = realloc(registry->defs, registry->capacity * sizeof(global_def_t));
	}

	def = &registry->defs[registry->count++];
	def->name = name;
	def->name_length = name_length;
	def->type = type;
	def->line = line;
	def->column = column;
	return true;
}

/* Registers the name and declared type of every top-level
   `type name = expr;`. It runs after register_structs(), since a
   global's type may name a struct, and before
   compiler_collect_signatures(), so any function body can use any
   global in the file in any declaration order. Initializers are
   compiled later (see compiler_compile()). */
static void register_globals(compiler_t *script_compiler, const ast_node_t *program, global_registry_t *globals) {
	size_t i;
	const ast_node_t *global_node;
	type_t declared_type;

	for (i = 0; i < program->as.program.globals.count; i++) {
		global_node = program->as.program.globals.items[i];
		declared_type = compiler_resolve_type_ref(script_compiler, global_node->as.var_decl.type, global_node->line, global_node->column);

		if (!global_registry_add(globals, global_node->as.var_decl.name.start, global_node->as.var_decl.name.length, declared_type, global_node->line, global_node->column)) {
			compiler_error(script_compiler, global_node->as.var_decl.name.line, global_node->as.var_decl.name.column, (int)global_node->as.var_decl.name.length, "compiler::duplicate-global", "'%.*s' is already declared as a global variable", (int)global_node->as.var_decl.name.length, global_node->as.var_decl.name.start);
		}
	}
}

/* Applies each decorator on fn_node to sig. Only a top-level fn can
   have decorators. An unknown name is a compile error. */
void compiler_apply_decorators(compiler_t *compiler, const ast_node_t *fn_node, signature_t *sig, size_t param_count) {
	size_t d;
	const ast_node_t *decorator_node;
	token_t dname;
	token_t msg_token;

	for (d = 0; d < fn_node->as.function_decl.decorators.count; d++) {
		decorator_node = fn_node->as.function_decl.decorators.items[d];
		dname = decorator_node->as.decorator.name;

		if (dname.length == 4 && memcmp(dname.start, "test", 4) == 0) {
			if (decorator_node->as.decorator.arg != NULL) {
				compiler_error(compiler, decorator_node->line, decorator_node->column, 0, "compiler::decorator-takes-no-argument", "'@test' does not take an argument");
			}
			if (param_count != 0) {
				compiler_error(compiler, fn_node->line, fn_node->column, 0, "compiler::test-function-must-have-no-parameters", "a '@test' function must take no parameters");
			}
		} else if (dname.length == 10 && memcmp(dname.start, "deprecated", 10) == 0) {
			sig->is_deprecated = true;
			if (decorator_node->as.decorator.arg != NULL) {
				msg_token = decorator_node->as.decorator.arg->as.literal.token;
				sig->deprecated_message = msg_token.start + 1;
				sig->deprecated_message_length = msg_token.length - 2;
			} else {
				sig->deprecated_message = NULL;
				sig->deprecated_message_length = 0;
			}
		} else {
			compiler_error(compiler, decorator_node->line, decorator_node->column, (int)dname.length + 1, "compiler::unknown-decorator", "unknown decorator '@%.*s'", (int)dname.length, dname.start);
		}
	}
}

/* Pass 1: reads every top-level fn's signature and allocates its empty
   obj_function_t. No body is compiled. This is what makes forward
   references, recursion, and overloads work in any declaration order.
   Returns one signature_t* per entry in program->as.program.functions,
   in order. An entry is NULL if its signature was a duplicate. The
   caller owns the array. file_signatures owns what it points to. */
signature_t **compiler_collect_signatures(compiler_t *script_compiler, const ast_node_t *program, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, table_t *file_globals, obj_module_t *home_module, signature_table_t *file_signatures) {
	signature_t **signatures;
	bool *accepted;
	size_t i;
	size_t p;
	const ast_node_t *fn_node;
	const ast_node_t *param_node;
	type_t *param_types;
	size_t param_count;
	type_t return_type;
	bool has_return_type;
	obj_function_t *fn;
	const signature_t *sig;

	signatures = malloc(program->as.program.functions.count * sizeof(signature_t *));
	accepted = malloc(program->as.program.functions.count * sizeof(bool));

	// Register every signature before resolving any. signature_table_add()
	// can reallocate an overload set that a later function shares, which
	// would invalidate a signature_t* taken earlier. After this loop the
	// table stops growing, so pointers taken below are stable. accepted[]
	// records which declarations were rejected as duplicates. Resolving
	// alone cannot tell a duplicate from the original.
	for (i = 0; i < program->as.program.functions.count; i++) {
		fn_node = program->as.program.functions.items[i];

		param_count = fn_node->as.function_decl.params.count;
		param_types = param_count > 0 ? malloc(param_count * sizeof(type_t)) : NULL;
		for (p = 0; p < param_count; p++) {
			param_node = fn_node->as.function_decl.params.items[p];
			param_types[p] = compiler_resolve_type_ref(script_compiler, param_node->as.param.type, param_node->line, param_node->column);
		}

		if (param_count > UINT8_MAX) {
			compiler_error(script_compiler, fn_node->line, fn_node->column, 0, "compiler::too-many-parameters", "a function can have at most 255 parameters");
		}

		has_return_type = fn_node->as.function_decl.has_return_type;
		return_type = has_return_type ? compiler_resolve_type_ref(script_compiler, fn_node->as.function_decl.return_type, fn_node->line, fn_node->column) : type_void();

		accepted[i] = signature_table_add(file_signatures, fn_node->as.function_decl.name.start, fn_node->as.function_decl.name.length, param_types, param_count, return_type, has_return_type) != NULL;
		free(param_types);

		if (!accepted[i]) {
			compiler_error(script_compiler, fn_node->as.function_decl.name.line, fn_node->as.function_decl.name.column, (int)fn_node->as.function_decl.name.length, "compiler::duplicate-signature", "'%.*s' is already declared with this parameter list", (int)fn_node->as.function_decl.name.length, fn_node->as.function_decl.name.start);
		}
	}

	for (i = 0; i < program->as.program.functions.count; i++) {
		if (!accepted[i]) {
			signatures[i] = NULL;
			continue;
		}

		fn_node = program->as.program.functions.items[i];

		param_count = fn_node->as.function_decl.params.count;
		param_types = param_count > 0 ? malloc(param_count * sizeof(type_t)) : NULL;
		for (p = 0; p < param_count; p++) {
			// Already validated above. NULL here avoids reporting the same
			// "unknown type" error twice.
			param_node = fn_node->as.function_decl.params.items[p];
			param_types[p] = type_from_type_ref(param_node->as.param.type, script_compiler->structs, script_compiler->imported_modules, script_compiler->imported_module_count, NULL, NULL);
		}

		signature_table_resolve(file_signatures, fn_node->as.function_decl.name.start, fn_node->as.function_decl.name.length, param_types, param_count, &sig);
		free(param_types);

		fn = object_new_function(heap);
		fn->name = object_new_string(heap, strings, fn_node->as.function_decl.name.start, fn_node->as.function_decl.name.length);
		fn->arity = (int)param_count;
		fn->globals = file_globals;
		fn->module = home_module;
		fn->filename = filename;
		fn->source = source;

		((signature_t *)sig)->function = fn;
		compiler_apply_decorators(script_compiler, fn_node, (signature_t *)sig, param_count);
		signatures[i] = (signature_t *)sig;
	}

	free(accepted);
	return signatures;
}

/* The same two passes as compiler_collect_signatures(), for one
   struct's functions in def->methods. It runs after register_structs()
   has resolved every struct's fields, so a method's types may name any
   struct in the file. Struct functions have no decorators. */
void compiler_collect_struct_method_signatures(compiler_t *script_compiler, const ast_node_t *struct_node, struct_def_t *def, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, table_t *file_globals, obj_module_t *home_module) {
	size_t count;
	bool *accepted;
	size_t i;
	size_t p;
	const ast_node_t *fn_node;
	const ast_node_t *param_node;
	type_t *param_types;
	size_t param_count;
	type_t return_type;
	bool has_return_type;
	obj_function_t *fn;
	const signature_t *sig;
	char name_buf[160];
	int name_len;

	count = struct_node->as.struct_decl.functions.count;
	if (count == 0) {
		return;
	}
	accepted = malloc(count * sizeof(bool));

	for (i = 0; i < count; i++) {
		fn_node = struct_node->as.struct_decl.functions.items[i];

		param_count = fn_node->as.function_decl.params.count;
		param_types = param_count > 0 ? malloc(param_count * sizeof(type_t)) : NULL;
		for (p = 0; p < param_count; p++) {
			param_node = fn_node->as.function_decl.params.items[p];
			param_types[p] = compiler_resolve_type_ref(script_compiler, param_node->as.param.type, param_node->line, param_node->column);
		}

		if (param_count > UINT8_MAX) {
			compiler_error(script_compiler, fn_node->line, fn_node->column, 0, "compiler::too-many-parameters", "a function can have at most 255 parameters");
		}

		has_return_type = fn_node->as.function_decl.has_return_type;
		return_type = has_return_type ? compiler_resolve_type_ref(script_compiler, fn_node->as.function_decl.return_type, fn_node->line, fn_node->column) : type_void();

		accepted[i] = signature_table_add(def->methods, fn_node->as.function_decl.name.start, fn_node->as.function_decl.name.length, param_types, param_count, return_type, has_return_type) != NULL;
		free(param_types);

		if (!accepted[i]) {
			compiler_error(script_compiler, fn_node->as.function_decl.name.line, fn_node->as.function_decl.name.column, (int)fn_node->as.function_decl.name.length, "compiler::duplicate-signature", "'%.*s.%.*s' is already declared with this parameter list", (int)def->name_length, def->name, (int)fn_node->as.function_decl.name.length, fn_node->as.function_decl.name.start);
		}
	}

	for (i = 0; i < count; i++) {
		if (!accepted[i]) {
			continue;
		}

		fn_node = struct_node->as.struct_decl.functions.items[i];

		param_count = fn_node->as.function_decl.params.count;
		param_types = param_count > 0 ? malloc(param_count * sizeof(type_t)) : NULL;
		for (p = 0; p < param_count; p++) {
			// Already validated above. NULL here avoids reporting the same
			// "unknown type" error twice.
			param_node = fn_node->as.function_decl.params.items[p];
			param_types[p] = type_from_type_ref(param_node->as.param.type, script_compiler->structs, script_compiler->imported_modules, script_compiler->imported_module_count, NULL, NULL);
		}

		signature_table_resolve(def->methods, fn_node->as.function_decl.name.start, fn_node->as.function_decl.name.length, param_types, param_count, &sig);
		free(param_types);

		fn = object_new_function(heap);
		// A qualified display name ("Point.origin") for stack traces. Call
		// resolution never reads it.
		name_len = snprintf(name_buf, sizeof(name_buf), "%.*s.%.*s", (int)def->name_length, def->name, (int)fn_node->as.function_decl.name.length, fn_node->as.function_decl.name.start);
		fn->name = object_new_string(heap, strings, name_buf, (size_t)(name_len > 0 ? name_len : 0));
		fn->arity = (int)param_count;
		fn->globals = file_globals;
		fn->module = home_module;
		fn->filename = filename;
		fn->source = source;

		((signature_t *)sig)->function = fn;
	}

	free(accepted);
}

obj_function_t *compiler_compile(const ast_node_t *program, const char *filename, const char *source, gc_heap_t *heap, table_t *strings, table_t *file_globals, obj_module_t *home_module, signature_table_t *out_exports, const module_unit_t *const *imported_modules, size_t imported_module_count, struct_registry_t **out_structs, bool run_tests_mode) {
	compiler_t script;
	size_t i;
	const ast_node_t *fn_node;
	const ast_node_t *struct_node;
	struct_def_t *struct_def;
	bool had_error;
	bool is_module_def;
	signature_table_t file_signatures;
	struct_registry_t *structs;
	global_registry_t globals;
	signature_t **signatures;
	const signature_t *main_sig;

	ensure_native_signatures();

	had_error = false;
	is_module_def = program->as.program.module_def != NULL;

	signature_table_init(&file_signatures);
	// Heap-allocated so ownership can pass to the caller through
	// out_structs. See the bottom of this function.
	structs = malloc(sizeof(struct_registry_t));
	struct_registry_init(structs);
	global_registry_init(&globals);
	if (out_exports != NULL) {
		signature_table_init(out_exports);
	}

	script.function = object_new_function(heap);
	script.function->name = NULL;
	script.function->globals = file_globals;
	script.function->module = home_module;
	script.function->filename = filename;
	script.function->source = source;

	// Root the script now. file_globals is a GC root, but the caller may
	// compile and run another file before this script reaches the VM
	// stack, and a collection in that window would free it. "<script>"
	// cannot collide with a real name: identifiers cannot contain '<' or
	// '>'.
	{
		obj_string_t *script_key = object_new_string(heap, strings, "<script>", 8);
		table_set(file_globals, script_key, value_obj((obj_t *)script.function));
	}

	script.heap = heap;
	script.strings = strings;
	script.filename = filename;
	script.source = source;
	script.had_error = false;
	script.is_module_def = false;
	script.compiling_main = false;
	script.return_type = type_void();
	script.has_return_type = false;
	script.file_signatures = &file_signatures;
	script.home_module = home_module;
	script.out_exports = out_exports;
	script.imported_modules = imported_modules;
	script.imported_module_count = imported_module_count;
	script.structs = structs;
	script.globals = &globals;
	script.local_count = 0;
	script.scope_depth = 0;
	memset(script.local_reg_seen, 0, sizeof(script.local_reg_seen));
	memset(script.local_reg_eligible, 0, sizeof(script.local_reg_eligible));
	memset(script.local_reg_decl_end, 0, sizeof(script.local_reg_decl_end));
	memset(script.local_reg_dead_at, 0, sizeof(script.local_reg_dead_at));
	script.local_slot_high_water = 0;
	script.enclosing = NULL;
	script.upvalue_count = 0;
	script.loops = NULL;
	script.loop_count = 0;
	script.loop_capacity = 0;

	// Structs first: a parameter or return type may name a struct.
	register_structs(&script, program, structs, imported_modules, imported_module_count);
	had_error = had_error || script.had_error;

	// Then global names and types, so a function body can use any global
	// in the file.
	register_globals(&script, program, &globals);
	had_error = had_error || script.had_error;

	// Then every struct's function signatures, before the top-level
	// signatures and before any body. A struct function may name any
	// struct and call any function, in any declaration order.
	for (i = 0; i < program->as.program.structs.count; i++) {
		struct_node = program->as.program.structs.items[i];
		struct_def = struct_registry_find(structs, struct_node->as.struct_decl.name.start, struct_node->as.struct_decl.name.length);

		if (struct_def == NULL) {
			// A duplicate struct name. register_structs() reported it.
			continue;
		}
		compiler_collect_struct_method_signatures(&script, struct_node, struct_def, heap, strings, filename, source, file_globals, home_module);
	}
	had_error = had_error || script.had_error;

	signatures = compiler_collect_signatures(&script, program, heap, strings, filename, source, file_globals, home_module, &file_signatures);
	had_error = had_error || script.had_error;

	for (i = 0; i < program->as.program.functions.count; i++) {
		if (signatures[i] == NULL) {
			continue;
		}
		fn_node = program->as.program.functions.items[i];
		compile_function_decl(fn_node, signatures[i], heap, strings, filename, source, is_module_def, home_module, out_exports, &file_signatures, imported_modules, imported_module_count, structs, &globals, false, &had_error);

		compiler_emit_constant(&script, value_obj((obj_t *)signatures[i]->function), fn_node->line, fn_node->column);
		compiler_emit_name_op_str(&script, OP_DEFINE_GLOBAL, signatures[i]->mangled_name, strlen(signatures[i]->mangled_name), fn_node->line, fn_node->column);
	}

	free(signatures);

	// Global initializers run after every function is defined, so an
	// initializer may call any function in the file. They are compiled in
	// file order. An initializer may use a global declared above it and
	// never one declared below. OP_DEFINE_GLOBAL stores the value.
	for (i = 0; i < program->as.program.globals.count; i++) {
		const ast_node_t *global_node = program->as.program.globals.items[i];
		const global_def_t *def = compiler_global_registry_find(&globals, global_node->as.var_decl.name.start, global_node->as.var_decl.name.length);
		type_t initializer_type;

		if (def == NULL) {
			// A duplicate name. register_globals() reported it.
			continue;
		}

		initializer_type = compile_expr_typed(&script, global_node->as.var_decl.initializer, &def->type);
		if (!type_is_assignable(def->type, initializer_type)) {
			compiler_error(&script, global_node->line, global_node->column, 0, "compiler::type-mismatch", "cannot initialize a %s variable with a %s value", type_to_string(def->type), type_to_string(initializer_type));
		}

		compiler_emit_name_op_str(&script, OP_DEFINE_GLOBAL, global_node->as.var_decl.name.start, global_node->as.var_decl.name.length, global_node->line, global_node->column);
	}
	had_error = had_error || script.had_error;

	// Struct function bodies compile last, once every signature is known.
	for (i = 0; i < program->as.program.structs.count; i++) {
		struct_node = program->as.program.structs.items[i];
		struct_def = struct_registry_find(structs, struct_node->as.struct_decl.name.start, struct_node->as.struct_decl.name.length);

		if (struct_def == NULL) {
			continue;
		}
		compile_struct_functions(struct_node, struct_def, heap, strings, filename, source, is_module_def, home_module, out_exports, &file_signatures, imported_modules, imported_module_count, structs, &globals, &had_error);
	}

	if (signature_table_resolve(&file_signatures, "main", 4, NULL, 0, &main_sig) != SIGNATURE_MATCH_OK) {
		compiler_error(&script, program->line, program->column, 0, "compiler::missing-main", "no 'main' function found");
		main_sig = NULL;
	}

	// A module's main() only hosts `export` statements and never runs.
	// Only the entry file's script calls main(), or every @test function
	// in run_tests_mode.
	if (home_module == NULL && main_sig != NULL) {
		if (run_tests_mode) {
			compiler_emit_test_runner(&script, program, &file_signatures);
		} else {
			compiler_emit_name_op_str(&script, OP_GET_GLOBAL, main_sig->mangled_name, strlen(main_sig->mangled_name), program->line, program->column);
			compiler_emit_bytes(&script, OP_CALL, 0, program->line, program->column);
			compiler_emit_byte(&script, OP_POP, program->line, program->column);
		}
	}

	compiler_emit_byte(&script, OP_NULL, program->line, program->column);
	compiler_emit_byte(&script, OP_RETURN, program->line, program->column);

	had_error = had_error || script.had_error;
	free(script.loops);

	signature_table_free(&file_signatures);
	global_registry_free(&globals);

	// Ownership transfer. An exported struct's type_t holds a raw
	// struct_def_t* into this registry, and TYPE_STRUCT equality is
	// pointer identity. An importer needs these exact instances, so the
	// registry is handed over and not copied. Only a #moduledef compile
	// passes out_structs.
	if (out_structs != NULL) {
		*out_structs = structs;
	} else {
		struct_registry_free(structs);
		free(structs);
	}

	return had_error ? NULL : script.function;
}
