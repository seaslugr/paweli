#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "jit_internal.h"

/* The callee and its arg_count arguments are already on the stack, as
   for the interpreter's OP_CALL.

   The stack overflow check and the GC check always run first.

   Fast path: the callee is compared with the call cache. On a match
   this inlines what call_value() does for a function or closure: the
   frame-count check, the new call_frame_t, the frame_count increment.
   Then it calls the cached function's jit_entry directly. A different
   callee, a full frame stack, or an empty cell goes to
   jit_helper_call_cached(). */
void jit_emit_op_call(jit_asm_t *a, const chunk_t *chunk, size_t offset, uint8_t arg_count, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_call_cache_t *call_cache) {
	int32_t callee_disp;
	size_t overflow_site;
	size_t miss_tag;
	size_t miss_identity;
	size_t miss_frame_overflow;
	size_t slow_offset;
	size_t fast_done_site;
	size_t done_site;

	jit_emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_mem_reg(a, JIT_R12, offsetof(vm_t, stack_top), JIT_R14);
	overflow_site = jit_emit_stack_overflow_check(a);

	// The callee is arbitrary paweli code and can allocate, so this is a
	// GC safepoint. Without it a JIT loop would allocate with no check
	// until control returned to run(). It must run before rax and the
	// other scratch registers are loaded below.
	jit_emit_gc_check(a, ra, offset);

	callee_disp = -(int32_t)(arg_count + 1) * (int32_t)sizeof(value_t);

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, callee_disp);
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_OBJ);
	miss_tag = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_imm64(a, JIT_RCX, (uint64_t)(uintptr_t)call_cache);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R14, callee_disp + 8);
	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_RCX, offsetof(jit_call_cache_t, callee));
	jit_emit_cmp_reg_reg(a, JIT_RAX, JIT_RDX);
	miss_identity = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R12, offsetof(vm_t, frame_count));
	jit_emit_mov32_reg_mem(a, JIT_RDX, JIT_R12, offsetof(vm_t, max_frames));
	jit_emit_cmp_reg_reg(a, JIT_RAX, JIT_RDX);
	miss_frame_overflow = jit_emit_jcc(a, JIT_CC_AE);

	// frame = vm->frames + frame_count * sizeof(call_frame_t). frame_count
	// is incremented after every field is written, as in call_value().
	jit_emit_imul_reg_reg_imm32(a, JIT_RAX, JIT_RAX, (int32_t)sizeof(call_frame_t));
	jit_emit_mov_reg_mem(a, JIT_R8, JIT_R12, offsetof(vm_t, frames));
	jit_emit_add_reg_reg(a, JIT_R8, JIT_RAX);

	jit_emit_mov_reg_mem(a, JIT_R9, JIT_RCX, offsetof(jit_call_cache_t, function));
	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_RCX, offsetof(jit_call_cache_t, closure));
	jit_emit_mov_mem_reg(a, JIT_R8, offsetof(call_frame_t, function), JIT_R9);
	jit_emit_mov_mem_reg(a, JIT_R8, offsetof(call_frame_t, closure), JIT_RDX);

	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R9, offsetof(obj_function_t, chunk) + offsetof(chunk_t, code));
	jit_emit_mov_mem_reg(a, JIT_R8, offsetof(call_frame_t, ip), JIT_RAX);

	jit_emit_mov_reg_reg(a, JIT_RAX, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RAX, (int32_t)arg_count * (int32_t)sizeof(value_t));
	jit_emit_mov_mem_reg(a, JIT_R8, offsetof(call_frame_t, slots), JIT_RAX);

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R12, offsetof(vm_t, frame_count));
	jit_emit_add_reg_imm32(a, JIT_RAX, 1);
	jit_emit_mov32_mem_reg(a, JIT_R12, offsetof(vm_t, frame_count), JIT_RAX);

	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R9, offsetof(obj_function_t, jit_entry));
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_call(a, ra);
	jit_emit_mov_reg_mem(a, JIT_R14, JIT_R12, offsetof(vm_t, stack_top));
	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));
	fast_done_site = jit_emit_jmp(a);

	slow_offset = a->count;
	jit_patch_rel32(a, miss_tag, slow_offset);
	jit_patch_rel32(a, miss_identity, slow_offset);
	jit_patch_rel32(a, miss_frame_overflow, slow_offset);

	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov32_reg_imm32(a, JIT_RSI, arg_count);
	jit_emit_mov_reg_imm64(a, JIT_RDX, (uint64_t)(uintptr_t)call_cache);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_call_cached);
	jit_emit_call(a, ra);
	jit_emit_mov_reg_mem(a, JIT_R14, JIT_R12, offsetof(vm_t, stack_top));
	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));
	done_site = jit_emit_jmp(a);

	jit_patch_rel32(a, overflow_site, a->count);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_error_stack_overflow);
	jit_emit_call(a, ra);
	jit_emit_mov32_reg_imm32(a, JIT_RAX, 0);
	jit_fixup_list_push(epilogue_fixups, jit_emit_jmp(a));

	jit_patch_rel32(a, fast_done_site, a->count);
	jit_patch_rel32(a, done_site, a->count);
}

