#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "jit_internal.h"

static void emit_win64_call(jit_asm_t *a);

/* Sets up the frame registers and the register locals.

   mark_roots() scans frame->slots and never the registers, so the tag
   of every register local's memory slot is written here, before any
   safepoint can run. After this only the payload changes, and it
   changes in the register. The write is harmless for a parameter,
   whose tag is already correct.

   For a VAL_OBJ local the tag means "the GC may scan this cell". The
   payload tells null from object: 0 is null (see
   jit_emit_op_get_local()). A non-parameter VAL_OBJ register is zeroed
   here, so a flush in jit_emit_gc_check() can never write whatever the
   register held on entry. A parameter loads its real payload from
   memory.

   r10 and r11 are always pushed, bound or not. jit_emit_call() relies
   on that fixed set. */
void jit_emit_prologue(jit_asm_t *a, const jit_reg_alloc_t *ra, int arity) {
	int i;

	jit_emit_push_reg(a, JIT_RBP);
	jit_emit_mov_reg_reg(a, JIT_RBP, JIT_RSP);
	jit_emit_push_reg(a, JIT_RBX);
	jit_emit_push_reg(a, JIT_R12);
	jit_emit_push_reg(a, JIT_R13);
	jit_emit_push_reg(a, JIT_R14);
	jit_emit_push_reg(a, JIT_R15);
	jit_emit_push_reg(a, JIT_R10);
	jit_emit_push_reg(a, JIT_R11);
	if (JIT_ABI_WIN64) {
		// rsi and rdi are scratch in this file. Win64 makes them
		// callee-saved, so the caller expects them back.
		jit_emit_push_reg(a, JIT_RSI);
		jit_emit_push_reg(a, JIT_RDI);
	}
	jit_emit_sub_reg_imm32(a, JIT_RSP, 8);

	jit_emit_mov_reg_reg(a, JIT_R12, JIT_ENTRY_ARG_REG);

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R12, offsetof(vm_t, frame_count));
	jit_emit_sub_reg_imm32(a, JIT_RAX, 1);
	jit_emit_imul_reg_reg_imm32(a, JIT_RAX, JIT_RAX, (int32_t)sizeof(call_frame_t));
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R12, offsetof(vm_t, frames));
	jit_emit_add_reg_reg(a, JIT_RAX, JIT_RCX);
	jit_emit_mov_reg_reg(a, JIT_RBX, JIT_RAX);

	jit_emit_mov_reg_mem(a, JIT_R13, JIT_RBX, offsetof(call_frame_t, slots));
	jit_emit_mov_reg_mem(a, JIT_R14, JIT_R12, offsetof(vm_t, stack_top));

	for (i = 0; i < ra->count; i++) {
		int32_t disp = (int32_t)ra->slot[i] * (int32_t)sizeof(value_t);

		jit_emit_mov32_reg_imm32(a, JIT_RCX, (uint32_t)ra->type[i]);
		jit_emit_mov32_mem_reg(a, JIT_R13, disp, JIT_RCX);
		if (ra->slot[i] < arity) {
			jit_emit_mov_reg_mem(a, ra->reg[i], JIT_R13, disp + 8);
		} else if (ra->type[i] == VAL_OBJ) {
			jit_emit_xor_reg_reg(a, ra->reg[i], ra->reg[i]);
		}
	}
}

/* The prologue pushes 8 registers, or 10 on Win64. With the return
   address that is an odd number of 8-byte slots, so the stack is 8
   bytes off 16-byte alignment. The sub/add rsp,8 pair fixes that. */
