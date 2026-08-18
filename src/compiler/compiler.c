#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "diagnostics/diagnostic.h"
#include "native_signatures.h"
#include "signature.h"
#include "type.h"

#define MAX_LOCALS 256

// Every native the compiler must be able to type-check calls against
// (print/assert as bare globals, every stdlib.* member), built once and
// reused for every file compiled during this process's lifetime. Freed
// by compiler_shutdown(), called once from main.c after the VM is done.
static signature_table_t g_native_globals;
static signature_table_t g_stdlib_members;
static bool g_native_signatures_ready = false;

typedef struct {
	token_t name;
	type_t type;
	int depth;
	// True once some nested fn's body has resolved this local as an
	// upvalue. Scope-exit (end_scope()/emit_pop_to_depth()) emits
	// OP_CLOSE_UPVALUE instead of OP_POP for a captured local, so any
	// closure that outlives this scope still sees the right storage.
	bool is_captured;
} local_t;

// One upvalue a nested fn's body resolved from an enclosing scope.
// is_local true means index is a slot in the *immediately* enclosing
// function's own locals[]; false means index is itself an upvalue index
// of the immediately enclosing function (a capture threaded through more
// than one level of nesting). type is the captured value's type, needed
// to type-check a call through it (see compile_value_call()) since
// there is no function-type syntax to re-derive it from.
typedef struct {
	uint8_t index;
	bool is_local;
	type_t type;
} upvalue_t;

// One file-private `type name = expr;` declared at true top level (see
// register_globals()). Unlike struct_def_t, nothing else ever needs a
// stable pointer to one of these. A global is only ever looked up by
// name to read off its declared type, never referenced by identity. So
// global_registry_t is a plain growable array of values, not an array of
// heap-allocated pointers.
typedef struct {
	const char *name;
	size_t name_length;
	type_t type;
	int line;
	int column;
} global_def_t;

// Every global declared in one file, in declaration order. Always
// file-local: there is no export/import story for a global yet (see this
// feature's design notes), so unlike struct_registry_t this never gets
// handed to an importer.
typedef struct {
	global_def_t *defs;
	size_t count;
	size_t capacity;
} global_registry_t;

// One entry per loop currently being compiled (nested loops push more
// than one). continue_target is where `continue` jumps back to: the
// condition re-check for `while`, or the increment for `for`.
// continue_depth/break_depth are the scope depths `continue`/`break`
// must pop locals back down to before jumping. This differs for `for`,
// since its own init-clause variable needs to survive a `continue` but
// not a `break`. break_jumps collects the offset of every `break`'s
// still-unpatched forward jump, patched once the loop's own bytecode is
// fully emitted and "just past the loop" is a known position.
typedef struct {
	size_t continue_target;
	int continue_depth;
	int break_depth;
	size_t *break_jumps;
	size_t break_jump_count;
	size_t break_jump_capacity;
} loop_context_t;

typedef struct compiler {
	obj_function_t *function;
	gc_heap_t *heap;
	table_t *strings;
	const char *filename;
	// Whole file's null-terminated text, kept only to render source
	// snippets in diagnostics. compiler_t does not own it.
	const char *source;
	bool had_error;
	bool is_module_def;
	bool compiling_main;
	type_t return_type;
	bool has_return_type;

	// This file's own top-level signatures (pass 1 output, shared by
	// every per-function compiler_t compiling a body in this file).
	signature_table_t *file_signatures;
	// Non-NULL only when compiling a #moduledef file: the module object
	// `export` statements populate, and the signature table handed back
	// to whatever file imports it.
	obj_module_t *home_module;
	signature_table_t *out_exports;
	// Every #module import this file has, zero or more.
	const module_unit_t *const *imported_modules;
	size_t imported_module_count;

	// This file's struct declarations, resolved before any signature or
	// function body is compiled. File-local: not threaded through module
	// exports/imports.
	const struct_registry_t *structs;

	// This file's file-private global variables, resolved before any
	// signature or function body is compiled (register_globals(), run
	// right after register_structs(). A global's declared type may
	// name a struct). File-local, same as structs: never threaded
	// through module exports/imports.
	const global_registry_t *globals;

	local_t locals[MAX_LOCALS];
	int local_count;
	int scope_depth;

	// Per-slot (not per-declaration) register-allocation eligibility,
	// built up alongside locals[] as add_local()/resolve_upvalue() run
	// and finalized onto the chunk once this function's whole body is
	// compiled (finalize_local_reg_info()). Unlike locals[]/local_count,
	// this never shrinks when a scope closes, since a slot index can be
	// reused by a different, later local in a sibling scope and this
	// needs to remember every local that ever occupied it, not just the
	// currently-live one. See add_local()'s own comment for exactly
	// what disqualifies a slot.
	bool local_reg_seen[MAX_LOCALS];
	bool local_reg_eligible[MAX_LOCALS];
	value_type_t local_reg_types[MAX_LOCALS];
	// Bytecode offset right after a (non-parameter) eligible local's own
	// declaration: current_chunk(compiler)->count at the exact point
	// note_local_reg_slot() runs, which is always right after
	// compile_var_decl()'s initializer expression has already emitted
	// its bytecode (a local's *declaration* is never a dedicated opcode
	// of its own: the initializer's pushed value just becomes the local
	// by virtue of stack position, so there's no OP_SET_LOCAL for the
	// JIT to intercept the way there is for every later
	// read/write). src/jit/jit_compile.c uses this to inject its own
	// "sync r15 from what was just pushed" at exactly that offset, so
	// it fires as real emitted code every time execution reaches it,
	// re-firing on every iteration if the declaration sits inside a
	// loop body. Meaningless
	// (never read) for a parameter slot, which the JIT instead
	// populates directly from frame->slots at function entry.
	size_t local_reg_decl_end[MAX_LOCALS];
	// Bytecode offset right after a register-eligible local's own
	// scope-closing pop (end_scope()'s own comment on why only a real,
	// permanent scope exit sets this, never a break/continue's
	// temporary emit_pop_to_depth()). (size_t)-1 (the default, matching
	// decl_end's own parameter sentinel) means the slot never explicitly
	// closes before the function itself returns -- true for a
	// parameter, and for any local declared directly in the function's
	// own outermost scope. Meaningful only for a VAL_OBJ-typed binding:
	// jit_compile.c's emit_gc_check() uses [decl_end, dead_at) as the
	// bytecode-offset window during which this slot's memory is safe to
	// flush a live register into, since outside that window the same
	// address is ordinary operand-stack space that something else may
	// already be using (see emit_gc_check()'s own comment for the two
	// concrete failure shapes this guards against).
	size_t local_reg_dead_at[MAX_LOCALS];
	int local_slot_high_water;

	// NULL for a top-level fn's own compiler_t (and for the file's
	// synthetic "<script>" compiler_t): top-level fns have no enclosing
	// scope, so resolve_upvalue() returns "not found" immediately for
	// them, degrading to exactly today's local-then-global lookup with
	// no extra work. Non-NULL only while compiling a nested fn's body,
	// pointing at the compiler_t for whichever fn (or the top-level
	// function, or another nested fn) directly contains it.
	struct compiler *enclosing;
	upvalue_t upvalues[MAX_LOCALS];
	int upvalue_count;

	loop_context_t *loops;
	int loop_count;
	int loop_capacity;
} compiler_t;

static void ensure_native_signatures(void);

static chunk_t *current_chunk(compiler_t *compiler);
static void compiler_error(compiler_t *compiler, int line, int column, int length, const char *code, const char *format, ...);
static void compiler_warning(compiler_t *compiler, int line, int column, int length, const char *code, const char *format, ...);
static void emit_byte(compiler_t *compiler, uint8_t byte, int line, int column);
static void emit_bytes(compiler_t *compiler, uint8_t b1, uint8_t b2, int line, int column);
static size_t emit_jump(compiler_t *compiler, uint8_t instruction, int line, int column);
static void patch_jump(compiler_t *compiler, size_t offset, int line);
static void emit_loop(compiler_t *compiler, size_t loop_start, int line, int column);
static void emit_constant(compiler_t *compiler, value_t value, int line, int column);
static void emit_name_op_str(compiler_t *compiler, uint8_t opcode, const char *name, size_t length, int line, int column);
static void patch_name_op(compiler_t *compiler, size_t offset, const char *name, size_t length, int line, int column);
static int64_t parse_int_literal(token_t token);
static double parse_float_literal(token_t token);

static void begin_scope(compiler_t *compiler);
static void end_scope(compiler_t *compiler, int line, int column);
static bool identifiers_equal(token_t a, token_t b);
static bool register_cacheable_value_type(type_t type, value_type_t *out_vtype);
static void note_local_reg_slot(compiler_t *compiler, int slot, type_t type);
static void disqualify_local_reg_slot(compiler_t *compiler, int slot);
static void finalize_local_reg_info(compiler_t *compiler);
static bool add_local(compiler_t *compiler, token_t name, type_t type, int line);
static bool declare_local(compiler_t *compiler, token_t name, type_t type, int line);
static void mark_initialized(compiler_t *compiler);
static int resolve_local(compiler_t *compiler, token_t name);
static int add_upvalue(compiler_t *compiler, uint8_t index, bool is_local, type_t type, int line, int column);
static int resolve_upvalue(compiler_t *compiler, token_t name);

static void begin_loop(compiler_t *compiler, size_t continue_target, int continue_depth, int break_depth);
static void end_loop(compiler_t *compiler, int line);
static void emit_pop_to_depth(compiler_t *compiler, int depth, int line, int column);
static token_t synthetic_token(const char *text);

static type_t resolve_type_ref(compiler_t *compiler, type_ref_t type_ref, int line, int column);
static void root_struct_namespace(compiler_t *compiler, const struct_def_t *def);
static void register_structs(compiler_t *script_compiler, const ast_node_t *program, struct_registry_t *structs, const module_unit_t *const *imported_modules, size_t imported_module_count);
static const module_unit_t *find_imported_module(const module_unit_t *const *imported_modules, size_t imported_module_count, const char *name, size_t name_length);

static void global_registry_init(global_registry_t *registry);
static void global_registry_free(global_registry_t *registry);
static const global_def_t *global_registry_find(const global_registry_t *registry, const char *name, size_t name_length);
static bool global_registry_add(global_registry_t *registry, const char *name, size_t name_length, type_t type, int line, int column);
static void register_globals(compiler_t *script_compiler, const ast_node_t *program, global_registry_t *globals);

static void apply_decorators(compiler_t *compiler, const ast_node_t *fn_node, signature_t *sig, size_t param_count);
static void emit_test_runner(compiler_t *script, const ast_node_t *program, const signature_table_t *file_signatures);
static signature_t **collect_signatures(compiler_t *script_compiler, const ast_node_t *program, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, table_t *file_globals, obj_module_t *home_module, signature_table_t *file_signatures);
static void collect_struct_method_signatures(compiler_t *script_compiler, const ast_node_t *struct_node, struct_def_t *def, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, table_t *file_globals, obj_module_t *home_module);
static void compile_function_decl(const ast_node_t *fn_node, const signature_t *sig, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, bool is_module_def, obj_module_t *home_module, signature_table_t *out_exports, signature_table_t *file_signatures, const module_unit_t *const *imported_modules, size_t imported_module_count, const struct_registry_t *structs, const global_registry_t *globals, bool is_struct_method, bool *had_error);
static void compile_struct_functions(const ast_node_t *struct_node, struct_def_t *def, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, bool is_module_def, obj_module_t *home_module, signature_table_t *out_exports, signature_table_t *file_signatures, const module_unit_t *const *imported_modules, size_t imported_module_count, const struct_registry_t *structs, const global_registry_t *globals, bool *had_error);
static void compile_nested_function_decl(compiler_t *compiler, const ast_node_t *node);
static void emit_closure(compiler_t *compiler, obj_function_t *function, compiler_t *inner, int line, int column);
static void compile_closure_body(compiler_t *compiler, const ast_list_t *params, ast_node_t *body, type_t *param_types, size_t param_count, type_t return_type, bool has_return_type, token_t function_name, int line, int column);
static type_t compile_value_call(compiler_t *compiler, const ast_node_t *node, type_t callee_type);

static void compile_statements(compiler_t *compiler, const ast_node_t *block_node);
static void compile_block_stmt(compiler_t *compiler, const ast_node_t *block_node);
static void compile_statement(compiler_t *compiler, const ast_node_t *node);
static void compile_var_decl(compiler_t *compiler, const ast_node_t *node);
static void compile_export_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_if_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_while_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_for_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_foreach_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_break_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_continue_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_return_stmt(compiler_t *compiler, const ast_node_t *node);
static void compile_expr_stmt(compiler_t *compiler, const ast_node_t *node);
static type_t compile_condition(compiler_t *compiler, const ast_node_t *node, const char *context);

static type_t compile_expr_typed(compiler_t *compiler, const ast_node_t *node, const type_t *expected);
static type_t compile_assign_expr(compiler_t *compiler, const ast_node_t *node);
static uint8_t compound_assign_opcode(token_type_t op);
static type_t check_arithmetic_or_bitwise(compiler_t *compiler, uint8_t opcode, type_t left_type, type_t right_type, const ast_node_t *left_node, const ast_node_t *right_node, int line, int column);
static type_t compile_compound_assign_value(compiler_t *compiler, const ast_node_t *node, type_t target_type, const ast_node_t *target_node);
static type_t compile_binary_expr(compiler_t *compiler, const ast_node_t *node);
static type_t compile_logical_expr(compiler_t *compiler, const ast_node_t *node);
static type_t compile_unary_expr(compiler_t *compiler, const ast_node_t *node);
static type_t compile_index_expr(compiler_t *compiler, const ast_node_t *node);
static type_t compile_array_literal(compiler_t *compiler, const ast_node_t *node, const type_t *expected);
static type_t compile_identifier_expr(compiler_t *compiler, const ast_node_t *node, const type_t *expected);
static bool signature_matches_func_type(const signature_t *sig, const func_type_t *ft);
static type_t compile_function_value_ref(compiler_t *compiler, const ast_node_t *node, const overload_set_t *set, const type_t *expected);
static type_t compile_lambda_expr(compiler_t *compiler, const ast_node_t *node);
static type_t compile_try_expr(compiler_t *compiler, const ast_node_t *node);
static void compile_throw_stmt(compiler_t *compiler, const ast_node_t *node);
static type_t compile_struct_literal(compiler_t *compiler, const ast_node_t *node, const type_t *expected);
static type_t compile_member_expr(compiler_t *compiler, const ast_node_t *node);
static int resolve_struct_field(compiler_t *compiler, const ast_node_t *node, const type_t *object_type, const char *name, size_t name_length, const struct_def_t **out_def);

static type_t compile_call_expr(compiler_t *compiler, const ast_node_t *node);
static type_t compile_bare_call(compiler_t *compiler, const ast_node_t *node, token_t name);
static type_t compile_member_call(compiler_t *compiler, const ast_node_t *node);
static type_t compile_stdlib_call(compiler_t *compiler, const ast_node_t *node, token_t member_name);
static type_t compile_module_call(compiler_t *compiler, const ast_node_t *node, token_t member_name, const module_unit_t *module);
static type_t compile_struct_call(compiler_t *compiler, const ast_node_t *node, const struct_def_t *def, token_t member_name);
static type_t compile_length_call(compiler_t *compiler, const ast_node_t *node);
static type_t compile_push_call(compiler_t *compiler, const ast_node_t *node);
static type_t compile_pop_call(compiler_t *compiler, const ast_node_t *node);
static type_t compile_sort_call(compiler_t *compiler, const ast_node_t *node);
static type_t finish_resolved_call(compiler_t *compiler, const signature_table_t *table, const char *name, size_t name_length, int name_line, int name_column, const ast_node_t *call_node, uint8_t patch_opcode, bool use_mangled_name, const char *prefix_name, size_t prefix_name_length, obj_t *prefix_constant);

static void ensure_native_signatures(void) {
	if (g_native_signatures_ready) {
		return;
	}
	signature_table_init(&g_native_globals);
	signature_table_init(&g_stdlib_members);
	native_signatures_init(&g_native_globals, &g_stdlib_members);
	g_native_signatures_ready = true;
}

void compiler_shutdown(void) {
	type_shutdown();

	if (!g_native_signatures_ready) {
		return;
	}
	signature_table_free(&g_native_globals);
	signature_table_free(&g_stdlib_members);
	g_native_signatures_ready = false;
}

static chunk_t *current_chunk(compiler_t *compiler) {
	return &compiler->function->chunk;
}

static void compiler_error(compiler_t *compiler, int line, int column, int length, const char *code, const char *format, ...) {
	va_list args;
	char message[512];

	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	diagnostic_emit(DIAG_ERROR, code, compiler->filename, compiler->source, line, column, length, "%s", message);
	compiler->had_error = true;
}

/* Same shape as compiler_error(), deliberately without the
   `compiler->had_error = true`, since a warning never fails compilation.
   Used by @deprecated (compile-time only; nothing here is ever checked
   against runtime state). */
static void compiler_warning(compiler_t *compiler, int line, int column, int length, const char *code, const char *format, ...) {
	va_list args;
	char message[512];

	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	diagnostic_emit(DIAG_WARNING, code, compiler->filename, compiler->source, line, column, length, "%s", message);
}

static void emit_byte(compiler_t *compiler, uint8_t byte, int line, int column) {
	chunk_write(current_chunk(compiler), byte, line, column);
}

static void emit_bytes(compiler_t *compiler, uint8_t b1, uint8_t b2, int line, int column) {
	emit_byte(compiler, b1, line, column);
	emit_byte(compiler, b2, line, column);
}

static size_t emit_jump(compiler_t *compiler, uint8_t instruction, int line, int column) {
	emit_byte(compiler, instruction, line, column);
	emit_byte(compiler, 0xff, line, column);
	emit_byte(compiler, 0xff, line, column);
	return current_chunk(compiler)->count - 2;
}

static void patch_jump(compiler_t *compiler, size_t offset, int line) {
	size_t jump;

	jump = current_chunk(compiler)->count - offset - 2;

	if (jump > UINT16_MAX) {
		compiler_error(compiler, line, 0, 0, "compiler::jump-too-large", "too much code to jump over");
		return;
	}

	current_chunk(compiler)->code[offset] = (uint8_t)((jump >> 8) & 0xff);
	current_chunk(compiler)->code[offset + 1] = (uint8_t)(jump & 0xff);
}

static void emit_loop(compiler_t *compiler, size_t loop_start, int line, int column) {
	size_t offset;
	size_t jump;

	emit_byte(compiler, OP_LOOP, line, column);

	offset = current_chunk(compiler)->count + 2;
	jump = offset - loop_start;

	if (jump > UINT16_MAX) {
		compiler_error(compiler, line, 0, 0, "compiler::loop-too-large", "loop body too large");
		jump = 0;
	}

	emit_byte(compiler, (uint8_t)((jump >> 8) & 0xff), line, column);
	emit_byte(compiler, (uint8_t)(jump & 0xff), line, column);
}

static void emit_constant(compiler_t *compiler, value_t value, int line, int column) {
	size_t index;

	index = chunk_add_constant(current_chunk(compiler), value);
	if (index > UINT16_MAX) {
		compiler_error(compiler, line, 0, 0, "compiler::too-many-constants", "too many constants in one function");
		return;
	}

	emit_byte(compiler, OP_CONSTANT, line, column);
	emit_bytes(compiler, (uint8_t)((index >> 8) & 0xff), (uint8_t)(index & 0xff), line, column);
}

static void emit_name_op_str(compiler_t *compiler, uint8_t opcode, const char *name, size_t length, int line, int column) {
	obj_string_t *string;
	size_t index;

	string = object_new_string(compiler->heap, compiler->strings, name, length);
	index = chunk_add_constant(current_chunk(compiler), value_obj((obj_t *)string));
	if (index > UINT16_MAX) {
		compiler_error(compiler, line, 0, 0, "compiler::too-many-constants", "too many constants in one function");
		return;
	}

	emit_byte(compiler, opcode, line, column);
	emit_bytes(compiler, (uint8_t)((index >> 8) & 0xff), (uint8_t)(index & 0xff), line, column);
}

/* Reserves a fixed-size opcode+operand pair now, before the operand's
   value is known, and returns its offset for patch_name_op() to fill in
   later. Used for a call's callee reference: which specific overload it
   ends up naming depends on the arguments' types, which are only known
   after compiling them, but the callee has to sit *before* the
   arguments on the stack. Mirrors emit_jump()/patch_jump()'s existing
   forward-reference pattern, just patching a constant index instead of
   a jump offset. */
