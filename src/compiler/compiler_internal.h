#pragma once

#include <errno.h>
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


typedef struct {
	token_t name;
	type_t type;
	int depth;
	// True once a nested fn has captured this local as an upvalue. Scope
	// exit then emits OP_CLOSE_UPVALUE in place of OP_POP, so a closure
	// that outlives the scope keeps the value.
	bool is_captured;
} local_t;

// One upvalue that a nested fn's body resolved from an enclosing
// scope. With is_local set, index is a slot in the locals of the
// directly enclosing function. Otherwise index is an upvalue index of
// that function, for a capture through more than one level. type is
// needed to type-check a call through the upvalue (see
// compile_value_call()).
typedef struct {
	uint8_t index;
	bool is_local;
	type_t type;
} upvalue_t;

// One file-private `type name = expr;` declared at top level (see
// register_globals()). A global is only looked up by name to read its
// type, so the registry is a plain array of values.
typedef struct {
	const char *name;
	size_t name_length;
	type_t type;
	int line;
	int column;
} global_def_t;

// Every global declared in one file, in declaration order. Globals
// cannot be exported, so an importer never sees this.
typedef struct {
	global_def_t *defs;
	size_t count;
	size_t capacity;
} global_registry_t;

// One entry per loop being compiled. continue_target is where
// `continue` jumps: the condition for `while`, the increment for
// `for`. continue_depth and break_depth are the scope depths to pop
// locals down to before the jump. They differ for `for`, whose init
// variable survives a `continue` and dies on a `break`. break_jumps
// holds every `break` jump still to be patched to the loop's end.
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
	// The whole file as null-terminated text, for diagnostic snippets.
	// Not owned.
	const char *source;
	bool had_error;
	bool is_module_def;
	bool compiling_main;
	type_t return_type;
	bool has_return_type;

	// This file's top-level signatures, shared by every compiler_t that
	// compiles a body in this file.
	signature_table_t *file_signatures;
	// Non-NULL only in a #moduledef file: the module object that `export`
	// fills, and the signature table handed to importers.
	obj_module_t *home_module;
	signature_table_t *out_exports;
	// Every #module import this file has, zero or more.
	const module_unit_t *const *imported_modules;
	size_t imported_module_count;

	// This file's struct declarations, resolved before any signature or
	// body is compiled.
	const struct_registry_t *structs;

	// This file's global variables, resolved right after the structs,
	// since a global's type may name a struct.
	const global_registry_t *globals;

	local_t locals[MAX_LOCALS];
	int local_count;
	int scope_depth;

	// Register eligibility per slot, built as locals are added and
	// finalized onto the chunk by compiler_finalize_local_reg_info().
	// Unlike locals[], this never shrinks when a scope closes. A later
	// local in a sibling scope can reuse a slot index, and this must
	// remember every local that ever used it. See add_local() for what
	// disqualifies a slot.
	bool local_reg_seen[MAX_LOCALS];
	bool local_reg_eligible[MAX_LOCALS];
	value_type_t local_reg_types[MAX_LOCALS];
	// The bytecode offset right after an eligible local's declaration. A
	// declaration has no opcode of its own. The initializer's pushed
	// value becomes the local by stack position. The JIT emits a register
	// load at this offset (see jit_compile_function()). Not used for a
	// parameter.
	size_t local_reg_decl_end[MAX_LOCALS];
	// The bytecode offset right after an eligible local's scope-closing
	// pop. Only a real scope exit sets it, never the temporary pop of a
	// break or continue (see compiler_end_scope()). (size_t)-1 means the
	// slot stays live until the function returns, as for a parameter.
	// Only used for a VAL_OBJ local: jit_emit_gc_check() flushes the
	// register to the slot only inside [decl_end, dead_at).
	size_t local_reg_dead_at[MAX_LOCALS];
	int local_slot_high_water;

	// NULL for a top-level fn and for the "<script>" function. Non-NULL
	// while compiling a nested fn's body, where it points at the
	// compiler_t of the function that contains it.
	// compiler_resolve_upvalue() walks this chain.
	struct compiler *enclosing;
	upvalue_t upvalues[MAX_LOCALS];
	int upvalue_count;

	loop_context_t *loops;
	int loop_count;
	int loop_capacity;
} compiler_t;

extern signature_table_t compiler_native_globals;
extern signature_table_t compiler_stdlib_members;

const global_def_t *compiler_global_registry_find(const global_registry_t *registry, const char *name, size_t name_length);
void compiler_apply_decorators(compiler_t *compiler, const ast_node_t *fn_node, signature_t *sig, size_t param_count);
signature_t **compiler_collect_signatures(compiler_t *script_compiler, const ast_node_t *program, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, table_t *file_globals, obj_module_t *home_module, signature_table_t *file_signatures);
void compiler_collect_struct_method_signatures(compiler_t *script_compiler, const ast_node_t *struct_node, struct_def_t *def, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, table_t *file_globals, obj_module_t *home_module);