void jit_emit_epilogue(jit_asm_t *a) {
	jit_emit_add_reg_imm32(a, JIT_RSP, 8);
	if (JIT_ABI_WIN64) {
		jit_emit_pop_reg(a, JIT_RDI);
		jit_emit_pop_reg(a, JIT_RSI);
	}
	jit_emit_pop_reg(a, JIT_R11);
	jit_emit_pop_reg(a, JIT_R10);
	jit_emit_pop_reg(a, JIT_R15);
	jit_emit_pop_reg(a, JIT_R14);
	jit_emit_pop_reg(a, JIT_R13);
	jit_emit_pop_reg(a, JIT_R12);
	jit_emit_pop_reg(a, JIT_RBX);
	jit_emit_pop_reg(a, JIT_RBP);
	jit_emit_ret(a);
}

/* next_offset is the bytecode offset one past this instruction. It is
   what the interpreter's frame->ip holds at the same point, since
   READ_BYTE() advances ip before a handler can fail or allocate. */
void jit_emit_write_frame_ip(jit_asm_t *a, const chunk_t *chunk, size_t next_offset) {
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)(chunk->code + next_offset));
	jit_emit_mov_mem_reg(a, JIT_RBX, offsetof(call_frame_t, ip), JIT_RAX);
}

/* The Win64 form of the call itself. Every call site stages its
   arguments the System V way: rdi, rsi, rdx, rcx, r8, r9. Microsoft
   x64 wants rcx, rdx, r8, r9, then two stack slots above 32 bytes of
   shadow space. This moves all six across, in an order that never
   overwrites a register before it is read. It always moves all six,
   since a helper ignores the slots it does not take. That is only
   valid while every helper takes integer or pointer arguments and
   returns one in rax, so no helper may take or return a value_t by
   value (see jit_helper_array_push() and jit_helper_make_closure()). */
static void emit_win64_call(jit_asm_t *a) {
	jit_emit_sub_reg_imm32(a, JIT_RSP, JIT_WIN64_CALL_FRAME_SIZE);
	jit_emit_mov_mem_reg(a, JIT_RSP, JIT_WIN64_ARG5_OFFSET, JIT_R8);
	jit_emit_mov_mem_reg(a, JIT_RSP, JIT_WIN64_ARG6_OFFSET, JIT_R9);
	jit_emit_mov_reg_reg(a, JIT_R9, JIT_RCX);
	jit_emit_mov_reg_reg(a, JIT_R8, JIT_RDX);
	jit_emit_mov_reg_reg(a, JIT_RDX, JIT_RSI);
	jit_emit_mov_reg_reg(a, JIT_RCX, JIT_RDI);
	jit_emit_call_reg(a, JIT_RAX);
	jit_emit_add_reg_imm32(a, JIT_RSP, JIT_WIN64_CALL_FRAME_SIZE);
}

/* Emits every call to a C function. The call site puts the callee
   address in rax first.

   r10 and r11 are caller-saved, so a register local bound to one is
   pushed before the call and popped after. This keeps the program's
   value. It is not what keeps a VAL_OBJ local safe from the GC: the
   native stack is never scanned. jit_emit_gc_check() does that, and
   every allocating call site runs it first.

   An odd number of saved registers would misalign the stack by 8
   bytes, so one pad slot is added in that case. */
void jit_emit_call(jit_asm_t *a, const jit_reg_alloc_t *ra) {
	jit_reg_t saved[JIT_MAX_REG_LOCALS];
	int saved_count;
	int i;

	saved_count = 0;
	for (i = 0; i < ra->count; i++) {
		if (ra->reg[i] != JIT_R15) {
			saved[saved_count] = ra->reg[i];
			saved_count++;
		}
	}

	if (saved_count % 2 == 1) {
		jit_emit_sub_reg_imm32(a, JIT_RSP, 8);
	}
	for (i = 0; i < saved_count; i++) {
		jit_emit_push_reg(a, saved[i]);
	}

	if (JIT_ABI_WIN64) {
		emit_win64_call(a);
	} else {
		jit_emit_call_reg(a, JIT_RAX);
	}

	for (i = saved_count - 1; i >= 0; i--) {
		jit_emit_pop_reg(a, saved[i]);
	}
	if (saved_count % 2 == 1) {
		jit_emit_add_reg_imm32(a, JIT_RSP, 8);
	}
}

