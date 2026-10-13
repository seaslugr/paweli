#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "jit_internal.h"

static void emit_call_binary_slow_path(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, uint8_t opcode, bool bitwise, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);

/* OP_EQUAL and OP_NOT_EQUAL. value_equals() never allocates and never
   fails, so the call needs no frame->ip write and no GC check. The
   helper takes pointers to the two operands on the stack. */
void jit_emit_op_comparison_bool(jit_asm_t *a, const jit_reg_alloc_t *ra, bool (*helper)(const value_t *, const value_t *)) {
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RDI, 2 * (int32_t)sizeof(value_t));
	jit_emit_mov_reg_reg(a, JIT_RSI, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RSI, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)helper);
	jit_emit_call(a, ra);

	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_mov32_reg_imm32(a, JIT_RCX, (uint32_t)VAL_BOOL);
	jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_RAX);
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

/* Leaves 1 in result_reg if the value at [r14-16] is falsey (null or
   false), else 0. For a bool it reads only the one byte value_bool()
   writes. The rest of the union is uninitialized. */
void jit_emit_compute_falsey(jit_asm_t *a, jit_reg_t result_reg) {
	size_t null_site;
	size_t not_bool_site;
	size_t done_from_bool;
	size_t done_from_truthy;

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -16);
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_NULL);
	null_site = jit_emit_jcc(a, JIT_CC_E);
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_BOOL);
	not_bool_site = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_movzx8_reg_mem(a, result_reg, JIT_R14, -8);
	jit_emit_xor_reg_imm32(a, result_reg, 1);
	done_from_bool = jit_emit_jmp(a);

	jit_patch_rel32(a, not_bool_site, a->count);
	jit_emit_mov32_reg_imm32(a, result_reg, 0);
	done_from_truthy = jit_emit_jmp(a);

	jit_patch_rel32(a, null_site, a->count);
	jit_emit_mov32_reg_imm32(a, result_reg, 1);

	jit_patch_rel32(a, done_from_bool, a->count);
	jit_patch_rel32(a, done_from_truthy, a->count);
}

void jit_emit_op_not(jit_asm_t *a) {
	jit_emit_compute_falsey(a, JIT_RAX);
	jit_emit_mov32_reg_imm32(a, JIT_RCX, (uint32_t)VAL_BOOL);
	jit_emit_mov32_mem_reg(a, JIT_R14, -16, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, -8, JIT_RAX);
}

void jit_emit_op_negate(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	size_t not_int_site;
	size_t not_float_site;
	size_t int_done;
	size_t float_done;

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -16);
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_INT);
	not_int_site = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -8);
	jit_emit_neg_reg(a, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, -8, JIT_RCX);
	int_done = jit_emit_jmp(a);

	jit_patch_rel32(a, not_int_site, a->count);
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_FLOAT);
	not_float_site = jit_emit_jcc(a, JIT_CC_NE);

	// Negating an IEEE-754 double flips the sign bit.
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -8);
	jit_emit_mov_reg_imm64(a, JIT_RDX, 0x8000000000000000ULL);
	jit_emit_xor_reg_reg(a, JIT_RCX, JIT_RDX);
	jit_emit_mov_mem_reg(a, JIT_R14, -8, JIT_RCX);
	float_done = jit_emit_jmp(a);

	jit_patch_rel32(a, not_float_site, a->count);
	jit_emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_error_negate_non_numeric);
	jit_emit_call(a, ra);
	jit_emit_mov32_reg_imm32(a, JIT_RAX, 0);
	jit_fixup_list_push(epilogue_fixups, jit_emit_jmp(a));

	jit_patch_rel32(a, int_done, a->count);
	jit_patch_rel32(a, float_done, a->count);
}

void jit_emit_op_bit_not(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	size_t not_int_site;
	size_t done_site;

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -16);
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_INT);
	not_int_site = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -8);
	jit_emit_not_reg(a, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, -8, JIT_RCX);
	done_site = jit_emit_jmp(a);

	jit_patch_rel32(a, not_int_site, a->count);
	jit_emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_error_bit_not_non_int);
	jit_emit_call(a, ra);
	jit_emit_mov32_reg_imm32(a, JIT_RAX, 0);
	jit_fixup_list_push(epilogue_fixups, jit_emit_jmp(a));

	jit_patch_rel32(a, done_site, a->count);
}

