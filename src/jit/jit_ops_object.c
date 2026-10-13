#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "jit_internal.h"

/* Container at [r14-32], index at [r14-16]. Pops two, pushes one.

   Fast path: an array, an int index, in bounds. It reads the 16 bytes
   at array->values[index] with no call. It cannot allocate or fail, so
   it needs no GC check and no frame->ip write. Everything else goes to
   the helper, which is the full implementation.

   Only scratch registers are used: rax, rcx, rdx, rsi, r8. The bounds
   check matches vm_get_index(): `index < 0 || (size_t)index >= count`.
   TEST clears OF, so jl tests the sign. The count compare is unsigned. */
void jit_emit_op_get_index(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	size_t fail_obj;
	size_t fail_array;
	size_t fail_int;
	size_t fail_neg;
	size_t fail_count;
	size_t slow_offset;
	size_t done_site;

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -2 * (int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_OBJ);
	fail_obj = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8);
	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_RCX, 0);
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)OBJ_ARRAY);
	fail_array = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -(int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_INT);
	fail_int = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_R14, -(int32_t)sizeof(value_t) + 8);
	jit_emit_test_reg_reg(a, JIT_RDX, JIT_RDX);
	fail_neg = jit_emit_jcc(a, JIT_CC_L);
	jit_emit_mov_reg_mem(a, JIT_RSI, JIT_RCX, offsetof(obj_array_t, values));
	jit_emit_mov_reg_mem(a, JIT_R8, JIT_RCX, offsetof(obj_array_t, count));
	jit_emit_cmp_reg_reg(a, JIT_RDX, JIT_R8);
	fail_count = jit_emit_jcc(a, JIT_CC_AE);

	jit_emit_imul_reg_reg_imm32(a, JIT_RDX, JIT_RDX, (int32_t)sizeof(value_t));
	jit_emit_add_reg_reg(a, JIT_RDX, JIT_RSI);
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_RDX, 0);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RDX, 8);
	jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_RAX);
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
	done_site = jit_emit_jmp(a);

	slow_offset = a->count;
	jit_patch_rel32(a, fail_obj, slow_offset);
	jit_patch_rel32(a, fail_array, slow_offset);
	jit_patch_rel32(a, fail_int, slow_offset);
	jit_patch_rel32(a, fail_neg, slow_offset);
	jit_patch_rel32(a, fail_count, slow_offset);

	// Slow path. jit_helper_get_index() can allocate (indexing a string
	// builds a string) and can fail, so it needs the GC check and the
	// frame->ip write. out is the container's own slot. The helper copies
	// its arguments before it writes through out.
	jit_emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_gc_check(a, ra, offset);

	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_reg(a, JIT_RSI, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RSI, 2 * (int32_t)sizeof(value_t));
	jit_emit_mov_reg_reg(a, JIT_RDX, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RDX, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_reg(a, JIT_RCX, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RCX, 2 * (int32_t)sizeof(value_t));
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_get_index);
	jit_emit_call(a, ra);

	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));

	jit_patch_rel32(a, done_site, a->count);
}

/* Container at [r14-48], index at [r14-32], value at [r14-16]. Pops
   three, pushes one. The fast path has the same shape as
   jit_emit_op_get_index(): an in-bounds int index into an array is one
   16-byte write. Everything else goes to jit_helper_set_index(). The
   result is the stored value, written to the container's slot. */
void jit_emit_op_set_index(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	size_t fail_obj;
	size_t fail_array;
	size_t fail_int;
	size_t fail_neg;
	size_t fail_count;
	size_t slow_offset;
	size_t done_site;

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -3 * (int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_OBJ);
	fail_obj = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -3 * (int32_t)sizeof(value_t) + 8);
	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_RCX, 0);
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)OBJ_ARRAY);
	fail_array = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -2 * (int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_INT);
	fail_int = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8);
	jit_emit_test_reg_reg(a, JIT_RDX, JIT_RDX);
	fail_neg = jit_emit_jcc(a, JIT_CC_L);
	jit_emit_mov_reg_mem(a, JIT_RSI, JIT_RCX, offsetof(obj_array_t, values));
	jit_emit_mov_reg_mem(a, JIT_R8, JIT_RCX, offsetof(obj_array_t, count));
	jit_emit_cmp_reg_reg(a, JIT_RDX, JIT_R8);
	fail_count = jit_emit_jcc(a, JIT_CC_AE);

	jit_emit_imul_reg_reg_imm32(a, JIT_RDX, JIT_RDX, (int32_t)sizeof(value_t));
	jit_emit_add_reg_reg(a, JIT_RDX, JIT_RSI);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R14, -(int32_t)sizeof(value_t));
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -(int32_t)sizeof(value_t) + 8);
	jit_emit_mov_mem_reg(a, JIT_RDX, 0, JIT_RAX);
	jit_emit_mov_mem_reg(a, JIT_RDX, 8, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, -3 * (int32_t)sizeof(value_t), JIT_RAX);
	jit_emit_mov_mem_reg(a, JIT_R14, -3 * (int32_t)sizeof(value_t) + 8, JIT_RCX);
	jit_emit_sub_reg_imm32(a, JIT_R14, 2 * (int32_t)sizeof(value_t));
	done_site = jit_emit_jmp(a);

	slow_offset = a->count;
	jit_patch_rel32(a, fail_obj, slow_offset);
	jit_patch_rel32(a, fail_array, slow_offset);
	jit_patch_rel32(a, fail_int, slow_offset);
	jit_patch_rel32(a, fail_neg, slow_offset);
	jit_patch_rel32(a, fail_count, slow_offset);

	// Slow path. vm_set_index() can fail but never allocates, so this
	// needs the frame->ip write and no GC check. out is the container's
	// own slot.
	jit_emit_write_frame_ip(a, chunk, next_offset);

	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_reg(a, JIT_RSI, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RSI, 3 * (int32_t)sizeof(value_t));
	jit_emit_mov_reg_reg(a, JIT_RDX, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RDX, 2 * (int32_t)sizeof(value_t));
	jit_emit_mov_reg_reg(a, JIT_RCX, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RCX, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_reg(a, JIT_R8, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_R8, 3 * (int32_t)sizeof(value_t));
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_set_index);
	jit_emit_call(a, ra);

	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));
	jit_emit_sub_reg_imm32(a, JIT_R14, 2 * (int32_t)sizeof(value_t));

	jit_patch_rel32(a, done_site, a->count);
}

