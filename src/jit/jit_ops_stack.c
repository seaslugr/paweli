#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "jit_internal.h"

void jit_emit_push_immediate(jit_asm_t *a, value_type_t type, uint64_t payload) {
	jit_emit_mov32_reg_imm32(a, JIT_RAX, (uint32_t)type);
	jit_emit_mov32_mem_reg(a, JIT_R14, 0, JIT_RAX);
	jit_emit_mov_reg_imm64(a, JIT_RCX, payload);
	jit_emit_mov_mem_reg(a, JIT_R14, 8, JIT_RCX);
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

void jit_emit_op_constant(jit_asm_t *a, const chunk_t *chunk, uint16_t const_index) {
	value_t *slot;

	slot = &chunk->constants[const_index];
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)slot);
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_RAX, 0);
	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_RAX, 8);
	jit_emit_mov_mem_reg(a, JIT_R14, 0, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, 8, JIT_RDX);
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

/* Returns the fixup site of the trailing jmp to the shared epilogue.
   The caller patches it once the epilogue offset is known. */
size_t jit_emit_op_return(jit_asm_t *a, const jit_reg_alloc_t *ra) {
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_reg(a, JIT_RSI, JIT_R13);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)vm_close_upvalues_from);
	jit_emit_call(a, ra);

	// The result is still at the top of the operand stack. The call above
	// does not touch stack contents.
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -16);
	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_R14, -8);

	// The return value overwrites the callee value at frame->slots - 1,
	// as in the interpreter's OP_RETURN. call_value() leaves the callee in
	// that slot.
	jit_emit_mov_reg_reg(a, JIT_R14, JIT_R13);
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
	jit_emit_mov_mem_reg(a, JIT_R14, 0, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, 8, JIT_RDX);
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));

	jit_emit_mov_mem_reg(a, JIT_R12, offsetof(vm_t, stack_top), JIT_R14);

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R12, offsetof(vm_t, frame_count));
	jit_emit_sub_reg_imm32(a, JIT_RAX, 1);
	jit_emit_mov32_mem_reg(a, JIT_R12, offsetof(vm_t, frame_count), JIT_RAX);

	jit_emit_mov32_reg_imm32(a, JIT_RAX, 1);

	return jit_emit_jmp(a);
}

/* count is a bytecode operand, so every displacement is a constant and
   no runtime loop is needed. See OP_DUP_N in chunk.h for the copy
   order. */
void jit_emit_op_dup_n(jit_asm_t *a, uint8_t count) {
	int32_t src_base;
	int i;

	src_base = -(int32_t)count * (int32_t)sizeof(value_t);
	for (i = 0; i < count; i++) {
		int32_t src_disp = src_base + i * (int32_t)sizeof(value_t);
		int32_t dst_disp = i * (int32_t)sizeof(value_t);

		jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R14, src_disp);
		jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, src_disp + 8);
		jit_emit_mov_mem_reg(a, JIT_R14, dst_disp, JIT_RAX);
		jit_emit_mov_mem_reg(a, JIT_R14, dst_disp + 8, JIT_RCX);
	}
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)count * (int32_t)sizeof(value_t));
}

/* A register local's register holds its payload, so the push needs no
   memory read. For a scalar local the tag is a constant (ra->type[i]):
   the compiler only makes a slot register-eligible if its type never
   changes.

   A VAL_OBJ local can hold null, so its tag is rebuilt here from the
   payload: 0 is null, anything else is an object. A live object is
   never at address 0. This is the only place that rebuild happens.
   Every other opcode reads the value from the VM stack. */
