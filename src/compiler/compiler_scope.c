#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_internal.h"

static bool add_local(compiler_t *compiler, token_t name, type_t type, int line);

void compiler_begin_scope(compiler_t *compiler) {
	compiler->scope_depth++;
}

void compiler_end_scope(compiler_t *compiler, int line, int column) {
	int slot;

	compiler->scope_depth--;

	while (compiler->local_count > 0 && compiler->locals[compiler->local_count - 1].depth > compiler->scope_depth) {
		if (compiler->locals[compiler->local_count - 1].is_captured) {
			compiler_emit_byte(compiler, OP_CLOSE_UPVALUE, line, column);
		} else {
			compiler_emit_byte(compiler, OP_POP, line, column);
		}
		// The scope has really closed, so the JIT must stop flushing a
		// register into this slot. See local_reg_dead_at. A break or continue
		// pops through compiler_emit_pop_to_depth() and does not come here.
		slot = compiler->local_count - 1;
		if (compiler->local_reg_eligible[slot]) {
			compiler->local_reg_dead_at[slot] = compiler_current_chunk(compiler)->count;
		}
		compiler->local_count--;
	}
}

/* True if a local of this type can live in a JIT register. Writes its
   runtime tag to out_vtype. int, float, and bool are scalars with a
   fixed tag. string, struct, closure, and every array are VAL_OBJ: a
   heap pointer whose tag jit_emit_op_get_local() rebuilds from the
   payload. An `int[]` is an obj_array_t* at runtime although its kind
   is TYPE_INT, so array_depth is tested first. */