static void patch_name_op(compiler_t *compiler, size_t offset, const char *name, size_t length, int line, int column) {
	obj_string_t *string;
	size_t index;

	string = object_new_string(compiler->heap, compiler->strings, name, length);
	index = chunk_add_constant(current_chunk(compiler), value_obj((obj_t *)string));
	if (index > UINT16_MAX) {
		compiler_error(compiler, line, column, 0, "compiler::too-many-constants", "too many constants in one function");
		return;
	}

	current_chunk(compiler)->code[offset + 1] = (uint8_t)((index >> 8) & 0xff);
	current_chunk(compiler)->code[offset + 2] = (uint8_t)(index & 0xff);
}

/* Token text is not null-terminated. The character right after it is not
   guaranteed to be non-numeric: strtod in particular would happily read
   past the token into whatever source text follows, for example an
   exponent suffix. Copy into a bounded buffer first, so parsing cannot
   run past the token's actual end. */
static int64_t parse_int_literal(token_t token) {
	char buffer[64];
	size_t length;

	length = token.length < sizeof(buffer) - 1 ? token.length : sizeof(buffer) - 1;
	memcpy(buffer, token.start, length);
	buffer[length] = '\0';

	return strtoll(buffer, NULL, 10);
}

static double parse_float_literal(token_t token) {
	char buffer[64];
	size_t length;

	length = token.length < sizeof(buffer) - 1 ? token.length : sizeof(buffer) - 1;
	memcpy(buffer, token.start, length);
	buffer[length] = '\0';

	return strtod(buffer, NULL);
}

static void begin_scope(compiler_t *compiler) {
	compiler->scope_depth++;
}

static void end_scope(compiler_t *compiler, int line, int column) {
	int slot;

	compiler->scope_depth--;

	while (compiler->local_count > 0 && compiler->locals[compiler->local_count - 1].depth > compiler->scope_depth) {
		if (compiler->locals[compiler->local_count - 1].is_captured) {
			emit_byte(compiler, OP_CLOSE_UPVALUE, line, column);
		} else {
			emit_byte(compiler, OP_POP, line, column);
		}
		// This slot's own memory stops being safe for emit_gc_check()
		// to flush a bound register into once its scope has genuinely
		// closed (as opposed to emit_pop_to_depth()'s break/continue
		// pop, which doesn't reach here since the scope isn't really
		// over on that path). See local_reg_dead_at's own comment.
		slot = compiler->local_count - 1;
		if (compiler->local_reg_eligible[slot]) {
			compiler->local_reg_dead_at[slot] = current_chunk(compiler)->count;
		}
		compiler->local_count--;
	}
}

static bool identifiers_equal(token_t a, token_t b) {
	if (a.length != b.length) {
		return false;
	}
	return memcmp(a.start, b.start, a.length) == 0;
}

/* type qualifies for JIT register allocation (src/jit/jit_compile.c) iff
   it's int/float/bool (VAL_INT/VAL_FLOAT/VAL_BOOL, a scalar payload with
   a fixed, permanently-baked tag), or string/struct/closure/any array
   (VAL_OBJ, a heap pointer whose logical tag src/jit/jit_compile.c's
   emit_op_get_local() reconstructs at runtime from the payload: 0 means
   null, nonzero means a real object -- see that function's own comment).
   An `int[]` local is a heap-allocated obj_array_t* at runtime despite
   kind == TYPE_INT, hence the array_depth check running before the
   switch. On success, writes the matching runtime tag through
   out_vtype and returns true. */
static bool register_cacheable_value_type(type_t type, value_type_t *out_vtype) {
	if (type.array_depth > 0) {
		*out_vtype = VAL_OBJ;
		return true;
	}
	switch (type.kind) {
		case TYPE_INT:
			*out_vtype = VAL_INT;
			return true;
		case TYPE_FLOAT:
			*out_vtype = VAL_FLOAT;
			return true;
		case TYPE_BOOL:
			*out_vtype = VAL_BOOL;
			return true;
		case TYPE_STRING:
		case TYPE_STRUCT:
		case TYPE_FUNCTION:
			*out_vtype = VAL_OBJ;
			return true;
		default:
			return false;
	}
}

/* Updates slot's register-eligibility as a new local_t claims it (see
   compiler_t's own comment on local_reg_seen/eligible/types/decl_end
   for why this is tracked separately from locals[] itself). A slot is
   eligible only if it's register-cacheable-typed AND claimed by exactly
   one local_t for the entire function: a *second* declaration reusing
   the same slot index (a sibling scope, after the first one closed)
   disqualifies it unconditionally, even if the type would have
   matched. This is stricter than
   "types agree" because the JIT's own local_reg_decl_end mechanism
   (see compiler_t's comment on it) can only ever record one
   declaration point per slot; a second one would be silently
   ignored, which type-matching alone wouldn't catch. Once
   disqualified, a slot never becomes eligible again (see also
   disqualify_local_reg_slot() below, the other way a slot loses
   eligibility after already being seen). */
static void note_local_reg_slot(compiler_t *compiler, int slot, type_t type) {
	value_type_t vtype;
	bool cacheable;

	cacheable = register_cacheable_value_type(type, &vtype);

	if (!compiler->local_reg_seen[slot]) {
		compiler->local_reg_seen[slot] = true;
		compiler->local_reg_eligible[slot] = cacheable;
		if (cacheable) {
			compiler->local_reg_types[slot] = vtype;
			compiler->local_reg_decl_end[slot] = current_chunk(compiler)->count;
			compiler->local_reg_dead_at[slot] = (size_t)-1;
		}
	} else {
		compiler->local_reg_eligible[slot] = false;
	}

	if (slot + 1 > compiler->local_slot_high_water) {
		compiler->local_slot_high_water = slot + 1;
	}
}

/* Revokes slot's register-eligibility outright, whether or not it was
   ever eligible to begin with (harmless no-op if not). Used for a
   scalar-typed slot the compiler can see a `null` literal ever flow
   into (compile_var_decl()/compile_assign_expr(), whenever the
   assigned value's static type is TYPE_NULL): unlike an object pointer,
   a scalar's payload being 0 is ambiguous with the real value 0, so
   there is no runtime sentinel available to reconstruct VAL_NULL the
   way emit_op_get_local() does for VAL_OBJ. Falling back to this slot's
   existing, already-correct memory-resident handling (a real VAL_NULL
   tag written to memory, same as any other local) is simpler than
   inventing a side channel. Only ever meaningful for a slot already
   marked local_reg_seen; called before that is a no-op since
   note_local_reg_slot() would just overwrite whatever this set. */
static void disqualify_local_reg_slot(compiler_t *compiler, int slot) {
	compiler->local_reg_eligible[slot] = false;
}

/* Copies compiler's per-slot register-eligibility table onto its own
   chunk, once this function's whole body has finished compiling (so
   local_slot_high_water reflects every slot the function ever used,
   not just whatever's still in scope). Called once per function, from
   compile_function_decl() and compile_closure_body(), mirroring how
   both already free(compiler->loops) at the same point. */
static void finalize_local_reg_info(compiler_t *compiler) {
	chunk_t *chunk;
	int count;
	int i;

	chunk = current_chunk(compiler);
	count = compiler->local_slot_high_water;
	chunk->local_slot_count = count;
	if (count == 0) {
		return;
	}

	chunk->local_reg_eligible = malloc((size_t)count * sizeof(bool));
	chunk->local_reg_types = malloc((size_t)count * sizeof(value_type_t));
	chunk->local_reg_decl_end = malloc((size_t)count * sizeof(size_t));
	chunk->local_reg_dead_at = malloc((size_t)count * sizeof(size_t));
	for (i = 0; i < count; i++) {
		chunk->local_reg_eligible[i] = compiler->local_reg_eligible[i];
		chunk->local_reg_types[i] = compiler->local_reg_types[i];
		chunk->local_reg_decl_end[i] = compiler->local_reg_decl_end[i];
		chunk->local_reg_dead_at[i] = compiler->local_reg_dead_at[i];
	}
}

/* Returns false (and reports an error) without touching compiler state
   when the locals table is full, so a caller that skips mark_initialized()
   on failure never mistakes the previous local for the one that failed
   to get a slot. */
static bool add_local(compiler_t *compiler, token_t name, type_t type, int line) {
	local_t *local;

	if (compiler->local_count >= MAX_LOCALS) {
		compiler_error(compiler, line, name.column, (int)name.length, "compiler::too-many-locals", "too many local variables in one function");
		return false;
	}

	note_local_reg_slot(compiler, compiler->local_count, type);

	local = &compiler->locals[compiler->local_count];
	local->name = name;
	local->type = type;
	local->depth = -1;
	local->is_captured = false;
	compiler->local_count++;
	return true;
}

/* A local only counts as a collision if it belongs to the current scope.
   Walking backwards, the first local from an outer, lower-depth scope
   means everything past it is legitimately shadowable.

   Returns false (and reports an error) on a duplicate name without adding
   a local. Callers must skip mark_initialized() when this returns false:
   otherwise, with no new local pushed, mark_initialized() would instead
   re-stamp the depth of whichever local already occupies the top slot. */
static bool declare_local(compiler_t *compiler, token_t name, type_t type, int line) {
	int i;

	for (i = compiler->local_count - 1; i >= 0; i--) {
		if (compiler->locals[i].depth != -1 && compiler->locals[i].depth < compiler->scope_depth) {
			break;
		}
		if (identifiers_equal(name, compiler->locals[i].name)) {
			compiler_error(compiler, line, name.column, (int)name.length, "compiler::duplicate-local", "a variable with this name already exists in this scope");
			return false;
		}
	}

	return add_local(compiler, name, type, line);
}

static void mark_initialized(compiler_t *compiler) {
	compiler->locals[compiler->local_count - 1].depth = compiler->scope_depth;
}

static int resolve_local(compiler_t *compiler, token_t name) {
	int i;

	for (i = compiler->local_count - 1; i >= 0; i--) {
		if (identifiers_equal(name, compiler->locals[i].name)) {
			if (compiler->locals[i].depth == -1) {
				compiler_error(compiler, name.line, name.column, (int)name.length, "compiler::local-read-in-own-initializer", "cannot read a local variable in its own initializer");
			}
			return i;
		}
	}

	return -1;
}

/* Records a newly resolved upvalue, or reuses an existing entry that
   already refers to the same slot (two closures created in the same
   scope capturing the same local must resolve to the same upvalue
   index, so both end up sharing one runtime obj_upvalue_t; see
   vm.c's capture_upvalue()). index/is_local together are the key: index
   alone is ambiguous between "a local slot" and "an upvalue slot of the
   enclosing function". */
static int add_upvalue(compiler_t *compiler, uint8_t index, bool is_local, type_t type, int line, int column) {
	int upvalue_count;
	int i;

	upvalue_count = compiler->upvalue_count;

	for (i = 0; i < upvalue_count; i++) {
		if (compiler->upvalues[i].index == index && compiler->upvalues[i].is_local == is_local) {
			return i;
		}
	}

	if (upvalue_count == MAX_LOCALS) {
		compiler_error(compiler, line, column, 0, "compiler::too-many-upvalues", "too many upvalues captured in one function");
		return 0;
	}

	compiler->upvalues[upvalue_count].is_local = is_local;
	compiler->upvalues[upvalue_count].index = index;
	compiler->upvalues[upvalue_count].type = type;
	compiler->upvalue_count++;
	return compiler->upvalue_count - 1;
}

/* Walks outward through enclosing compiler_ts looking for name as a
   local, threading a fresh upvalue entry through every intermediate
   function so a closure nested more than one level deep can still reach
   it (mirrors CLox's classic algorithm). Returns -1 the same way
   resolve_local() does for "not found here": either compiler has no
   enclosing scope at all (a top-level fn: nothing to capture, ever), or
   name genuinely is not declared in any enclosing scope either. */
static int resolve_upvalue(compiler_t *compiler, token_t name) {
	int local;
	int upvalue;

	if (compiler->enclosing == NULL) {
		return -1;
	}

	local = resolve_local(compiler->enclosing, name);
	if (local != -1) {
		compiler->enclosing->locals[local].is_captured = true;
		// A captured local's slot is aliased directly by
		// obj_upvalue_t->location, which a closure reads/writes as real
		// memory (see capture_upvalue(), vm.c). Register-caching it
		// in the JIT would be invisible to that access, so it's
		// permanently disqualified the moment any nested fn captures it,
		// regardless of how numeric-looking its type is.
		compiler->enclosing->local_reg_eligible[local] = false;
		return add_upvalue(compiler, (uint8_t)local, true, compiler->enclosing->locals[local].type, name.line, name.column);
	}

	upvalue = resolve_upvalue(compiler->enclosing, name);
	if (upvalue != -1) {
		return add_upvalue(compiler, (uint8_t)upvalue, false, compiler->enclosing->upvalues[upvalue].type, name.line, name.column);
	}

	return -1;
}

static void begin_loop(compiler_t *compiler, size_t continue_target, int continue_depth, int break_depth) {
	loop_context_t *loop;

	if (compiler->loop_count == compiler->loop_capacity) {
		compiler->loop_capacity = compiler->loop_capacity < 4 ? 4 : compiler->loop_capacity * 2;
		compiler->loops = realloc(compiler->loops, (size_t)compiler->loop_capacity * sizeof(loop_context_t));
	}

	loop = &compiler->loops[compiler->loop_count];
	loop->continue_target = continue_target;
	loop->continue_depth = continue_depth;
	loop->break_depth = break_depth;
	loop->break_jumps = NULL;
	loop->break_jump_count = 0;
	loop->break_jump_capacity = 0;
	compiler->loop_count++;
}

/* Patches every break in this loop to land here, so must be called once
   this loop's own bytecode, including any wrapping scope's end_scope()
   pops, is fully emitted; "here" is only the right target once nothing
   else about the loop remains to compile. */
static void end_loop(compiler_t *compiler, int line) {
	loop_context_t *loop;
	size_t i;

	loop = &compiler->loops[compiler->loop_count - 1];
	for (i = 0; i < loop->break_jump_count; i++) {
		patch_jump(compiler, loop->break_jumps[i], line);
	}
	free(loop->break_jumps);
	compiler->loop_count--;
}

/* Pops every local declared more deeply than depth, without touching
   compiler->locals[] itself: used for break/continue, an early exit
   that does not actually end the scopes those locals belong to (the
   statement after the loop still needs compiler->locals[] to reflect
   what a *non*-jumping path would see). Mirrors end_scope()'s own pop
   loop, just without the bookkeeping that assumes the scope is really
   over. */
static void emit_pop_to_depth(compiler_t *compiler, int depth, int line, int column) {
	int i;

	for (i = compiler->local_count - 1; i >= 0 && compiler->locals[i].depth > depth; i--) {
		if (compiler->locals[i].is_captured) {
			emit_byte(compiler, OP_CLOSE_UPVALUE, line, column);
		} else {
			emit_byte(compiler, OP_POP, line, column);
		}
	}
}

/* A synthetic local's name: identifiers cannot start with '$', so
   "$for_array" etc. can never collide with a real one. Used only by
   compile_foreach_stmt(), matching how the entry file's own compiled
   script gets rooted under a reserved "<script>" globals key elsewhere
   in this project for the same reason, picking a string real source can
   never produce. */
static token_t synthetic_token(const char *text) {
	token_t token;

	token.type = TOKEN_IDENTIFIER;
	token.start = text;
	token.length = strlen(text);
	token.line = 0;
	token.column = 0;
	token.error_code = NULL;

	return token;
}

/* Resolves a type_ref_t against compiler->structs, falling back to every
   one of compiler->imported_modules' exported structs when the name
   isn't declared locally, reporting an "unknown type" error (and
   returning TYPE_VOID) if it matches none of them, or an
   "ambiguous-struct-name" error (also TYPE_VOID) if it matches more than
   one import's exports. Every call site that turns a type_ref_t into a
   type_t and wants either error reported goes through this instead of
   calling type_from_type_ref() directly, so that error gets reported
   exactly once, at the position that actually names the bad type. */
static type_t resolve_type_ref(compiler_t *compiler, type_ref_t type_ref, int line, int column) {
	type_t type;
	bool found;
	bool ambiguous;

	type = type_from_type_ref(type_ref, compiler->structs, compiler->imported_modules, compiler->imported_module_count, &found, &ambiguous);
	if (ambiguous) {
		compiler_error(compiler, line, column, (int)type_ref.struct_name.length, "compiler::ambiguous-struct-name", "'%.*s' is exported by more than one imported module", (int)type_ref.struct_name.length, type_ref.struct_name.start);
	} else if (!found) {
		compiler_error(compiler, line, column, (int)type_ref.struct_name.length, "compiler::unknown-type", "unknown type '%.*s'", (int)type_ref.struct_name.length, type_ref.struct_name.start);
	}
	return type;
}

/* Linear scan by local_name, the same lookup every call site that
   dispatches on "is this identifier one of my imported modules" used to
   duplicate as a direct equality check against a single module_unit_t*,
   back when a file could have at most one #module import. NULL if name
   does not match any of compiler->imported_modules. */
static const module_unit_t *find_imported_module(const module_unit_t *const *imported_modules, size_t imported_module_count, const char *name, size_t name_length) {
	size_t i;

	for (i = 0; i < imported_module_count; i++) {
		if (imported_modules[i]->local_name_length == name_length && memcmp(imported_modules[i]->local_name, name, name_length) == 0) {
			return imported_modules[i];
		}
	}

	return NULL;
}

/* Roots def->namespace_obj into this file's own globals table (already
   an unconditional GC root) under a reserved key, mirroring exactly the
   reasoning compiler_compile()'s own "<script>" key already established:
   identifiers cannot contain '<' or '>', so this can never collide with
   real source. Nothing ever looks this key up by name at runtime;
   compile_struct_call() always pushes namespace_obj directly as a
   compile-time constant, since the compiler already holds the resolved
   struct_def_t*. This exists purely to keep the object reachable, since
   nothing else references it. */
static void root_struct_namespace(compiler_t *compiler, const struct_def_t *def) {
	char key_buf[128];
	int key_len;
	obj_string_t *key;

	key_len = snprintf(key_buf, sizeof(key_buf), "<struct %.*s>", (int)def->name_length, def->name);
	key = object_new_string(compiler->heap, compiler->strings, key_buf, (size_t)(key_len > 0 ? key_len : 0));
	table_set(compiler->function->globals, key, value_obj((obj_t *)def->namespace_obj));
}

/* Registers every top-level `struct Name { ... }` declaration, resolving
   each field's type as it goes. Runs before collect_signatures(): a
   function's parameter or return type may name a struct, so every
   struct has to already be in the registry by the time signatures are
   collected.

   A field's type may name the struct currently being registered, or any
   struct registered earlier in this same loop (struct_registry_add()
   inserts the (initially fields-less) struct_def_t before this function
   resolves its fields, so self-reference resolves too), but never a
   struct declared later in the file. Fields are just value_t slots
   (like an array's elements), so a self- or mutually-recursive struct
   type is not a representation problem; the restriction here is only
   about keeping name resolution a single, simple top-to-bottom pass. */
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
		// resolve_type_ref()), so a local struct sharing a name with an
		// imported one would otherwise just silently win with no way to
		// reach the other. Reject it outright instead, the same way
		// two local structs sharing a name already is. Checked against
		// every import, not just one: a local struct colliding with any
		// of them is equally unreachable.
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
			fields[f].type = resolve_type_ref(script_compiler, field_node->as.param.type, field_node->line, field_node->column);
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

static const global_def_t *global_registry_find(const global_registry_t *registry, const char *name, size_t name_length) {
	size_t i;

	for (i = 0; i < registry->count; i++) {
		if (registry->defs[i].name_length == name_length && memcmp(registry->defs[i].name, name, name_length) == 0) {
			return &registry->defs[i];
		}
	}
	return NULL;
}

/* Returns false (adding nothing) if name is already registered (a
   duplicate global declaration), leaving the error report to the
   caller, same division of responsibility as struct_registry_add(). */
