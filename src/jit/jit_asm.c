#include <stdlib.h>

#include "jit_asm.h"

static void emit8(jit_asm_t *a, uint8_t byte);
static void emit32(jit_asm_t *a, uint32_t value);
static void emit64(jit_asm_t *a, uint64_t value);
static uint8_t rex(int w, jit_reg_t reg_field, jit_reg_t rm_or_base);
static void emit_modrm_reg_reg(jit_asm_t *a, jit_reg_t reg_field, jit_reg_t rm_field);
static void emit_modrm_mem(jit_asm_t *a, jit_reg_t reg_field, jit_reg_t base, int32_t disp);
static void emit_group1_imm32(jit_asm_t *a, uint8_t digit, jit_reg_t dst, int32_t imm);
static void emit_group3(jit_asm_t *a, uint8_t digit, jit_reg_t reg);
static void emit_shift_cl(jit_asm_t *a, uint8_t digit, jit_reg_t dst);
static void emit_sse_reg_reg(jit_asm_t *a, uint8_t mandatory_prefix, uint8_t opcode, jit_xmm_reg_t dst, jit_xmm_reg_t src);

/* -fanalyzer flags the realloc() below as a theoretical leak-on-NULL
   (CWE-401): same unchecked-realloc-and-reassign idiom chunk_t's own
   chunk_write() (src/compiler/chunk.c) already uses for its growable
   buffer, which the analyzer doesn't happen to flag. This project
   doesn't check allocation failure anywhere (an out-of-memory abort
   isn't a scenario worth guarding against for a hobby VM). This is
   the same accepted pattern the analyzer's path exploration reaches
   here and not there. */
static void emit8(jit_asm_t *a, uint8_t byte) {
	if (a->count == a->capacity) {
		a->capacity = a->capacity < 64 ? 64 : a->capacity * 2;
		a->code = realloc(a->code, a->capacity);
	}
	a->code[a->count] = byte;
	a->count++;
}

static void emit32(jit_asm_t *a, uint32_t value) {
	emit8(a, (uint8_t)(value & 0xFF));
	emit8(a, (uint8_t)((value >> 8) & 0xFF));
	emit8(a, (uint8_t)((value >> 16) & 0xFF));
	emit8(a, (uint8_t)((value >> 24) & 0xFF));
}

static void emit64(jit_asm_t *a, uint64_t value) {
	emit32(a, (uint32_t)(value & 0xFFFFFFFFu));
	emit32(a, (uint32_t)(value >> 32));
}

/* Always emits a REX prefix, even a bare 0x40 with nothing set. Legal
   in 64-bit mode and simpler than tracking when one can be omitted. */
static uint8_t rex(int w, jit_reg_t reg_field, jit_reg_t rm_or_base) {
	uint8_t r;
	uint8_t b;

	r = reg_field >= JIT_R8 ? 1 : 0;
	b = rm_or_base >= JIT_R8 ? 1 : 0;
	return (uint8_t)(0x40 | (w ? 0x08 : 0) | (r << 2) | b);
}

static void emit_modrm_reg_reg(jit_asm_t *a, jit_reg_t reg_field, jit_reg_t rm_field) {
	emit8(a, (uint8_t)(0xC0 | ((reg_field & 7) << 3) | (rm_field & 7)));
}

/* Always encodes mod=10 (disp32), even when disp is 0. mod=00 with an
   rm of 101 (rbp/r13) means RIP-relative addressing instead of
   [rbp+disp], so always using mod=10 sidesteps that special case
   entirely. rm=100 (rsp/r12) always requires a SIB byte regardless of
   mod; SIB 0x24 (scale=00, index=100 "none", base=100) is that byte,
   with the actual base register communicated via REX.B, same as usual. */
static void emit_modrm_mem(jit_asm_t *a, jit_reg_t reg_field, jit_reg_t base, int32_t disp) {
	int base_low;

	base_low = base & 7;
	emit8(a, (uint8_t)(0x80 | ((reg_field & 7) << 3) | base_low));
	if (base_low == 4) {
		emit8(a, 0x24);
	}
	emit32(a, (uint32_t)disp);
}

static void emit_group1_imm32(jit_asm_t *a, uint8_t digit, jit_reg_t dst, int32_t imm) {
	emit8(a, rex(1, JIT_RAX, dst));
	emit8(a, 0x81);
	emit8(a, (uint8_t)(0xC0 | (digit << 3) | (dst & 7)));
	emit32(a, (uint32_t)imm);
}