bool compiler_register_cacheable_value_type(type_t type, value_type_t *out_vtype) {
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

/* Updates a slot's register eligibility when a new local claims it. A
   slot is eligible only if its type is register-cacheable and exactly
   one local uses it in the whole function. A second declaration that
   reuses the slot index in a sibling scope disqualifies it, even with
   a matching type. The JIT records one declaration point per slot
   (local_reg_decl_end) and would miss the second. A disqualified slot
   never becomes eligible again. */
void compiler_note_local_reg_slot(compiler_t *compiler, int slot, type_t type) {
	value_type_t vtype;
	bool cacheable;

	cacheable = compiler_register_cacheable_value_type(type, &vtype);

	if (!compiler->local_reg_seen[slot]) {
		compiler->local_reg_seen[slot] = true;
		compiler->local_reg_eligible[slot] = cacheable;
		if (cacheable) {
			compiler->local_reg_types[slot] = vtype;
			compiler->local_reg_decl_end[slot] = compiler_current_chunk(compiler)->count;
			compiler->local_reg_dead_at[slot] = (size_t)-1;
		}
	} else {
		compiler->local_reg_eligible[slot] = false;
	}

	if (slot + 1 > compiler->local_slot_high_water) {
		compiler->local_slot_high_water = slot + 1;
	}
}

/* Revokes a slot's register eligibility. Used when a `null` literal
   can flow into a scalar slot (see compile_var_decl() and
   compile_assign_expr()). A scalar payload of 0 is a real value, so
   there is no way to rebuild VAL_NULL from it as there is for VAL_OBJ.
   The slot goes back to plain memory, where the tag is stored. A call
   before the slot is marked seen has no effect. */
void compiler_disqualify_local_reg_slot(compiler_t *compiler, int slot) {
	compiler->local_reg_eligible[slot] = false;
}

/* Copies the per-slot eligibility table onto the chunk. Called once
   per function after its whole body is compiled, so
   local_slot_high_water covers every slot the function used. */
void compiler_finalize_local_reg_info(compiler_t *compiler) {
	chunk_t *chunk;
	int count;
	int i;

	chunk = compiler_current_chunk(compiler);
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

/* Returns false and reports an error when the locals table is full.
   Compiler state is not changed. */
static bool add_local(compiler_t *compiler, token_t name, type_t type, int line) {
	local_t *local;

	if (compiler->local_count >= MAX_LOCALS) {
		compiler_error(compiler, line, name.column, (int)name.length, "compiler::too-many-locals", "too many local variables in one function");
		return false;
	}

	compiler_note_local_reg_slot(compiler, compiler->local_count, type);

	local = &compiler->locals[compiler->local_count];
	local->name = name;
	local->type = type;
	local->depth = -1;
	local->is_captured = false;
	compiler->local_count++;
	return true;
}

/* A name only collides with a local of the current scope. Walking
   backward, the first local from an outer scope ends the search.

   Returns false and reports an error on a duplicate. No local is
   added. The caller must then skip compiler_mark_initialized(), which
   would otherwise re-stamp the depth of the local on top. */
bool compiler_declare_local(compiler_t *compiler, token_t name, type_t type, int line) {
	int i;

	for (i = compiler->local_count - 1; i >= 0; i--) {
		if (compiler->locals[i].depth != -1 && compiler->locals[i].depth < compiler->scope_depth) {
			break;
		}
		if (compiler_identifiers_equal(name, compiler->locals[i].name)) {
			compiler_error(compiler, line, name.column, (int)name.length, "compiler::duplicate-local", "a variable with this name already exists in this scope");
			return false;
		}
	}

	return add_local(compiler, name, type, line);
}

void compiler_mark_initialized(compiler_t *compiler) {
	compiler->locals[compiler->local_count - 1].depth = compiler->scope_depth;
}

int compiler_resolve_local(compiler_t *compiler, token_t name) {
	int i;

	for (i = compiler->local_count - 1; i >= 0; i--) {
		if (compiler_identifiers_equal(name, compiler->locals[i].name)) {
			if (compiler->locals[i].depth == -1) {
				compiler_error(compiler, name.line, name.column, (int)name.length, "compiler::local-read-in-own-initializer", "cannot read a local variable in its own initializer");
			}
			return i;
		}
	}

	return -1;
}

/* Records a resolved upvalue, or reuses the entry for the same slot.
   Two closures in one scope that capture the same local must get the
   same upvalue index, so they share one obj_upvalue_t at runtime (see
   capture_upvalue() in vm.c). The key is index plus is_local. index
   alone could mean a local slot or an upvalue slot. */
int compiler_add_upvalue(compiler_t *compiler, uint8_t index, bool is_local, type_t type, int line, int column) {
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

/* Looks for name as a local in each enclosing compiler_t, outward. It
   adds an upvalue entry to every function in between, so a closure
   nested several levels deep can reach it. Returns -1 if there is no
   enclosing scope or the name is not found. */
int compiler_resolve_upvalue(compiler_t *compiler, token_t name) {
	int local;
	int upvalue;

	if (compiler->enclosing == NULL) {
		return -1;
	}

	local = compiler_resolve_local(compiler->enclosing, name);
	if (local != -1) {
		compiler->enclosing->locals[local].is_captured = true;
		// A closure reads and writes a captured local's slot as memory,
		// through obj_upvalue_t->location. A JIT register copy would miss
		// those writes, so capture disqualifies the slot for good.
		compiler->enclosing->local_reg_eligible[local] = false;
		return compiler_add_upvalue(compiler, (uint8_t)local, true, compiler->enclosing->locals[local].type, name.line, name.column);
	}

	upvalue = compiler_resolve_upvalue(compiler->enclosing, name);
	if (upvalue != -1) {
		return compiler_add_upvalue(compiler, (uint8_t)upvalue, false, compiler->enclosing->upvalues[upvalue].type, name.line, name.column);
	}

	return -1;
}

void compiler_begin_loop(compiler_t *compiler, size_t continue_target, int continue_depth, int break_depth) {
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

/* Patches every break in this loop to land here. Call it once all of
   the loop's bytecode is emitted, including the pops of any wrapping
   scope. */
void compiler_end_loop(compiler_t *compiler, int line) {
	loop_context_t *loop;
	size_t i;

	loop = &compiler->loops[compiler->loop_count - 1];
	for (i = 0; i < loop->break_jump_count; i++) {
		compiler_patch_jump(compiler, loop->break_jumps[i], line);
	}
	free(loop->break_jumps);
	compiler->loop_count--;
}

/* Emits pops for every local deeper than depth without changing
   compiler->locals[]. Used for break and continue, which leave the
   scopes early but do not end them. The code after the jump still
   needs locals[] as the normal path sees it. */
void compiler_emit_pop_to_depth(compiler_t *compiler, int depth, int line, int column) {
	int i;

	for (i = compiler->local_count - 1; i >= 0 && compiler->locals[i].depth > depth; i--) {
		if (compiler->locals[i].is_captured) {
			compiler_emit_byte(compiler, OP_CLOSE_UPVALUE, line, column);
		} else {
			compiler_emit_byte(compiler, OP_POP, line, column);
		}
	}
}

/* Resolves a type_ref_t against the file's structs, then against the
   exported structs of every import. Reports "unknown type" if nothing
   matches and "ambiguous-struct-name" if more than one import matches.
   Returns TYPE_VOID in both cases. Every call site that wants those
   errors reported uses this, so each is reported once, at the position
   that names the type. */
type_t compiler_resolve_type_ref(compiler_t *compiler, type_ref_t type_ref, int line, int column) {
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

/* Finds an imported module by its local name. Returns NULL if there is
   none. */
const module_unit_t *compiler_find_imported_module(const module_unit_t *const *imported_modules, size_t imported_module_count, const char *name, size_t name_length) {
	size_t i;

	for (i = 0; i < imported_module_count; i++) {
		if (imported_modules[i]->local_name_length == name_length && memcmp(imported_modules[i]->local_name, name, name_length) == 0) {
			return imported_modules[i];
		}
	}

	return NULL;
}
