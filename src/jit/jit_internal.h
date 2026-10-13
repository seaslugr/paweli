#pragma once

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "compiler/chunk.h"
#include "jit_abi.h"
#include "jit_asm.h"
#include "jit_compile.h"
#include "jit_exec_mem.h"
#include "runtime/gc.h"
#include "runtime/value.h"
#include "runtime/vm.h"

// Registers a compiled function holds for its whole run. All four are
// callee-saved in both C calling conventions, so a call to a C helper
// never changes them:
//   r12 = vm
//   rbx = this call's frame, &vm->frames[vm->frame_count - 1]. Nested
//         calls only push frames above it, so it stays valid.
//   r13 = frame->slots, the base of this call's locals and arguments.
//   r14 = vm->stack_top, cached. Before a call to a helper that reads
//         or writes vm->stack_top, write r14 back. Reload it after.
//
// GC safety: every value pushed through r14 is a complete, correctly
// tagged value_t, written to the same stack slot the interpreter uses
// at that bytecode offset. This holds at every instruction, so
// mark_roots() in gc.c scans vm->stack for JIT frames with no stack
// maps.
//
// Per-opcode strategy: arithmetic, locals, jumps, and constants compile
// to inline machine code. Where the interpreter branches on a type, the
// JIT emits the same two-sided check. It never speculates, so there is
// nothing to deoptimize. Operands outside the int and float fast paths
// go to vm_binary_op() or vm_bitwise_op(), the same helpers run() uses.
// frame->ip is written before every call that can fail, so
// vm_runtime_error(), mark_roots(), and close_upvalues() work the same
// for interpreted and JIT frames. An opcode with no emitter aborts
// compilation, and the function stays interpreted (see jit_compile.h).
//
// Opcode reference. A call that can allocate needs jit_emit_gc_check()
// before it. Update this table when you add an opcode or change what a
// helper does.
//
//   opcode                     emitter                          allocates?
//   OP_CONSTANT                jit_emit_op_constant             no
//   OP_NULL/TRUE/FALSE         jit_emit_push_immediate          no
//   OP_POP                     (inline)                         no
//   OP_DUP_N                   jit_emit_op_dup_n                no
//   OP_GET_LOCAL/SET_LOCAL     jit_emit_op_get_local/set_local  no
//   OP_EQUAL/NOT_EQUAL         jit_emit_op_comparison_bool      no
//   OP_LESS.../ADD/SUBTRACT/   jit_emit_op_arith_or_cmp         OP_ADD on two strings only,
//     MULTIPLY                                                  see jit_binary_op_may_allocate()
//   OP_DIVIDE/MODULO           jit_emit_op_div_mod              no
//   OP_BIT_AND/OR/XOR          jit_emit_op_bitwise_and_or_xor   no
//   OP_SHIFT_LEFT/RIGHT        jit_emit_op_shift                no
//   OP_NOT                     jit_emit_op_not                  no
//   OP_NEGATE                  jit_emit_op_negate               no
//   OP_BIT_NOT                 jit_emit_op_bit_not              no
//   OP_JUMP/JUMP_IF_FALSE/LOOP (inline)/jit_emit_op_jump_if_false no
//   OP_GET_GLOBAL/SET_GLOBAL   jit_emit_op_get_global/set_global no
//   OP_CALL                    jit_emit_op_call                 yes, runs paweli code
//   OP_GET_INDEX               jit_emit_op_get_index            slow path only (indexing a
//                                                               string builds a string)
//   OP_SET_INDEX               jit_emit_op_set_index            no
//   OP_GET_FIELD/SET_FIELD     jit_emit_op_get_field/set_field  no
//   OP_ARRAY                   jit_emit_op_array                yes, object_new_array() and
//                                                               object_array_push()
//   OP_STRUCT                  jit_emit_op_struct               yes, object_new_struct()
//   OP_CLOSURE                 jit_emit_op_closure              yes, vm_make_closure()
//   OP_GET_UPVALUE/SET_UPVALUE jit_emit_op_get_upvalue/set_upvalue no
//   OP_CLOSE_UPVALUE           jit_emit_op_close_upvalue        no
//   OP_THROW                   jit_emit_op_throw                no
//   OP_GET_PROPERTY            jit_emit_op_get_property         no
//   OP_TRY_CALL                jit_emit_op_try_call             yes, vm_try_call()
//   OP_RETURN                  jit_emit_op_return               no
//   anything else              (default case)                   aborts compilation

