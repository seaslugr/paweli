#pragma once

#include <stddef.h>
#include <stdint.h>

// Register numbers are the x86-64 encoding (0-15), so they go straight
// into ModRM and REX math.
typedef enum {
	JIT_RAX = 0,
	JIT_RCX = 1,
	JIT_RDX = 2,
	JIT_RBX = 3,
	JIT_RSP = 4,
	JIT_RBP = 5,
	JIT_RSI = 6,
	JIT_RDI = 7,
	JIT_R8 = 8,
	JIT_R9 = 9,
	JIT_R10 = 10,
	JIT_R11 = 11,
	JIT_R12 = 12,
	JIT_R13 = 13,
	JIT_R14 = 14,
	JIT_R15 = 15,
} jit_reg_t;

// Same 0-15 numbering as jit_reg_t, as a separate type so an XMM
// register is never passed as a GPR. The JIT only uses xmm0 and xmm1,
// as scratch inside one opcode. No XMM value lives across a call.
typedef enum {
	JIT_XMM0 = 0,
	JIT_XMM1 = 1,
	JIT_XMM2 = 2,
	JIT_XMM3 = 3,
	JIT_XMM4 = 4,
	JIT_XMM5 = 5,
	JIT_XMM6 = 6,
	JIT_XMM7 = 7,
	JIT_XMM8 = 8,
	JIT_XMM9 = 9,
	JIT_XMM10 = 10,
	JIT_XMM11 = 11,
	JIT_XMM12 = 12,
	JIT_XMM13 = 13,
	JIT_XMM14 = 14,
	JIT_XMM15 = 15,
} jit_xmm_reg_t;

// Values are the second opcode byte of the two-byte Jcc form (0F 8x).
typedef enum {
	JIT_CC_E = 0x84,
	JIT_CC_NE = 0x85,
	JIT_CC_B = 0x82,
	JIT_CC_AE = 0x83,
	JIT_CC_BE = 0x86,
	JIT_CC_A = 0x87,
	JIT_CC_L = 0x8C,
	JIT_CC_GE = 0x8D,
	JIT_CC_LE = 0x8E,
	JIT_CC_G = 0x8F,
} jit_cc_t;

// A growable machine-code buffer. This file only knows x86-64
// encoding. It knows nothing about paweli bytecode or runtime layout.
typedef struct {
	uint8_t *code;
	size_t count;
	size_t capacity;
} jit_asm_t;

void jit_asm_init(jit_asm_t *a);
void jit_asm_free(jit_asm_t *a);

/* dst = src (64-bit). */
void jit_emit_mov_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src);
/* dst = imm (movabs). */
void jit_emit_mov_reg_imm64(jit_asm_t *a, jit_reg_t dst, uint64_t imm);
/* dst = [base + disp] (64-bit load). */
void jit_emit_mov_reg_mem(jit_asm_t *a, jit_reg_t dst, jit_reg_t base, int32_t disp);
/* [base + disp] = src (64-bit store). */
void jit_emit_mov_mem_reg(jit_asm_t *a, jit_reg_t base, int32_t disp, jit_reg_t src);
/* dst = [base + disp], zero-extended (32-bit load). */
void jit_emit_mov32_reg_mem(jit_asm_t *a, jit_reg_t dst, jit_reg_t base, int32_t disp);
/* [base + disp] = src (32-bit store). */
void jit_emit_mov32_mem_reg(jit_asm_t *a, jit_reg_t base, int32_t disp, jit_reg_t src);
/* dst = imm, zero-extended (32-bit immediate). */
void jit_emit_mov32_reg_imm32(jit_asm_t *a, jit_reg_t dst, uint32_t imm);

/* dst += src / dst -= src / dst = dst * src (all 64-bit, two-operand). */
void jit_emit_add_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src);
void jit_emit_sub_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src);
void jit_emit_imul_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src);
void jit_emit_and_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src);
void jit_emit_or_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src);
void jit_emit_xor_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src);
/* dst = dst * imm (64-bit, three-operand). */
void jit_emit_imul_reg_reg_imm32(jit_asm_t *a, jit_reg_t dst, jit_reg_t src, int32_t imm);

/* Sets flags from a_reg - b_reg, or a_reg & b_reg. Neither operand is written. */
void jit_emit_cmp_reg_reg(jit_asm_t *a, jit_reg_t a_reg, jit_reg_t b_reg);
void jit_emit_test_reg_reg(jit_asm_t *a, jit_reg_t a_reg, jit_reg_t b_reg);

