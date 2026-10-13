#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "jit_internal.h"

/* Counts the OP_GET_GLOBAL and OP_GET_PROPERTY sites and the OP_CALL
   sites in a chunk, so jit_compile_function() can allocate the cache
   arrays at their final size. It stops at an opcode it does not know.
   That is safe: the main loop rejects such a chunk anyway. */
void jit_count_cached_lookup_sites(const chunk_t *chunk, size_t *out_lookup_count, size_t *out_call_count) {
	size_t offset;
	uint8_t opcode;
	size_t lookup_count;
	size_t call_count;

	lookup_count = 0;
	call_count = 0;
	offset = 0;
	while (offset < chunk->count) {
		opcode = chunk->code[offset];
		switch (opcode) {
			case OP_GET_GLOBAL:
			case OP_SET_GLOBAL:
			case OP_GET_PROPERTY:
				lookup_count++;
				offset += 3;
				break;
			case OP_CALL:
				call_count++;
				offset += 2;
				break;
			case OP_CONSTANT:
			case OP_DEFINE_GLOBAL:
				offset += 3;
				break;
			case OP_ARRAY:
			case OP_GET_FIELD:
			case OP_SET_FIELD:
			case OP_DUP_N:
			case OP_GET_UPVALUE:
			case OP_SET_UPVALUE:
			case OP_GET_LOCAL:
			case OP_SET_LOCAL:
				offset += 2;
				break;
			case OP_STRUCT:
				offset += 4;
				break;
			case OP_JUMP:
			case OP_JUMP_IF_FALSE:
			case OP_LOOP:
			case OP_TRY_CALL:
				offset += 3;
				break;
			case OP_CLOSURE:
				offset += 4 + (size_t)chunk->code[offset + 3] * 2;
				break;
			case OP_NULL:
			case OP_TRUE:
			case OP_FALSE:
			case OP_POP:
			case OP_EQUAL:
			case OP_NOT_EQUAL:
			case OP_LESS:
			case OP_LESS_EQUAL:
			case OP_GREATER:
			case OP_GREATER_EQUAL:
			case OP_ADD:
			case OP_SUBTRACT:
			case OP_MULTIPLY:
			case OP_DIVIDE:
			case OP_MODULO:
			case OP_BIT_AND:
			case OP_BIT_OR:
			case OP_BIT_XOR:
			case OP_SHIFT_LEFT:
			case OP_SHIFT_RIGHT:
			case OP_NOT:
			case OP_NEGATE:
			case OP_BIT_NOT:
			case OP_GET_INDEX:
			case OP_SET_INDEX:
			case OP_CLOSE_UPVALUE:
			case OP_RETURN:
			case OP_THROW:
				offset += 1;
				break;
			default:
				*out_lookup_count = lookup_count;
				*out_call_count = call_count;
				return;
		}
	}
	*out_lookup_count = lookup_count;
	*out_call_count = call_count;
}

/* Chooses which register-eligible locals (chunk->local_reg_eligible[],
   set by the compiler) get a register. It picks up to
   JIT_MAX_REG_LOCALS, ranked by OP_GET_LOCAL and OP_SET_LOCAL count,
   and assigns r15, r10, r11 in that order. The count is flat, with no
   weight for loop depth. A poor choice costs speed and never
   correctness: every slot index is bounds-checked and
   eligibility-checked. It stops at an opcode it does not know. */