/* True for the one case where vm_binary_op() allocates: OP_ADD on two
   strings, which builds a new string. vm_bitwise_op() never allocates.
   Keep this in step with binary_op() in vm.c. If that gains another
   allocating case and this does not, the GC check below misses it. */
bool jit_binary_op_may_allocate(uint8_t opcode, bool bitwise) {
	return !bitwise && opcode == OP_ADD;
}

/* The shared slow path for arithmetic, comparison, bitwise, and shift
   opcodes. It writes r14 to vm->stack_top (the helper pops its
   operands through that field), writes frame->ip (the helper can
   report a runtime error), calls the helper, reloads r14, and jumps to
   the epilogue if the helper returned false. al already holds that
   false.

   It is a GC safepoint when jit_binary_op_may_allocate() says so. */
static void emit_call_binary_slow_path(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, uint8_t opcode, bool bitwise, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	uint64_t helper_addr;

	jit_emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_mem_reg(a, JIT_R12, offsetof(vm_t, stack_top), JIT_R14);
	// The GC check must run before the argument registers are loaded. It
	// uses rax, rcx, rdx, and rdi as scratch and would overwrite them.
	if (jit_binary_op_may_allocate(opcode, bitwise)) {
		jit_emit_gc_check(a, ra, offset);
	}
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov32_reg_imm32(a, JIT_RSI, opcode);
	helper_addr = bitwise ? (uint64_t)(uintptr_t)vm_bitwise_op : (uint64_t)(uintptr_t)vm_binary_op;
	jit_emit_mov_reg_imm64(a, JIT_RAX, helper_addr);
	jit_emit_call(a, ra);
	jit_emit_mov_reg_mem(a, JIT_R14, JIT_R12, offsetof(vm_t, stack_top));
	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));
}

/* The int and float branches write the result the same way: tag at
   [r14 - 2 * sizeof(value_t)], payload 8 bytes above it, then r14
   drops one slot. */