/* The JIT's form of the GC check run() does per instruction. Emit it
   right before any call that can allocate.

   It always writes r14 to vm->stack_top, since mark_roots() reads that
   field.

   It also flushes every bound VAL_OBJ register to its frame slot. Only
   the payload is written. The tag is already set. mark_roots() never
   scans registers. No reload is needed after: the collector frees
   objects and never moves them.

   A register is only flushed while its local is in scope, between
   decl_end and dead_at. Outside that window the slot's memory is
   ordinary operand-stack space:
   - Before decl_end, the slot can hold the local's own initializer in
     progress. `int[] nums = [1, 2, 3];` runs OP_ARRAY's GC check while
     the three elements sit in that memory. A flush would overwrite
     them.
   - At or after dead_at, the local's scope has closed and later pushes
     reuse the memory. A runtime test against r14 cannot tell that case
     from a live local, so the window is fixed at compile time. */
void jit_emit_gc_check(jit_asm_t *a, const jit_reg_alloc_t *ra, size_t offset) {
	size_t stress_taken;
	size_t threshold_skip;
	int i;

	jit_emit_mov_mem_reg(a, JIT_R12, offsetof(vm_t, stack_top), JIT_R14);

	// Reaching this code counts as a safepoint for
	// PAWELI_GC_SAFEPOINT_CHECK (see gc_heap_note_alloc() in gc.c),
	// whether or not a collection runs. A direct memory write is cheaper
	// than a call, and rax and rcx are scratch here.
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R12, offsetof(vm_t, heap));
	jit_emit_xor_reg_reg(a, JIT_RCX, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_RAX, offsetof(gc_heap_t, bytes_since_safepoint), JIT_RCX);

	for (i = 0; i < ra->count; i++) {
		if (ra->type[i] == VAL_OBJ && (ra->decl_end[i] == (size_t)-1 || offset >= ra->decl_end[i]) && offset < ra->dead_at[i]) {
			int32_t disp = (int32_t)ra->slot[i] * (int32_t)sizeof(value_t);

			jit_emit_mov_mem_reg(a, JIT_R13, disp + 8, ra->reg[i]);
		}
	}

	jit_emit_movzx8_reg_mem(a, JIT_RAX, JIT_R12, offsetof(vm_t, gc_stress));
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	stress_taken = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R12, offsetof(vm_t, heap));
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_RAX, offsetof(gc_heap_t, bytes_allocated));
	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_RAX, offsetof(gc_heap_t, next_gc));
	jit_emit_cmp_reg_reg(a, JIT_RCX, JIT_RDX);
	threshold_skip = jit_emit_jcc(a, JIT_CC_BE);

	jit_patch_rel32(a, stress_taken, a->count);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)gc_collect);
	jit_emit_call(a, ra);

	jit_patch_rel32(a, threshold_skip, a->count);
}

/* Checks that the value stack (r14) has room. call_value() limits
   frame depth for every call, but only this guards the value stack in
   JIT code. run() does the same check per instruction. It is emitted
   where a whole new frame is about to be pushed: jit_emit_op_call()
   and jit_emit_op_try_call(). Returns the fixup site of the overflow
   jump. The caller patches it to a block that reports
   vm::stack-overflow. */
size_t jit_emit_stack_overflow_check(jit_asm_t *a) {
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R12, offsetof(vm_t, stack));
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R12, offsetof(vm_t, stack_size));
	jit_emit_imul_reg_reg_imm32(a, JIT_RCX, JIT_RCX, (int32_t)sizeof(value_t));
	jit_emit_add_reg_reg(a, JIT_RAX, JIT_RCX);
	jit_emit_cmp_reg_reg(a, JIT_R14, JIT_RAX);
	return jit_emit_jcc(a, JIT_CC_AE);
}