static bool global_registry_add(global_registry_t *registry, const char *name, size_t name_length, type_t type, int line, int column) {
	global_def_t *def;

	if (global_registry_find(registry, name, name_length) != NULL) {
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

/* Registers every top-level `type name = expr;` declaration's name and
   declared type. The initializer compiles later, once every function
   signature is also known, see compiler_compile(). Runs after
   register_structs() (a global's declared type may name a struct) and
   before collect_signatures(), so a function body compiled afterward can
   already resolve any global in this file regardless of declaration
   order, the same forward-reference guarantee functions already give
   each other. A global's *initializer expression* gets no such
   guarantee (see compiler_compile()'s own comment on that): this pass
   only resolves the type annotation, which never depends on another
   global's value. */
static void register_globals(compiler_t *script_compiler, const ast_node_t *program, global_registry_t *globals) {
	size_t i;
	const ast_node_t *global_node;
	type_t declared_type;

	for (i = 0; i < program->as.program.globals.count; i++) {
		global_node = program->as.program.globals.items[i];
		declared_type = resolve_type_ref(script_compiler, global_node->as.var_decl.type, global_node->line, global_node->column);

		if (!global_registry_add(globals, global_node->as.var_decl.name.start, global_node->as.var_decl.name.length, declared_type, global_node->line, global_node->column)) {
			compiler_error(script_compiler, global_node->as.var_decl.name.line, global_node->as.var_decl.name.column, (int)global_node->as.var_decl.name.length, "compiler::duplicate-global", "'%.*s' is already declared as a global variable", (int)global_node->as.var_decl.name.length, global_node->as.var_decl.name.start);
		}
	}
}

/* Walks fn_node's decorator list (only ever non-empty for a top-level
   fn, since parse_statement() never looks for a leading '@', so a nested
   fn's own list is always empty) and applies each known name onto sig.
   An unknown name is a compile error. This matches how an unknown type
   name or identifier is already a hard error rather than something that
   resolves to a default. */
static void apply_decorators(compiler_t *compiler, const ast_node_t *fn_node, signature_t *sig, size_t param_count) {
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

/* Pass 1: parses every top-level fn's signature (name, param types,
   return type) and allocates its (still empty) obj_function_t, without
   touching any body. This is what makes forward references, recursion,
   and overload resolution across declaration order all work: by the
   time pass 2 compiles any function's body, every sibling's signature
   already exists to resolve calls against. Returns one signature_t* per
   entry in program->as.program.functions, in the same order (NULL for
   an entry whose signature was a duplicate and so never got a
   function); the caller owns the returned array (not what it points
   to, that stays owned by file_signatures). */
static signature_t **collect_signatures(compiler_t *script_compiler, const ast_node_t *program, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, table_t *file_globals, obj_module_t *home_module, signature_table_t *file_signatures) {
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

	// Registering every signature first, before resolving any of them,
	// matters: signature_table_add() can grow (and reallocate) the very
	// overload_set_t a *later* function in this same file belongs to
	// (two overloads sharing a name), which would silently invalidate a
	// signature_t* taken from an earlier call in the same loop. Once
	// every signature is registered, the table stops growing for the
	// rest of this file's compilation, so resolving (and taking stable
	// pointers) in a second pass below is safe. accepted[] separately
	// remembers which declarations were rejected as exact duplicates: a
	// rejected declaration's own (name, param types) still resolves
	// successfully in pass two (to whichever earlier declaration first
	// registered it), so resolving alone cannot tell a duplicate apart
	// from the original it collides with.
	for (i = 0; i < program->as.program.functions.count; i++) {
		fn_node = program->as.program.functions.items[i];

		param_count = fn_node->as.function_decl.params.count;
		param_types = param_count > 0 ? malloc(param_count * sizeof(type_t)) : NULL;
		for (p = 0; p < param_count; p++) {
			param_node = fn_node->as.function_decl.params.items[p];
			param_types[p] = resolve_type_ref(script_compiler, param_node->as.param.type, param_node->line, param_node->column);
		}

		if (param_count > UINT8_MAX) {
			compiler_error(script_compiler, fn_node->line, fn_node->column, 0, "compiler::too-many-parameters", "a function can have at most 255 parameters");
		}

		has_return_type = fn_node->as.function_decl.has_return_type;
		return_type = has_return_type ? resolve_type_ref(script_compiler, fn_node->as.function_decl.return_type, fn_node->line, fn_node->column) : type_void();

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
			// Already validated (and any error reported) in the pass
			// above; NULL here avoids reporting the same "unknown type"
			// error twice for an accepted signature.
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
		apply_decorators(script_compiler, fn_node, (signature_t *)sig, param_count);
		signatures[i] = (signature_t *)sig;
	}

	free(accepted);
	return signatures;
}

/* Per-struct sibling of collect_signatures() above: same two-pass shape
   (register every signature in this struct's own function list first,
   since signature_table_add() can reallocate and invalidate an
   earlier-taken pointer within the same struct's table; then resolve and
   allocate each one's obj_function_t), scoped to def->methods instead of
   the file's file_signatures. Called once per struct, after
   register_structs() has resolved every struct's fields, so a method's
   param/return type may reference any struct in the file, including
   its own (self-reference, exactly like a field already could) or one
   declared later. Struct functions do not support decorators (there is
   no grammar for one inside a struct body, see parser.c), so
   apply_decorators() is never called here, unlike collect_signatures(). */
static void collect_struct_method_signatures(compiler_t *script_compiler, const ast_node_t *struct_node, struct_def_t *def, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, table_t *file_globals, obj_module_t *home_module) {
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
			param_types[p] = resolve_type_ref(script_compiler, param_node->as.param.type, param_node->line, param_node->column);
		}

		if (param_count > UINT8_MAX) {
			compiler_error(script_compiler, fn_node->line, fn_node->column, 0, "compiler::too-many-parameters", "a function can have at most 255 parameters");
		}

		has_return_type = fn_node->as.function_decl.has_return_type;
		return_type = has_return_type ? resolve_type_ref(script_compiler, fn_node->as.function_decl.return_type, fn_node->line, fn_node->column) : type_void();

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
			// Already validated (and any error reported) in the pass
			// above; NULL here avoids reporting the same "unknown type"
			// error twice for an accepted signature.
			param_node = fn_node->as.function_decl.params.items[p];
			param_types[p] = type_from_type_ref(param_node->as.param.type, script_compiler->structs, script_compiler->imported_modules, script_compiler->imported_module_count, NULL, NULL);
		}

		signature_table_resolve(def->methods, fn_node->as.function_decl.name.start, fn_node->as.function_decl.name.length, param_types, param_count, &sig);
		free(param_types);

		fn = object_new_function(heap);
		// Qualified display name ("Point.origin") purely for nicer stack
		// traces/backtraces. It has no bearing on how a call actually
		// resolves, that's entirely def->methods (a name lookup scoped to
		// this struct) plus namespace_obj->members (a mangled-name lookup
		// scoped to this struct's own runtime object), neither of which
		// ever sees this string.
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

static void compile_function_decl(const ast_node_t *fn_node, const signature_t *sig, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, bool is_module_def, obj_module_t *home_module, signature_table_t *out_exports, signature_table_t *file_signatures, const module_unit_t *const *imported_modules, size_t imported_module_count, const struct_registry_t *structs, const global_registry_t *globals, bool is_struct_method, bool *had_error) {
	compiler_t compiler;
	size_t i;
	const ast_node_t *param;

	compiler.function = sig->function;
	compiler.heap = heap;
	compiler.strings = strings;
	compiler.filename = filename;
	compiler.source = source;
	compiler.had_error = false;
	compiler.is_module_def = is_module_def;
	// A struct function literally named "main" is not THE entry point
	// (that's only ever a real top-level fn); is_struct_method suppresses
	// this name-sniff so `export ...;` inside such a function can't
	// wrongly slip past compile_export_stmt()'s "must be a direct
	// statement in the real top-level main()" check.
	compiler.compiling_main = !is_struct_method && fn_node->as.function_decl.name.length == 4 && memcmp(fn_node->as.function_decl.name.start, "main", 4) == 0;
	compiler.return_type = sig->return_type;
	compiler.has_return_type = sig->has_return_type;
	compiler.file_signatures = file_signatures;
	compiler.home_module = home_module;
	compiler.out_exports = out_exports;
	compiler.imported_modules = imported_modules;
	compiler.imported_module_count = imported_module_count;
	compiler.structs = structs;
	compiler.globals = globals;
	compiler.local_count = 0;
	compiler.scope_depth = 0;
	memset(compiler.local_reg_seen, 0, sizeof(compiler.local_reg_seen));
	memset(compiler.local_reg_eligible, 0, sizeof(compiler.local_reg_eligible));
	memset(compiler.local_reg_decl_end, 0, sizeof(compiler.local_reg_decl_end));
	memset(compiler.local_reg_dead_at, 0, sizeof(compiler.local_reg_dead_at));
	compiler.local_slot_high_water = 0;
	// A top-level fn has no enclosing scope: nothing to capture, ever.
	compiler.enclosing = NULL;
	compiler.upvalue_count = 0;
	compiler.loops = NULL;
	compiler.loop_count = 0;
	compiler.loop_capacity = 0;

	begin_scope(&compiler);

	for (i = 0; i < fn_node->as.function_decl.params.count; i++) {
		param = fn_node->as.function_decl.params.items[i];
		// Already validated (and any error reported) during signature
		// collection; NULL avoids a duplicate "unknown type" report.
		if (declare_local(&compiler, param->as.param.name, type_from_type_ref(param->as.param.type, compiler.structs, compiler.imported_modules, compiler.imported_module_count, NULL, NULL), param->line)) {
			mark_initialized(&compiler);
		}
	}

	compile_statements(&compiler, fn_node->as.function_decl.body);

	// Implicit `return null;` if the body has no explicit return.
	// This keeps the call convention uniform: every call leaves
	// exactly one value on the caller's stack. Whether this actually
	// matches a declared return type is not checked: that would need
	// all-paths-return control-flow analysis, which is out of scope
	// here. Every *explicit* `return expr;` is checked, see
	// compile_return_stmt().
	emit_byte(&compiler, OP_NULL, fn_node->line, fn_node->column);
	emit_byte(&compiler, OP_RETURN, fn_node->line, fn_node->column);

	finalize_local_reg_info(&compiler);

	*had_error = *had_error || compiler.had_error;
	free(compiler.loops);
}

/* Compiles every struct function's body (compile_function_decl(), same as
   any top-level fn, is_struct_method=true) and, once compiled, installs
   it into the struct's own runtime namespace object. This is the
   struct-function equivalent of a top-level fn's
   OP_CONSTANT+OP_DEFINE_GLOBAL pair, just written directly into
   namespace_obj->members instead of emitted as bytecode, since nothing
   needs to *run* for this binding to take effect (mirrors exactly how
   compile_export_stmt() already does the same table_set() for a
   module's exports). Re-resolves each fn_node's exact signature by
   recomputing its param types, rather than threading an array through
   from collect_struct_method_signatures(); it is a cheap plain lookup
   at this point (no more registration happening, so no
   reallocation-invalidation risk), and keeps the two passes independent.
   Called once per struct, any time after collect_struct_method_signatures()
   has resolved every struct's method signatures (across every struct in
   the file, not just this one), needed so one struct's method body can
   call another struct's, or its own, regardless of declaration order. */
static void compile_struct_functions(const ast_node_t *struct_node, struct_def_t *def, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, bool is_module_def, obj_module_t *home_module, signature_table_t *out_exports, signature_table_t *file_signatures, const module_unit_t *const *imported_modules, size_t imported_module_count, const struct_registry_t *structs, const global_registry_t *globals, bool *had_error) {
	size_t i;
	size_t p;
	const ast_node_t *fn_node;
	const ast_node_t *param_node;
	type_t *param_types;
	size_t param_count;
	const signature_t *sig;
	signature_match_result_t match;
	obj_string_t *key;

	for (i = 0; i < struct_node->as.struct_decl.functions.count; i++) {
		fn_node = struct_node->as.struct_decl.functions.items[i];

		param_count = fn_node->as.function_decl.params.count;
		param_types = param_count > 0 ? malloc(param_count * sizeof(type_t)) : NULL;
		for (p = 0; p < param_count; p++) {
			param_node = fn_node->as.function_decl.params.items[p];
			param_types[p] = type_from_type_ref(param_node->as.param.type, structs, imported_modules, imported_module_count, NULL, NULL);
		}

		match = signature_table_resolve(def->methods, fn_node->as.function_decl.name.start, fn_node->as.function_decl.name.length, param_types, param_count, &sig);
		free(param_types);

		if (match != SIGNATURE_MATCH_OK) {
			// A duplicate declaration rejected by
			// collect_struct_method_signatures() (already reported
			// there); nothing to compile for it.
			continue;
		}

		compile_function_decl(fn_node, sig, heap, strings, filename, source, is_module_def, home_module, out_exports, file_signatures, imported_modules, imported_module_count, structs, globals, true, had_error);

		key = object_new_string(heap, strings, sig->mangled_name, strlen(sig->mangled_name));
		table_set(def->namespace_obj->members, key, value_obj((obj_t *)sig->function));
	}
}

/* Emits the OP_CLOSURE that creates inner's already-fully-compiled
   function value, into compiler's (the enclosing function's) own chunk,
   reading inner->upvalues (populated by resolve_upvalue()/add_upvalue()
   while inner's body compiled) for the descriptor bytes that follow the
   opcode. Kept separate from compile_nested_function_decl() purely to
   keep that function's own control flow linear. */
static void emit_closure(compiler_t *compiler, obj_function_t *function, compiler_t *inner, int line, int column) {
	size_t const_index;
	int i;

	const_index = chunk_add_constant(current_chunk(compiler), value_obj((obj_t *)function));
	if (const_index > UINT16_MAX) {
		compiler_error(compiler, line, column, 0, "compiler::too-many-constants", "too many constants in one function");
		return;
	}
	if (inner->upvalue_count > UINT8_MAX) {
		compiler_error(compiler, line, column, 0, "compiler::too-many-upvalues", "too many upvalues captured in one function");
		return;
	}

	emit_byte(compiler, OP_CLOSURE, line, column);
	emit_bytes(compiler, (uint8_t)((const_index >> 8) & 0xff), (uint8_t)(const_index & 0xff), line, column);
	emit_byte(compiler, (uint8_t)inner->upvalue_count, line, column);
	for (i = 0; i < inner->upvalue_count; i++) {
		emit_byte(compiler, inner->upvalues[i].is_local ? 1 : 0, line, column);
		emit_byte(compiler, inner->upvalues[i].index, line, column);
	}
}

/* Shared by compile_nested_function_decl(), compile_lambda_expr(), and
   compile_try_expr(): builds a fresh compiler_t for the function body,
   compiles it, and emits the OP_CLOSURE that produces its value into
   compiler's own chunk. param_types/param_count/return_type/
   has_return_type are already resolved by the caller (the callers
   differ in *why*; compile_nested_function_decl() needs fn_type
   before this runs, to declare_local() the fn's own name for self-
   recursion, so it can't just resolve them internally here);
   function_name only ever distinguishes a stack trace's own frame
   label ("in add()" vs. "in <lambda>()" vs. "in <try>()"), nothing
   structural depends on it. Takes params/body directly (an ast_list_t
   of AST_PARAM nodes, and the body block) rather than digging them out
   of a single ast_node_t*, since callers' own bodies live in different
   AST shapes: an AST_FUNCTION_DECL's own `.params`/`.body` for the
   first two, but a try-block's body is a bare AST_BLOCK with no
   parameters at all (params/param_count are NULL/0 there, so params is
   never actually dereferenced). */
static void compile_closure_body(compiler_t *compiler, const ast_list_t *params, ast_node_t *body, type_t *param_types, size_t param_count, type_t return_type, bool has_return_type, token_t function_name, int line, int column) {
	compiler_t inner;
	size_t i;
	const ast_node_t *param;

	inner.function = object_new_function(compiler->heap);
	inner.function->name = object_new_string(compiler->heap, compiler->strings, function_name.start, function_name.length);
	inner.function->arity = (int)param_count;
	inner.function->globals = compiler->function->globals;
	inner.function->module = compiler->function->module;
	inner.function->filename = compiler->filename;
	inner.function->source = compiler->source;

	inner.heap = compiler->heap;
	inner.strings = compiler->strings;
	inner.filename = compiler->filename;
	inner.source = compiler->source;
	inner.had_error = false;
	inner.is_module_def = compiler->is_module_def;
	inner.compiling_main = false;
	inner.return_type = return_type;
	inner.has_return_type = has_return_type;
	inner.file_signatures = compiler->file_signatures;
	inner.home_module = compiler->home_module;
	inner.out_exports = compiler->out_exports;
	inner.imported_modules = compiler->imported_modules;
	inner.imported_module_count = compiler->imported_module_count;
	inner.structs = compiler->structs;
	inner.globals = compiler->globals;
	inner.local_count = 0;
	inner.scope_depth = 0;
	memset(inner.local_reg_seen, 0, sizeof(inner.local_reg_seen));
	memset(inner.local_reg_eligible, 0, sizeof(inner.local_reg_eligible));
	memset(inner.local_reg_decl_end, 0, sizeof(inner.local_reg_decl_end));
	memset(inner.local_reg_dead_at, 0, sizeof(inner.local_reg_dead_at));
	inner.local_slot_high_water = 0;
	inner.enclosing = compiler;
	inner.upvalue_count = 0;
	inner.loops = NULL;
	inner.loop_count = 0;
	inner.loop_capacity = 0;

	begin_scope(&inner);

	for (i = 0; i < param_count; i++) {
		param = params->items[i];
		if (declare_local(&inner, param->as.param.name, param_types[i], param->line)) {
			mark_initialized(&inner);
		}
	}

	compile_statements(&inner, body);

	emit_byte(&inner, OP_NULL, line, column);
	emit_byte(&inner, OP_RETURN, line, column);

	finalize_local_reg_info(&inner);

	compiler->had_error = compiler->had_error || inner.had_error;
	free(inner.loops);

	emit_closure(compiler, inner.function, &inner, line, column);
}

/* `fn name(...) { ... }` as a statement inside a function body (as
   opposed to the identically-shaped top-level declaration
   compile_function_decl() handles). The name is declared as a local in
   the ENCLOSING scope (this compiler, not the fresh one built for the
   nested fn's own body) before that body ever compiles, specifically so
   a self-call inside it resolves as an upvalue pointing at itself.
   This is what makes self-recursion work. A forward reference to a
   sibling nested fn declared later in the same block is deliberately
   not supported: nothing hoists a nested fn's name the way top-level
   fns are hoisted by collect_signatures(), so it just resolves as an
   ordinary "undefined variable"/"undefined function" error, the same
   way referencing any not-yet-declared local would.

   Always emits OP_CLOSURE regardless of whether declare_local()
   succeeded, mirroring compile_var_decl()'s own convention: a duplicate
   name is a compile error either way, so whatever bytecode a failed
   declare_local() leaves stack-unbalanced never actually runs. */
static void compile_nested_function_decl(compiler_t *compiler, const ast_node_t *node) {
	func_type_t *ft;
	type_t fn_type;
	size_t i;
	const ast_node_t *param;
	type_t *param_types;
	size_t param_count;
	type_t return_type;
	bool has_return_type;
	token_t name;

	name = node->as.function_decl.name;
	param_count = node->as.function_decl.params.count;

	param_types = param_count > 0 ? malloc(param_count * sizeof(type_t)) : NULL;
	for (i = 0; i < param_count; i++) {
		param = node->as.function_decl.params.items[i];
		param_types[i] = resolve_type_ref(compiler, param->as.param.type, param->line, param->column);
	}

	if (param_count > UINT8_MAX) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::too-many-parameters", "a function can have at most 255 parameters");
	}

	has_return_type = node->as.function_decl.has_return_type;
	return_type = has_return_type ? resolve_type_ref(compiler, node->as.function_decl.return_type, node->line, node->column) : type_void();

	ft = type_new_func_type();
	ft->param_types = param_types;
	ft->param_count = param_count;
	ft->return_type = return_type;
	ft->has_return_type = has_return_type;

	fn_type = type_function(ft);

	{
		int slot = -1;

		if (declare_local(compiler, name, fn_type, node->line)) {
			slot = compiler->local_count - 1;
			mark_initialized(compiler);
		}

		// Unlike a plain compile_var_decl(), this local is declared
		// (for self-recursion) BEFORE its own OP_CLOSURE has even been
		// emitted, so note_local_reg_slot() (inside declare_local()
		// above) recorded local_reg_decl_end[slot] too early -- right
		// before OP_CLOSURE, not right after. Harmless while a
		// TYPE_FUNCTION local could never be register-eligible, but
		// now that it can be, a stale decl_end there would make the
		// JIT sync this slot's bound register from whatever was on the
		// stack *before* the closure was actually built. Corrected
		// here, after the fact, once OP_CLOSURE has genuinely been
		// emitted into this chunk.
		compile_closure_body(compiler, &node->as.function_decl.params, node->as.function_decl.body, param_types, param_count, return_type, has_return_type, name, node->line, node->column);

		if (slot >= 0 && compiler->local_reg_eligible[slot]) {
			compiler->local_reg_decl_end[slot] = current_chunk(compiler)->count;
		}
	}
}

/* `fn(TYPE NAME, ...) -> TYPE { ... }` in expression position (parsed by
   parse_lambda_expr() into an AST_FUNCTION_DECL node with a synthetic
   empty name; see compile_expr_typed()'s own AST_FUNCTION_DECL case).
   Unlike compile_nested_function_decl(), there is no name to declare_local()
   for self-recursion; a lambda gets a real "<lambda>" name only for its
   own stack-trace frame label, not bound to anything a program could
   reference. Returns the resulting TYPE_FUNCTION as this expression's
   type, exactly the same shape compile_function_value_ref() returns for
   a named function used as a value. */
static type_t compile_lambda_expr(compiler_t *compiler, const ast_node_t *node) {
	func_type_t *ft;
	size_t i;
	const ast_node_t *param;
	type_t *param_types;
	size_t param_count;
	type_t return_type;
	bool has_return_type;
	token_t lambda_name;

	param_count = node->as.function_decl.params.count;

	param_types = param_count > 0 ? malloc(param_count * sizeof(type_t)) : NULL;
	for (i = 0; i < param_count; i++) {
		param = node->as.function_decl.params.items[i];
		param_types[i] = resolve_type_ref(compiler, param->as.param.type, param->line, param->column);
	}

	if (param_count > UINT8_MAX) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::too-many-parameters", "a function can have at most 255 parameters");
	}

	has_return_type = node->as.function_decl.has_return_type;
	return_type = has_return_type ? resolve_type_ref(compiler, node->as.function_decl.return_type, node->line, node->column) : type_void();

	ft = type_new_func_type();
	ft->param_types = param_types;
	ft->param_count = param_count;
	ft->return_type = return_type;
	ft->has_return_type = has_return_type;

	lambda_name = (token_t){.type = TOKEN_FN, .start = "<lambda>", .length = 8, .line = node->line, .column = node->column, .error_code = NULL};
	compile_closure_body(compiler, &node->as.function_decl.params, node->as.function_decl.body, param_types, param_count, return_type, has_return_type, lambda_name, node->line, node->column);

	return type_function(ft);
}