void jit_emit_op_arith_or_cmp(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	size_t int_fail1;
	size_t int_fail2;
	size_t float_fail1;
	size_t float_fail2;
	size_t check_float_offset;
	size_t slow_offset;
	size_t done_site;
	size_t float_done_site;

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -2 * (int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_INT);
	int_fail1 = jit_emit_jcc(a, JIT_CC_NE);
	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -(int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_INT);
	int_fail2 = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8);
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -(int32_t)sizeof(value_t) + 8);

	switch (opcode) {
		case OP_ADD:
			jit_emit_add_reg_reg(a, JIT_RAX, JIT_RCX);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_INT);
			break;
		case OP_SUBTRACT:
			jit_emit_sub_reg_reg(a, JIT_RAX, JIT_RCX);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_INT);
			break;
		case OP_MULTIPLY:
			jit_emit_imul_reg_reg(a, JIT_RAX, JIT_RCX);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_INT);
			break;
		case OP_LESS:
			jit_emit_cmp_reg_reg(a, JIT_RAX, JIT_RCX);
			jit_emit_setcc_reg8(a, JIT_CC_L, JIT_RAX);
			jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_BOOL);
			break;
		case OP_LESS_EQUAL:
			jit_emit_cmp_reg_reg(a, JIT_RAX, JIT_RCX);
			jit_emit_setcc_reg8(a, JIT_CC_LE, JIT_RAX);
			jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_BOOL);
			break;
		case OP_GREATER:
			jit_emit_cmp_reg_reg(a, JIT_RAX, JIT_RCX);
			jit_emit_setcc_reg8(a, JIT_CC_G, JIT_RAX);
			jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_BOOL);
			break;
		default:
			jit_emit_cmp_reg_reg(a, JIT_RAX, JIT_RCX);
			jit_emit_setcc_reg8(a, JIT_CC_GE, JIT_RAX);
			jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_BOOL);
			break;
	}

	jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RDX);
	jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_RAX);
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
	done_site = jit_emit_jmp(a);

	// Not both int. Try both float before the slow path. Mixed int and
	// float goes to vm_binary_op(), which handles promotion.
	check_float_offset = a->count;
	jit_patch_rel32(a, int_fail1, check_float_offset);
	jit_patch_rel32(a, int_fail2, check_float_offset);

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -2 * (int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_FLOAT);
	float_fail1 = jit_emit_jcc(a, JIT_CC_NE);
	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -(int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_FLOAT);
	float_fail2 = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_movsd_reg_mem(a, JIT_XMM0, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8);
	jit_emit_movsd_reg_mem(a, JIT_XMM1, JIT_R14, -(int32_t)sizeof(value_t) + 8);

	switch (opcode) {
		case OP_ADD:
			jit_emit_addsd_reg_reg(a, JIT_XMM0, JIT_XMM1);
			jit_emit_movsd_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_XMM0);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_FLOAT);
			jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RDX);
			break;
		case OP_SUBTRACT:
			jit_emit_subsd_reg_reg(a, JIT_XMM0, JIT_XMM1);
			jit_emit_movsd_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_XMM0);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_FLOAT);
			jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RDX);
			break;
		case OP_MULTIPLY:
			jit_emit_mulsd_reg_reg(a, JIT_XMM0, JIT_XMM1);
			jit_emit_movsd_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_XMM0);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_FLOAT);
			jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RDX);
			break;
		// Relational ops compare with the operands reversed, as C compilers do
		// for doubles. UCOMISD sets CF on an unordered compare, and SETA and
		// SETAE are false when CF is set. So a comparison with NaN is false
		// with no parity check. OP_EQUAL and OP_NOT_EQUAL never come here.
		case OP_LESS:
			jit_emit_ucomisd_reg_reg(a, JIT_XMM1, JIT_XMM0);
			jit_emit_setcc_reg8(a, JIT_CC_A, JIT_RAX);
			jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_BOOL);
			jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RDX);
			jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_RAX);
			break;
		case OP_LESS_EQUAL:
			jit_emit_ucomisd_reg_reg(a, JIT_XMM1, JIT_XMM0);
			jit_emit_setcc_reg8(a, JIT_CC_AE, JIT_RAX);
			jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_BOOL);
			jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RDX);
			jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_RAX);
			break;
		case OP_GREATER:
			jit_emit_ucomisd_reg_reg(a, JIT_XMM0, JIT_XMM1);
			jit_emit_setcc_reg8(a, JIT_CC_A, JIT_RAX);
			jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_BOOL);
			jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RDX);
			jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_RAX);
			break;
		default:
			jit_emit_ucomisd_reg_reg(a, JIT_XMM0, JIT_XMM1);
			jit_emit_setcc_reg8(a, JIT_CC_AE, JIT_RAX);
			jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
			jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_BOOL);
			jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RDX);
			jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_RAX);
			break;
	}

	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
	float_done_site = jit_emit_jmp(a);

	slow_offset = a->count;
	jit_patch_rel32(a, float_fail1, slow_offset);
	jit_patch_rel32(a, float_fail2, slow_offset);
	emit_call_binary_slow_path(a, chunk, offset, next_offset, opcode, false, epilogue_fixups, ra);

	jit_patch_rel32(a, done_site, a->count);
	jit_patch_rel32(a, float_done_site, a->count);
}

/* OP_MODULO has no float fast path. x86 has no float remainder
   instruction, so it would need a call to fmod() anyway. A float
   OP_DIVIDE needs no zero check: IEEE-754 division by zero gives inf
   or NaN and does not trap, the same as `a / b` in C. */