/* vm_make_closure() never fails. It returns a value_t by value, so the
   call goes through jit_helper_make_closure(), which writes the result
   straight into the next free operand-stack slot (r14). function and
   the descriptor bytes are compile-time constants baked into the code,
   the same way OP_CONSTANT bakes constant-pool addresses. frame (rbx)
   is the one runtime value needed. */
void jit_emit_op_closure(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, const jit_reg_alloc_t *ra) {
	obj_function_t *function;
	uint8_t upvalue_count;
	const uint8_t *descriptors;

	function = (obj_function_t *)value_as_obj(chunk->constants[(uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2])]);
	upvalue_count = chunk->code[offset + 3];
	descriptors = &chunk->code[offset + 4];

	jit_emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_gc_check(a, ra, offset);

	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_reg(a, JIT_RSI, JIT_RBX);
	jit_emit_mov_reg_imm64(a, JIT_RDX, (uint64_t)(uintptr_t)function);
	jit_emit_mov_reg_imm64(a, JIT_RCX, (uint64_t)(uintptr_t)descriptors);
	jit_emit_mov32_reg_imm32(a, JIT_R8, upvalue_count);
	jit_emit_mov_reg_reg(a, JIT_R9, JIT_R14);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_make_closure);
	jit_emit_call(a, ra);

	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

void jit_emit_op_throw(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	jit_emit_write_frame_ip(a, chunk, next_offset);

	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_mem(a, JIT_RSI, JIT_R14, -8);
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_throw);
	jit_emit_call(a, ra);

	jit_emit_mov32_reg_imm32(a, JIT_RAX, 0);
	jit_fixup_list_push(epilogue_fixups, jit_emit_jmp(a));
}

/* vm_try_call() runs the call and, on failure, unwinds and builds the
   error value. So this is one call and a branch. The try-block closure
   is already on the stack. It needs the stack overflow check, since
   the call can nest to any depth, and the GC check, since the failure
   path allocates the message string.

   Returns the fixup site of the forward jump to the catch handler,
   like jit_emit_op_jump_if_false(). On success the result is on the
   stack. On failure the message is.

   The failure jump is the one jump that must run a decl_end sync on
   arrival: its target can be the decl_end of the `catch(string name)`
   binding. The shared sync in the main loop only runs by fall-through.
   So the failure jump lands on a sync block emitted here, which loads
   any register local whose decl_end is target_bytecode_offset from
   [r14-8], then jumps on to the target. */
size_t jit_emit_op_try_call(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, size_t target_bytecode_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	size_t overflow_site;
	size_t ok_site;
	size_t catch_site;
	size_t catch_sync_jump;
	int ri;

	jit_emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_gc_check(a, ra, offset);
	overflow_site = jit_emit_stack_overflow_check(a);

	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)vm_try_call);
	jit_emit_call(a, ra);
	jit_emit_mov_reg_mem(a, JIT_R14, JIT_R12, offsetof(vm_t, stack_top));
	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	catch_site = jit_emit_jcc(a, JIT_CC_E);
	ok_site = jit_emit_jmp(a);

	jit_patch_rel32(a, overflow_site, a->count);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_error_stack_overflow);
	jit_emit_call(a, ra);
	jit_emit_mov32_reg_imm32(a, JIT_RAX, 0);
	jit_fixup_list_push(epilogue_fixups, jit_emit_jmp(a));

	// Catch entry. The failure jump lands here. Sync, then jump to the
	// target's ordinary entry.
	jit_patch_rel32(a, catch_site, a->count);
	for (ri = 0; ri < ra->count; ri++) {
		if (ra->decl_end[ri] == target_bytecode_offset) {
			jit_emit_mov_reg_mem(a, ra->reg[ri], JIT_R14, -8);
		}
	}
	catch_sync_jump = jit_emit_jmp(a);

	// Success lands here, after the catch sync. The value came from the
	// try block, so the sync must not run.
	jit_patch_rel32(a, ok_site, a->count);
	return catch_sync_jump;
}