/* `expr catch(TYPE fallback)`, or `try { ... } catch(...) { ... }` in
   either position. One AST shape (parse_call()'s postfix sugar
   desugars into the same AST_TRY_EXPR a full try/catch produces), one
   compiler path.

   The try-block always compiles as an anonymous 0-arg closure (the
   same compile_closure_body() a lambda already uses), called through
   the new OP_TRY_CALL. This opcode is required, since the only
   existing way run()'s dispatch loop reports a failure is
   vm_runtime_error() + `return VM_RUNTIME_ERROR` straight out of that
   whole run() invocation, with no way to redirect to a catch label a
   few instructions later in the *same* invocation. OP_TRY_CALL
   isolates the failure to "this one reentrant call, no further", the
   same way vm_call_value() already does for a native calling back into
   paweli. See vm.c's own comment on it.

   Bytecode shape mirrors an ordinary if/else's jump-patching exactly,
   with OP_TRY_CALL playing the role of the conditional branch:

       OP_CLOSURE <try-body>
       OP_TRY_CALL <catch>      ; success: push result, fall through
       OP_JUMP <end>            ; (success path only) skip the catch region
     <catch>:
       [bind or pop the pushed message]
       [catch_body, if any]
       [fallback_expr, or OP_NULL if no fallback]
     <end>:

   One deliberate simplification: fallback_expr compiles *after*
   end_scope() closes the catch binding's scope, so it cannot reference
   the caught message, keeping the "pop the binding, then push the
   result" stack bookkeeping simple, at the cost of the fallback not
   being able to depend on the error text. Anything that needs to do
   that belongs in catch_body instead. */
static type_t compile_try_expr(compiler_t *compiler, const ast_node_t *node) {
	type_t binding_type;
	type_t fallback_type;
	type_t fallback_actual_type;
	type_t try_return_type;
	bool try_has_return_type;
	size_t try_call_offset;
	size_t end_jump_offset;
	token_t try_name;

	try_has_return_type = node->as.try_expr.has_fallback;
	try_return_type = type_void();
	fallback_type = type_void();

	if (node->as.try_expr.has_fallback) {
		fallback_type = resolve_type_ref(compiler, node->as.try_expr.fallback_type, node->line, node->column);
		try_return_type = fallback_type;
	}

	try_name = (token_t){.type = TOKEN_TRY, .start = "<try>", .length = 5, .line = node->line, .column = node->column, .error_code = NULL};
	compile_closure_body(compiler, NULL, node->as.try_expr.try_body, NULL, 0, try_return_type, try_has_return_type, try_name, node->line, node->column);

	try_call_offset = emit_jump(compiler, OP_TRY_CALL, node->line, node->column);
	end_jump_offset = emit_jump(compiler, OP_JUMP, node->line, node->column);
	patch_jump(compiler, try_call_offset, node->line);

	begin_scope(compiler);
	if (node->as.try_expr.has_binding) {
		binding_type = resolve_type_ref(compiler, node->as.try_expr.binding_type, node->line, node->column);
		if (!type_equals(binding_type, type_string())) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::catch-binding-must-be-string", "a caught error is always a string; declare the binding as 'string'");
		}
		if (declare_local(compiler, node->as.try_expr.binding_name, type_string(), node->line)) {
			mark_initialized(compiler);
		}
	} else {
		emit_byte(compiler, OP_POP, node->line, node->column);
	}

	if (node->as.try_expr.catch_body != NULL) {
		compile_statement(compiler, node->as.try_expr.catch_body);
	}

	end_scope(compiler, node->line, node->column);

	if (node->as.try_expr.has_fallback) {
		fallback_actual_type = compile_expr_typed(compiler, node->as.try_expr.fallback_expr, &fallback_type);
		if (!type_is_assignable(fallback_type, fallback_actual_type)) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "catch's fallback must be %s, got %s", type_to_string(fallback_type), type_to_string(fallback_actual_type));
		}
	} else {
		emit_byte(compiler, OP_NULL, node->line, node->column);
	}

	patch_jump(compiler, end_jump_offset, node->line);

	return try_has_return_type ? fallback_type : type_void();
}

static void compile_throw_stmt(compiler_t *compiler, const ast_node_t *node) {
	type_t value_type;

	value_type = compile_expr_typed(compiler, node->as.throw_stmt.value, NULL);
	if (!type_equals(value_type, type_string())) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "throw expects a string, got %s", type_to_string(value_type));
	}

	emit_byte(compiler, OP_THROW, node->line, node->column);
}

static void compile_statements(compiler_t *compiler, const ast_node_t *block_node) {
	size_t i;

	for (i = 0; i < block_node->as.block.statements.count; i++) {
		compile_statement(compiler, block_node->as.block.statements.items[i]);
	}
}

static void compile_block_stmt(compiler_t *compiler, const ast_node_t *block_node) {
	begin_scope(compiler);
	compile_statements(compiler, block_node);
	end_scope(compiler, block_node->line, block_node->column);
}

static void compile_statement(compiler_t *compiler, const ast_node_t *node) {
	switch (node->kind) {
		case AST_VAR_DECL:
			compile_var_decl(compiler, node);
			break;
		case AST_FUNCTION_DECL:
			compile_nested_function_decl(compiler, node);
			break;
		case AST_EXPORT_STMT:
			compile_export_stmt(compiler, node);
			break;
		case AST_IF_STMT:
			compile_if_stmt(compiler, node);
			break;
		case AST_WHILE_STMT:
			compile_while_stmt(compiler, node);
			break;
		case AST_FOR_STMT:
			compile_for_stmt(compiler, node);
			break;
		case AST_FOREACH_STMT:
			compile_foreach_stmt(compiler, node);
			break;
		case AST_BREAK_STMT:
			compile_break_stmt(compiler, node);
			break;
		case AST_CONTINUE_STMT:
			compile_continue_stmt(compiler, node);
			break;
		case AST_RETURN_STMT:
			compile_return_stmt(compiler, node);
			break;
		case AST_EXPR_STMT:
			compile_expr_stmt(compiler, node);
			break;
		case AST_THROW_STMT:
			compile_throw_stmt(compiler, node);
			break;
		case AST_TRY_EXPR:
			// Reached when parse_statement() parses a `try` directly
			// (its TOKEN_TRY case returns the AST_TRY_EXPR node itself,
			// not wrapped in an AST_EXPR_STMT the way an ordinary bare
			// expression-as-statement would be), same "compile it,
			// then discard whatever value it produced" treatment
			// compile_expr_stmt() already gives any other expression
			// used as a statement.
			compile_expr_typed(compiler, node, NULL);
			emit_byte(compiler, OP_POP, node->line, node->column);
			break;
		case AST_BLOCK:
			compile_block_stmt(compiler, node);
			break;
		default:
			compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-statement", "expected a statement");
			break;
	}
}

static void compile_var_decl(compiler_t *compiler, const ast_node_t *node) {
	type_t declared_type;
	type_t initializer_type;

	declared_type = resolve_type_ref(compiler, node->as.var_decl.type, node->line, node->column);
	initializer_type = compile_expr_typed(compiler, node->as.var_decl.initializer, &declared_type);

	if (!type_is_assignable(declared_type, initializer_type)) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot initialize a %s variable with a %s value", type_to_string(declared_type), type_to_string(initializer_type));
	}

	if (declare_local(compiler, node->as.var_decl.name, declared_type, node->line)) {
		mark_initialized(compiler);
		if (initializer_type.kind == TYPE_NULL) {
			value_type_t vtype;

			if (register_cacheable_value_type(declared_type, &vtype) && vtype != VAL_OBJ) {
				disqualify_local_reg_slot(compiler, compiler->local_count - 1);
			}
		}
	}
}

/* export is a purely compile-time declaration. It copies the signature
   (and, for the module's own runtime use, the already-compiled
   obj_function_t) of every overload of the named function into the
   module's public interface, and emits no bytecode.
   Restricted to a direct top-level statement in main(), not nested in
   any conditional or loop: since it no longer runs at import time,
   `if (x) { export y; }` would be misleading (y ends up exported
   unconditionally either way), so it is rejected outright instead. */
static void compile_export_stmt(compiler_t *compiler, const ast_node_t *node) {
	overload_set_t *set;
	size_t i;
	obj_string_t *key;
	const signature_t *sig;
	struct_def_t *struct_def;

	if (!compiler->is_module_def) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::export-outside-module", "'export' can only be used in a file with a #moduledef directive");
		return;
	}

	if (!compiler->compiling_main || compiler->scope_depth != 1) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::export-must-be-unconditional", "'export' must be a direct statement in main(), not nested in a conditional or loop");
		return;
	}

	set = signature_table_find_set(compiler->file_signatures, node->as.export_stmt.name.start, node->as.export_stmt.name.length);
	if (set == NULL) {
		// Not a function name: a struct declared in this file is the only
		// other thing `export` can name. Exporting a struct is purely a
		// compile-time flag flip (is_exported), the same way exporting a
		// function is purely a compile-time signature copy; neither
		// runs anything, matching how this whole statement works.
		struct_def = struct_registry_find(compiler->structs, node->as.export_stmt.name.start, node->as.export_stmt.name.length);
		if (struct_def == NULL) {
			compiler_error(compiler, node->as.export_stmt.name.line, node->as.export_stmt.name.column, (int)node->as.export_stmt.name.length, "compiler::undefined-variable", "cannot export undefined name '%.*s'", (int)node->as.export_stmt.name.length, node->as.export_stmt.name.start);
			return;
		}
		struct_def->is_exported = true;
		return;
	}

	for (i = 0; i < set->overload_count; i++) {
		signature_t *exported_sig;

		sig = &set->overloads[i];
		key = object_new_string(compiler->heap, compiler->strings, sig->mangled_name, strlen(sig->mangled_name));
		table_set(compiler->home_module->members, key, value_obj((obj_t *)sig->function));
		exported_sig = signature_table_add(compiler->out_exports, node->as.export_stmt.name.start, node->as.export_stmt.name.length, sig->param_types, sig->param_count, sig->return_type, sig->has_return_type);
		// @deprecated crosses the module boundary too: an importer
		// calling a deprecated exported overload should still see the
		// warning, not just code inside the exporting file itself.
		// deprecated_message stays a borrowed pointer into this file's
		// own source text, which module.c keeps alive (via
		// vm_track_owned_source()) for the rest of the process, well
		// past the point any importer might read it.
		exported_sig->is_deprecated = sig->is_deprecated;
		exported_sig->deprecated_message = sig->deprecated_message;
		exported_sig->deprecated_message_length = sig->deprecated_message_length;
	}
}

static type_t compile_condition(compiler_t *compiler, const ast_node_t *node, const char *context) {
	type_t bool_hint;
	type_t actual;

	bool_hint = type_bool();
	actual = compile_expr_typed(compiler, node, &bool_hint);
	if (!type_equals(actual, type_bool())) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", context);
	}
	return actual;
}

static void compile_if_stmt(compiler_t *compiler, const ast_node_t *node) {
	size_t then_jump;
	size_t else_jump;

	compile_condition(compiler, node->as.if_stmt.condition, "'if' condition must be bool");

	then_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
	emit_byte(compiler, OP_POP, node->line, node->column);
	compile_block_stmt(compiler, node->as.if_stmt.then_branch);

	else_jump = emit_jump(compiler, OP_JUMP, node->line, node->column);

	patch_jump(compiler, then_jump, node->line);
	emit_byte(compiler, OP_POP, node->line, node->column);

	if (node->as.if_stmt.else_branch != NULL) {
		if (node->as.if_stmt.else_branch->kind == AST_IF_STMT) {
			compile_if_stmt(compiler, node->as.if_stmt.else_branch);
		} else {
			compile_block_stmt(compiler, node->as.if_stmt.else_branch);
		}
	}

	patch_jump(compiler, else_jump, node->line);
}

static void compile_while_stmt(compiler_t *compiler, const ast_node_t *node) {
	size_t loop_start;
	size_t exit_jump;

	loop_start = current_chunk(compiler)->count;

	compile_condition(compiler, node->as.while_stmt.condition, "'while' condition must be bool");
	exit_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
	emit_byte(compiler, OP_POP, node->line, node->column);

	// A `while` has no scope of its own beyond the body's: `break` and
	// `continue` both only ever need to pop the body's own locals.
	begin_loop(compiler, loop_start, compiler->scope_depth, compiler->scope_depth);
	compile_block_stmt(compiler, node->as.while_stmt.body);

	emit_loop(compiler, loop_start, node->line, node->column);

	patch_jump(compiler, exit_jump, node->line);
	emit_byte(compiler, OP_POP, node->line, node->column);

	end_loop(compiler, node->line);
}

static void compile_for_stmt(compiler_t *compiler, const ast_node_t *node) {
	size_t loop_start;
	bool has_exit_jump;
	size_t exit_jump;
	size_t body_jump;
	size_t increment_start;

	begin_scope(compiler);

	if (node->as.for_stmt.initializer != NULL) {
		compile_statement(compiler, node->as.for_stmt.initializer);
	}

	loop_start = current_chunk(compiler)->count;

	has_exit_jump = false;
	exit_jump = 0;
	if (node->as.for_stmt.condition != NULL) {
		compile_condition(compiler, node->as.for_stmt.condition, "'for' condition must be bool");
		exit_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
		emit_byte(compiler, OP_POP, node->line, node->column);
		has_exit_jump = true;
	}

	if (node->as.for_stmt.increment != NULL) {
		body_jump = emit_jump(compiler, OP_JUMP, node->line, node->column);
		increment_start = current_chunk(compiler)->count;

		compile_expr_typed(compiler, node->as.for_stmt.increment, NULL);
		emit_byte(compiler, OP_POP, node->line, node->column);

		emit_loop(compiler, loop_start, node->line, node->column);
		loop_start = increment_start;
		patch_jump(compiler, body_jump, node->line);
	}

	// `continue` must leave the init-clause variable alone (it re-runs
	// the increment/condition, which need it), but `break` truly leaves
	// the loop, so it also has to pop that variable, one scope
	// shallower than continue_depth, matching the begin_scope() above.
	begin_loop(compiler, loop_start, compiler->scope_depth, compiler->scope_depth - 1);
	compile_block_stmt(compiler, node->as.for_stmt.body);

	emit_loop(compiler, loop_start, node->line, node->column);

	if (has_exit_jump) {
		patch_jump(compiler, exit_jump, node->line);
		emit_byte(compiler, OP_POP, node->line, node->column);
	}

	// end_scope() has to run before end_loop(): break already popped
	// its way down to the pre-loop depth inline (see compile_break_stmt),
	// so its jump target must land *after* end_scope()'s own pops for
	// the non-breaking path, not before; otherwise the same locals
	// would get popped twice on the path that broke out.
	end_scope(compiler, node->line, node->column);
	end_loop(compiler, node->line);
}

/* `for (T name : array_expr) body`, over an array's elements or a
   string's characters (stdlib.length()/OP_GET_INDEX already handle
   both, so string iteration falls out for free). Cannot be desugared
   into ordinary AST nodes at parse time the way the range form is:
   array_expr's element type is not known until it is compiled, so
   there is no type_ref_t to give a hidden "$for_array" var_decl until
   that has already happened. Instead this hand-emits bytecode that
   mirrors compile_for_stmt()'s already-verified structure exactly
   (condition check, jump-over-increment, increment, loop-back,
   begin_loop()/end_loop() at the same relative point), just counting
   over a hidden index into a hidden copy of the array/string instead of
   whatever the user's own three clauses would have said. */
static void compile_foreach_stmt(compiler_t *compiler, const ast_node_t *node) {
	type_t array_type;
	type_t declared_type;
	token_t array_token;
	token_t index_token;
	int array_slot;
	int index_slot;
	size_t loop_start;
	size_t exit_jump;
	size_t body_jump;
	size_t increment_start;

	begin_scope(compiler);

	array_token = synthetic_token("$for_array");
	array_type = compile_expr_typed(compiler, node->as.foreach_stmt.array_expr, NULL);
	if (declare_local(compiler, array_token, array_type, node->line)) {
		mark_initialized(compiler);
	}
	array_slot = compiler->local_count - 1;

	declared_type = resolve_type_ref(compiler, node->as.foreach_stmt.type, node->line, node->column);
	if (array_type.array_depth > 0) {
		type_t element_type = type_element_of(array_type);
		if (!type_equals(declared_type, element_type)) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "loop variable is declared %s but the array holds %s", type_to_string(declared_type), type_to_string(element_type));
		}
	} else if (type_equals(array_type, type_string())) {
		if (!type_equals(declared_type, type_string())) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "iterating a string yields string, but the loop variable is declared %s", type_to_string(declared_type));
		}
	} else {
		compiler_error(compiler, node->as.foreach_stmt.array_expr->line, node->as.foreach_stmt.array_expr->column, 0, "compiler::not-indexable", "'for (T x : ...)' requires an array or a string, got %s", type_to_string(array_type));
	}

	index_token = synthetic_token("$for_index");
	emit_constant(compiler, value_int(0), node->line, node->column);
	if (declare_local(compiler, index_token, type_int(), node->line)) {
		mark_initialized(compiler);
	}
	index_slot = compiler->local_count - 1;

	loop_start = current_chunk(compiler)->count;

	// condition: $for_index < stdlib.length($for_array)
	emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)index_slot, node->line, node->column);
	emit_name_op_str(compiler, OP_GET_GLOBAL, "stdlib", 6, node->line, node->column);
	emit_name_op_str(compiler, OP_GET_PROPERTY, "length", 6, node->line, node->column);
	emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)array_slot, node->line, node->column);
	emit_bytes(compiler, OP_CALL, 1, node->line, node->column);
	emit_byte(compiler, OP_LESS, node->line, node->column);

	exit_jump = emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
	emit_byte(compiler, OP_POP, node->line, node->column);

	// increment: $for_index = $for_index + 1
	body_jump = emit_jump(compiler, OP_JUMP, node->line, node->column);
	increment_start = current_chunk(compiler)->count;

	emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)index_slot, node->line, node->column);
	emit_constant(compiler, value_int(1), node->line, node->column);
	emit_byte(compiler, OP_ADD, node->line, node->column);
	emit_bytes(compiler, OP_SET_LOCAL, (uint8_t)index_slot, node->line, node->column);
	emit_byte(compiler, OP_POP, node->line, node->column);

	emit_loop(compiler, loop_start, node->line, node->column);
	loop_start = increment_start;
	patch_jump(compiler, body_jump, node->line);

	// break must also pop $for_array/$for_index (one scope shallower
	// than continue, exactly like compile_for_stmt's own init-clause
	// variable), since it leaves the loop entirely; continue only needs
	// the per-iteration loop-variable scope popped, see below.
	begin_loop(compiler, loop_start, compiler->scope_depth, compiler->scope_depth - 1);

	begin_scope(compiler);
	emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)array_slot, node->line, node->column);
	emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)index_slot, node->line, node->column);
	emit_byte(compiler, OP_GET_INDEX, node->line, node->column);
	if (declare_local(compiler, node->as.foreach_stmt.name, declared_type, node->line)) {
		mark_initialized(compiler);
	}
	compile_statements(compiler, node->as.foreach_stmt.body);
	end_scope(compiler, node->line, node->column);

	emit_loop(compiler, loop_start, node->line, node->column);

	patch_jump(compiler, exit_jump, node->line);
	emit_byte(compiler, OP_POP, node->line, node->column);

	end_scope(compiler, node->line, node->column);
	end_loop(compiler, node->line);
}