void jit_emit_add_reg_imm32(jit_asm_t *a, jit_reg_t dst, int32_t imm);
void jit_emit_sub_reg_imm32(jit_asm_t *a, jit_reg_t dst, int32_t imm);
void jit_emit_cmp_reg_imm32(jit_asm_t *a, jit_reg_t dst, int32_t imm);

void jit_emit_xor_reg_imm32(jit_asm_t *a, jit_reg_t dst, int32_t imm);

void jit_emit_neg_reg(jit_asm_t *a, jit_reg_t reg);
void jit_emit_not_reg(jit_asm_t *a, jit_reg_t reg);

/* dst = (uint8_t)[base + disp], zero-extended. */
void jit_emit_movzx8_reg_mem(jit_asm_t *a, jit_reg_t dst, jit_reg_t base, int32_t disp);
/* dst = (uint8_t)src, zero-extended. Only the low byte of src is read. */
void jit_emit_movzx8_reg_reg(jit_asm_t *a, jit_reg_t dst, jit_reg_t src);
/* Sets the low byte of reg to 1 if cc holds, else 0. The upper bytes
   keep their old value. Follow with jit_emit_movzx8_reg_reg(reg, reg)
   for a clean 0 or 1. */
void jit_emit_setcc_reg8(jit_asm_t *a, jit_cc_t cc, jit_reg_t reg);

/* Sign-extends rax into rdx:rax, as idiv's dividend setup requires. */
void jit_emit_cqo(jit_asm_t *a);
/* Signed-divides rdx:rax by reg; quotient in rax, remainder in rdx. */
void jit_emit_idiv_reg(jit_asm_t *a, jit_reg_t reg);
/* dst <<= cl, and dst = dst >> cl (arithmetic). x86-64 always takes the
   shift amount from cl. */
void jit_emit_shl_reg_cl(jit_asm_t *a, jit_reg_t dst);
void jit_emit_sar_reg_cl(jit_asm_t *a, jit_reg_t dst);

/* dst = [base + disp] (scalar double load, F2 0F 10 /r). */
void jit_emit_movsd_reg_mem(jit_asm_t *a, jit_xmm_reg_t dst, jit_reg_t base, int32_t disp);
/* [base + disp] = src (scalar double store, F2 0F 11 /r). */
void jit_emit_movsd_mem_reg(jit_asm_t *a, jit_reg_t base, int32_t disp, jit_xmm_reg_t src);

/* dst = dst OP src, all scalar double (F2 0F 58/5C/59/5E /r). */
void jit_emit_addsd_reg_reg(jit_asm_t *a, jit_xmm_reg_t dst, jit_xmm_reg_t src);
void jit_emit_subsd_reg_reg(jit_asm_t *a, jit_xmm_reg_t dst, jit_xmm_reg_t src);
void jit_emit_mulsd_reg_reg(jit_asm_t *a, jit_xmm_reg_t dst, jit_xmm_reg_t src);
void jit_emit_divsd_reg_reg(jit_asm_t *a, jit_xmm_reg_t dst, jit_xmm_reg_t src);

/* Sets flags from a_reg - b_reg as an IEEE-754 unordered compare
   (66 0F 2E /r). ZF, PF, and CF are all 1 if either operand is NaN.
   Neither operand is written. */
void jit_emit_ucomisd_reg_reg(jit_asm_t *a, jit_xmm_reg_t a_reg, jit_xmm_reg_t b_reg);

void jit_emit_push_reg(jit_asm_t *a, jit_reg_t reg);
void jit_emit_pop_reg(jit_asm_t *a, jit_reg_t reg);
void jit_emit_ret(jit_asm_t *a);
/* Calls the absolute address held in reg. */
void jit_emit_call_reg(jit_asm_t *a, jit_reg_t reg);

/* Emits a jump with a placeholder rel32 and returns the byte offset of
   that rel32. Pass it to jit_patch_rel32() once the target offset is
   known. */
size_t jit_emit_jmp(jit_asm_t *a);
size_t jit_emit_jcc(jit_asm_t *a, jit_cc_t cc);
void jit_patch_rel32(jit_asm_t *a, size_t patch_site, size_t target_offset);