/* No type or bounds check. The compiler only emits field_index for an
   object it has proved is an instance of that struct type (see
   OP_GET_FIELD in run(), vm.c). */
void jit_emit_op_get_field(jit_asm_t *a, uint8_t field_index) {
	int32_t disp;

	disp = (int32_t)field_index * (int32_t)sizeof(value_t);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R14, -(int32_t)sizeof(value_t) + 8);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RAX, offsetof(obj_struct_t, fields));
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_RAX, disp);
	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_RAX, disp + 8);
	jit_emit_mov_mem_reg(a, JIT_R14, -(int32_t)sizeof(value_t), JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, -(int32_t)sizeof(value_t) + 8, JIT_RDX);
}

void jit_emit_op_set_field(jit_asm_t *a, uint8_t field_index) {
	int32_t disp;

	disp = (int32_t)field_index * (int32_t)sizeof(value_t);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RAX, offsetof(obj_struct_t, fields));
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -(int32_t)sizeof(value_t));
	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_R14, -(int32_t)sizeof(value_t) + 8);
	jit_emit_mov_mem_reg(a, JIT_RAX, disp, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_RAX, disp + 8, JIT_RDX);

	jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_RDX);
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

/* object_new_array() and object_array_push() never fail, so there is
   no bailout path.

   The new array's pointer must survive the push calls. r15, r10, and
   r11 may hold register locals, so it is kept on the native stack. The
   slot is 16 bytes to keep the stack aligned.

   Each element is passed to jit_helper_array_push() by address. It
   already sits on the operand stack. */
void jit_emit_op_array(jit_asm_t *a, const chunk_t *chunk, size_t offset, uint8_t count, size_t next_offset, const jit_reg_alloc_t *ra) {
	int32_t src_base;
	int i;

	jit_emit_write_frame_ip(a, chunk, next_offset);
	// Covers object_new_array() and every push below. It must run before
	// any argument register is loaded.
	jit_emit_gc_check(a, ra, offset);

	jit_emit_mov_reg_mem(a, JIT_RDI, JIT_R12, offsetof(vm_t, heap));
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)object_new_array);
	jit_emit_call(a, ra);
	jit_emit_sub_reg_imm32(a, JIT_RSP, 16);
	jit_emit_mov_mem_reg(a, JIT_RSP, 0, JIT_RAX);

	src_base = -(int32_t)count * (int32_t)sizeof(value_t);
	for (i = 0; i < count; i++) {
		int32_t disp = src_base + i * (int32_t)sizeof(value_t);

		jit_emit_mov_reg_mem(a, JIT_RDI, JIT_R12, offsetof(vm_t, heap));
		jit_emit_mov_reg_mem(a, JIT_RSI, JIT_RSP, 0);
		jit_emit_mov_reg_reg(a, JIT_RDX, JIT_R14);
		jit_emit_add_reg_imm32(a, JIT_RDX, disp);
		jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_array_push);
		jit_emit_call(a, ra);
	}

	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RSP, 0);
	jit_emit_add_reg_imm32(a, JIT_RSP, 16);

	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)count * (int32_t)sizeof(value_t));
	jit_emit_mov32_reg_imm32(a, JIT_RCX, (uint32_t)VAL_OBJ);
	jit_emit_mov32_mem_reg(a, JIT_R14, 0, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, 8, JIT_RAX);
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

/* object_new_struct() never fails. A struct's field array has a fixed
   size, so each field is a direct memory write with no helper call.

   The struct pointer is kept on the native stack while rax holds the
   fields pointer, for the same reason as in jit_emit_op_array(). */