static void compile_break_stmt(compiler_t *compiler, const ast_node_t *node) {
	loop_context_t *loop;
	size_t jump;

	if (compiler->loop_count == 0) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::break-outside-loop", "'break' can only be used inside a loop");
		return;
	}

	loop = &compiler->loops[compiler->loop_count - 1];
	emit_pop_to_depth(compiler, loop->break_depth, node->line, node->column);
	jump = emit_jump(compiler, OP_JUMP, node->line, node->column);

	if (loop->break_jump_count == loop->break_jump_capacity) {
		loop->break_jump_capacity = loop->break_jump_capacity < 4 ? 4 : loop->break_jump_capacity * 2;
		loop->break_jumps = realloc(loop->break_jumps, loop->break_jump_capacity * sizeof(size_t));
	}
	loop->break_jumps[loop->break_jump_count++] = jump;
}

static void compile_continue_stmt(compiler_t *compiler, const ast_node_t *node) {
	loop_context_t *loop;

	if (compiler->loop_count == 0) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::continue-outside-loop", "'continue' can only be used inside a loop");
		return;
	}

	loop = &compiler->loops[compiler->loop_count - 1];
	emit_pop_to_depth(compiler, loop->continue_depth, node->line, node->column);
	emit_loop(compiler, loop->continue_target, node->line, node->column);
}

static void compile_return_stmt(compiler_t *compiler, const ast_node_t *node) {
	type_t value_type;

	if (node->as.return_stmt.value != NULL) {
		if (!compiler->has_return_type) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::return-value-without-type", "cannot return a value from a function with no declared '-> type'");
			compile_expr_typed(compiler, node->as.return_stmt.value, NULL);
		} else {
			value_type = compile_expr_typed(compiler, node->as.return_stmt.value, &compiler->return_type);
			if (!type_is_assignable(compiler->return_type, value_type)) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "returned a %s value but this function is declared to return %s", type_to_string(value_type), type_to_string(compiler->return_type));
			}
		}
	} else {
		emit_byte(compiler, OP_NULL, node->line, node->column);
	}
	emit_byte(compiler, OP_RETURN, node->line, node->column);
}

static void compile_expr_stmt(compiler_t *compiler, const ast_node_t *node) {
	compile_expr_typed(compiler, node->as.expr_stmt.expression, NULL);
	emit_byte(compiler, OP_POP, node->line, node->column);
}

static type_t compile_expr_typed(compiler_t *compiler, const ast_node_t *node, const type_t *expected) {
	obj_string_t *string;

	switch (node->kind) {
		case AST_ASSIGN_EXPR:
			return compile_assign_expr(compiler, node);
		case AST_BINARY_EXPR:
			if (node->as.binary_expr.op == TOKEN_AMP_AMP || node->as.binary_expr.op == TOKEN_PIPE_PIPE) {
				return compile_logical_expr(compiler, node);
			}
			return compile_binary_expr(compiler, node);
		case AST_UNARY_EXPR:
			return compile_unary_expr(compiler, node);
		case AST_CALL_EXPR:
			return compile_call_expr(compiler, node);
		case AST_MEMBER_EXPR:
			return compile_member_expr(compiler, node);
		case AST_INDEX_EXPR:
			return compile_index_expr(compiler, node);
		case AST_ARRAY_LITERAL:
			return compile_array_literal(compiler, node, expected);
		case AST_STRUCT_LITERAL:
			return compile_struct_literal(compiler, node, expected);
		case AST_IDENTIFIER_EXPR:
			return compile_identifier_expr(compiler, node, expected);
		case AST_FUNCTION_DECL:
			return compile_lambda_expr(compiler, node);
		case AST_TRY_EXPR:
			return compile_try_expr(compiler, node);
		case AST_INT_LITERAL:
			emit_constant(compiler, value_int(parse_int_literal(node->as.literal.token)), node->line, node->column);
			return type_int();
		case AST_FLOAT_LITERAL:
			emit_constant(compiler, value_float(parse_float_literal(node->as.literal.token)), node->line, node->column);
			return type_float();
		case AST_STRING_LITERAL:
			string = object_new_string_from_literal(compiler->heap, compiler->strings, node->as.literal.token.start + 1, node->as.literal.token.length - 2);
			emit_constant(compiler, value_obj((obj_t *)string), node->line, node->column);
			return type_string();
		case AST_BOOL_LITERAL:
			emit_byte(compiler, node->as.literal.token.type == TOKEN_TRUE ? OP_TRUE : OP_FALSE, node->line, node->column);
			return type_bool();
		case AST_NULL_LITERAL:
			emit_byte(compiler, OP_NULL, node->line, node->column);
			return type_null();
		default:
			compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-expression", "expected an expression");
			return type_void();
	}
}

/* Shared tail for a compound-assignment target (`x += y`, `arr[i] &= y`,
   `s.f <<= y`): the caller has already emitted whatever bytecode reads
   the target's CURRENT value onto the stack (OP_GET_LOCAL/
   OP_GET_UPVALUE for an identifier; OP_DUP_N + OP_GET_INDEX/OP_GET_FIELD
   for an index/member target, so the object/index/field sub-expressions
   are not re-evaluated). This compiles the right-hand side, emits the
   implied binary opcode (see compound_assign_opcode()), and checks the
   result can be stored back into target_type. It does NOT emit the
   final OP_SET_*, since which one that is depends on the target kind,
   which only the caller knows. Always returns target_type, matching
   plain assignment's own convention that an assignment expression's
   value is the target's declared type. */
static type_t compile_compound_assign_value(compiler_t *compiler, const ast_node_t *node, type_t target_type, const ast_node_t *target_node) {
	type_t value_type;
	uint8_t opcode;
	type_t result_type;

	value_type = compile_expr_typed(compiler, node->as.assign_expr.value, NULL);
	opcode = compound_assign_opcode(node->as.assign_expr.op);
	result_type = check_arithmetic_or_bitwise(compiler, opcode, target_type, value_type, target_node, node->as.assign_expr.value, node->line, node->column);
	emit_byte(compiler, opcode, node->line, node->column);

	if (!type_is_assignable(target_type, result_type)) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot assign a %s result back into a %s target", type_to_string(result_type), type_to_string(target_type));
	}

	return target_type;
}

static type_t compile_assign_expr(compiler_t *compiler, const ast_node_t *node) {
	int slot;
	type_t target_type;
	type_t value_type;
	type_t object_type;
	type_t index_type;
	type_t element_expected;
	const ast_node_t *target;
	int field_index;
	const struct_def_t *def;
	bool is_compound;

	target = node->as.assign_expr.target;
	is_compound = node->as.assign_expr.op != TOKEN_EQUAL;

	if (target->kind == AST_IDENTIFIER_EXPR) {
		slot = resolve_local(compiler, target->as.identifier_expr.name);
		if (slot == -1) {
			int upvalue = resolve_upvalue(compiler, target->as.identifier_expr.name);

			if (upvalue != -1) {
				target_type = compiler->upvalues[upvalue].type;
				if (is_compound) {
					emit_bytes(compiler, OP_GET_UPVALUE, (uint8_t)upvalue, node->line, node->column);
					compile_compound_assign_value(compiler, node, target_type, target);
				} else {
					value_type = compile_expr_typed(compiler, node->as.assign_expr.value, &target_type);
					if (!type_is_assignable(target_type, value_type)) {
						compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot assign a %s value to a %s variable", type_to_string(value_type), type_to_string(target_type));
					}
				}
				emit_bytes(compiler, OP_SET_UPVALUE, (uint8_t)upvalue, node->line, node->column);
				return target_type;
			}

			{
				const global_def_t *global_def = global_registry_find(compiler->globals, target->as.identifier_expr.name.start, target->as.identifier_expr.name.length);

				if (global_def != NULL) {
					target_type = global_def->type;
					if (is_compound) {
						emit_name_op_str(compiler, OP_GET_GLOBAL, target->as.identifier_expr.name.start, target->as.identifier_expr.name.length, node->line, node->column);
						compile_compound_assign_value(compiler, node, target_type, target);
					} else {
						value_type = compile_expr_typed(compiler, node->as.assign_expr.value, &target_type);
						if (!type_is_assignable(target_type, value_type)) {
							compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot assign a %s value to a %s variable", type_to_string(value_type), type_to_string(target_type));
						}
					}
					emit_name_op_str(compiler, OP_SET_GLOBAL, target->as.identifier_expr.name.start, target->as.identifier_expr.name.length, node->line, node->column);
					return target_type;
				}
			}

			compiler_error(compiler, node->line, node->column, 0, "compiler::undeclared-assignment-target", "cannot assign to an undeclared or non-local variable");
			compile_expr_typed(compiler, node->as.assign_expr.value, NULL);
			return type_void();
		}
		target_type = compiler->locals[slot].type;
		if (is_compound) {
			emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)slot, node->line, node->column);
			compile_compound_assign_value(compiler, node, target_type, target);
		} else {
			value_type = compile_expr_typed(compiler, node->as.assign_expr.value, &target_type);
			if (!type_is_assignable(target_type, value_type)) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot assign a %s value to a %s variable", type_to_string(value_type), type_to_string(target_type));
			}
			// A later `x = null;` is just as ambiguous for a register-
			// cached scalar as an initializer's `null` is (see
			// disqualify_local_reg_slot()'s own comment); compound
			// assignment can never produce TYPE_NULL, so only this
			// branch needs the check.
			if (value_type.kind == TYPE_NULL) {
				value_type_t vtype;

				if (register_cacheable_value_type(target_type, &vtype) && vtype != VAL_OBJ) {
					disqualify_local_reg_slot(compiler, slot);
				}
			}
		}
		emit_bytes(compiler, OP_SET_LOCAL, (uint8_t)slot, node->line, node->column);
		return target_type;
	}

	if (target->kind == AST_INDEX_EXPR) {
		object_type = compile_expr_typed(compiler, target->as.index_expr.object, NULL);

		if (type_equals(object_type, type_string())) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::string-immutable", "cannot assign into a string index; strings are immutable");
		} else if (object_type.array_depth == 0) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::not-indexable", "'[]' assignment only works on arrays, got %s", type_to_string(object_type));
		}

		element_expected = type_int();
		index_type = compile_expr_typed(compiler, target->as.index_expr.index, &element_expected);
		if (!type_equals(index_type, type_int())) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::index-not-integer", "array index must be an int");
		}

		element_expected = type_element_of(object_type);

		if (is_compound) {
			// [object, index] -> [object, index, object, index] -> [object, index, current]
			emit_bytes(compiler, OP_DUP_N, 2, node->line, node->column);
			emit_byte(compiler, OP_GET_INDEX, node->line, node->column);
			value_type = compile_compound_assign_value(compiler, node, element_expected, target);
		} else if (object_type.array_depth > 0) {
			value_type = compile_expr_typed(compiler, node->as.assign_expr.value, &element_expected);
			if (!type_is_assignable(element_expected, value_type)) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot assign a %s into an array of %s", type_to_string(value_type), type_to_string(element_expected));
			}
		} else {
			value_type = compile_expr_typed(compiler, node->as.assign_expr.value, NULL);
		}

		emit_byte(compiler, OP_SET_INDEX, node->line, node->column);
		return value_type;
	}

	if (target->kind == AST_MEMBER_EXPR) {
		object_type = compile_expr_typed(compiler, target->as.member_expr.object, NULL);

		if (object_type.kind != TYPE_STRUCT || object_type.array_depth > 0) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-member-assignment", "'.' assignment only works on struct fields");
			compile_expr_typed(compiler, node->as.assign_expr.value, NULL);
			return type_void();
		}

		field_index = resolve_struct_field(compiler, target, &object_type, target->as.member_expr.name.start, target->as.member_expr.name.length, &def);
		if (field_index < 0) {
			compile_expr_typed(compiler, node->as.assign_expr.value, NULL);
			return type_void();
		}

		target_type = def->fields[field_index].type;

		if (is_compound) {
			// [object] -> [object, object] -> [object, current]
			emit_bytes(compiler, OP_DUP_N, 1, node->line, node->column);
			emit_bytes(compiler, OP_GET_FIELD, (uint8_t)field_index, node->line, node->column);
			compile_compound_assign_value(compiler, node, target_type, target);
		} else {
			value_type = compile_expr_typed(compiler, node->as.assign_expr.value, &target_type);
			if (!type_is_assignable(target_type, value_type)) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot assign a %s value to field '%.*s' of type %s", type_to_string(value_type), (int)def->fields[field_index].name_length, def->fields[field_index].name, type_to_string(target_type));
			}
		}

		emit_bytes(compiler, OP_SET_FIELD, (uint8_t)field_index, node->line, node->column);
		return target_type;
	}

	compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-member-assignment", "assignment to member expressions is not supported yet");
	compile_expr_typed(compiler, node->as.assign_expr.value, NULL);
	return type_void();
}

/* Maps a compound-assignment token (TOKEN_PLUS_EQUAL, ...) to the plain
   binary opcode it implies (OP_ADD, ...). Only ever called with one of
   the ten TOKEN_*_EQUAL compound tokens; the default case is
   unreachable in practice, kept only so the switch doesn't need a
   bogus extra "can't happen" return path. */
static uint8_t compound_assign_opcode(token_type_t op) {
	switch (op) {
		case TOKEN_PLUS_EQUAL: return OP_ADD;
		case TOKEN_MINUS_EQUAL: return OP_SUBTRACT;
		case TOKEN_STAR_EQUAL: return OP_MULTIPLY;
		case TOKEN_SLASH_EQUAL: return OP_DIVIDE;
		case TOKEN_PERCENT_EQUAL: return OP_MODULO;
		case TOKEN_AMP_EQUAL: return OP_BIT_AND;
		case TOKEN_PIPE_EQUAL: return OP_BIT_OR;
		case TOKEN_CARET_EQUAL: return OP_BIT_XOR;
		case TOKEN_LESS_LESS_EQUAL: return OP_SHIFT_LEFT;
		case TOKEN_GREATER_GREATER_EQUAL: return OP_SHIFT_RIGHT;
		default: return OP_ADD;
	}
}

/* Type-checks left_type/right_type against opcode and returns the
   operation's result type, reporting a compiler::type-mismatch at
   left_node's/right_node's own position for whichever side is wrong
   (line/column are only used for the one error, modulo-on-float,
   that doesn't clearly belong to either operand alone). Shared by
   compile_binary_expr() (for the plain `+ - * / % & | ^ << >>` forms)
   and compile_assign_expr()'s three compound-assignment branches (the
   same operators via `+= -= *= ...`): equality and comparison operators
   never participate in compound assignment, so they stay entirely in
   compile_binary_expr() and are not handled here. */
static type_t check_arithmetic_or_bitwise(compiler_t *compiler, uint8_t opcode, type_t left_type, type_t right_type, const ast_node_t *left_node, const ast_node_t *right_node, int line, int column) {
	bool is_bitwise;

	// Bitwise/shift: int-only, with no float-promotion path at all.
	// Unlike +, -, *, /, a float operand here is always a type error,
	// never silently accepted.
	is_bitwise = opcode == OP_BIT_AND || opcode == OP_BIT_OR || opcode == OP_BIT_XOR || opcode == OP_SHIFT_LEFT || opcode == OP_SHIFT_RIGHT;
	if (is_bitwise) {
		if (!type_equals(left_type, type_int())) {
			compiler_error(compiler, left_node->line, left_node->column, 0, "compiler::type-mismatch", "left operand must be int, got %s", type_to_string(left_type));
		}
		if (!type_equals(right_type, type_int())) {
			compiler_error(compiler, right_node->line, right_node->column, 0, "compiler::type-mismatch", "right operand must be int, got %s", type_to_string(right_type));
		}
		return type_int();
	}

	if (!type_is_numeric(left_type)) {
		compiler_error(compiler, left_node->line, left_node->column, 0, "compiler::type-mismatch", "left operand must be int or float, got %s", type_to_string(left_type));
	}
	if (!type_is_numeric(right_type)) {
		compiler_error(compiler, right_node->line, right_node->column, 0, "compiler::type-mismatch", "right operand must be int or float, got %s", type_to_string(right_type));
	}

	if (opcode == OP_MODULO && (left_type.kind == TYPE_FLOAT || right_type.kind == TYPE_FLOAT)) {
		compiler_error(compiler, line, column, 0, "compiler::type-mismatch", "modulo requires int operands, got %s and %s", type_to_string(left_type), type_to_string(right_type));
		return type_int();
	}

	// Arithmetic: int only if both sides are int, float if either side
	// is float, matching the runtime's own numeric promotion in
	// binary_op() (src/runtime/vm.c).
	if (left_type.kind == TYPE_FLOAT || right_type.kind == TYPE_FLOAT) {
		return type_float();
	}
	return type_int();
}

static type_t compile_binary_expr(compiler_t *compiler, const ast_node_t *node) {
	type_t left_type;
	type_t right_type;
	uint8_t opcode;
	bool is_equality;
	bool is_comparison;

	left_type = compile_expr_typed(compiler, node->as.binary_expr.left, NULL);
	right_type = compile_expr_typed(compiler, node->as.binary_expr.right, NULL);

	is_equality = node->as.binary_expr.op == TOKEN_EQUAL_EQUAL || node->as.binary_expr.op == TOKEN_BANG_EQUAL;

	switch (node->as.binary_expr.op) {
		case TOKEN_PLUS: opcode = OP_ADD; break;
		case TOKEN_MINUS: opcode = OP_SUBTRACT; break;
		case TOKEN_STAR: opcode = OP_MULTIPLY; break;
		case TOKEN_SLASH: opcode = OP_DIVIDE; break;
		case TOKEN_PERCENT: opcode = OP_MODULO; break;
		case TOKEN_EQUAL_EQUAL: opcode = OP_EQUAL; break;
		case TOKEN_BANG_EQUAL: opcode = OP_NOT_EQUAL; break;
		case TOKEN_LESS: opcode = OP_LESS; break;
		case TOKEN_LESS_EQUAL: opcode = OP_LESS_EQUAL; break;
		case TOKEN_GREATER: opcode = OP_GREATER; break;
		case TOKEN_GREATER_EQUAL: opcode = OP_GREATER_EQUAL; break;
		case TOKEN_AMP: opcode = OP_BIT_AND; break;
		case TOKEN_PIPE: opcode = OP_BIT_OR; break;
		case TOKEN_CARET: opcode = OP_BIT_XOR; break;
		case TOKEN_LESS_LESS: opcode = OP_SHIFT_LEFT; break;
		case TOKEN_GREATER_GREATER: opcode = OP_SHIFT_RIGHT; break;
		default:
			compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-binary-operator", "unsupported binary operator");
			emit_byte(compiler, OP_EQUAL, node->line, node->column);
			return type_bool();
	}

	emit_byte(compiler, opcode, node->line, node->column);

	if (is_equality) {
		bool numeric_pair = type_is_numeric(left_type) && type_is_numeric(right_type);
		bool either_null = left_type.kind == TYPE_NULL || right_type.kind == TYPE_NULL;
		if (!type_equals(left_type, right_type) && !numeric_pair && !either_null) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot compare %s and %s for equality", type_to_string(left_type), type_to_string(right_type));
		}
		return type_bool();
	}

	is_comparison = opcode == OP_LESS || opcode == OP_LESS_EQUAL || opcode == OP_GREATER || opcode == OP_GREATER_EQUAL;
	if (is_comparison) {
		if (!type_is_numeric(left_type)) {
			compiler_error(compiler, node->as.binary_expr.left->line, node->as.binary_expr.left->column, 0, "compiler::type-mismatch", "left operand must be int or float, got %s", type_to_string(left_type));
		}
		if (!type_is_numeric(right_type)) {
			compiler_error(compiler, node->as.binary_expr.right->line, node->as.binary_expr.right->column, 0, "compiler::type-mismatch", "right operand must be int or float, got %s", type_to_string(right_type));
		}
		return type_bool();
	}

	// Everything left is arithmetic or bitwise/shift, shared with
	// compound assignment's own read-modify-write compilation, see
	// check_arithmetic_or_bitwise()'s own comment.
	return check_arithmetic_or_bitwise(compiler, opcode, left_type, right_type, node->as.binary_expr.left, node->as.binary_expr.right, node->line, node->column);
}