chunk_t *compiler_current_chunk(compiler_t *compiler);
void compiler_error(compiler_t *compiler, int line, int column, int length, const char *code, const char *format, ...);
void compiler_warning(compiler_t *compiler, int line, int column, int length, const char *code, const char *format, ...);
void compiler_emit_byte(compiler_t *compiler, uint8_t byte, int line, int column);
void compiler_emit_bytes(compiler_t *compiler, uint8_t b1, uint8_t b2, int line, int column);
size_t compiler_emit_jump(compiler_t *compiler, uint8_t instruction, int line, int column);
void compiler_patch_jump(compiler_t *compiler, size_t offset, int line);
void compiler_emit_loop(compiler_t *compiler, size_t loop_start, int line, int column);
void compiler_emit_constant(compiler_t *compiler, value_t value, int line, int column);
void compiler_emit_name_op_str(compiler_t *compiler, uint8_t opcode, const char *name, size_t length, int line, int column);
void compiler_patch_name_op(compiler_t *compiler, size_t offset, const char *name, size_t length, int line, int column);
int64_t compiler_parse_int_literal(compiler_t *compiler, token_t token, int line, int column);
int64_t compiler_parse_negated_int_literal(compiler_t *compiler, token_t token, int line, int column);
double compiler_parse_float_literal(token_t token);
bool compiler_identifiers_equal(token_t a, token_t b);
token_t compiler_synthetic_token(const char *text);

void compiler_begin_scope(compiler_t *compiler);
void compiler_end_scope(compiler_t *compiler, int line, int column);
bool compiler_register_cacheable_value_type(type_t type, value_type_t *out_vtype);
void compiler_note_local_reg_slot(compiler_t *compiler, int slot, type_t type);
void compiler_disqualify_local_reg_slot(compiler_t *compiler, int slot);
void compiler_finalize_local_reg_info(compiler_t *compiler);
bool compiler_declare_local(compiler_t *compiler, token_t name, type_t type, int line);
void compiler_mark_initialized(compiler_t *compiler);
int compiler_resolve_local(compiler_t *compiler, token_t name);
int compiler_add_upvalue(compiler_t *compiler, uint8_t index, bool is_local, type_t type, int line, int column);
int compiler_resolve_upvalue(compiler_t *compiler, token_t name);
void compiler_begin_loop(compiler_t *compiler, size_t continue_target, int continue_depth, int break_depth);
void compiler_end_loop(compiler_t *compiler, int line);
void compiler_emit_pop_to_depth(compiler_t *compiler, int depth, int line, int column);
type_t compiler_resolve_type_ref(compiler_t *compiler, type_ref_t type_ref, int line, int column);
const module_unit_t *compiler_find_imported_module(const module_unit_t *const *imported_modules, size_t imported_module_count, const char *name, size_t name_length);

void compile_function_decl(const ast_node_t *fn_node, const signature_t *sig, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, bool is_module_def, obj_module_t *home_module, signature_table_t *out_exports, signature_table_t *file_signatures, const module_unit_t *const *imported_modules, size_t imported_module_count, const struct_registry_t *structs, const global_registry_t *globals, bool is_struct_method, bool *had_error);
void compile_struct_functions(const ast_node_t *struct_node, struct_def_t *def, gc_heap_t *heap, table_t *strings, const char *filename, const char *source, bool is_module_def, obj_module_t *home_module, signature_table_t *out_exports, signature_table_t *file_signatures, const module_unit_t *const *imported_modules, size_t imported_module_count, const struct_registry_t *structs, const global_registry_t *globals, bool *had_error);
void compiler_emit_closure(compiler_t *compiler, obj_function_t *function, compiler_t *inner, int line, int column);
void compile_closure_body(compiler_t *compiler, const ast_list_t *params, ast_node_t *body, type_t *param_types, size_t param_count, type_t return_type, bool has_return_type, token_t function_name, int line, int column);
void compile_nested_function_decl(compiler_t *compiler, const ast_node_t *node);
type_t compile_lambda_expr(compiler_t *compiler, const ast_node_t *node);
type_t compile_try_expr(compiler_t *compiler, const ast_node_t *node);

void compile_statements(compiler_t *compiler, const ast_node_t *block_node);
void compile_statement(compiler_t *compiler, const ast_node_t *node);
void compile_var_decl(compiler_t *compiler, const ast_node_t *node);
void compile_export_stmt(compiler_t *compiler, const ast_node_t *node);
void compile_if_stmt(compiler_t *compiler, const ast_node_t *node);
void compile_foreach_stmt(compiler_t *compiler, const ast_node_t *node);
void compile_return_stmt(compiler_t *compiler, const ast_node_t *node);
void compiler_emit_local_increment(compiler_t *script, int slot, int line, int column);

type_t compile_expr_typed(compiler_t *compiler, const ast_node_t *node, const type_t *expected);
type_t compile_assign_expr(compiler_t *compiler, const ast_node_t *node);
type_t compile_unary_expr(compiler_t *compiler, const ast_node_t *node);
type_t compile_function_value_ref(compiler_t *compiler, const ast_node_t *node, const overload_set_t *set, const type_t *expected);
type_t compile_identifier_expr(compiler_t *compiler, const ast_node_t *node, const type_t *expected);
type_t compile_member_expr(compiler_t *compiler, const ast_node_t *node);
type_t compile_struct_literal(compiler_t *compiler, const ast_node_t *node, const type_t *expected);

type_t compile_call_expr(compiler_t *compiler, const ast_node_t *node);
type_t compile_member_call(compiler_t *compiler, const ast_node_t *node);
type_t compile_struct_call(compiler_t *compiler, const ast_node_t *node, const struct_def_t *def, token_t member_name);
type_t compiler_finish_resolved_call(compiler_t *compiler, const signature_table_t *table, const char *name, size_t name_length, int name_line, int name_column, const ast_node_t *call_node, uint8_t patch_opcode, bool use_mangled_name, const char *prefix_name, size_t prefix_name_length, obj_t *prefix_constant);

void compiler_emit_test_runner(compiler_t *script, const ast_node_t *program, const signature_table_t *file_signatures);