static void emit_group3(jit_asm_t *a, uint8_t digit, jit_reg_t reg) {
	emit8(a, rex(1, JIT_RAX, reg));
	emit8(a, 0xF7);
	emit8(a, (uint8_t)(0xC0 | (digit << 3) | (reg & 7)));
}

static void emit_shift_cl(jit_asm_t *a, uint8_t digit, jit_reg_t dst) {
	emit8(a, rex(1, JIT_RAX, dst));
	emit8(a, 0xD3);
	emit8(a, (uint8_t)(0xC0 | (digit << 3) | (dst & 7)));
}

/* Every SSE2 scalar-double reg-reg instruction here shares this exact
   shape: a one-byte mandatory prefix (F2 for arithmetic, 66 for
   UCOMISD. Easy to mix up, which is exactly why it's a parameter
   rather than hardcoded per call site), then REX (W=0, since operand size
   for these is fixed by the opcode/prefix, not REX.W, unlike the GPR
   primitives above), then the two-byte 0F-prefixed opcode, then a
   mod=11 ModRM. The reg/rm role split (dst is always ModRM.reg, src is
   always ModRM.rm) matches Intel's own operand order for every
   instruction this is used for. jit_xmm_reg_t and jit_reg_t share the
   same 0-15 numbering by construction (see jit_asm.h), so casting into
   the existing GPR-typed rex()/emit_modrm_reg_reg() helpers is exactly
   as safe as it looks. They only ever do numeric >=8/mask-by-7
   arithmetic on the register number, never anything GPR-specific. */
static void emit_sse_reg_reg(jit_asm_t *a, uint8_t mandatory_prefix, uint8_t opcode, jit_xmm_reg_t dst, jit_xmm_reg_t src) {
	emit8(a, mandatory_prefix);
	emit8(a, rex(0, (jit_reg_t)dst, (jit_reg_t)src));
	emit8(a, 0x0F);
	emit8(a, opcode);
	emit_modrm_reg_reg(a, (jit_reg_t)dst, (jit_reg_t)src);
}

void jit_asm_init(jit_asm_t *a) {
	a->code = NULL;
	a->count = 0;
	a->capacity = 0;
}

void jit_asm_free(jit_asm_t *a) {
	free(a->code);
	jit_asm_init(a);
}

void jit_emit_mov_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src) {
	emit8(a, rex(1, src, dst));
	emit8(a, 0x89);
	emit_modrm_reg_reg(a, src, dst);
}

void jit_emit_mov_reg_imm64(jit_asm_t *a, jit_reg_t dst, uint64_t imm) {
	emit8(a, rex(1, JIT_RAX, dst));
	emit8(a, (uint8_t)(0xB8 + (dst & 7)));
	emit64(a, imm);
}

void jit_emit_mov_reg_mem(jit_asm_t *a, jit_reg_t dst, jit_reg_t base, int32_t disp) {
	emit8(a, rex(1, dst, base));
	emit8(a, 0x8B);
	emit_modrm_mem(a, dst, base, disp);
}

void jit_emit_mov_mem_reg(jit_asm_t *a, jit_reg_t base, int32_t disp, jit_reg_t src) {
	emit8(a, rex(1, src, base));
	emit8(a, 0x89);
	emit_modrm_mem(a, src, base, disp);
}

void jit_emit_mov32_reg_mem(jit_asm_t *a, jit_reg_t dst, jit_reg_t base, int32_t disp) {
	emit8(a, rex(0, dst, base));
	emit8(a, 0x8B);
	emit_modrm_mem(a, dst, base, disp);
}

void jit_emit_mov32_mem_reg(jit_asm_t *a, jit_reg_t base, int32_t disp, jit_reg_t src) {
	emit8(a, rex(0, src, base));
	emit8(a, 0x89);
	emit_modrm_mem(a, src, base, disp);
}

void jit_emit_mov32_reg_imm32(jit_asm_t *a, jit_reg_t dst, uint32_t imm) {
	emit8(a, rex(0, JIT_RAX, dst));
	emit8(a, (uint8_t)(0xB8 + (dst & 7)));
	emit32(a, imm);
}

void jit_emit_add_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src) {
	emit8(a, rex(1, src, dst));
	emit8(a, 0x01);
	emit_modrm_reg_reg(a, src, dst);
}