/* && and || short-circuit via jumps instead of compiling to a plain binary
   opcode. The right-hand side must not run at all once the left side
   already determines the result. Both operands are required to be bool:
   there is no generic "truthy" coercion anywhere in the type-checked
   language, matching if/while/for's own conditions and assert's first
   argument. */
static type_t compile_logical_expr(compiler_t *compiler, const ast_node_t *node) {
	size_t jump;
	size_t end_jump;
	type_t bool_hint;
	type_t left_type;
	type_t right_type;
	bool is_and;

	bool_hint = type_bool();
	is_and = node->as.binary_expr.op == TOKEN_AMP_AMP;

	left_type = compile_expr_typed(compiler, node->as.binary_expr.left, &bool_hint);
	if (!type_equals(left_type, type_bool())) {
		compiler_error(compiler, node->as.binary_expr.left->line, node->as.binary_expr.left->column, 0, "compiler::type-mismatch", "left operand of '%s' must be bool, got %s", is_and ? "&&" : "||", type_to_string(left_type));
	}

	if (is_and) {
		jump = emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
		emit_byte(compiler, OP_POP, node->line, node->column);
		right_type = compile_expr_typed(compiler, node->as.binary_expr.right, &bool_hint);
		patch_jump(compiler, jump, node->line);
	} else {
		jump = emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
		end_jump = emit_jump(compiler, OP_JUMP, node->line, node->column);
		patch_jump(compiler, jump, node->line);
		emit_byte(compiler, OP_POP, node->line, node->column);
		right_type = compile_expr_typed(compiler, node->as.binary_expr.right, &bool_hint);
		patch_jump(compiler, end_jump, node->line);
	}

	if (!type_equals(right_type, type_bool())) {
		compiler_error(compiler, node->as.binary_expr.right->line, node->as.binary_expr.right->column, 0, "compiler::type-mismatch", "right operand of '%s' must be bool, got %s", is_and ? "&&" : "||", type_to_string(right_type));
	}

	return type_bool();
}

static type_t compile_unary_expr(compiler_t *compiler, const ast_node_t *node) {
	type_t operand_type;

	operand_type = compile_expr_typed(compiler, node->as.unary_expr.operand, NULL);

	switch (node->as.unary_expr.op) {
		case TOKEN_MINUS:
			if (!type_is_numeric(operand_type)) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "unary '-' requires an int or float operand, got %s", type_to_string(operand_type));
			}
			emit_byte(compiler, OP_NEGATE, node->line, node->column);
			return type_is_numeric(operand_type) ? operand_type : type_int();
		case TOKEN_BANG:
			if (!type_equals(operand_type, type_bool())) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "unary '!' requires a bool operand, got %s", type_to_string(operand_type));
			}
			emit_byte(compiler, OP_NOT, node->line, node->column);
			return type_bool();
		case TOKEN_TILDE:
			if (!type_equals(operand_type, type_int())) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "unary '~' requires an int operand, got %s", type_to_string(operand_type));
			}
			emit_byte(compiler, OP_BIT_NOT, node->line, node->column);
			return type_int();
		default:
			compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-unary-operator", "unsupported unary operator");
			emit_byte(compiler, OP_NOT, node->line, node->column);
			return type_bool();
	}
}

static type_t compile_index_expr(compiler_t *compiler, const ast_node_t *node) {
	type_t object_type;
	type_t index_type;
	type_t element_expected;

	element_expected = type_int();
	object_type = compile_expr_typed(compiler, node->as.index_expr.object, NULL);
	index_type = compile_expr_typed(compiler, node->as.index_expr.index, &element_expected);

	if (!type_equals(index_type, type_int())) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::index-not-integer", "array/string index must be an int");
	}

	emit_byte(compiler, OP_GET_INDEX, node->line, node->column);

	if (object_type.array_depth > 0) {
		return type_element_of(object_type);
	}
	if (type_equals(object_type, type_string())) {
		return type_string();
	}

	compiler_error(compiler, node->line, node->column, 0, "compiler::not-indexable", "'[]' access only works on arrays and strings, got %s", type_to_string(object_type));
	return type_int();
}

static type_t compile_array_literal(compiler_t *compiler, const ast_node_t *node, const type_t *expected) {
	size_t i;
	size_t count;
	type_t element_type;
	type_t element_expected;
	type_t next_type;
	bool have_element_expected;
	const ast_node_t *element_node;

	count = node->as.array_literal.elements.count;

	have_element_expected = expected != NULL && expected->array_depth > 0;
	element_expected = have_element_expected ? type_element_of(*expected) : type_int();

	if (count > UINT8_MAX) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::too-many-elements", "an array literal can have at most 255 elements");
		count = 0;
	}

	if (count == 0) {
		emit_bytes(compiler, OP_ARRAY, 0, node->line, node->column);
		if (!have_element_expected) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::ambiguous-empty-array", "cannot infer the element type of an empty array literal here");
		}
		return type_array_of(element_expected);
	}

	element_node = node->as.array_literal.elements.items[0];
	element_type = compile_expr_typed(compiler, element_node, have_element_expected ? &element_expected : NULL);

	for (i = 1; i < count; i++) {
		element_node = node->as.array_literal.elements.items[i];
		next_type = compile_expr_typed(compiler, element_node, &element_type);
		if (!type_equals(next_type, element_type)) {
			compiler_error(compiler, element_node->line, element_node->column, 0, "compiler::array-literal-type-mismatch", "array literal elements must all have the same type");
		}
	}

	emit_bytes(compiler, OP_ARRAY, (uint8_t)count, node->line, node->column);
	return type_array_of(element_type);
}

/* True if sig's own parameter/return shape matches ft exactly. Used to
   pick which overload a bare, ambiguous function name refers to when
   used as a value (compile_function_value_ref()), the same "exact
   match against a known expected type" rule overload resolution at a
   call site already uses, just compared directly against sig's fields
   instead of building a func_type_t first. */
static bool signature_matches_func_type(const signature_t *sig, const func_type_t *ft) {
	size_t i;

	if (sig->param_count != ft->param_count || sig->has_return_type != ft->has_return_type) {
		return false;
	}
	for (i = 0; i < sig->param_count; i++) {
		if (!type_equals(sig->param_types[i], ft->param_types[i])) {
			return false;
		}
	}
	if (sig->has_return_type && !type_equals(sig->return_type, ft->return_type)) {
		return false;
	}
	return true;
}

/* A bare name that resolved to a known overload set instead of a local/
   upvalue variable (compile_identifier_expr() below): which overload it
   refers to as a *value* (not a call, which resolves this differently,
   from the argument types at finish_resolved_call()). Exactly one
   overload needs no help, since there is nothing to disambiguate. More
   than one needs expected to be a TYPE_FUNCTION whose shape exactly
   matches precisely one candidate, the same "resolve from the type
   context already available at this expression's position" rule an
   empty `[]` or a bare `{ ... }` already uses. Emits OP_GET_GLOBAL
   <mangled_name>, exactly the callee-loading step an ordinary direct
   call already emits. Referencing a top-level function's value needs
   no new runtime representation, since its obj_function_t is already
   sitting in this file's globals table under that mangled name. */
static type_t compile_function_value_ref(compiler_t *compiler, const ast_node_t *node, const overload_set_t *set, const type_t *expected) {
	const signature_t *sig;
	size_t i;
	size_t match_count;
	func_type_t *ft;

	sig = NULL;
	if (set->overload_count == 1) {
		sig = &set->overloads[0];
	} else if (expected != NULL && expected->kind == TYPE_FUNCTION) {
		match_count = 0;
		for (i = 0; i < set->overload_count; i++) {
			if (signature_matches_func_type(&set->overloads[i], expected->func_type)) {
				sig = &set->overloads[i];
				match_count++;
			}
		}
		if (match_count != 1) {
			sig = NULL;
		}
	}

	if (sig == NULL) {
		compiler_error(compiler, node->line, node->column, (int)node->as.identifier_expr.name.length, "compiler::ambiguous-function-reference", "'%.*s' has multiple overloads; a known function type is needed here to tell which one this refers to", (int)node->as.identifier_expr.name.length, node->as.identifier_expr.name.start);
		return type_void();
	}

	emit_name_op_str(compiler, OP_GET_GLOBAL, sig->mangled_name, strlen(sig->mangled_name), node->line, node->column);

	ft = type_new_func_type();
	ft->param_count = sig->param_count;
	ft->param_types = sig->param_count > 0 ? malloc(sig->param_count * sizeof(type_t)) : NULL;
	for (i = 0; i < sig->param_count; i++) {
		ft->param_types[i] = sig->param_types[i];
	}
	ft->has_return_type = sig->has_return_type;
	ft->return_type = sig->has_return_type ? sig->return_type : type_void();

	return type_function(ft);
}

static type_t compile_identifier_expr(compiler_t *compiler, const ast_node_t *node, const type_t *expected) {
	int slot;
	int upvalue;
	token_t name;
	overload_set_t *set;

	name = node->as.identifier_expr.name;

	slot = resolve_local(compiler, name);
	if (slot != -1) {
		emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)slot, node->line, node->column);
		return compiler->locals[slot].type;
	}

	upvalue = resolve_upvalue(compiler, name);
	if (upvalue != -1) {
		emit_bytes(compiler, OP_GET_UPVALUE, (uint8_t)upvalue, node->line, node->column);
		return compiler->upvalues[upvalue].type;
	}

	set = signature_table_find_set(compiler->file_signatures, name.start, name.length);
	if (set != NULL) {
		return compile_function_value_ref(compiler, node, set, expected);
	}

	if (name.length == 6 && memcmp(name.start, "stdlib", 6) == 0) {
		compiler_error(compiler, node->line, node->column, 6, "compiler::module-used-as-value", "'stdlib' is a module; access a member and call it");
		return type_void();
	}
	if (find_imported_module(compiler->imported_modules, compiler->imported_module_count, name.start, name.length) != NULL) {
		compiler_error(compiler, node->line, node->column, (int)name.length, "compiler::module-used-as-value", "'%.*s' is a module; access a member and call it", (int)name.length, name.start);
		return type_void();
	}

	{
		const global_def_t *global_def = global_registry_find(compiler->globals, name.start, name.length);
		if (global_def != NULL) {
			emit_name_op_str(compiler, OP_GET_GLOBAL, name.start, name.length, node->line, node->column);
			return global_def->type;
		}
	}

	compiler_error(compiler, node->line, node->column, (int)name.length, "compiler::undefined-variable", "undefined variable '%.*s'", (int)name.length, name.start);
	return type_void();
}

/* Resolves node's member name against object_type's struct fields,
   reporting "unknown field" (and returning -1) if there is none. Shared
   by both a struct field read (compile_member_expr) and a struct field
   write (compile_assign_expr's AST_MEMBER_EXPR target). Assumes
   object_type is already known to be a (non-array) TYPE_STRUCT. */
static int resolve_struct_field(compiler_t *compiler, const ast_node_t *node, const type_t *object_type, const char *name, size_t name_length, const struct_def_t **out_def) {
	int index;

	*out_def = object_type->struct_def;
	index = struct_def_find_field(object_type->struct_def, name, name_length);
	if (index < 0) {
		compiler_error(compiler, node->as.member_expr.name.line, node->as.member_expr.name.column, (int)name_length, "compiler::unknown-field", "'%.*s' has no field '%.*s'", (int)object_type->struct_def->name_length, object_type->struct_def->name, (int)name_length, name);
	}
	return index;
}

/* `object.name` in a plain (non-call) expression position: `.` on a
   struct-typed object reads a field; `.` on `stdlib` or an imported
   module can only ever be the callee of a call (compile_member_call),
   never a bare value, so that case is still rejected here exactly like
   before this function existed, checked directly against the object's
   raw identifier text, without compiling it as an ordinary expression
   first, the same way compile_member_call itself never compiles its
   object either (a module is not a real runtime value paweli code can
   hold). */
static type_t compile_member_expr(compiler_t *compiler, const ast_node_t *node) {
	const ast_node_t *object_node;
	token_t object_name;
	type_t object_type;
	int field_index;
	const struct_def_t *def;
	struct_def_t *type_def;

	object_node = node->as.member_expr.object;

	if (object_node->kind == AST_IDENTIFIER_EXPR) {
		object_name = object_node->as.identifier_expr.name;

		if (object_name.length == 6 && memcmp(object_name.start, "stdlib", 6) == 0) {
			compiler_error(compiler, node->as.member_expr.name.line, node->as.member_expr.name.column, (int)node->as.member_expr.name.length, "compiler::unsupported-member-access", "cannot use 'stdlib.%.*s' without calling it", (int)node->as.member_expr.name.length, node->as.member_expr.name.start);
			return type_void();
		}

		// A struct type name used bare (`Point.origin`, not called):
		// same "cannot use without calling it" treatment as stdlib/module,
		// for the same reason; a struct function is only ever a call
		// target, never a value. Whether it's ambiguous across two
		// imports doesn't matter for this check specifically, only
		// whether *some* struct by this name exists at all; an actual
		// use of the type elsewhere is what reports ambiguity.
		type_def = struct_registry_find(compiler->structs, object_name.start, object_name.length);
		if (type_def == NULL) {
			size_t m;
			for (m = 0; m < compiler->imported_module_count; m++) {
				struct_def_t *imported_type_def = struct_registry_find(compiler->imported_modules[m]->exported_structs, object_name.start, object_name.length);
				if (imported_type_def != NULL && imported_type_def->is_exported) {
					type_def = imported_type_def;
					break;
				}
			}
		}
		if (type_def != NULL) {
			compiler_error(compiler, node->as.member_expr.name.line, node->as.member_expr.name.column, (int)node->as.member_expr.name.length, "compiler::unsupported-member-access", "cannot use '%.*s.%.*s' without calling it", (int)object_name.length, object_name.start, (int)node->as.member_expr.name.length, node->as.member_expr.name.start);
			return type_void();
		}

		if (find_imported_module(compiler->imported_modules, compiler->imported_module_count, object_name.start, object_name.length) != NULL) {
			compiler_error(compiler, node->as.member_expr.name.line, node->as.member_expr.name.column, (int)node->as.member_expr.name.length, "compiler::unsupported-member-access", "cannot use '%.*s.%.*s' without calling it", (int)object_name.length, object_name.start, (int)node->as.member_expr.name.length, node->as.member_expr.name.start);
			return type_void();
		}
	}

	object_type = compile_expr_typed(compiler, object_node, NULL);

	if (object_type.kind != TYPE_STRUCT || object_type.array_depth > 0) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-member-access", "'.' only works on struct fields, or a module (which must be called)");
		return type_void();
	}

	field_index = resolve_struct_field(compiler, node, &object_type, node->as.member_expr.name.start, node->as.member_expr.name.length, &def);
	if (field_index < 0) {
		return type_void();
	}

	emit_bytes(compiler, OP_GET_FIELD, (uint8_t)field_index, node->line, node->column);
	return def->fields[field_index].type;
}

/* `TypeName { field: value, ... }`. Fields are compiled in the struct's
   DECLARED order, not the literal's written order: OP_STRUCT pops a
   fixed number of values expecting that order, since OP_GET_FIELD/
   OP_SET_FIELD later address fields by that same fixed index. A field
   the literal omits falls back to its declared default, recompiled
   fresh at this call site (safe: defaults cannot reference locals or
   sibling fields, so the same default AST node compiles identically no
   matter which struct literal in the file omits it). */
static type_t compile_struct_literal(compiler_t *compiler, const ast_node_t *node, const type_t *expected) {
	const struct_def_t *def;
	size_t lit_count;
	size_t i;
	size_t j;
	int field_index;
	const ast_node_t *field_init;
	const ast_node_t *earlier;
	const ast_node_t *matched;
	type_t value_type;

	lit_count = node->as.struct_literal.fields.count;

	if (node->as.struct_literal.has_type_name) {
		def = struct_registry_find(compiler->structs, node->as.struct_literal.type_name.start, node->as.struct_literal.type_name.length);
		if (def == NULL) {
			size_t m;
			size_t match_count = 0;
			for (m = 0; m < compiler->imported_module_count; m++) {
				struct_def_t *candidate = struct_registry_find(compiler->imported_modules[m]->exported_structs, node->as.struct_literal.type_name.start, node->as.struct_literal.type_name.length);
				if (candidate != NULL && candidate->is_exported) {
					def = candidate;
					match_count++;
				}
			}
			if (match_count > 1) {
				compiler_error(compiler, node->line, node->column, (int)node->as.struct_literal.type_name.length, "compiler::ambiguous-struct-name", "'%.*s' is exported by more than one imported module", (int)node->as.struct_literal.type_name.length, node->as.struct_literal.type_name.start);
				return type_void();
			}
		}
		if (def == NULL) {
			compiler_error(compiler, node->line, node->column, (int)node->as.struct_literal.type_name.length, "compiler::unknown-type", "unknown struct type '%.*s'", (int)node->as.struct_literal.type_name.length, node->as.struct_literal.type_name.start);
			return type_void();
		}
	} else {
		// A bare `{ ... }`: the type has to come from context, the same
		// way an empty array literal `[]` borrows its element type from
		// `expected` instead of spelling it out.
		if (expected == NULL || expected->kind != TYPE_STRUCT || expected->array_depth > 0) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::ambiguous-struct-literal", "cannot infer the struct type of '{ ... }' here; give an explicit type name");
			return type_void();
		}
		def = expected->struct_def;
	}

	// Unknown/duplicate field names in the literal, checked up front.
	// Does not emit anything: the values themselves are compiled below,
	// in declared order, not this loop's (arbitrary) literal order.
	for (i = 0; i < lit_count; i++) {
		field_init = node->as.struct_literal.fields.items[i];
		field_index = struct_def_find_field(def, field_init->as.struct_field_init.name.start, field_init->as.struct_field_init.name.length);
		if (field_index < 0) {
			compiler_error(compiler, field_init->line, field_init->column, (int)field_init->as.struct_field_init.name.length, "compiler::unknown-field", "'%.*s' has no field '%.*s'", (int)def->name_length, def->name, (int)field_init->as.struct_field_init.name.length, field_init->as.struct_field_init.name.start);
			continue;
		}
		for (j = 0; j < i; j++) {
			earlier = node->as.struct_literal.fields.items[j];
			if (identifiers_equal(earlier->as.struct_field_init.name, field_init->as.struct_field_init.name)) {
				compiler_error(compiler, field_init->line, field_init->column, (int)field_init->as.struct_field_init.name.length, "compiler::duplicate-field", "field '%.*s' is set more than once", (int)field_init->as.struct_field_init.name.length, field_init->as.struct_field_init.name.start);
				break;
			}
		}
	}

	if (def->field_count > UINT8_MAX) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::too-many-fields", "a struct can have at most 255 fields");
	}

	for (i = 0; i < def->field_count; i++) {
		matched = NULL;
		for (j = 0; j < lit_count; j++) {
			field_init = node->as.struct_literal.fields.items[j];
			if (field_init->as.struct_field_init.name.length == def->fields[i].name_length && memcmp(field_init->as.struct_field_init.name.start, def->fields[i].name, def->fields[i].name_length) == 0) {
				matched = field_init;
				break;
			}
		}

		if (matched != NULL) {
			value_type = compile_expr_typed(compiler, matched->as.struct_field_init.value, &def->fields[i].type);
			if (!type_is_assignable(def->fields[i].type, value_type)) {
				compiler_error(compiler, matched->line, matched->column, (int)matched->as.struct_field_init.name.length, "compiler::type-mismatch", "field '%.*s' expects %s, got %s", (int)def->fields[i].name_length, def->fields[i].name, type_to_string(def->fields[i].type), type_to_string(value_type));
			}
		} else if (def->fields[i].default_value != NULL) {
			value_type = compile_expr_typed(compiler, def->fields[i].default_value, &def->fields[i].type);
			if (!type_is_assignable(def->fields[i].type, value_type)) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "default value for field '%.*s' must be %s, got %s", (int)def->fields[i].name_length, def->fields[i].name, type_to_string(def->fields[i].type), type_to_string(value_type));
			}
		} else {
			compiler_error(compiler, node->line, node->column, 0, "compiler::missing-field", "struct literal for '%.*s' is missing required field '%.*s'", (int)def->name_length, def->name, (int)def->fields[i].name_length, def->fields[i].name);
			emit_byte(compiler, OP_NULL, node->line, node->column);
		}
	}

	emit_name_op_str(compiler, OP_STRUCT, def->name, def->name_length, node->line, node->column);
	emit_byte(compiler, (uint8_t)def->field_count, node->line, node->column);

	return type_struct(def);
}