// Up to 3 locals live in registers for the whole function: r15, r10,
// r11. r15 is callee-saved and survives a call at no cost. r10 and r11
// are caller-saved, so jit_emit_call() saves and restores them around
// every call. jit_select_register_locals() ranks locals by access count
// and gives r15 to the hottest.
#define JIT_MAX_REG_LOCALS 3

typedef struct {
	int slot[JIT_MAX_REG_LOCALS];
	jit_reg_t reg[JIT_MAX_REG_LOCALS];
	value_type_t type[JIT_MAX_REG_LOCALS];
	size_t decl_end[JIT_MAX_REG_LOCALS];
	// Only meaningful for a VAL_OBJ binding (see local_reg_dead_at in
	// chunk_t). (size_t)-1 for anything else, which makes the
	// offset < dead_at test in jit_emit_gc_check() always true.
	size_t dead_at[JIT_MAX_REG_LOCALS];
	int count;
} jit_reg_alloc_t;

// Inline cache cell for OP_GET_GLOBAL and OP_GET_PROPERTY. A function
// has one cell per call site. The array is allocated at its final size
// before code is emitted, because the generated code holds cell
// addresses as constants. A growing array would move them.
// obj_function_t stores the array as an opaque void *jit_table_cache,
// since object.h cannot name table_entry_t.
//
// receiver is only used by OP_GET_PROPERTY: the module the last lookup
// resolved against. generation is the target table's generation when
// entry was cached. A mismatch means entries may have moved (see
// table.h). entry == NULL means not cached yet. calloc gives every cell
// that state.
typedef struct {
	void *receiver;
	uint32_t generation;
	table_entry_t *entry;
} jit_table_cache_t;

// Inline cache cell for OP_CALL. It needs no staleness check.
// jit_state_t only moves forward (JIT_NOT_COMPILED to JIT_COMPILED or
// JIT_FAILED), and a cell is only filled once the callee is
// JIT_COMPILED, so it stays valid for the life of that object. The
// identity check against callee catches a call site that sees a
// different object.
//
// callee is the popped OBJ_FUNCTION or OBJ_CLOSURE pointer, used only
// for the identity check. function is the obj_function_t to run.
// closure is NULL for a plain function, as in call_frame_t.
// callee == NULL means not cached yet.
typedef struct {
	void *callee;
	obj_function_t *function;
	obj_closure_t *closure;
} jit_call_cache_t;

typedef struct {
	size_t *sites;
	size_t count;
	size_t capacity;
} fixup_list_t;

// A forward jump is emitted before its target is compiled. It is
// recorded here and patched once the whole function is translated.
// OP_LOOP jumps backward, so its target is already known.
typedef struct {
	size_t site;
	size_t target_bytecode_offset;
} forward_fixup_t;

typedef struct {
	forward_fixup_t *items;
	size_t count;
	size_t capacity;
} forward_fixup_list_t;

void jit_fixup_list_push(fixup_list_t *list, size_t site);

void jit_count_cached_lookup_sites(const chunk_t *chunk, size_t *out_lookup_count, size_t *out_call_count);
void jit_select_register_locals(const chunk_t *chunk, jit_reg_alloc_t *out);

void jit_emit_prologue(jit_asm_t *a, const jit_reg_alloc_t *ra, int arity);
void jit_emit_epilogue(jit_asm_t *a);
void jit_emit_write_frame_ip(jit_asm_t *a, const chunk_t *chunk, size_t next_offset);
void jit_emit_call(jit_asm_t *a, const jit_reg_alloc_t *ra);
void jit_emit_gc_check(jit_asm_t *a, const jit_reg_alloc_t *ra, size_t offset);
size_t jit_emit_stack_overflow_check(jit_asm_t *a);