void jit_emit_sub_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src) {
	emit8(a, rex(1, src, dst));
	emit8(a, 0x29);
	emit_modrm_reg_reg(a, src, dst);
}

void jit_emit_imul_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src) {
	emit8(a, rex(1, dst, src));
	emit8(a, 0x0F);
	emit8(a, 0xAF);
	emit_modrm_reg_reg(a, dst, src);
}

void jit_emit_and_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src) {
	emit8(a, rex(1, src, dst));
	emit8(a, 0x21);
	emit_modrm_reg_reg(a, src, dst);
}

void jit_emit_or_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src) {
	emit8(a, rex(1, src, dst));
	emit8(a, 0x09);
	emit_modrm_reg_reg(a, src, dst);
}

void jit_emit_xor_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src) {
	emit8(a, rex(1, src, dst));
	emit8(a, 0x31);
	emit_modrm_reg_reg(a, src, dst);
}

void jit_emit_imul_reg_reg_imm32(jit_asm_t *a, jit_reg_t dst, jit_reg_t src, int32_t imm) {
	emit8(a, rex(1, dst, src));
	emit8(a, 0x69);
	emit_modrm_reg_reg(a, dst, src);
	emit32(a, (uint32_t)imm);
}

void jit_emit_cmp_reg_reg(jit_asm_t *a, jit_reg_t a_reg, jit_reg_t b_reg) {
	emit8(a, rex(1, b_reg, a_reg));
	emit8(a, 0x39);
	emit_modrm_reg_reg(a, b_reg, a_reg);
}

void jit_emit_test_reg_reg(jit_asm_t *a, jit_reg_t a_reg, jit_reg_t b_reg) {
	emit8(a, rex(1, b_reg, a_reg));
	emit8(a, 0x85);
	emit_modrm_reg_reg(a, b_reg, a_reg);
}

void jit_emit_add_reg_imm32(jit_asm_t *a, jit_reg_t dst, int32_t imm) {
	emit_group1_imm32(a, 0, dst, imm);
}

void jit_emit_sub_reg_imm32(jit_asm_t *a, jit_reg_t dst, int32_t imm) {
	emit_group1_imm32(a, 5, dst, imm);
}

void jit_emit_cmp_reg_imm32(jit_asm_t *a, jit_reg_t dst, int32_t imm) {
	emit_group1_imm32(a, 7, dst, imm);
}

void jit_emit_xor_reg_imm32(jit_asm_t *a, jit_reg_t dst, int32_t imm) {
	emit_group1_imm32(a, 6, dst, imm);
}

void jit_emit_neg_reg(jit_asm_t *a, jit_reg_t reg) {
	emit_group3(a, 3, reg);
}

void jit_emit_not_reg(jit_asm_t *a, jit_reg_t reg) {
	emit_group3(a, 2, reg);
}

void jit_emit_movzx8_reg_mem(jit_asm_t *a, jit_reg_t dst, jit_reg_t base, int32_t disp) {
	emit8(a, rex(0, dst, base));
	emit8(a, 0x0F);
	emit8(a, 0xB6);
	emit_modrm_mem(a, dst, base, disp);
}

void jit_emit_movzx8_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src) {
	emit8(a, rex(1, dst, src));
	emit8(a, 0x0F);
	emit8(a, 0xB6);
	emit_modrm_reg_reg(a, dst, src);
}

void jit_emit_setcc_reg8(jit_asm_t *a, jit_cc_t cc, jit_reg_t reg) {
	emit8(a, rex(0, JIT_RAX, reg));
	emit8(a, 0x0F);
	emit8(a, (uint8_t)(cc + 0x10));
	emit8(a, (uint8_t)(0xC0 | (reg & 7)));
}

/* Mem-operand forms use ModRM.reg for the XMM register regardless of
   which side (dst or src) it's on. Intel's own MOVSD encoding always
   puts the register operand in ModRM.reg and the memory operand in
   ModRM.rm, unlike the reg-reg SSE ops above where both operands are
   registers and the role split is dst=reg/src=rm by convention. The
   base GPR for addressing goes through the same emit_modrm_mem() every
   GPR load/store already uses (mod=10/disp32 always, SIB 0x24 when
   base is rsp/r12), just with an XMM register in the reg-field slot
   instead of a GPR. */