static type_t compile_call_expr(compiler_t *compiler, const ast_node_t *node) {
	const ast_node_t *callee;
	token_t name;
	int slot;
	int upvalue;

	callee = node->as.call_expr.callee;

	if (callee->kind == AST_IDENTIFIER_EXPR) {
		name = callee->as.identifier_expr.name;

		// A local or upvalue holding a nested fn's closure takes priority
		// over a same-named top-level function/native: ordinary variable
		// shadowing, the same way compile_identifier_expr already prefers
		// a local over anything else. Anything that resolves as a local
		// but is NOT a closure (an int, say) falls straight through to
		// compile_bare_call() unchanged; this is pre-existing behavior, since
		// fn was never a local's type before nested fns existed either.
		slot = resolve_local(compiler, name);
		if (slot != -1) {
			if (compiler->locals[slot].type.kind == TYPE_FUNCTION) {
				emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)slot, node->line, node->column);
				return compile_value_call(compiler, node, compiler->locals[slot].type);
			}
		} else {
			upvalue = resolve_upvalue(compiler, name);
			if (upvalue != -1 && compiler->upvalues[upvalue].type.kind == TYPE_FUNCTION) {
				emit_bytes(compiler, OP_GET_UPVALUE, (uint8_t)upvalue, node->line, node->column);
				return compile_value_call(compiler, node, compiler->upvalues[upvalue].type);
			}
		}

		return compile_bare_call(compiler, node, name);
	}

	if (callee->kind == AST_MEMBER_EXPR) {
		const ast_node_t *object;

		// A struct instance's own field can hold a function value too
		// (`h.callback(4)`, h a local/upvalue struct variable), told
		// apart from compile_member_call()'s module/stdlib/struct-
		// namespace access (`stdlib.sqrt(...)`, `geometry.area(...)`,
		// `Point.origin()`) the same way compile_identifier_expr()
		// already tells a variable apart from a function name: none of
		// those are ever stored as an ordinary local/upvalue, so
		// resolving as one first is enough to identify a real struct
		// field rather than a module-style access, with no risk of
		// misrouting the existing cases.
		object = callee->as.member_expr.object;
		if (object->kind == AST_IDENTIFIER_EXPR) {
			token_t object_name = object->as.identifier_expr.name;

			if (resolve_local(compiler, object_name) != -1 || resolve_upvalue(compiler, object_name) != -1) {
				type_t member_type = compile_member_expr(compiler, callee);

				if (member_type.kind != TYPE_FUNCTION) {
					compiler_error(compiler, node->line, node->column, 0, "compiler::not-callable", "this expression cannot be called");
					return type_void();
				}
				return compile_value_call(compiler, node, member_type);
			}
		}

		return compile_member_call(compiler, node);
	}

	compiler_error(compiler, node->line, node->column, 0, "compiler::not-callable", "this expression cannot be called");
	return type_void();
}

/* Calling through a value already pushed onto the stack (compile_call_expr
   just emitted the OP_GET_LOCAL/OP_GET_UPVALUE for it): the only call
   shape besides a resolved name (finish_resolved_call) or a module member
   (compile_module_call/compile_stdlib_call), needed because a nested fn's
   closure is a plain runtime value, not a name any signature_table_t
   knows about. Type-checks arguments against callee_type.func_type the
   same way finish_resolved_call() checks against a resolved signature_t's
   param_types. */
static type_t compile_value_call(compiler_t *compiler, const ast_node_t *node, type_t callee_type) {
	const func_type_t *ft;
	size_t arg_count;
	size_t i;
	type_t arg_type;
	type_t hint;
	const ast_node_t *arg_node;

	ft = callee_type.func_type;
	arg_count = node->as.call_expr.args.count;

	if (arg_count > UINT8_MAX) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::too-many-arguments", "a call can have at most 255 arguments");
		arg_count = 0;
	}

	if (arg_count != ft->param_count) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-count-mismatch", "expected %zu arguments but got %zu", ft->param_count, arg_count);
	}

	for (i = 0; i < arg_count; i++) {
		arg_node = node->as.call_expr.args.items[i];
		if (i < ft->param_count) {
			hint = ft->param_types[i];
			arg_type = compile_expr_typed(compiler, arg_node, &hint);
			if (!type_is_assignable(ft->param_types[i], arg_type)) {
				compiler_error(compiler, arg_node->line, arg_node->column, 0, "compiler::argument-type-mismatch", "argument %zu expects %s, got %s", i + 1, type_to_string(ft->param_types[i]), type_to_string(arg_type));
			}
		} else {
			compile_expr_typed(compiler, arg_node, NULL);
		}
	}

	emit_bytes(compiler, OP_CALL, (uint8_t)arg_count, node->line, node->column);

	return ft->has_return_type ? ft->return_type : type_void();
}

static type_t compile_bare_call(compiler_t *compiler, const ast_node_t *node, token_t name) {
	if (signature_table_find_set(compiler->file_signatures, name.start, name.length) != NULL) {
		return finish_resolved_call(compiler, compiler->file_signatures, name.start, name.length, name.line, name.column, node, OP_GET_GLOBAL, true, NULL, 0, NULL);
	}

	// print(x) accepts any type for its single argument, checked
	// dynamically at runtime the same way it always has been: there is
	// no way to express "any type" as a fixed signature_t, so it is
	// special-cased here rather than registered in g_native_globals.
	// Only reached once the local-file table above has already come up
	// empty, so a user's own `fn print(...)` still shadows this.
	if (name.length == 5 && memcmp(name.start, "print", 5) == 0 && node->as.call_expr.args.count == 1) {
		emit_name_op_str(compiler, OP_GET_GLOBAL, "print", 5, node->line, node->column);
		compile_expr_typed(compiler, node->as.call_expr.args.items[0], NULL);
		emit_bytes(compiler, OP_CALL, 1, node->line, node->column);
		return type_void();
	}

	return finish_resolved_call(compiler, &g_native_globals, name.start, name.length, name.line, name.column, node, OP_GET_GLOBAL, false, NULL, 0, NULL);
}

static type_t compile_member_call(compiler_t *compiler, const ast_node_t *node) {
	const ast_node_t *callee;
	const ast_node_t *object;
	token_t member_name;
	token_t object_name;
	struct_def_t *def;
	const module_unit_t *module;
	size_t m;
	size_t match_count;

	callee = node->as.call_expr.callee;
	object = callee->as.member_expr.object;
	member_name = callee->as.member_expr.name;

	if (object->kind != AST_IDENTIFIER_EXPR) {
		compiler_error(compiler, object->line, object->column, 0, "compiler::unsupported-member-access", "'.' access only works on modules");
		return type_void();
	}

	object_name = object->as.identifier_expr.name;

	if (object_name.length == 6 && memcmp(object_name.start, "stdlib", 6) == 0) {
		return compile_stdlib_call(compiler, node, member_name);
	}

	// A struct type name: `Point.name(args)`, the only way a struct
	// function is ever called (see type.h's struct_def_t.methods/
	// namespace_obj). Local structs win over an imported one sharing the
	// same name, matching every other unqualified struct-name lookup
	// (resolve_type_ref(), compile_struct_literal()); two different
	// imports sharing the name, with no local one to win instead, is
	// ambiguous rather than picking either.
	def = struct_registry_find(compiler->structs, object_name.start, object_name.length);
	if (def == NULL) {
		match_count = 0;
		for (m = 0; m < compiler->imported_module_count; m++) {
			struct_def_t *imported_def = struct_registry_find(compiler->imported_modules[m]->exported_structs, object_name.start, object_name.length);
			if (imported_def != NULL && imported_def->is_exported) {
				def = imported_def;
				match_count++;
			}
		}
		if (match_count > 1) {
			compiler_error(compiler, object_name.line, object_name.column, (int)object_name.length, "compiler::ambiguous-struct-name", "'%.*s' is exported by more than one imported module", (int)object_name.length, object_name.start);
			return type_void();
		}
	}
	if (def != NULL) {
		return compile_struct_call(compiler, node, def, member_name);
	}

	module = find_imported_module(compiler->imported_modules, compiler->imported_module_count, object_name.start, object_name.length);
	if (module != NULL) {
		return compile_module_call(compiler, node, member_name, module);
	}

	compiler_error(compiler, object_name.line, object_name.column, (int)object_name.length, "compiler::unknown-module", "'%.*s' is not a known module", (int)object_name.length, object_name.start);
	return type_void();
}

static type_t compile_stdlib_call(compiler_t *compiler, const ast_node_t *node, token_t member_name) {
	if (member_name.length == 6 && memcmp(member_name.start, "length", 6) == 0) {
		return compile_length_call(compiler, node);
	}
	if (member_name.length == 4 && memcmp(member_name.start, "push", 4) == 0) {
		return compile_push_call(compiler, node);
	}
	if (member_name.length == 3 && memcmp(member_name.start, "pop", 3) == 0) {
		return compile_pop_call(compiler, node);
	}
	if (member_name.length == 4 && memcmp(member_name.start, "sort", 4) == 0) {
		return compile_sort_call(compiler, node);
	}

	return finish_resolved_call(compiler, &g_stdlib_members, member_name.start, member_name.length, member_name.line, member_name.column, node, OP_GET_PROPERTY, false, "stdlib", 6, NULL);
}

static type_t compile_module_call(compiler_t *compiler, const ast_node_t *node, token_t member_name, const module_unit_t *module) {
	return finish_resolved_call(compiler, &module->exports, member_name.start, member_name.length, member_name.line, member_name.column, node, OP_GET_PROPERTY, true, module->local_name, module->local_name_length, NULL);
}

/* `Point.name(args)`: def is already resolved by the caller
   (compile_member_call()), so unlike a module there is nothing to look
   up by name at runtime. def->namespace_obj is pushed directly as a
   constant (see finish_resolved_call()'s prefix_constant handling).
   def->methods is a table scoped to this one struct, so the plain
   (unqualified) mangled name is enough; no cross-struct collision is
   possible. */
static type_t compile_struct_call(compiler_t *compiler, const ast_node_t *node, const struct_def_t *def, token_t member_name) {
	return finish_resolved_call(compiler, def->methods, member_name.start, member_name.length, member_name.line, member_name.column, node, OP_GET_PROPERTY, true, NULL, 0, (obj_t *)def->namespace_obj);
}

/* length/push/pop are polymorphic over array element type in a way a
   fixed signature_t cannot express ("(T[]) -> int" for any T), so they
   are resolved by inspecting the argument's actual compiled type
   directly instead of going through the normal overload-matching path.
   The string form of length still shares the ordinary signature table
   (see native_signatures.c); only the array form needs this. */
static type_t compile_length_call(compiler_t *compiler, const ast_node_t *node) {
	type_t arg_type;

	if (node->as.call_expr.args.count != 1) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-count-mismatch", "length expects 1 argument");
		return type_int();
	}

	emit_name_op_str(compiler, OP_GET_GLOBAL, "stdlib", 6, node->line, node->column);
	emit_name_op_str(compiler, OP_GET_PROPERTY, "length", 6, node->line, node->column);
	arg_type = compile_expr_typed(compiler, node->as.call_expr.args.items[0], NULL);
	emit_bytes(compiler, OP_CALL, 1, node->line, node->column);

	if (arg_type.array_depth > 0 || type_equals(arg_type, type_string())) {
		return type_int();
	}

	compiler_error(compiler, node->line, node->column, 0, "compiler::argument-type-mismatch", "length expects a string or an array argument");
	return type_int();
}

static type_t compile_push_call(compiler_t *compiler, const ast_node_t *node) {
	type_t array_type;
	type_t value_type;
	type_t element_expected;

	if (node->as.call_expr.args.count != 2) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-count-mismatch", "push expects 2 arguments");
		return type_void();
	}

	emit_name_op_str(compiler, OP_GET_GLOBAL, "stdlib", 6, node->line, node->column);
	emit_name_op_str(compiler, OP_GET_PROPERTY, "push", 4, node->line, node->column);

	array_type = compile_expr_typed(compiler, node->as.call_expr.args.items[0], NULL);

	if (array_type.array_depth == 0) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-type-mismatch", "push expects an array as its first argument");
		compile_expr_typed(compiler, node->as.call_expr.args.items[1], NULL);
		emit_bytes(compiler, OP_CALL, 2, node->line, node->column);
		return type_void();
	}

	element_expected = type_element_of(array_type);
	value_type = compile_expr_typed(compiler, node->as.call_expr.args.items[1], &element_expected);

	if (!type_is_assignable(element_expected, value_type)) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-type-mismatch", "cannot push this value onto an array of a different element type");
	}

	emit_bytes(compiler, OP_CALL, 2, node->line, node->column);
	return type_void();
}

static type_t compile_pop_call(compiler_t *compiler, const ast_node_t *node) {
	type_t array_type;

	if (node->as.call_expr.args.count != 1) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-count-mismatch", "pop expects 1 argument");
		return type_void();
	}

	emit_name_op_str(compiler, OP_GET_GLOBAL, "stdlib", 6, node->line, node->column);
	emit_name_op_str(compiler, OP_GET_PROPERTY, "pop", 3, node->line, node->column);

	array_type = compile_expr_typed(compiler, node->as.call_expr.args.items[0], NULL);
	emit_bytes(compiler, OP_CALL, 1, node->line, node->column);

	if (array_type.array_depth == 0) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-type-mismatch", "pop expects an array argument");
		return type_int();
	}

	return type_element_of(array_type);
}

/* stdlib.sort(T[] array, fn(T,T)->bool less_than): sorts array in place,
   given a comparator that reports whether its first argument should
   sort before its second (matching std::sort's convention). Special-
   cased in the compiler for the same reason length/push/pop are: it is
   polymorphic over the array's element type T, which no fixed
   signature_t in the native signature table can express. Unlike those
   three, the second argument's *own* required type (fn(T,T)->bool)
   depends on T too, built here as an `expected` hint the same way
   compile_push_call() builds one for its value argument, letting a
   bare comparator name or an inline lambda resolve against it exactly
   like any other "infer from context" position already does. */
static type_t compile_sort_call(compiler_t *compiler, const ast_node_t *node) {
	type_t array_type;
	type_t element_type;
	type_t comparator_type;
	type_t expected;
	func_type_t *expected_ft;

	if (node->as.call_expr.args.count != 2) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-count-mismatch", "sort expects 2 arguments");
		return type_void();
	}

	emit_name_op_str(compiler, OP_GET_GLOBAL, "stdlib", 6, node->line, node->column);
	emit_name_op_str(compiler, OP_GET_PROPERTY, "sort", 4, node->line, node->column);

	array_type = compile_expr_typed(compiler, node->as.call_expr.args.items[0], NULL);

	if (array_type.array_depth == 0) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-type-mismatch", "sort expects an array as its first argument");
		compile_expr_typed(compiler, node->as.call_expr.args.items[1], NULL);
		emit_bytes(compiler, OP_CALL, 2, node->line, node->column);
		return type_void();
	}

	element_type = type_element_of(array_type);

	expected_ft = type_new_func_type();
	expected_ft->param_count = 2;
	expected_ft->param_types = malloc(2 * sizeof(type_t));
	expected_ft->param_types[0] = element_type;
	expected_ft->param_types[1] = element_type;
	expected_ft->has_return_type = true;
	expected_ft->return_type = type_bool();
	expected = type_function(expected_ft);

	comparator_type = compile_expr_typed(compiler, node->as.call_expr.args.items[1], &expected);
	if (!type_equals(expected, comparator_type)) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::argument-type-mismatch", "sort's comparator must be %s, got %s", type_to_string(expected), type_to_string(comparator_type));
	}

	emit_bytes(compiler, OP_CALL, 2, node->line, node->column);
	return type_void();
}

/* Shared by every kind of call (local function, native, or module
   member): resolves name against table by argument count and type,
   emitting the callee reference (prefix_name.name, or just name if
   prefix_name is NULL) as a reserve-then-patch pair, since which exact
   overload gets called is only known after the arguments are compiled
   (see patch_name_op()), but the callee has to sit under them on the
   stack. use_mangled_name picks whether the resolved overload's
   mangled_name or its plain source name is what actually gets looked
   up at runtime: mangled for user functions (locally defined or
   imported from a module, where overloading is real), plain for
   natives (never mangled, since exactly one obj_native_t backs every
   overload signature.c type-checks for a given name). */
static type_t finish_resolved_call(compiler_t *compiler, const signature_table_t *table, const char *name, size_t name_length, int name_line, int name_column, const ast_node_t *call_node, uint8_t patch_opcode, bool use_mangled_name, const char *prefix_name, size_t prefix_name_length, obj_t *prefix_constant) {
	overload_set_t *set;
	size_t arg_count;
	size_t i;
	type_t *arg_types;
	const type_t *sole_candidate_params;
	bool have_sole_candidate;
	size_t patch_offset;
	const signature_t *resolved;
	signature_match_result_t match;
	const ast_node_t *arg_node;
	type_t hint;

	set = signature_table_find_set(table, name, name_length);
	arg_count = call_node->as.call_expr.args.count;

	if (arg_count > UINT8_MAX) {
		compiler_error(compiler, call_node->line, call_node->column, 0, "compiler::too-many-arguments", "a call can have at most 255 arguments");
		arg_count = 0;
	}

	have_sole_candidate = false;
	sole_candidate_params = NULL;
	if (set != NULL) {
		for (i = 0; i < set->overload_count; i++) {
			if (set->overloads[i].param_count == arg_count) {
				if (have_sole_candidate) {
					have_sole_candidate = false;
					break;
				}
				have_sole_candidate = true;
				sole_candidate_params = set->overloads[i].param_types;
			}
		}
	}

	if (prefix_name != NULL) {
		emit_name_op_str(compiler, OP_GET_GLOBAL, prefix_name, prefix_name_length, call_node->line, call_node->column);
	} else if (prefix_constant != NULL) {
		// A struct function call: the namespace object is already known
		// at compile time (the caller already holds the resolved
		// struct_def_t*), so it's pushed directly as a constant instead
		// of looked up by name. Unlike a module, which has to be
		// resolved against the *calling* frame's own globals table,
		// since it might live in a different file's.
		emit_constant(compiler, value_obj(prefix_constant), call_node->line, call_node->column);
	}

	patch_offset = current_chunk(compiler)->count;
	emit_byte(compiler, patch_opcode, call_node->line, call_node->column);
	emit_bytes(compiler, 0xff, 0xff, call_node->line, call_node->column);

	arg_types = arg_count > 0 ? malloc(arg_count * sizeof(type_t)) : NULL;
	for (i = 0; i < arg_count; i++) {
		arg_node = call_node->as.call_expr.args.items[i];
		if (have_sole_candidate) {
			hint = sole_candidate_params[i];
			arg_types[i] = compile_expr_typed(compiler, arg_node, &hint);
		} else {
			arg_types[i] = compile_expr_typed(compiler, arg_node, NULL);
		}
	}

	if (set == NULL) {
		compiler_error(compiler, name_line, name_column, (int)name_length, "compiler::undefined-function", "undefined function '%.*s'", (int)name_length, name);
		free(arg_types);
		return type_void();
	}

	match = signature_table_resolve(table, name, name_length, arg_types, arg_count, &resolved);

	if (match != SIGNATURE_MATCH_OK) {
		char arg_list[256];
		size_t pos;

		pos = 0;
		for (i = 0; i < arg_count && pos < sizeof(arg_list) - 16; i++) {
			pos += (size_t)snprintf(arg_list + pos, sizeof(arg_list) - pos, "%s%s", i > 0 ? ", " : "", type_to_string(arg_types[i]));
		}
		arg_list[pos] = '\0';

		compiler_error(compiler, name_line, name_column, (int)name_length, "compiler::no-matching-overload", "no overload of '%.*s' takes (%s)", (int)name_length, name, arg_list);
		free(arg_types);
		return type_void();
	}
	free(arg_types);

	if (resolved->is_deprecated) {
		if (resolved->deprecated_message_length > 0) {
			compiler_warning(compiler, name_line, name_column, (int)name_length, "compiler::deprecated-call", "'%.*s' is deprecated: %.*s", (int)name_length, name, (int)resolved->deprecated_message_length, resolved->deprecated_message);
		} else {
			compiler_warning(compiler, name_line, name_column, (int)name_length, "compiler::deprecated-call", "'%.*s' is deprecated", (int)name_length, name);
		}
	}

	if (use_mangled_name) {
		patch_name_op(compiler, patch_offset, resolved->mangled_name, strlen(resolved->mangled_name), call_node->line, call_node->column);
	} else {
		patch_name_op(compiler, patch_offset, name, name_length, call_node->line, call_node->column);
	}

	emit_bytes(compiler, OP_CALL, (uint8_t)arg_count, call_node->line, call_node->column);

	return resolved->has_return_type ? resolved->return_type : type_void();
}