bool jit_helper_equals(const value_t *a, const value_t *b);
bool jit_helper_not_equals(const value_t *a, const value_t *b);
void jit_helper_error_negate_non_numeric(vm_t *vm);
void jit_helper_error_bit_not_non_int(vm_t *vm);
bool jit_helper_get_global_cached(vm_t *vm, table_t *globals, obj_string_t *name, value_t *out, jit_table_cache_t *cache);
bool jit_helper_set_global_cached(vm_t *vm, table_t *globals, obj_string_t *name, const value_t *value, jit_table_cache_t *cache);
bool jit_helper_call_cached(vm_t *vm, int arg_count, jit_call_cache_t *cache);
void jit_helper_error_stack_overflow(vm_t *vm);
bool jit_helper_get_index(vm_t *vm, const value_t *container, const value_t *index_value, value_t *out);
bool jit_helper_set_index(vm_t *vm, const value_t *container, const value_t *index_value, const value_t *value, value_t *out);
void jit_helper_array_push(gc_heap_t *heap, obj_array_t *array, const value_t *value);
void jit_helper_make_closure(vm_t *vm, call_frame_t *frame, obj_function_t *function, const uint8_t *descriptors, uint8_t upvalue_count, value_t *out);
void jit_helper_throw(vm_t *vm, obj_string_t *thrown);
bool jit_helper_get_property_cached(vm_t *vm, obj_string_t *name, const value_t *object, value_t *out, jit_table_cache_t *cache);

void jit_emit_push_immediate(jit_asm_t *a, value_type_t type, uint64_t payload);
void jit_emit_op_constant(jit_asm_t *a, const chunk_t *chunk, uint16_t const_index);
size_t jit_emit_op_return(jit_asm_t *a, const jit_reg_alloc_t *ra);
void jit_emit_op_dup_n(jit_asm_t *a, uint8_t count);
void jit_emit_op_get_local(jit_asm_t *a, uint8_t slot, const jit_reg_alloc_t *ra);
void jit_emit_op_set_local(jit_asm_t *a, uint8_t slot, const jit_reg_alloc_t *ra);
size_t jit_emit_op_jump_if_false(jit_asm_t *a);
void jit_emit_op_get_global(jit_asm_t *a, const chunk_t *chunk, table_t *globals, uint16_t const_index, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_table_cache_t *cache);
void jit_emit_op_set_global(jit_asm_t *a, const chunk_t *chunk, table_t *globals, uint16_t const_index, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_table_cache_t *cache);
void jit_emit_op_get_upvalue(jit_asm_t *a, uint8_t slot);
void jit_emit_op_set_upvalue(jit_asm_t *a, uint8_t slot);
void jit_emit_op_close_upvalue(jit_asm_t *a, const jit_reg_alloc_t *ra);

void jit_emit_op_comparison_bool(jit_asm_t *a, const jit_reg_alloc_t *ra, bool (*helper)(const value_t *, const value_t *));
void jit_emit_compute_falsey(jit_asm_t *a, jit_reg_t result_reg);
void jit_emit_op_not(jit_asm_t *a);
void jit_emit_op_negate(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
void jit_emit_op_bit_not(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
bool jit_binary_op_may_allocate(uint8_t opcode, bool bitwise);
void jit_emit_op_arith_or_cmp(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
void jit_emit_op_div_mod(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
void jit_emit_op_bitwise_and_or_xor(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
void jit_emit_op_shift(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);

void jit_emit_op_call(jit_asm_t *a, const chunk_t *chunk, size_t offset, uint8_t arg_count, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_call_cache_t *call_cache);
void jit_emit_op_closure(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, const jit_reg_alloc_t *ra);
void jit_emit_op_throw(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
size_t jit_emit_op_try_call(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, size_t target_bytecode_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);

void jit_emit_op_get_index(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
void jit_emit_op_set_index(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
void jit_emit_op_get_field(jit_asm_t *a, uint8_t field_index);
void jit_emit_op_set_field(jit_asm_t *a, uint8_t field_index);
void jit_emit_op_array(jit_asm_t *a, const chunk_t *chunk, size_t offset, uint8_t count, size_t next_offset, const jit_reg_alloc_t *ra);
void jit_emit_op_struct(jit_asm_t *a, const chunk_t *chunk, size_t offset, uint8_t count, obj_string_t *type_name, size_t next_offset, const jit_reg_alloc_t *ra);
void jit_emit_op_get_property(jit_asm_t *a, const chunk_t *chunk, obj_string_t *name, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_table_cache_t *cache);
