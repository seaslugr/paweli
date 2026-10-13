#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "jit_internal.h"

static void fixup_list_free(fixup_list_t *list);
static void forward_fixup_list_push(forward_fixup_list_t *list, size_t site, size_t target_bytecode_offset);
static void forward_fixup_list_free(forward_fixup_list_t *list);

void jit_fixup_list_push(fixup_list_t *list, size_t site) {
	if (list->count == list->capacity) {
		list->capacity = list->capacity < 8 ? 8 : list->capacity * 2;
		list->sites = realloc(list->sites, list->capacity * sizeof(size_t));
	}
	list->sites[list->count] = site;
	list->count++;
}

static void fixup_list_free(fixup_list_t *list) {
	free(list->sites);
	list->sites = NULL;
	list->count = 0;
	list->capacity = 0;
}

static void forward_fixup_list_push(forward_fixup_list_t *list, size_t site, size_t target_bytecode_offset) {
	if (list->count == list->capacity) {
		list->capacity = list->capacity < 8 ? 8 : list->capacity * 2;
		list->items = realloc(list->items, list->capacity * sizeof(forward_fixup_t));
	}
	list->items[list->count].site = site;
	list->items[list->count].target_bytecode_offset = target_bytecode_offset;
	list->count++;
}

static void forward_fixup_list_free(forward_fixup_list_t *list) {
	free(list->items);
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}