void jit_emit_op_get_local(jit_asm_t *a, uint8_t slot, const jit_reg_alloc_t *ra) {
	int32_t disp;
	int i;

	for (i = 0; i < ra->count; i++) {
		if (ra->slot[i] >= 0 && slot == (uint8_t)ra->slot[i]) {
			if (ra->type[i] == VAL_OBJ) {
				size_t null_site;
				size_t tag_done;

				jit_emit_test_reg_reg(a, ra->reg[i], ra->reg[i]);
				null_site = jit_emit_jcc(a, JIT_CC_E);
				jit_emit_mov32_reg_imm32(a, JIT_RCX, (uint32_t)VAL_OBJ);
				tag_done = jit_emit_jmp(a);
				jit_patch_rel32(a, null_site, a->count);
				jit_emit_mov32_reg_imm32(a, JIT_RCX, (uint32_t)VAL_NULL);
				jit_patch_rel32(a, tag_done, a->count);
			} else {
				jit_emit_mov32_reg_imm32(a, JIT_RCX, (uint32_t)ra->type[i]);
			}
			jit_emit_mov32_mem_reg(a, JIT_R14, 0, JIT_RCX);
			jit_emit_mov_mem_reg(a, JIT_R14, 8, ra->reg[i]);
			jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
			return;
		}
	}

	disp = (int32_t)slot * (int32_t)sizeof(value_t);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R13, disp);
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R13, disp + 8);
	jit_emit_mov_mem_reg(a, JIT_R14, 0, JIT_RAX);
	jit_emit_mov_mem_reg(a, JIT_R14, 8, JIT_RCX);
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

/* For a register local, read the payload at the top of the stack into
   its register. The tag in memory is never rewritten (see
   jit_emit_prologue()). */
void jit_emit_op_set_local(jit_asm_t *a, uint8_t slot, const jit_reg_alloc_t *ra) {
	int32_t disp;
	int i;

	for (i = 0; i < ra->count; i++) {
		if (ra->slot[i] >= 0 && slot == (uint8_t)ra->slot[i]) {
			jit_emit_mov_reg_mem(a, ra->reg[i], JIT_R14, -8);
			return;
		}
	}

	disp = (int32_t)slot * (int32_t)sizeof(value_t);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R14, -16);
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -8);
	jit_emit_mov_mem_reg(a, JIT_R13, disp, JIT_RAX);
	jit_emit_mov_mem_reg(a, JIT_R13, disp + 8, JIT_RCX);
}

/* Returns the fixup site of the forward conditional jump. Does not pop
   the condition, like peek(vm, 0) in the interpreter. */
size_t jit_emit_op_jump_if_false(jit_asm_t *a) {
	jit_emit_compute_falsey(a, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	return jit_emit_jcc(a, JIT_CC_NE);
}

/* globals is function->globals, a fixed address for the life of the
   function, so it is baked in as a constant. There is no receiver to
   check, only whether the table's entries have moved (see generation
   in table.h).

   Fast path: the cell has an entry and its generation matches the
   table's. The value is read straight from the entry with no call.
   Otherwise jit_helper_get_global_cached() does the real lookup, which
   can fail, and refreshes the cell. Both paths give the same result.

   frame->ip is only written on the slow path. Only vm_runtime_error()
   reads it, and the fast path cannot fail. */
void jit_emit_op_get_global(jit_asm_t *a, const chunk_t *chunk, table_t *globals, uint16_t const_index, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_table_cache_t *cache) {
	obj_string_t *name;
	size_t miss1;
	size_t miss2;
	size_t slow_offset;
	size_t done_site;

	name = (obj_string_t *)value_as_obj(chunk->constants[const_index]);

	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)cache);
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_RAX, offsetof(jit_table_cache_t, entry));
	jit_emit_test_reg_reg(a, JIT_RCX, JIT_RCX);
	miss1 = jit_emit_jcc(a, JIT_CC_E);

	jit_emit_mov32_reg_mem(a, JIT_RDX, JIT_RAX, offsetof(jit_table_cache_t, generation));
	jit_emit_mov_reg_imm64(a, JIT_R8, (uint64_t)(uintptr_t)globals);
	jit_emit_mov32_reg_mem(a, JIT_R9, JIT_R8, offsetof(table_t, generation));
	jit_emit_cmp_reg_reg(a, JIT_RDX, JIT_R9);
	miss2 = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov32_reg_mem(a, JIT_RDX, JIT_RCX, offsetof(table_entry_t, value));
	jit_emit_mov_reg_mem(a, JIT_R8, JIT_RCX, offsetof(table_entry_t, value) + 8);
	jit_emit_mov32_mem_reg(a, JIT_R14, 0, JIT_RDX);
	jit_emit_mov_mem_reg(a, JIT_R14, 8, JIT_R8);
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
	done_site = jit_emit_jmp(a);

	slow_offset = a->count;
	jit_patch_rel32(a, miss1, slow_offset);
	jit_patch_rel32(a, miss2, slow_offset);

	jit_emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RSI, (uint64_t)(uintptr_t)globals);
	jit_emit_mov_reg_imm64(a, JIT_RDX, (uint64_t)(uintptr_t)name);
	jit_emit_mov_reg_reg(a, JIT_RCX, JIT_R14);
	jit_emit_mov_reg_imm64(a, JIT_R8, (uint64_t)(uintptr_t)cache);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_get_global_cached);
	jit_emit_call(a, ra);
	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));

	jit_patch_rel32(a, done_site, a->count);
}