void jit_emit_movsd_reg_mem(jit_asm_t *a, jit_xmm_reg_t dst, jit_reg_t base, int32_t disp) {
	emit8(a, 0xF2);
	emit8(a, rex(0, (jit_reg_t)dst, base));
	emit8(a, 0x0F);
	emit8(a, 0x10);
	emit_modrm_mem(a, (jit_reg_t)dst, base, disp);
}

void jit_emit_movsd_mem_reg(jit_asm_t *a, jit_reg_t base, int32_t disp, jit_xmm_reg_t src) {
	emit8(a, 0xF2);
	emit8(a, rex(0, (jit_reg_t)src, base));
	emit8(a, 0x0F);
	emit8(a, 0x11);
	emit_modrm_mem(a, (jit_reg_t)src, base, disp);
}

void jit_emit_addsd_reg_reg(jit_asm_t *a, jit_xmm_reg_t dst, jit_xmm_reg_t src) {
	emit_sse_reg_reg(a, 0xF2, 0x58, dst, src);
}

void jit_emit_subsd_reg_reg(jit_asm_t *a, jit_xmm_reg_t dst, jit_xmm_reg_t src) {
	emit_sse_reg_reg(a, 0xF2, 0x5C, dst, src);
}

void jit_emit_mulsd_reg_reg(jit_asm_t *a, jit_xmm_reg_t dst, jit_xmm_reg_t src) {
	emit_sse_reg_reg(a, 0xF2, 0x59, dst, src);
}

void jit_emit_divsd_reg_reg(jit_asm_t *a, jit_xmm_reg_t dst, jit_xmm_reg_t src) {
	emit_sse_reg_reg(a, 0xF2, 0x5E, dst, src);
}

void jit_emit_ucomisd_reg_reg(jit_asm_t *a, jit_xmm_reg_t a_reg, jit_xmm_reg_t b_reg) {
	emit_sse_reg_reg(a, 0x66, 0x2E, a_reg, b_reg);
}

void jit_emit_cqo(jit_asm_t *a) {
	emit8(a, 0x48);
	emit8(a, 0x99);
}

void jit_emit_idiv_reg(jit_asm_t *a, jit_reg_t reg) {
	emit_group3(a, 7, reg);
}

void jit_emit_shl_reg_cl(jit_asm_t *a, jit_reg_t dst) {
	emit_shift_cl(a, 4, dst);
}

void jit_emit_sar_reg_cl(jit_asm_t *a, jit_reg_t dst) {
	emit_shift_cl(a, 7, dst);
}

void jit_emit_push_reg(jit_asm_t *a, jit_reg_t reg) {
	if (reg >= JIT_R8) {
		emit8(a, 0x41);
	}
	emit8(a, (uint8_t)(0x50 + (reg & 7)));
}

void jit_emit_pop_reg(jit_asm_t *a, jit_reg_t reg) {
	if (reg >= JIT_R8) {
		emit8(a, 0x41);
	}
	emit8(a, (uint8_t)(0x58 + (reg & 7)));
}

void jit_emit_ret(jit_asm_t *a) {
	emit8(a, 0xC3);
}

void jit_emit_call_reg(jit_asm_t *a, jit_reg_t reg) {
	if (reg >= JIT_R8) {
		emit8(a, 0x41);
	}
	emit8(a, 0xFF);
	emit8(a, (uint8_t)(0xD0 | (reg & 7)));
}

size_t jit_emit_jmp(jit_asm_t *a) {
	size_t patch_site;

	emit8(a, 0xE9);
	patch_site = a->count;
	emit32(a, 0);
	return patch_site;
}

size_t jit_emit_jcc(jit_asm_t *a, jit_cc_t cc) {
	size_t patch_site;

	emit8(a, 0x0F);
	emit8(a, (uint8_t)cc);
	patch_site = a->count;
	emit32(a, 0);
	return patch_site;
}

void jit_patch_rel32(jit_asm_t *a, size_t patch_site, size_t target_offset) {
	int32_t rel;

	rel = (int32_t)((int64_t)target_offset - (int64_t)(patch_site + 4));
	a->code[patch_site] = (uint8_t)(rel & 0xFF);
	a->code[patch_site + 1] = (uint8_t)((rel >> 8) & 0xFF);
	a->code[patch_site + 2] = (uint8_t)((rel >> 16) & 0xFF);
	a->code[patch_site + 3] = (uint8_t)((rel >> 24) & 0xFF);
}