/* counter += 1, counter being one of the two script-scope locals
   emit_test_runner() declares for its running pass/fail tally. Mirrors
   compile_foreach_stmt()'s own $for_index increment
   (GET_LOCAL/CONSTANT/ADD/SET_LOCAL/POP). */
static void emit_local_increment(compiler_t *script, int slot, int line, int column) {
	emit_bytes(script, OP_GET_LOCAL, (uint8_t)slot, line, column);
	emit_constant(script, value_int(1), line, column);
	emit_byte(script, OP_ADD, line, column);
	emit_bytes(script, OP_SET_LOCAL, (uint8_t)slot, line, column);
	emit_byte(script, OP_POP, line, column);
}

/* True (and *out_sig set) iff fn_node is a well-formed @test function:
   decorated "test", with a signature that resolved cleanly (a malformed
   one, e.g. nonzero arity, was already reported by apply_decorators();
   had_error is already set, so the whole compile fails regardless --
   this just skips emitting a call against the wrong overload). Shared
   by emit_test_runner()'s own two passes (counting, then emitting), so
   "how many tests" and "which ones" can never disagree. */
static bool resolve_test_function(const ast_node_t *fn_node, const signature_table_t *file_signatures, const signature_t **out_sig) {
	size_t d;
	const ast_node_t *decorator_node;
	token_t dname;
	token_t fname;

	for (d = 0; d < fn_node->as.function_decl.decorators.count; d++) {
		decorator_node = fn_node->as.function_decl.decorators.items[d];
		dname = decorator_node->as.decorator.name;
		if (dname.length == 4 && memcmp(dname.start, "test", 4) == 0) {
			fname = fn_node->as.function_decl.name;
			return signature_table_resolve(file_signatures, fname.start, fname.length, NULL, 0, out_sig) == SIGNATURE_MATCH_OK;
		}
	}
	return false;
}

/* Emits a `$test_progress(current, total, name)` call: --run-tests'
   per-test status line, a native (src/stdlib/stdlib.c) rather than
   hand-rolled bytecode because it needs an isatty() check (to decide
   between overwriting one line in place on a real terminal vs. the
   plain one-line-per-test scripts/run_tests.sh's own pass-count check
   greps for) that has no equivalent in the language itself. current
   and total are both compile-time constants: the loop emitting one
   call per test is fully unrolled, so "how many tests deep are we"
   never needs a runtime counter. */
static void emit_test_progress(compiler_t *script, int current, int total, const char *name, size_t name_length, int line, int column) {
	obj_string_t *name_str;

	name_str = object_new_string(script->heap, script->strings, name, name_length);
	emit_name_op_str(script, OP_GET_GLOBAL, "$test_progress", 14, line, column);
	emit_constant(script, value_int(current), line, column);
	emit_constant(script, value_int(total), line, column);
	emit_constant(script, value_obj((obj_t *)name_str), line, column);
	emit_bytes(script, OP_CALL, 3, line, column);
	emit_byte(script, OP_POP, line, column);
}

/* Emits one "running: NAME" progress update, then a call to the
   @test-tagged function wrapped in the same try/catch machinery
   compile_try_expr() uses for a source-level `try { ... } catch (msg:
   string) { ... }` (OP_CLOSURE + OP_TRY_CALL, see that function's own
   comment for the bytecode shape), in place of the usual single call
   to main(). See compiler_compile()'s run_tests_mode parameter. main()
   itself is still required to exist (checked unconditionally, same as
   always) but is never called in this mode.

   Catching every test's own failure here, rather than letting it abort
   the whole run the way an uncaught error normally would, is what makes
   this "every test runs, not just every test up to the first failure"
   possible. The caught message is whatever vm_runtime_error() raised it
   with (e.g. "assertion failed: x == y"), no file/line/caret/backtrace
   attached: that context only ever gets computed by walking live call
   frames at the moment an *uncaught* error is reported
   (vm_print_uncaught_error()), and OP_TRY_CALL has already unwound
   those frames by the time a catch handler runs. A failing test's name
   is still printed right next to its message, which is normally enough
   to find it again; re-running just that one test file gets the full
   diagnostic back.

   Once every test has run, prints a one-line pass/fail summary and, if
   anything failed, OP_THROWs a final summary error so this whole
   run_tests_mode script ends the same way an ordinary uncaught error
   would: nonzero exit code, reported via vm_print_uncaught_error() in
   main.c, with no VM-level changes needed to get there. */
static void emit_test_runner(compiler_t *script, const ast_node_t *program, const signature_table_t *file_signatures) {
	size_t i;
	const ast_node_t *fn_node;
	token_t fname;
	const signature_t *test_sig;
	int total_tests;
	int test_index;
	int pass_slot;
	int fail_slot;
	int msg_slot;
	size_t try_call_offset;
	size_t end_jump_offset;
	size_t skip_throw_jump;
	compiler_t test_body;
	obj_string_t *failure_msg;

	total_tests = 0;
	for (i = 0; i < program->as.program.functions.count; i++) {
		if (resolve_test_function(program->as.program.functions.items[i], file_signatures, &test_sig)) {
			total_tests++;
		}
	}

	{
		char header[64];
		int header_len;

		header_len = snprintf(header, sizeof(header), "Found %d Tests, running now.", total_tests);
		emit_name_op_str(script, OP_GET_GLOBAL, "print", 5, program->line, program->column);
		emit_constant(script, value_obj((obj_t *)object_new_string(script->heap, script->strings, header, (size_t)(header_len > 0 ? header_len : 0))), program->line, program->column);
		emit_bytes(script, OP_CALL, 1, program->line, program->column);
		emit_byte(script, OP_POP, program->line, program->column);
	}

	// vm_run() pushes the script's own function value before this chunk's
	// first instruction ever runs, and points this outermost frame's
	// slots straight at it (frame->slots == vm->stack, no "past the
	// callee" offset the way call_value() sets up every other frame --
	// see its own comment on the OP_RETURN case for stop_frame_count ==
	// 0). Compile-time slot 0 is otherwise always assumed free (every
	// other compiler_t, including this same `script`'s own file-level
	// signature/global setup, only ever reaches globals via
	// OP_DEFINE_GLOBAL/OP_GET_GLOBAL, never a local slot), so nothing
	// before this ever had to account for that reservation. Declaring
	// this placeholder first (no matching push: the value it aliases is
	// already sitting there) burns exactly that slot, so $test_pass_count
	// below lands on the runtime index its own OP_CONSTANT actually
	// pushed to.
	if (declare_local(script, synthetic_token("$test_script_self"), type_void(), program->line)) {
		mark_initialized(script);
	}

	emit_constant(script, value_int(0), program->line, program->column);
	if (declare_local(script, synthetic_token("$test_pass_count"), type_int(), program->line)) {
		mark_initialized(script);
	}
	pass_slot = script->local_count - 1;

	emit_constant(script, value_int(0), program->line, program->column);
	if (declare_local(script, synthetic_token("$test_fail_count"), type_int(), program->line)) {
		mark_initialized(script);
	}
	fail_slot = script->local_count - 1;

	test_index = 0;
	for (i = 0; i < program->as.program.functions.count; i++) {
		fn_node = program->as.program.functions.items[i];

		if (!resolve_test_function(fn_node, file_signatures, &test_sig)) {
			continue;
		}
		fname = fn_node->as.function_decl.name;
		test_index++;

		emit_test_progress(script, test_index, total_tests, fname.start, fname.length, fn_node->line, fn_node->column);

		// The try-body: a bare 0-arg closure that calls the test and
		// discards its result, exactly what compile_closure_body() would
		// produce for a `{ NAME(); }` block, minus the AST it would
		// otherwise need to compile that block from. Field-for-field the
		// same setup compile_closure_body() itself uses.
		test_body.function = object_new_function(script->heap);
		test_body.function->name = object_new_string(script->heap, script->strings, "<test>", 6);
		test_body.function->arity = 0;
		test_body.function->globals = script->function->globals;
		test_body.function->module = script->function->module;
		test_body.function->filename = script->filename;
		test_body.function->source = script->source;

		test_body.heap = script->heap;
		test_body.strings = script->strings;
		test_body.filename = script->filename;
		test_body.source = script->source;
		test_body.had_error = false;
		test_body.is_module_def = script->is_module_def;
		test_body.compiling_main = false;
		test_body.return_type = type_void();
		test_body.has_return_type = false;
		test_body.file_signatures = script->file_signatures;
		test_body.home_module = script->home_module;
		test_body.out_exports = script->out_exports;
		test_body.imported_modules = script->imported_modules;
		test_body.imported_module_count = script->imported_module_count;
		test_body.structs = script->structs;
		test_body.globals = script->globals;
		test_body.local_count = 0;
		test_body.scope_depth = 0;
		memset(test_body.local_reg_seen, 0, sizeof(test_body.local_reg_seen));
		memset(test_body.local_reg_eligible, 0, sizeof(test_body.local_reg_eligible));
		memset(test_body.local_reg_decl_end, 0, sizeof(test_body.local_reg_decl_end));
		memset(test_body.local_reg_dead_at, 0, sizeof(test_body.local_reg_dead_at));
		test_body.local_slot_high_water = 0;
		test_body.enclosing = script;
		test_body.upvalue_count = 0;
		test_body.loops = NULL;
		test_body.loop_count = 0;
		test_body.loop_capacity = 0;

		emit_name_op_str(&test_body, OP_GET_GLOBAL, test_sig->mangled_name, strlen(test_sig->mangled_name), fn_node->line, fn_node->column);
		emit_bytes(&test_body, OP_CALL, 0, fn_node->line, fn_node->column);
		emit_byte(&test_body, OP_POP, fn_node->line, fn_node->column);
		emit_byte(&test_body, OP_NULL, fn_node->line, fn_node->column);
		emit_byte(&test_body, OP_RETURN, fn_node->line, fn_node->column);

		finalize_local_reg_info(&test_body);
		script->had_error = script->had_error || test_body.had_error;
		free(test_body.loops);

		// A test that prints (tests/suite/basic.pw's test_print(), which
		// exists purely to prove print() works) would otherwise
		// interleave its own stdout with the [OK]/[FAIL] line and the
		// live progress line's in-place redraw below. Suppressed for the
		// whole reentrant call, restored right before either report line
		// prints, so the report itself is never swallowed too.
		emit_name_op_str(script, OP_GET_GLOBAL, "$test_output_suppress_begin", 27, fn_node->line, fn_node->column);
		emit_bytes(script, OP_CALL, 0, fn_node->line, fn_node->column);
		emit_byte(script, OP_POP, fn_node->line, fn_node->column);

		emit_closure(script, test_body.function, &test_body, fn_node->line, fn_node->column);

		try_call_offset = emit_jump(script, OP_TRY_CALL, fn_node->line, fn_node->column);

		// Success path: falls straight through from OP_TRY_CALL with the
		// try-body's (always null) return value on top of the stack.
		emit_byte(script, OP_POP, fn_node->line, fn_node->column);
		emit_name_op_str(script, OP_GET_GLOBAL, "$test_output_suppress_end", 25, fn_node->line, fn_node->column);
		emit_bytes(script, OP_CALL, 0, fn_node->line, fn_node->column);
		emit_byte(script, OP_POP, fn_node->line, fn_node->column);
		emit_name_op_str(script, OP_GET_GLOBAL, "$test_report_pass", 17, fn_node->line, fn_node->column);
		emit_constant(script, value_obj((obj_t *)object_new_string(script->heap, script->strings, fname.start, fname.length)), fn_node->line, fn_node->column);
		emit_bytes(script, OP_CALL, 1, fn_node->line, fn_node->column);
		emit_byte(script, OP_POP, fn_node->line, fn_node->column);
		emit_local_increment(script, pass_slot, fn_node->line, fn_node->column);
		end_jump_offset = emit_jump(script, OP_JUMP, fn_node->line, fn_node->column);

		// Catch path: OP_TRY_CALL landed here with the failure's message
		// pushed in place of a return value.
		patch_jump(script, try_call_offset, fn_node->line);
		begin_scope(script);
		if (declare_local(script, synthetic_token("$test_msg"), type_string(), fn_node->line)) {
			mark_initialized(script);
		}
		msg_slot = script->local_count - 1;

		emit_name_op_str(script, OP_GET_GLOBAL, "$test_output_suppress_end", 25, fn_node->line, fn_node->column);
		emit_bytes(script, OP_CALL, 0, fn_node->line, fn_node->column);
		emit_byte(script, OP_POP, fn_node->line, fn_node->column);
		emit_name_op_str(script, OP_GET_GLOBAL, "$test_report_failure", 20, fn_node->line, fn_node->column);
		emit_constant(script, value_obj((obj_t *)object_new_string(script->heap, script->strings, fname.start, fname.length)), fn_node->line, fn_node->column);
		emit_bytes(script, OP_GET_LOCAL, (uint8_t)msg_slot, fn_node->line, fn_node->column);
		emit_bytes(script, OP_CALL, 2, fn_node->line, fn_node->column);
		emit_byte(script, OP_POP, fn_node->line, fn_node->column);

		emit_local_increment(script, fail_slot, fn_node->line, fn_node->column);
		end_scope(script, fn_node->line, fn_node->column);

		patch_jump(script, end_jump_offset, fn_node->line);
	}

	emit_name_op_str(script, OP_GET_GLOBAL, "$test_report_summary", 20, program->line, program->column);
	emit_constant(script, value_int(total_tests), program->line, program->column);
	emit_bytes(script, OP_GET_LOCAL, (uint8_t)pass_slot, program->line, program->column);
	emit_bytes(script, OP_GET_LOCAL, (uint8_t)fail_slot, program->line, program->column);
	emit_bytes(script, OP_CALL, 3, program->line, program->column);
	emit_byte(script, OP_POP, program->line, program->column);

	// if ($test_fail_count > 0) throw "..."; -- same JUMP_IF_FALSE shape
	// compile_if_stmt() uses, condition popped on both the taken and the
	// not-taken path.
	emit_bytes(script, OP_GET_LOCAL, (uint8_t)fail_slot, program->line, program->column);
	emit_constant(script, value_int(0), program->line, program->column);
	emit_byte(script, OP_GREATER, program->line, program->column);
	skip_throw_jump = emit_jump(script, OP_JUMP_IF_FALSE, program->line, program->column);
	emit_byte(script, OP_POP, program->line, program->column);
	failure_msg = object_new_string(script->heap, script->strings, "one or more tests failed", 25);
	emit_constant(script, value_obj((obj_t *)failure_msg), program->line, program->column);
	emit_byte(script, OP_THROW, program->line, program->column);
	patch_jump(script, skip_throw_jump, program->line);
	emit_byte(script, OP_POP, program->line, program->column);
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
	// Ordinarily freed at the end of this call, same as file_signatures;
	// heap-allocated instead of a plain stack value only so ownership can
	// be handed to the caller (out_structs) when this file has exported
	// struct types that need to outlive this compile. See the matching
	// transfer-or-free logic at the bottom of this function.
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

	// Rooted immediately, not just once vm_run() eventually pushes it:
	// file_globals is already an unconditional GC root, but the caller
	// (main.c/module.c) may compile another file's script, and run
	// it, possibly triggering a collection, before this one ever
	// touches the VM stack. Without this, that window leaves a freshly
	// allocated script reachable from nothing at all. "<script>" can
	// never collide with a real name: identifiers cannot contain '<' or
	// '>', and neither can a mangled one.
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

	// Struct declarations resolve before any signature: a function's
	// parameter or return type may name a struct.
	register_structs(&script, program, structs, imported_modules, imported_module_count);
	had_error = had_error || script.had_error;

	// This file's global variables' names/types resolve next, before any
	// signature or body compiles: a function body may read or write any
	// global in this file regardless of declaration order (see
	// register_globals()'s own comment).
	register_globals(&script, program, &globals);
	had_error = had_error || script.had_error;

	// Every struct's own function signatures are collected next, before
	// either this file's top-level signatures or any body (top-level or
	// struct-function) compiles. A struct function's param/return type
	// may reference any struct, and its body may call any top-level
	// function or any other struct's function, regardless of declaration
	// order, exactly like top-level functions already support.
	for (i = 0; i < program->as.program.structs.count; i++) {
		struct_node = program->as.program.structs.items[i];
		struct_def = struct_registry_find(structs, struct_node->as.struct_decl.name.start, struct_node->as.struct_decl.name.length);

		if (struct_def == NULL) {
			// A duplicate struct name, already reported by register_structs().
			continue;
		}
		collect_struct_method_signatures(&script, struct_node, struct_def, heap, strings, filename, source, file_globals, home_module);
	}
	had_error = had_error || script.had_error;

	signatures = collect_signatures(&script, program, heap, strings, filename, source, file_globals, home_module, &file_signatures);
	had_error = had_error || script.had_error;

	for (i = 0; i < program->as.program.functions.count; i++) {
		if (signatures[i] == NULL) {
			continue;
		}
		fn_node = program->as.program.functions.items[i];
		compile_function_decl(fn_node, signatures[i], heap, strings, filename, source, is_module_def, home_module, out_exports, &file_signatures, imported_modules, imported_module_count, structs, &globals, false, &had_error);

		emit_constant(&script, value_obj((obj_t *)signatures[i]->function), fn_node->line, fn_node->column);
		emit_name_op_str(&script, OP_DEFINE_GLOBAL, signatures[i]->mangled_name, strlen(signatures[i]->mangled_name), fn_node->line, fn_node->column);
	}

	free(signatures);

	// Global initializers run after every function's OP_DEFINE_GLOBAL
	// above (so an initializer expression may call any function in this
	// file), each compiled in file order into this same top-level chunk:
	// an initializer may reference an earlier-declared global (the same
	// top-to-bottom rule a struct field's default_value already follows)
	// but never one declared later in the file. A plain top-to-bottom
	// rule, unlike Go's package-level init dependency-order resolution.
	// OP_DEFINE_GLOBAL is reused unchanged: its job is already
	// "take TOS, table_set it into globals under this name", exactly
	// what a variable's first (and only compile-time-known) assignment
	// needs.
	for (i = 0; i < program->as.program.globals.count; i++) {
		const ast_node_t *global_node = program->as.program.globals.items[i];
		const global_def_t *def = global_registry_find(&globals, global_node->as.var_decl.name.start, global_node->as.var_decl.name.length);
		type_t initializer_type;

		if (def == NULL) {
			// A duplicate name, already reported by register_globals().
			continue;
		}

		initializer_type = compile_expr_typed(&script, global_node->as.var_decl.initializer, &def->type);
		if (!type_is_assignable(def->type, initializer_type)) {
			compiler_error(&script, global_node->line, global_node->column, 0, "compiler::type-mismatch", "cannot initialize a %s variable with a %s value", type_to_string(def->type), type_to_string(initializer_type));
		}

		emit_name_op_str(&script, OP_DEFINE_GLOBAL, global_node->as.var_decl.name.start, global_node->as.var_decl.name.length, global_node->line, global_node->column);
	}
	had_error = had_error || script.had_error;

	// Struct function bodies compile last (of the two), once every
	// signature, top-level and every struct's own, is already known.
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

	// A module file's main() only exists to host `export` statements
	// (already fully handled at compile time, see compile_export_stmt());
	// it never actually runs. Only the entry file's main() gets called,
	// or, in run_tests_mode, every @test-tagged function instead; main()
	// is still required to exist either way (the check above), just not
	// called in that mode.
	if (home_module == NULL && main_sig != NULL) {
		if (run_tests_mode) {
			emit_test_runner(&script, program, &file_signatures);
		} else {
			emit_name_op_str(&script, OP_GET_GLOBAL, main_sig->mangled_name, strlen(main_sig->mangled_name), program->line, program->column);
			emit_bytes(&script, OP_CALL, 0, program->line, program->column);
			emit_byte(&script, OP_POP, program->line, program->column);
		}
	}

	emit_byte(&script, OP_NULL, program->line, program->column);
	emit_byte(&script, OP_RETURN, program->line, program->column);

	had_error = had_error || script.had_error;
	free(script.loops);

	signature_table_free(&file_signatures);
	global_registry_free(&globals);

	// Ownership transfer: an exported struct's type_t (in a function
	// signature copied into out_exports, or in a struct field/param/
	// return type an importer resolved straight out of *structs itself)
	// keeps a raw struct_def_t* into this registry, and TYPE_STRUCT
	// equality is pointer identity, so whoever asked for out_structs
	// needs the exact same struct_def_t instances to stay alive and
	// unmoved, not a copy. Only reached when this file has a #moduledef
	// directive (out_structs is passed together with out_exports, see
	// module.c's module_compile()); every other caller gets NULL here
	// and this registry is torn down the same way it always was.
	if (out_structs != NULL) {
		*out_structs = structs;
	} else {
		struct_registry_free(structs);
		free(structs);
	}

	return had_error ? NULL : script.function;
}