void jit_emit_op_div_mod(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	size_t fail1;
	size_t fail2;
	size_t fail_zero;
	size_t float_fail1;
	size_t float_fail2;
	size_t slow_offset;
	size_t done_site;
	size_t float_done_site;

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -2 * (int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_INT);
	fail1 = jit_emit_jcc(a, JIT_CC_NE);
	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -(int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_INT);
	fail2 = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -(int32_t)sizeof(value_t) + 8);
	jit_emit_test_reg_reg(a, JIT_RCX, JIT_RCX);
	fail_zero = jit_emit_jcc(a, JIT_CC_E);

	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8);
	jit_emit_cqo(a);
	jit_emit_idiv_reg(a, JIT_RCX);

	jit_emit_mov32_reg_imm32(a, JIT_R8, (uint32_t)VAL_INT);
	jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_R8);
	if (opcode == OP_DIVIDE) {
		jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_RAX);
	} else {
		jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_RDX);
	}
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
	done_site = jit_emit_jmp(a);

	if (opcode == OP_DIVIDE) {
		size_t check_float_offset;

		check_float_offset = a->count;
		jit_patch_rel32(a, fail1, check_float_offset);
		jit_patch_rel32(a, fail2, check_float_offset);

		jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -2 * (int32_t)sizeof(value_t));
		jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_FLOAT);
		float_fail1 = jit_emit_jcc(a, JIT_CC_NE);
		jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -(int32_t)sizeof(value_t));
		jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_FLOAT);
		float_fail2 = jit_emit_jcc(a, JIT_CC_NE);

		jit_emit_movsd_reg_mem(a, JIT_XMM0, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8);
		jit_emit_movsd_reg_mem(a, JIT_XMM1, JIT_R14, -(int32_t)sizeof(value_t) + 8);
		jit_emit_divsd_reg_reg(a, JIT_XMM0, JIT_XMM1);
		jit_emit_movsd_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_XMM0);
		jit_emit_mov32_reg_imm32(a, JIT_R8, (uint32_t)VAL_FLOAT);
		jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_R8);
		jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
		float_done_site = jit_emit_jmp(a);

		slow_offset = a->count;
		jit_patch_rel32(a, float_fail1, slow_offset);
		jit_patch_rel32(a, float_fail2, slow_offset);
	} else {
		slow_offset = a->count;
		jit_patch_rel32(a, fail1, slow_offset);
		jit_patch_rel32(a, fail2, slow_offset);
		float_done_site = 0;
	}
	jit_patch_rel32(a, fail_zero, slow_offset);
	emit_call_binary_slow_path(a, chunk, offset, next_offset, opcode, false, epilogue_fixups, ra);

	jit_patch_rel32(a, done_site, a->count);
	if (opcode == OP_DIVIDE) {
		jit_patch_rel32(a, float_done_site, a->count);
	}
}

void jit_emit_op_bitwise_and_or_xor(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	size_t fail1;
	size_t fail2;
	size_t slow_offset;
	size_t done_site;

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -2 * (int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_INT);
	fail1 = jit_emit_jcc(a, JIT_CC_NE);
	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -(int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_INT);
	fail2 = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8);
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -(int32_t)sizeof(value_t) + 8);
	switch (opcode) {
		case OP_BIT_AND:
			jit_emit_and_reg_reg(a, JIT_RAX, JIT_RCX);
			break;
		case OP_BIT_OR:
			jit_emit_or_reg_reg(a, JIT_RAX, JIT_RCX);
			break;
		default:
			jit_emit_xor_reg_reg(a, JIT_RAX, JIT_RCX);
			break;
	}
	jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_INT);
	jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RDX);
	jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_RAX);
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
	done_site = jit_emit_jmp(a);

	slow_offset = a->count;
	jit_patch_rel32(a, fail1, slow_offset);
	jit_patch_rel32(a, fail2, slow_offset);
	emit_call_binary_slow_path(a, chunk, offset, next_offset, opcode, true, epilogue_fixups, ra);

	jit_patch_rel32(a, done_site, a->count);
}

void jit_emit_op_shift(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	size_t fail1;
	size_t fail2;
	size_t fail_range;
	size_t slow_offset;
	size_t done_site;

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -2 * (int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_INT);
	fail1 = jit_emit_jcc(a, JIT_CC_NE);
	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -(int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_INT);
	fail2 = jit_emit_jcc(a, JIT_CC_NE);

	// The shift amount goes in rcx, since SHL and SAR take it from cl. The
	// range check below reads the same register.
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -(int32_t)sizeof(value_t) + 8);
	jit_emit_cmp_reg_imm32(a, JIT_RCX, 63);
	fail_range = jit_emit_jcc(a, JIT_CC_A);

	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8);
	if (opcode == OP_SHIFT_LEFT) {
		jit_emit_shl_reg_cl(a, JIT_RAX);
	} else {
		jit_emit_sar_reg_cl(a, JIT_RAX);
	}
	jit_emit_mov32_reg_imm32(a, JIT_RDX, (uint32_t)VAL_INT);
	jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RDX);
	jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_RAX);
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
	done_site = jit_emit_jmp(a);

	slow_offset = a->count;
	jit_patch_rel32(a, fail1, slow_offset);
	jit_patch_rel32(a, fail2, slow_offset);
	jit_patch_rel32(a, fail_range, slow_offset);
	emit_call_binary_slow_path(a, chunk, offset, next_offset, opcode, true, epilogue_fixups, ra);

	jit_patch_rel32(a, done_site, a->count);
}