void jit_emit_op_struct(jit_asm_t *a, const chunk_t *chunk, size_t offset, uint8_t count, obj_string_t *type_name, size_t next_offset, const jit_reg_alloc_t *ra) {
	int32_t src_base;
	int i;

	jit_emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_gc_check(a, ra, offset);

	jit_emit_mov_reg_mem(a, JIT_RDI, JIT_R12, offsetof(vm_t, heap));
	jit_emit_mov_reg_imm64(a, JIT_RSI, (uint64_t)(uintptr_t)type_name);
	jit_emit_mov32_reg_imm32(a, JIT_RDX, count);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)object_new_struct);
	jit_emit_call(a, ra);

	jit_emit_sub_reg_imm32(a, JIT_RSP, 16);
	jit_emit_mov_mem_reg(a, JIT_RSP, 0, JIT_RAX);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RAX, offsetof(obj_struct_t, fields));

	src_base = -(int32_t)count * (int32_t)sizeof(value_t);
	for (i = 0; i < count; i++) {
		int32_t src_disp = src_base + i * (int32_t)sizeof(value_t);
		int32_t dst_disp = i * (int32_t)sizeof(value_t);

		jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, src_disp);
		jit_emit_mov_reg_mem(a, JIT_RDX, JIT_R14, src_disp + 8);
		jit_emit_mov_mem_reg(a, JIT_RAX, dst_disp, JIT_RCX);
		jit_emit_mov_mem_reg(a, JIT_RAX, dst_disp + 8, JIT_RDX);
	}

	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RSP, 0);
	jit_emit_add_reg_imm32(a, JIT_RSP, 16);

	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)count * (int32_t)sizeof(value_t));
	jit_emit_mov32_reg_imm32(a, JIT_RCX, (uint32_t)VAL_OBJ);
	jit_emit_mov32_mem_reg(a, JIT_R14, 0, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, 8, JIT_RAX);
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

/* name is baked in as a constant. The object is popped and its slot is
   reused as out. The lookup never allocates but can fail.

   The table is the members of whichever module is on the stack, so the
   fast path first checks that the receiver is the cached one. It
   checks the VAL_OBJ tag before the pointer, so the payload of a
   non-object can never match a cached module by chance. */
void jit_emit_op_get_property(jit_asm_t *a, const chunk_t *chunk, obj_string_t *name, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_table_cache_t *cache) {
	size_t miss_tag;
	size_t miss_uncached;
	size_t miss_receiver;
	size_t miss_generation;
	size_t slow_offset;
	size_t done_site;

	jit_emit_mov32_reg_mem(a, JIT_RAX, JIT_R14, -(int32_t)sizeof(value_t));
	jit_emit_cmp_reg_imm32(a, JIT_RAX, (int32_t)VAL_OBJ);
	miss_tag = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_imm64(a, JIT_RCX, (uint64_t)(uintptr_t)cache);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RCX, offsetof(jit_table_cache_t, entry));
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	miss_uncached = jit_emit_jcc(a, JIT_CC_E);

	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_R14, -(int32_t)sizeof(value_t) + 8);
	jit_emit_mov_reg_mem(a, JIT_RDI, JIT_RCX, offsetof(jit_table_cache_t, receiver));
	jit_emit_cmp_reg_reg(a, JIT_RDX, JIT_RDI);
	miss_receiver = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov_reg_mem(a, JIT_R8, JIT_RDX, offsetof(obj_module_t, members));
	jit_emit_mov32_reg_mem(a, JIT_R9, JIT_R8, offsetof(table_t, generation));
	jit_emit_mov32_reg_mem(a, JIT_RDI, JIT_RCX, offsetof(jit_table_cache_t, generation));
	jit_emit_cmp_reg_reg(a, JIT_RDI, JIT_R9);
	miss_generation = jit_emit_jcc(a, JIT_CC_NE);

	jit_emit_mov32_reg_mem(a, JIT_RDX, JIT_RAX, offsetof(table_entry_t, value));
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_RAX, offsetof(table_entry_t, value) + 8);
	jit_emit_mov32_mem_reg(a, JIT_R14, -(int32_t)sizeof(value_t), JIT_RDX);
	jit_emit_mov_mem_reg(a, JIT_R14, -(int32_t)sizeof(value_t) + 8, JIT_RCX);
	done_site = jit_emit_jmp(a);

	slow_offset = a->count;
	jit_patch_rel32(a, miss_tag, slow_offset);
	jit_patch_rel32(a, miss_uncached, slow_offset);
	jit_patch_rel32(a, miss_receiver, slow_offset);
	jit_patch_rel32(a, miss_generation, slow_offset);

	jit_emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RSI, (uint64_t)(uintptr_t)name);
	jit_emit_mov_reg_reg(a, JIT_RDX, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RDX, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_reg(a, JIT_RCX, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RCX, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_imm64(a, JIT_R8, (uint64_t)(uintptr_t)cache);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_get_property_cached);
	jit_emit_call(a, ra);

	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));

	jit_patch_rel32(a, done_site, a->count);
}