/* The write side of jit_emit_op_get_global(), with the same cache test
   and its own cell. OP_SET_GLOBAL leaves its value on the stack, so
   the fast path reads the top slot and writes it through the cached
   entry without moving r14. The slow path passes that slot's address
   to jit_helper_set_global_cached(). */
void jit_emit_op_set_global(jit_asm_t *a, const chunk_t *chunk, table_t *globals, uint16_t const_index, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_table_cache_t *cache) {
	obj_string_t *name;
	size_t miss1;
	size_t miss2;
	size_t slow_offset;
	size_t done_site;

	name = (obj_string_t *)value_as_obj(chunk->constants[const_index]);

	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)cache);
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_RAX, offsetof(jit_table_cache_t, entry));
	jit_emit_test_reg_reg(a, JIT_RCX, JIT_RCX);
	miss1 = jit_emit_jcc(a, JIT_CC_E);

	jit_emit_mov32_reg_mem(a, JIT_RDX, JIT_RAX, offsetof(jit_table_cache_t, generation));
	jit_emit_mov_reg_imm64(a, JIT_R8, (uint64_t)(uintptr_t)globals);
	jit_emit_mov32_reg_mem(a, JIT_R9, JIT_R8, offsetof(table_t, generation));
	jit_emit_cmp_reg_reg(a, JIT_RDX, JIT_R9);
	miss2 = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_R14, -16);
	jit_emit_mov_reg_mem(a, JIT_R8, JIT_R14, -8);
	jit_emit_mov32_mem_reg(a, JIT_RCX, offsetof(table_entry_t, value), JIT_RDX);
	jit_emit_mov_mem_reg(a, JIT_RCX, offsetof(table_entry_t, value) + 8, JIT_R8);
	done_site = jit_emit_jmp(a);

	slow_offset = a->count;
	jit_patch_rel32(a, miss1, slow_offset);
	jit_patch_rel32(a, miss2, slow_offset);

	jit_emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RSI, (uint64_t)(uintptr_t)globals);
	jit_emit_mov_reg_imm64(a, JIT_RDX, (uint64_t)(uintptr_t)name);
	jit_emit_mov_reg_reg(a, JIT_RCX, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RCX, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_imm64(a, JIT_R8, (uint64_t)(uintptr_t)cache);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_set_global_cached);
	jit_emit_call(a, ra);
	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));

	jit_patch_rel32(a, done_site, a->count);
}

void jit_emit_op_get_upvalue(jit_asm_t *a, uint8_t slot) {
	int32_t disp;

	disp = (int32_t)slot * 8;
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RBX, offsetof(call_frame_t, closure));
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RAX, offsetof(obj_closure_t, upvalues));
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RAX, disp);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RAX, offsetof(obj_upvalue_t, location));
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_RAX, 0);
	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_RAX, 8);
	jit_emit_mov_mem_reg(a, JIT_R14, 0, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, 8, JIT_RDX);
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

void jit_emit_op_set_upvalue(jit_asm_t *a, uint8_t slot) {
	int32_t disp;

	disp = (int32_t)slot * 8;
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RBX, offsetof(call_frame_t, closure));
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RAX, offsetof(obj_closure_t, upvalues));
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RAX, disp);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RAX, offsetof(obj_upvalue_t, location));
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -(int32_t)sizeof(value_t));
	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_R14, -(int32_t)sizeof(value_t) + 8);
	jit_emit_mov_mem_reg(a, JIT_RAX, 0, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_RAX, 8, JIT_RDX);
}

void jit_emit_op_close_upvalue(jit_asm_t *a, const jit_reg_alloc_t *ra) {
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_reg(a, JIT_RSI, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RSI, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)vm_close_upvalues_from);
	jit_emit_call(a, ra);
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}