bool jit_compile_function(obj_function_t *function) {
	chunk_t *chunk;
	table_t *globals;
	jit_asm_t buf;
	fixup_list_t epilogue_fixups;
	forward_fixup_list_t forward_fixups;
	size_t *native_offset_for_bytecode;
	size_t offset;
	size_t epilogue_offset;
	bool ok;
	void *code_mem;
	size_t i;
	jit_reg_alloc_t ra;
	int reg_i;
	size_t lookup_cache_count;
	size_t call_cache_count;
	jit_table_cache_t *cache;
	size_t cache_index;
	jit_call_cache_t *call_cache;
	size_t call_cache_index;

	chunk = &function->chunk;
	globals = function->globals;
	jit_select_register_locals(chunk, &ra);
	// decl_end is only checked for a non-parameter register local. A
	// parameter has no declaration bytecode. jit_emit_prologue() loads its
	// register from frame->slots. dead_at is the other end of a VAL_OBJ
	// binding's live window. Both use (size_t)-1 for "none".
	for (reg_i = 0; reg_i < ra.count; reg_i++) {
		ra.decl_end[reg_i] = ra.slot[reg_i] >= function->arity ? chunk->local_reg_decl_end[ra.slot[reg_i]] : (size_t)-1;
		ra.dead_at[reg_i] = ra.slot[reg_i] >= function->arity ? chunk->local_reg_dead_at[ra.slot[reg_i]] : (size_t)-1;
	}
	// Both cache arrays get their final size before any code is emitted.
	// See jit_table_cache_t and jit_call_cache_t.
	jit_count_cached_lookup_sites(chunk, &lookup_cache_count, &call_cache_count);
	cache = lookup_cache_count > 0 ? calloc(lookup_cache_count, sizeof(jit_table_cache_t)) : NULL;
	cache_index = 0;
	call_cache = call_cache_count > 0 ? calloc(call_cache_count, sizeof(jit_call_cache_t)) : NULL;
	call_cache_index = 0;
	jit_asm_init(&buf);
	epilogue_fixups.sites = NULL;
	epilogue_fixups.count = 0;
	epilogue_fixups.capacity = 0;
	forward_fixups.items = NULL;
	forward_fixups.count = 0;
	forward_fixups.capacity = 0;
	// Allocation failure is not checked anywhere in this project.
	native_offset_for_bytecode = malloc((chunk->count + 1) * sizeof(size_t));

	jit_emit_prologue(&buf, &ra, function->arity);

	offset = 0;
	ok = true;
	while (ok && offset < chunk->count) {
		uint8_t opcode;
		uint8_t operand_a;
		uint16_t jump_operand;
		size_t next_offset;
		size_t target_bytecode_offset;

		// This is recorded after the previous opcode's decl_end sync (bottom
		// of this loop), so a jump that lands on this offset skips that sync.
		// A loop back-edge can target the same offset as a declaration's end,
		// as in `int i = 0; while (...)`. It brings no fresh value. Running
		// the sync there would load garbage from [r14-8].
		native_offset_for_bytecode[offset] = buf.count;
		opcode = chunk->code[offset];

		switch (opcode) {
			case OP_CONSTANT:
				jit_emit_op_constant(&buf, chunk, (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]));
				offset += 3;
				break;
			case OP_NULL:
				jit_emit_push_immediate(&buf, VAL_NULL, 0);
				offset += 1;
				break;
			case OP_TRUE:
				jit_emit_push_immediate(&buf, VAL_BOOL, 1);
				offset += 1;
				break;
			case OP_FALSE:
				jit_emit_push_immediate(&buf, VAL_BOOL, 0);
				offset += 1;
				break;
			case OP_POP:
				jit_emit_sub_reg_imm32(&buf, JIT_R14, (int32_t)sizeof(value_t));
				offset += 1;
				break;
			case OP_DUP_N:
				jit_emit_op_dup_n(&buf, chunk->code[offset + 1]);
				offset += 2;
				break;
			case OP_GET_LOCAL:
				jit_emit_op_get_local(&buf, chunk->code[offset + 1], &ra);
				offset += 2;
				break;
			case OP_SET_LOCAL:
				jit_emit_op_set_local(&buf, chunk->code[offset + 1], &ra);
				offset += 2;
				break;
			case OP_EQUAL:
				jit_emit_op_comparison_bool(&buf, &ra, jit_helper_equals);
				offset += 1;
				break;
			case OP_NOT_EQUAL:
				jit_emit_op_comparison_bool(&buf, &ra, jit_helper_not_equals);
				offset += 1;
				break;
			case OP_LESS:
			case OP_LESS_EQUAL:
			case OP_GREATER:
			case OP_GREATER_EQUAL:
			case OP_ADD:
			case OP_SUBTRACT:
			case OP_MULTIPLY:
				next_offset = offset + 1;
				jit_emit_op_arith_or_cmp(&buf, chunk, opcode, offset, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_DIVIDE:
			case OP_MODULO:
				next_offset = offset + 1;
				jit_emit_op_div_mod(&buf, chunk, opcode, offset, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_BIT_AND:
			case OP_BIT_OR:
			case OP_BIT_XOR:
				next_offset = offset + 1;
				jit_emit_op_bitwise_and_or_xor(&buf, chunk, opcode, offset, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_SHIFT_LEFT:
			case OP_SHIFT_RIGHT:
				next_offset = offset + 1;
				jit_emit_op_shift(&buf, chunk, opcode, offset, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_NOT:
				jit_emit_op_not(&buf);
				offset += 1;
				break;
			case OP_NEGATE:
				next_offset = offset + 1;
				jit_emit_op_negate(&buf, chunk, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_BIT_NOT:
				next_offset = offset + 1;
				jit_emit_op_bit_not(&buf, chunk, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_JUMP:
				operand_a = chunk->code[offset + 1];
				jump_operand = (uint16_t)((operand_a << 8) | chunk->code[offset + 2]);
				target_bytecode_offset = offset + 3 + jump_operand;
				forward_fixup_list_push(&forward_fixups, jit_emit_jmp(&buf), target_bytecode_offset);
				offset += 3;
				break;
			case OP_JUMP_IF_FALSE:
				operand_a = chunk->code[offset + 1];
				jump_operand = (uint16_t)((operand_a << 8) | chunk->code[offset + 2]);
				target_bytecode_offset = offset + 3 + jump_operand;
				forward_fixup_list_push(&forward_fixups, jit_emit_op_jump_if_false(&buf), target_bytecode_offset);
				offset += 3;
				break;
			case OP_LOOP:
				operand_a = chunk->code[offset + 1];
				jump_operand = (uint16_t)((operand_a << 8) | chunk->code[offset + 2]);
				target_bytecode_offset = offset + 3 - jump_operand;
				jit_patch_rel32(&buf, jit_emit_jmp(&buf), native_offset_for_bytecode[target_bytecode_offset]);
				offset += 3;
				break;
			case OP_GET_GLOBAL:
				next_offset = offset + 3;
				jit_emit_op_get_global(&buf, chunk, globals, (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]), next_offset, &epilogue_fixups, &ra, &cache[cache_index]);
				cache_index++;
				offset += 3;
				break;
			case OP_SET_GLOBAL:
				next_offset = offset + 3;
				jit_emit_op_set_global(&buf, chunk, globals, (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]), next_offset, &epilogue_fixups, &ra, &cache[cache_index]);
				cache_index++;
				offset += 3;
				break;
			case OP_CALL:
				next_offset = offset + 2;
				jit_emit_op_call(&buf, chunk, offset, chunk->code[offset + 1], next_offset, &epilogue_fixups, &ra, &call_cache[call_cache_index]);
				call_cache_index++;
				offset += 2;
				break;
			case OP_GET_INDEX:
				next_offset = offset + 1;
				jit_emit_op_get_index(&buf, chunk, offset, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_SET_INDEX:
				next_offset = offset + 1;
				jit_emit_op_set_index(&buf, chunk, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_GET_FIELD:
				jit_emit_op_get_field(&buf, chunk->code[offset + 1]);
				offset += 2;
				break;
			case OP_SET_FIELD:
				jit_emit_op_set_field(&buf, chunk->code[offset + 1]);
				offset += 2;
				break;
			case OP_ARRAY:
				next_offset = offset + 2;
				jit_emit_op_array(&buf, chunk, offset, chunk->code[offset + 1], next_offset, &ra);
				offset += 2;
				break;
			case OP_STRUCT:
				next_offset = offset + 4;
				jit_emit_op_struct(&buf, chunk, offset, chunk->code[offset + 3], (obj_string_t *)value_as_obj(chunk->constants[(uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2])]), next_offset, &ra);
				offset += 4;
				break;
			case OP_CLOSURE:
				operand_a = chunk->code[offset + 3];
				next_offset = offset + 4 + (size_t)operand_a * 2;
				jit_emit_op_closure(&buf, chunk, offset, next_offset, &ra);
				offset = next_offset;
				break;
			case OP_GET_UPVALUE:
				jit_emit_op_get_upvalue(&buf, chunk->code[offset + 1]);
				offset += 2;
				break;
			case OP_SET_UPVALUE:
				jit_emit_op_set_upvalue(&buf, chunk->code[offset + 1]);
				offset += 2;
				break;
			case OP_CLOSE_UPVALUE:
				jit_emit_op_close_upvalue(&buf, &ra);
				offset += 1;
				break;
			case OP_THROW:
				next_offset = offset + 1;
				jit_emit_op_throw(&buf, chunk, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_GET_PROPERTY:
				next_offset = offset + 3;
				jit_emit_op_get_property(&buf, chunk, (obj_string_t *)value_as_obj(chunk->constants[(uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2])]), next_offset, &epilogue_fixups, &ra, &cache[cache_index]);
				cache_index++;
				offset += 3;
				break;
			case OP_TRY_CALL:
				operand_a = chunk->code[offset + 1];
				jump_operand = (uint16_t)((operand_a << 8) | chunk->code[offset + 2]);
				next_offset = offset + 3;
				target_bytecode_offset = offset + 3 + jump_operand;
				forward_fixup_list_push(&forward_fixups, jit_emit_op_try_call(&buf, chunk, offset, next_offset, target_bytecode_offset, &epilogue_fixups, &ra), target_bytecode_offset);
				offset += 3;
				break;
			case OP_RETURN:
				jit_fixup_list_push(&epilogue_fixups, jit_emit_op_return(&buf, &ra));
				offset += 1;
				break;
			default:
				ok = false;
				break;
		}

		// A register local's declaration ends at this offset. Load its
		// register from the value just pushed at [r14-8]. A declaration inside
		// a loop body reaches this code again on every iteration by
		// fall-through. Two locals can share one decl_end.
		//
		// OP_TRY_CALL's catch handler jumps past this point and needs the same
		// sync. It emits its own copy (see jit_emit_op_try_call()).
		//
		// A VAL_OBJ local also gets its memory tag written again here.
		// jit_emit_prologue() writes the tag once, but a local in slot 0 has
		// its initializer pushed into that same memory. `Point p = null;`
		// pushes {VAL_NULL, 0} over the VAL_OBJ tag. jit_emit_gc_check() only
		// flushes the payload, so without this write the GC would see a
		// VAL_NULL cell and skip a live object.
		if (ok) {
			int ri;

			for (ri = 0; ri < ra.count; ri++) {
				if (offset == ra.decl_end[ri]) {
					if (ra.type[ri] == VAL_OBJ) {
						int32_t disp = (int32_t)ra.slot[ri] * (int32_t)sizeof(value_t);

						jit_emit_mov32_reg_imm32(&buf, JIT_RCX, (uint32_t)VAL_OBJ);
						jit_emit_mov32_mem_reg(&buf, JIT_R13, disp, JIT_RCX);
					}
					jit_emit_mov_reg_mem(&buf, ra.reg[ri], JIT_R14, -8);
				}
			}
		}
	}

	if (!ok) {
		free(cache);
		free(call_cache);
		free(native_offset_for_bytecode);
		forward_fixup_list_free(&forward_fixups);
		fixup_list_free(&epilogue_fixups);
		jit_asm_free(&buf);
		function->jit_state = JIT_FAILED;
		return false;
	}

	native_offset_for_bytecode[chunk->count] = buf.count;
	for (i = 0; i < forward_fixups.count; i++) {
		jit_patch_rel32(&buf, forward_fixups.items[i].site, native_offset_for_bytecode[forward_fixups.items[i].target_bytecode_offset]);
	}
	forward_fixup_list_free(&forward_fixups);
	free(native_offset_for_bytecode);

	epilogue_offset = buf.count;
	jit_emit_epilogue(&buf);

	for (i = 0; i < epilogue_fixups.count; i++) {
		jit_patch_rel32(&buf, epilogue_fixups.sites[i], epilogue_offset);
	}
	fixup_list_free(&epilogue_fixups);

	code_mem = jit_alloc_exec(buf.code, buf.count, function->name != NULL ? function->name->chars : "<script>");
	jit_asm_free(&buf);
	if (code_mem == NULL) {
		free(cache);
		free(call_cache);
		function->jit_state = JIT_FAILED;
		return false;
	}

	function->jit_entry = (jit_entry_fn_t)code_mem;
	function->jit_table_cache = cache;
	function->jit_call_cache = call_cache;
	function->jit_state = JIT_COMPILED;
	return true;
}