void jit_select_register_locals(const chunk_t *chunk, jit_reg_alloc_t *out) {
	static const jit_reg_t kRegOrder[JIT_MAX_REG_LOCALS] = { JIT_R15, JIT_R10, JIT_R11 };
	int *access_counts;
	size_t offset;
	uint8_t opcode;
	uint8_t slot;
	int i;
	int k;

	out->count = 0;
	for (i = 0; i < JIT_MAX_REG_LOCALS; i++) {
		out->slot[i] = -1;
		out->decl_end[i] = (size_t)-1;
	}

	if (chunk->local_slot_count <= 0) {
		return;
	}

	access_counts = calloc((size_t)chunk->local_slot_count, sizeof(int));

	offset = 0;
	while (offset < chunk->count) {
		opcode = chunk->code[offset];

		// A VAL_OBJ local costs something at every safepoint while it is
		// bound: the flush in jit_emit_gc_check(), plus a push and pop if it
		// sits in r10 or r11. Each of these opcodes counts against it. A
		// local with a count of zero or less gets no register. OP_GET_INDEX
		// and OP_ADD can also reach a safepoint, but only for some types, so
		// they are left out.
		if (opcode == OP_CALL || opcode == OP_ARRAY || opcode == OP_STRUCT || opcode == OP_CLOSURE || opcode == OP_TRY_CALL || jit_binary_op_may_allocate(opcode, false)) {
			for (i = 0; i < chunk->local_slot_count; i++) {
				if (chunk->local_reg_eligible[i] && chunk->local_reg_types[i] == VAL_OBJ && offset >= chunk->local_reg_decl_end[i] && offset < chunk->local_reg_dead_at[i]) {
					access_counts[i] -= 8;
				}
			}
		}

		switch (opcode) {
			case OP_GET_LOCAL:
			case OP_SET_LOCAL:
				slot = chunk->code[offset + 1];
				if (slot < chunk->local_slot_count && chunk->local_reg_eligible[slot]) {
					access_counts[slot]++;
				}
				offset += 2;
				break;
			case OP_CONSTANT:
			case OP_GET_GLOBAL:
			case OP_SET_GLOBAL:
			case OP_DEFINE_GLOBAL:
			case OP_GET_PROPERTY:
				offset += 3;
				break;
			case OP_ARRAY:
			case OP_GET_FIELD:
			case OP_SET_FIELD:
			case OP_DUP_N:
			case OP_CALL:
			case OP_GET_UPVALUE:
			case OP_SET_UPVALUE:
				offset += 2;
				break;
			case OP_STRUCT:
				offset += 4;
				break;
			case OP_JUMP:
			case OP_JUMP_IF_FALSE:
			case OP_LOOP:
			case OP_TRY_CALL:
				offset += 3;
				break;
			case OP_CLOSURE:
				offset += 4 + (size_t)chunk->code[offset + 3] * 2;
				break;
			case OP_NULL:
			case OP_TRUE:
			case OP_FALSE:
			case OP_POP:
			case OP_EQUAL:
			case OP_NOT_EQUAL:
			case OP_LESS:
			case OP_LESS_EQUAL:
			case OP_GREATER:
			case OP_GREATER_EQUAL:
			case OP_ADD:
			case OP_SUBTRACT:
			case OP_MULTIPLY:
			case OP_DIVIDE:
			case OP_MODULO:
			case OP_BIT_AND:
			case OP_BIT_OR:
			case OP_BIT_XOR:
			case OP_SHIFT_LEFT:
			case OP_SHIFT_RIGHT:
			case OP_NOT:
			case OP_NEGATE:
			case OP_BIT_NOT:
			case OP_GET_INDEX:
			case OP_SET_INDEX:
			case OP_CLOSE_UPVALUE:
			case OP_RETURN:
			case OP_THROW:
				offset += 1;
				break;
			default:
				offset = chunk->count;
				break;
		}
	}

	for (k = 0; k < JIT_MAX_REG_LOCALS; k++) {
		int best_slot = -1;
		int best_count = 0;

		for (i = 0; i < chunk->local_slot_count; i++) {
			int already_taken;
			int j;

			if (!chunk->local_reg_eligible[i]) {
				continue;
			}
			already_taken = 0;
			for (j = 0; j < k; j++) {
				if (out->slot[j] == i) {
					already_taken = 1;
					break;
				}
			}
			if (already_taken) {
				continue;
			}
			if (access_counts[i] > best_count) {
				best_count = access_counts[i];
				best_slot = i;
			}
		}

		if (best_slot < 0) {
			break;
		}

		out->slot[k] = best_slot;
		out->reg[k] = kRegOrder[k];
		out->type[k] = chunk->local_reg_types[best_slot];
		out->count = k + 1;
	}

	free(access_counts);
}
