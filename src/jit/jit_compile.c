#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "compiler/chunk.h"
#include "jit_asm.h"
#include "jit_compile.h"
#include "jit_exec_mem.h"
#include "runtime/gc.h"
#include "runtime/value.h"
#include "runtime/vm.h"

// Register assignment held for the whole lifetime of a compiled
// function's native code, chosen from the callee-saved set so a call
// out to any C helper (which only has to preserve callee-saved
// registers, per the System V AMD64 ABI) never disturbs them:
//   r12 = vm
//   rbx = &vm->frames[vm->frame_count - 1] (this call's own frame;
//         stable for the whole call, since nested calls only ever push
//         frames *above* this index and pop back down to it)
//   r13 = frame->slots (this call's locals/argument base; fixed once
//         set by call_value() before jit_entry ever runs)
//   r14 = vm->stack_top, cached in a register for speed. Any call out
//         to a C helper that reads or writes vm->stack_top (i.e. any
//         safepoint. see the file comment below) must write r14 back
//         to vm->stack_top first and reload it after, since the helper
//         has no idea r14 is a cached mirror of that field.
//
// GC safety: every value ever pushed via r14 is written as a complete,
// correctly-tagged value_t (two 8-byte quadwords: value_type_t+padding,
// then the 8-byte payload. value_t's union is 8-byte aligned, so this
// is equivalent to, and simpler to hand-encode than, a single 16-byte
// SSE move) at the exact stack slot the interpreter would use for that
// same bytecode offset. This is true at every point in the generated
// code, not just at safepoints, so gc.c's existing mark_roots() (which
// scans vm->stack[0..stack_top) precisely, with no notion of which
// frame owns which slice) needs no changes and no new stack-map
// metadata to stay correct for JIT'd frames.
//
// Per-opcode strategy: arithmetic/locals/jumps/constants compile to
// real inlined machine code, with a genuine two-sided type-check branch
// for anything the interpreter itself branches on (e.g. int-vs-float).
// Never a speculative guess, so there is nothing to deopt. Both-int
// fast paths for arithmetic/comparison/bitwise ops are real hardware
// instructions; anything outside that (float/mixed operands, wrong
// types, division/modulo by zero, an out-of-range shift amount) falls
// through to a call to vm_binary_op()/vm_bitwise_op(). the exact same
// C helpers run()'s own OP_ADD..OP_SHIFT_RIGHT cases already use.
// Immediately after writing frame->ip to the exact bytecode-equivalent
// offset, a genuine error safepoint (binary_op/bitwise_op can call
// vm_runtime_error(), even though neither one ever allocates). This
// keeps vm_runtime_error()/vm_print_uncaught_error()/mark_roots()/
// close_upvalues() all working unmodified regardless of whether a
// frame is interpreted or JIT'd. Calls that can allocate (struct/array/
// closure construction, OP_CALL, ...) are Phase (c)'s job; every
// opcode not yet handled here falls into the "not yet supported"
// default case and aborts compilation for the whole function, so an
// unsupported function is simply left to interpret forever (see
// jit_compile.h).
//
// Opcode reference: every opcode this file compiles, its emitter, and
// whether it can reach an allocation (and so needs emit_gc_check()
// before the call that might). Kept as a table, not prose, so the next
// audit of this file's GC-safety story is a lookup instead of a re-grep
// of 2900 lines (see the review that found the OP_ADD gap this table
// exists to prevent recurring). If you add JIT support for a new
// opcode, or change what a helper it already calls does, update this
// row, not just the code.
//
//   opcode                          emitter                        allocates?
//   ------------------------------  -----------------------------  ----------
//   OP_CONSTANT                     emit_op_constant               no
//   OP_NULL/TRUE/FALSE              emit_push_immediate            no
//   OP_POP                          (inline)                       no
//   OP_DUP_N                        emit_op_dup_n                  no
//   OP_GET_LOCAL/SET_LOCAL          emit_op_get_local/set_local    no
//   OP_EQUAL/NOT_EQUAL               emit_op_comparison_bool        no (value_equals() never allocates)
//   OP_LESS.../OP_ADD/SUBTRACT/     emit_op_arith_or_cmp           OP_ADD only, string+string operands,
//     MULTIPLY (fast path: int/float   -> emit_call_binary_slow_path   via jit_binary_op_may_allocate()
//     hardware ops; slow path: C)                                     (see its own comment)
//   OP_DIVIDE/MODULO                emit_op_div_mod                no (vm_binary_op() never allocates
//                                     -> emit_call_binary_slow_path    for these two ops)
//   OP_BIT_AND/OR/XOR               emit_op_bitwise_and_or_xor     no (vm_bitwise_op() never allocates)
//                                     -> emit_call_binary_slow_path
//   OP_SHIFT_LEFT/RIGHT             emit_op_shift                  no (vm_bitwise_op() never allocates)
//                                     -> emit_call_binary_slow_path
//   OP_NOT                          emit_op_not                    no
//   OP_NEGATE                       emit_op_negate                 no (error path only, fixed buffers)
//   OP_BIT_NOT                      emit_op_bit_not                no (error path only, fixed buffers)
//   OP_JUMP/JUMP_IF_FALSE/LOOP      (inline)/emit_op_jump_if_false no
//   OP_GET_GLOBAL/SET_GLOBAL        emit_op_get_global/set_global  no (table_get_entry() never allocates)
//   OP_CALL                         emit_op_call                   yes -- arbitrary paweli code
//   OP_GET_INDEX                    emit_op_get_index              slow path only (string indexing
//                                                                    builds a 1-char string); fast
//                                                                    (array) path never allocates
//   OP_SET_INDEX                    emit_op_set_index              no (vm_set_index() never allocates)
//   OP_GET_FIELD/SET_FIELD          emit_op_get_field/set_field    no
//   OP_ARRAY                        emit_op_array                  yes -- object_new_array() and every
//                                                                    object_array_push() (can realloc)
//   OP_STRUCT                       emit_op_struct                 yes -- object_new_struct()
//   OP_CLOSURE                      emit_op_closure                yes -- vm_make_closure()
//   OP_GET_UPVALUE/SET_UPVALUE      emit_op_get_upvalue/set_upvalue no
//   OP_CLOSE_UPVALUE                emit_op_close_upvalue          no (vm_close_upvalues_from() never
//                                                                    allocates, only boxes already do)
//   OP_THROW                        emit_op_throw                  no (error path only, fixed buffers)
//   OP_GET_PROPERTY                 emit_op_get_property           no (pure table lookup)
//   OP_TRY_CALL                     emit_op_try_call               yes -- wraps an arbitrary call
//                                                                    (vm_try_call())
//   OP_RETURN                       emit_op_return                 no (vm_close_upvalues_from() only)
//   anything else                   (default case)                 aborts compilation; interpreted only

// Up to 3 numeric locals cached in registers for a compiled function's
// whole lifetime: r15 (callee-saved, survives any call for free. see
// emit_prologue()'s own comment) plus r10/r11 (caller-saved, so
// emit_call() below must spill/reload them around every call). Ranked
// by access count in select_register_locals(), r15 always taking the
// hottest slot since it's the only one with zero spill cost.
#define JIT_MAX_REG_LOCALS 3

typedef struct {
	int slot[JIT_MAX_REG_LOCALS];
	jit_reg_t reg[JIT_MAX_REG_LOCALS];
	value_type_t type[JIT_MAX_REG_LOCALS];
	size_t decl_end[JIT_MAX_REG_LOCALS];
	// Meaningful only for a VAL_OBJ-typed binding (see chunk_t's own
	// comment on local_reg_dead_at); (size_t)-1 for anything else,
	// which combined with emit_gc_check()'s offset < dead_at check
	// (always true against the sentinel) makes the check a no-op for
	// scalar bindings without needing a separate type check there too.
	size_t dead_at[JIT_MAX_REG_LOCALS];
	int count;
} jit_reg_alloc_t;

// Inline-caching cell for OP_GET_GLOBAL/OP_GET_PROPERTY (see
// emit_op_get_global()/emit_op_get_property()'s own comments for the
// full design). One array, one cell per static call site in a
// function, allocated once at its final size before that function's
// code is emitted so the cell addresses baked into the generated code
// as compile-time constants never go stale (mirrors
// select_register_locals()'s own "pre-pass first, stable pointers
// during emission" shape, for the identical reason: a growable array
// reallocating mid-emission would invalidate every pointer already
// baked into earlier-emitted code). Privately defined here, not in
// object.h, since object.h forward-declares table_t specifically to
// avoid a circular include with table.h and has no way to name
// table_entry_t (an anonymous-struct typedef, not a tagged struct).
// obj_function_t only stores an opaque `void *jit_table_cache` for it.
//
// receiver is only meaningful for OP_GET_PROPERTY sites (the module
// value a property access last resolved against. OP_GET_GLOBAL's
// target table is a compile-time constant, so there's no receiver to
// check). generation mirrors the target table_t's own generation at
// the point entry was cached; a mismatch means entries may have moved
// since table_set()'s adjust_capacity() or table_remove_white() ran
// (see table.h's own comment on the field). entry == NULL means "never
// successfully cached yet"; calloc-zeroing the array at allocation
// time gives every cell this state for free.
typedef struct {
	void *receiver;
	uint32_t generation;
	table_entry_t *entry;
} jit_table_cache_t;

// Inline-caching cell for OP_CALL dispatch itself (see emit_op_call()'s
// own comment for the full design). Unlike jit_table_cache_t above,
// this needs no generation/staleness check at all: jit_state_t is
// monotonic (JIT_NOT_COMPILED -> JIT_COMPILED or JIT_FAILED, never
// back. see jit_exec_mem.h's "never recompiled" comment), so a cell
// only ever populated once the callee is already JIT_COMPILED stays
// valid for that object's entire remaining lifetime. The only thing
// that can go stale is *which object* a call site sees, which the
// identity check (comparing the current callee's pointer against
// `callee` below) already exists to catch.
//
// callee is the raw popped OBJ_FUNCTION/OBJ_CLOSURE pointer (whichever
// the call site actually saw), used purely for the identity check.
// function/closure are the already-resolved frame-setup targets.
// function is always the real obj_function_t to run; closure is NULL
// for a plain (non-nested) function call, matching call_frame_t's own
// closure field semantics exactly. callee == NULL (calloc-zeroed) means
// "never cached". no real object lives at address 0, so a call site's
// first invocation always misses, same trick jit_table_cache_t uses.
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

// A forward jump's target bytecode offset isn't compiled yet at the
// point the jump itself is emitted (unlike OP_LOOP, always backward, so
// its target's native offset is already known and gets patched
// immediately). Recorded here and resolved once the whole function has
// been translated and native_offset_for_bytecode is complete.
typedef struct {
	size_t site;
	size_t target_bytecode_offset;
} forward_fixup_t;

typedef struct {
	forward_fixup_t *items;
	size_t count;
	size_t capacity;
} forward_fixup_list_t;

static void fixup_list_push(fixup_list_t *list, size_t site);
static void fixup_list_free(fixup_list_t *list);
static void forward_fixup_list_push(forward_fixup_list_t *list, size_t site, size_t target_bytecode_offset);
static void forward_fixup_list_free(forward_fixup_list_t *list);

static void select_register_locals(const chunk_t *chunk, jit_reg_alloc_t *out);
static void count_cached_lookup_sites(const chunk_t *chunk, size_t *out_lookup_count, size_t *out_call_count);
static void emit_prologue(jit_asm_t *a, const jit_reg_alloc_t *ra, int arity);
static void emit_epilogue(jit_asm_t *a);
static void emit_write_frame_ip(jit_asm_t *a, const chunk_t *chunk, size_t next_offset);
static void emit_gc_check(jit_asm_t *a, const jit_reg_alloc_t *ra, size_t offset);
static void emit_call(jit_asm_t *a, const jit_reg_alloc_t *ra);

static bool jit_helper_equals(const value_t *a, const value_t *b);
static bool jit_helper_not_equals(const value_t *a, const value_t *b);
static void jit_helper_error_negate_non_numeric(vm_t *vm);
static void jit_helper_error_bit_not_non_int(vm_t *vm);
static bool jit_helper_get_global_cached(vm_t *vm, table_t *globals, obj_string_t *name, value_t *out, jit_table_cache_t *cache);
static bool jit_helper_set_global_cached(vm_t *vm, table_t *globals, obj_string_t *name, const value_t *value, jit_table_cache_t *cache);
static bool jit_helper_call_cached(vm_t *vm, int arg_count, jit_call_cache_t *cache);
static void jit_helper_error_stack_overflow(vm_t *vm);
static bool jit_helper_get_index(vm_t *vm, const value_t *container, const value_t *index_value, value_t *out);
static bool jit_helper_set_index(vm_t *vm, const value_t *container, const value_t *index_value, const value_t *value, value_t *out);
static void jit_helper_throw(vm_t *vm, obj_string_t *thrown);
static bool jit_helper_get_property_cached(vm_t *vm, obj_string_t *name, const value_t *object, value_t *out, jit_table_cache_t *cache);

static void emit_push_immediate(jit_asm_t *a, value_type_t type, uint64_t payload);
static void emit_op_constant(jit_asm_t *a, const chunk_t *chunk, uint16_t const_index);
static size_t emit_op_return(jit_asm_t *a, const jit_reg_alloc_t *ra);
static void emit_op_dup_n(jit_asm_t *a, uint8_t count);
static void emit_op_get_local(jit_asm_t *a, uint8_t slot, const jit_reg_alloc_t *ra);
static void emit_op_set_local(jit_asm_t *a, uint8_t slot, const jit_reg_alloc_t *ra);
static void emit_op_comparison_bool(jit_asm_t *a, const jit_reg_alloc_t *ra, bool (*helper)(const value_t *, const value_t *));
static void emit_compute_falsey(jit_asm_t *a, jit_reg_t result_reg);
static void emit_op_not(jit_asm_t *a);
static void emit_op_negate(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
static void emit_op_bit_not(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
static void emit_call_binary_slow_path(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, uint8_t opcode, bool bitwise, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
static void emit_op_arith_or_cmp(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
static void emit_op_div_mod(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
static void emit_op_bitwise_and_or_xor(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
static void emit_op_shift(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
static size_t emit_op_jump_if_false(jit_asm_t *a);
static void emit_op_get_global(jit_asm_t *a, const chunk_t *chunk, table_t *globals, uint16_t const_index, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_table_cache_t *cache);
static void emit_op_set_global(jit_asm_t *a, const chunk_t *chunk, table_t *globals, uint16_t const_index, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_table_cache_t *cache);
static size_t emit_stack_overflow_check(jit_asm_t *a);
static void emit_op_call(jit_asm_t *a, const chunk_t *chunk, size_t offset, uint8_t arg_count, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_call_cache_t *call_cache);
static void emit_op_get_index(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
static void emit_op_set_index(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
static void emit_op_get_field(jit_asm_t *a, uint8_t field_index);
static void emit_op_set_field(jit_asm_t *a, uint8_t field_index);
static void emit_op_array(jit_asm_t *a, const chunk_t *chunk, size_t offset, uint8_t count, size_t next_offset, const jit_reg_alloc_t *ra);
static void emit_op_struct(jit_asm_t *a, const chunk_t *chunk, size_t offset, uint8_t count, obj_string_t *type_name, size_t next_offset, const jit_reg_alloc_t *ra);
static void emit_op_closure(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, const jit_reg_alloc_t *ra);
static void emit_op_get_upvalue(jit_asm_t *a, uint8_t slot);
static void emit_op_set_upvalue(jit_asm_t *a, uint8_t slot);
static void emit_op_close_upvalue(jit_asm_t *a, const jit_reg_alloc_t *ra);
static void emit_op_throw(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);
static void emit_op_get_property(jit_asm_t *a, const chunk_t *chunk, obj_string_t *name, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_table_cache_t *cache);
static size_t emit_op_try_call(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, size_t target_bytecode_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra);

static void fixup_list_push(fixup_list_t *list, size_t site) {
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

/* ra->slot[i] (if any. see select_register_locals()) names each local
   register-cached for this function's whole lifetime, always the
   payload half only. Each one's memory slot's *tag* half still has to
   be correct before anything else runs: mark_roots() (gc.c) scans
   frame->slots directly, never these registers, so a non-parameter
   binding's memory could otherwise hold stale garbage. including a
   stale VAL_OBJ tag. left over from whatever this same stack region
   held the last time it was used. Baking the correct tag here, once,
   unconditionally, before any opcode (and hence any safepoint) can run
   makes it permanently safe: nothing after this point ever writes that
   memory slot's tag again, only its payload changes, in registers, from
   here on. Harmless-but-redundant for a parameter, whose tag is already
   correct. keeping the write unconditional avoids needing to
   special-case parameter vs. not here.

   A VAL_OBJ binding's tag, once baked here, means "this memory cell is
   GC-scannable", not "this is definitely a real object right now" --
   its logical null-vs-object distinction lives entirely in the payload
   (0 means null, nonzero means a real heap pointer; see
   emit_op_get_local()'s own comment on this convention). A
   *non-parameter* VAL_OBJ binding's register is explicitly zeroed here,
   right after the tag bake, for exactly the reason a scalar binding
   never needed this: emit_gc_check()'s new per-safepoint flush (see its
   own comment) writes whatever is currently in a bound VAL_OBJ
   register into this same memory cell on every safepoint from this
   point on, including any that run before the local's own declaration
   executes (an earlier OP_CALL/OP_ARRAY/etc. in this same function).
   Without zeroing first, that flush would write leftover garbage from
   whatever this register held on entry, and mark_object() would
   dereference it as a live pointer. A parameter needs no such zeroing:
   it loads its real payload (already 0 for a null argument, since the
   caller built it via value_null()) from memory a few lines below,
   before any safepoint in *this* function's own body can run.

   r10/r11 are pushed/popped here unconditionally, whether or not this
   particular function's allocator actually bound them, exactly like
   r15 already was before this phase. simpler than making the
   prologue's own shape conditional per function, and it's what keeps
   emit_call() below able to assume a fixed, known set of possibly-live
   registers to spill around a call. */
static void emit_prologue(jit_asm_t *a, const jit_reg_alloc_t *ra, int arity) {
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
	jit_emit_sub_reg_imm32(a, JIT_RSP, 8);

	jit_emit_mov_reg_reg(a, JIT_R12, JIT_RDI);

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

/* 8 pushes above (rbp,rbx,r12,r13,r14,r15,r10,r11) = 64 bytes, plus the
   `call jit_entry`'s own 8-byte return address already on the stack
   before the prologue starts = 72 bytes total, NOT 16-aligned (72 % 16
   == 8). same parity R1's 6-push prologue had (48 + 8 == 56, also
   %16==8), which is why the extra sub/add rsp,8 padding pair is still
   needed here despite the push count changing; alignment depends on
   total pushes being *odd* once the implicit return-address "push" is
   counted, not on the raw byte count alone. */
static void emit_epilogue(jit_asm_t *a) {
	jit_emit_add_reg_imm32(a, JIT_RSP, 8);
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

/* next_offset is the bytecode offset one past this instruction's own
   bytes. exactly what the interpreter's own frame->ip already holds
   by the time it reaches the equivalent point (READ_BYTE()/READ_SHORT()
   always advance ip before an opcode's handler can call
   vm_runtime_error() or anything that could fail/allocate; see
   frame_instruction_index()'s own comment in vm.c). */
static void emit_write_frame_ip(jit_asm_t *a, const chunk_t *chunk, size_t next_offset) {
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)(chunk->code + next_offset));
	jit_emit_mov_mem_reg(a, JIT_RBX, offsetof(call_frame_t, ip), JIT_RAX);
}

/* Wraps every call to a C helper/runtime function. rax already holds
   the callee address by every call site's own existing convention.
   Caller-saved bound registers (ra->reg[i] != JIT_R15. r15 is
   callee-saved and survives a call untouched per the System V AMD64
   ABI, exactly why R1 needed no spill machinery at all) must be
   saved/restored here since nothing else will: the callee is free to
   clobber them for its own purposes. This push/pop targets the native
   C call stack, which mark_roots() never scans, so it is not itself a
   GC-safety mechanism for a VAL_OBJ-typed binding (unlike a scalar
   binding, a register-eligible slot's tag genuinely can be VAL_OBJ now
   -- see compiler.c's register_cacheable_value_type()). GC safety for
   such a binding is handled earlier and separately, inside
   emit_gc_check()'s own flush to frame->slots (its own comment), which
   every call site reaching this function already runs first. What this
   push/pop alone still guarantees is purely that the *program's own
   value* survives the call correctly, exactly the same reason it
   already existed for a scalar binding before this phase.

   An odd number of saved registers (only possible value here: 1, since
   at most r10 and r11 are ever caller-saved bindings) would misalign
   the stack at the `call` below by 8 bytes; padded with one extra
   push-sized adjustment in that case, symmetric with how emit_prologue()
   keeps its own push count even. */
static void emit_call(jit_asm_t *a, const jit_reg_alloc_t *ra) {
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

	jit_emit_call_reg(a, JIT_RAX);

	for (i = saved_count - 1; i >= 0; i--) {
		jit_emit_pop_reg(a, saved[i]);
	}
	if (saved_count % 2 == 1) {
		jit_emit_add_reg_imm32(a, JIT_RSP, 8);
	}
}

/* The GC-triggering equivalent of run()'s own per-instruction check
   (`if (vm->gc_stress || heap->bytes_allocated > heap->next_gc)
   gc_collect(vm);`, vm.c). must be emitted immediately before any
   call-out that can allocate (nothing else in this file's generated
   code ever does; see gc.h's own updated safety-invariant comment).
   Always syncs r14 to vm->stack_top first, unconditionally, since
   mark_roots() reads that field directly and must see this frame's
   true current stack contents whether or not a collection actually
   runs this time.

   This is also the single point every one of this file's 6 allocating
   sites already funnels through (the generic OP_CALL path, OP_GET_INDEX's
   string-indexing slow path, OP_ARRAY, OP_STRUCT, OP_CLOSURE,
   OP_TRY_CALL), which makes it the one place a VAL_OBJ-typed register
   binding's live pointer needs to become visible to mark_roots() before
   a collection can run: unlike vm->stack (covered by the r14 sync
   above), mark_roots() never scans a CPU register directly. For every
   currently bound VAL_OBJ slot ALREADY DECLARED by this point (offset,
   this call site's own bytecode position, is at or past ra->decl_end[i]
   -- or this is a parameter, decl_end's (size_t)-1 sentinel, live from
   entry), write its register's payload into its own frame->slots[slot]
   memory cell (r13+disp+8; the tag half was already permanently baked
   by emit_prologue() and is never rewritten here). This is a pure
   spill, not a spill-and-reload: gc_collect() never moves an object,
   only frees an unreached one, so the register itself needs no update
   afterward regardless of whether a collection actually runs this time,
   the same "always sync, whether or not it's needed this time"
   reasoning the r14 sync above already uses.

   A slot's own reserved memory cell is not exclusively "its own" for
   the binding's entire register lifetime, only during [decl_end,
   dead_at): the compile-time bytecode-offset window between its own
   declaration and its own scope-closing pop (see chunk_t's and
   jit_reg_alloc_t's own comments on those two fields). Both ends are
   load-bearing, guarding the two opposite ends of the same underlying
   fact -- a slot's address is only "this local's own" while the local
   is actually in scope, and it's ordinary operand-stack space at every
   other point in the function:

   - Before decl_end: a non-parameter local's first-ever memory slot
     coincides with wherever r14 happened to be before anything was
     ever pushed (locals occupy a fixed prefix of the frame; the operand
     stack only ever grows above whatever's already declared). `int[]
     nums = [1, 2, 3];` itself contains a safepoint (OP_ARRAY's own
     emit_gc_check(), before the array is even built) that runs while
     the three literal elements are still sitting in that exact memory
     as scratch, not yet nums's own value. Flushing unconditionally
     there would clobber that in-flight data with whatever the
     not-yet-declared binding's register currently holds (0, per
     emit_prologue()'s own zeroing), corrupting the local's own
     initializer.
   - At or after dead_at: a block-scoped binding like a
     `catch(string message)` binding going out of scope at the end of
     its catch handler, well before this function itself returns.
     Once the local's own scope-closing OP_POP has run, r14 drops back
     below this slot's address, and whatever pushes next (another
     call's arguments, a later expression's temporaries -- concretely,
     the very next global/function value this same function pushes)
     legitimately reuses that exact memory. An earlier version of this
     check only gated the decl_end side and used a *runtime* r14
     comparison for this side ("is this slot's cell still within the
     currently pushed range"), on the theory that this alone would
     generalize to any scope shape -- but that check cannot distinguish
     "r14 is past this address because the local itself is still live"
     from "r14 grew past this address again because something *else*
     got pushed here after the local's own scope closed", and the
     second case is exactly what dead_at exists to rule out instead. */
static void emit_gc_check(jit_asm_t *a, const jit_reg_alloc_t *ra, size_t offset) {
	size_t stress_taken;
	size_t threshold_skip;
	int i;

	jit_emit_mov_mem_reg(a, JIT_R12, offsetof(vm_t, stack_top), JIT_R14);

	// Reaching this function at all is itself a safepoint, for
	// PAWELI_GC_SAFEPOINT_CHECK's purposes (gc.c's gc_heap_note_alloc()),
	// regardless of whether gc_stress/the threshold below actually
	// trigger a collection this time -- same "always run, unconditional
	// on the outcome" shape the r14 sync above already uses. A direct
	// memory write rather than a call to gc_note_safepoint(): cheaper,
	// and rax/rcx are already this function's own scratch registers, so
	// this can't clobber anything a caller staged (see emit_call()'s own
	// comment on why this function must always run before that staging
	// begins, never after).
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
	emit_call(a, ra);

	jit_patch_rel32(a, threshold_skip, a->count);
}

static bool jit_helper_equals(const value_t *a, const value_t *b) {
	return value_equals(*a, *b);
}

static bool jit_helper_not_equals(const value_t *a, const value_t *b) {
	return !value_equals(*a, *b);
}

static void jit_helper_error_negate_non_numeric(vm_t *vm) {
	vm_runtime_error(vm, "vm::non-numeric-operand", "operand of unary '-' must be a number");
}

static void jit_helper_error_bit_not_non_int(vm_t *vm) {
	vm_runtime_error(vm, "vm::bitwise-type-mismatch", "operand of unary '~' must be an int");
}

/* Refreshes cache after a real lookup, whether this is the first time
   this call site has ever run or the generation check in
   emit_op_get_global()'s own fast path just failed. Correctness never
   depends on the cache being right. table_get_entry() is the same
   real lookup this call site would do regardless of caching. it's
   purely a speed optimization layered on top of an already-correct
   path, exactly like every other fast path in this file. */
static bool jit_helper_get_global_cached(vm_t *vm, table_t *globals, obj_string_t *name, value_t *out, jit_table_cache_t *cache) {
	table_entry_t *entry;

	entry = table_get_entry(globals, name);
	if (entry == NULL) {
		vm_runtime_error(vm, "vm::undefined-variable", "undefined variable '%.*s'", (int)name->length, name->chars);
		return false;
	}
	*out = entry->value;
	cache->entry = entry;
	cache->generation = globals->generation;
	return true;
}

/* Write-side mirror of jit_helper_get_global_cached() above, refreshing
   the same kind of cache cell (OP_GET_GLOBAL and OP_SET_GLOBAL sites for
   the same name never share a cell, but both populate one shaped the
   same way). Uses table_get_entry() rather than table_set(): every
   OP_SET_GLOBAL the compiler emits targets a name register_globals()
   already proved is declared, and every global is OP_DEFINE_GLOBAL'd
   (which does grow the table if needed) before any function body can
   run (see chunk.h's own comment on OP_SET_GLOBAL). so the entry
   always already exists by the time this can run, and there is no
   capacity check left to do. The NULL branch below is accordingly
   unreachable in practice; kept as a real runtime error instead of an
   assert so a future bug here fails loudly rather than corrupting
   memory. */
static bool jit_helper_set_global_cached(vm_t *vm, table_t *globals, obj_string_t *name, const value_t *value, jit_table_cache_t *cache) {
	table_entry_t *entry;

	entry = table_get_entry(globals, name);
	if (entry == NULL) {
		vm_runtime_error(vm, "vm::undefined-variable", "undefined variable '%.*s'", (int)name->length, name->chars);
		return false;
	}
	entry->value = *value;
	cache->entry = entry;
	cache->generation = globals->generation;
	return true;
}

/* Wraps the existing vm_jit_call() (the exact same dispatch every
   OP_CALL already went through before this cache existed) rather than
   re-deriving call_value()'s OBJ_FUNCTION/OBJ_CLOSURE/OBJ_NATIVE
   dispatch a second time. this only ever adds a cache refresh on top
   of already-correct behavior, never changes what gets called or how.
   The callee has to be captured *before* vm_jit_call() runs: by the
   time it returns, the stack has already moved (frame pushed, args
   consumed, result in their place), so there would be nothing left to
   identify. `vm->stack_top[-1 - arg_count]` is exactly peek(vm,
   arg_count)'s own definition (a static inline in vm.c, not visible
   here). the callee sits arg_count slots below the top, same
   addressing emit_op_call()'s own generated code already uses.

   The callee object can't be GC'd between the peek and the cache write
   below: it's reachable on the stack for this whole call (it's
   literally what's being invoked), and this collector only ever frees
   at a sweep following a full mark pass, never synchronously mid-call. */
static bool jit_helper_call_cached(vm_t *vm, int arg_count, jit_call_cache_t *cache) {
	value_t callee_value;
	obj_function_t *function;
	obj_closure_t *closure;

	callee_value = vm->stack_top[-1 - arg_count];
	function = NULL;
	closure = NULL;
	if (value_is_obj(callee_value)) {
		obj_t *obj = value_as_obj(callee_value);
		if (obj->type == OBJ_FUNCTION) {
			function = (obj_function_t *)obj;
		} else if (obj->type == OBJ_CLOSURE) {
			closure = (obj_closure_t *)obj;
			function = closure->function;
		}
	}

	if (!vm_jit_call(vm, arg_count)) {
		return false;
	}

	// Only cache once the callee is actually JIT_COMPILED. jit_state
	// is monotonic (see jit_call_cache_t's own comment), so this can
	// never need re-checking once true. vm_jit_call() above may have
	// just compiled it via jit_maybe_compile(); either way, by this
	// point jit_state already reflects the outcome.
	if (function != NULL && function->jit_state == JIT_COMPILED) {
		cache->callee = value_as_obj(callee_value);
		cache->function = function;
		cache->closure = closure;
	}
	return true;
}

static void jit_helper_error_stack_overflow(vm_t *vm) {
	vm_runtime_error(vm, "vm::stack-overflow", "stack overflow");
}

/* Pointer-taking wrappers around vm_get_index()/vm_set_index(), for the
   same reason as jit_helper_equals() above. sidesteps hand-encoding
   the System V struct-by-value ABI for value_t arguments, since the
   values already live at known stack addresses anyway. */
static bool jit_helper_get_index(vm_t *vm, const value_t *container, const value_t *index_value, value_t *out) {
	return vm_get_index(vm, *container, *index_value, out);
}

static bool jit_helper_set_index(vm_t *vm, const value_t *container, const value_t *index_value, const value_t *value, value_t *out) {
	return vm_set_index(vm, *container, *index_value, *value, out);
}

static void jit_helper_throw(vm_t *vm, obj_string_t *thrown) {
	vm_runtime_error(vm, "user::thrown", "%.*s", (int)thrown->length, thrown->chars);
}

/* Same shape as jit_helper_get_global_cached() above, but with an
   extra receiver to track: unlike OP_GET_GLOBAL's table (a compile-time
   constant for the whole function), OP_GET_PROPERTY's target table is
   module->members for whatever module value happens to be on the stack
   at this call site. usually the same module every time in practice
   (`stdlib.push`), but not guaranteed, so the cache has to record which
   module it was actually resolved against and the fast path has to
   check the *current* receiver still matches before trusting it.
   Doesn't call vm_get_property(). that returns only the looked-up
   value, not the entry/module pointers this needs to populate the
   cache. so this reimplements its exact logic (including its error
   messages) instead. */
static bool jit_helper_get_property_cached(vm_t *vm, obj_string_t *name, const value_t *object, value_t *out, jit_table_cache_t *cache) {
	obj_module_t *module;
	table_entry_t *entry;

	if (!value_is_obj(*object) || value_as_obj(*object)->type != OBJ_MODULE) {
		vm_runtime_error(vm, "vm::unsupported-member-access", "'.' access only works on modules right now");
		return false;
	}

	module = (obj_module_t *)value_as_obj(*object);
	entry = table_get_entry(module->members, name);
	if (entry == NULL) {
		vm_runtime_error(vm, "vm::undefined-module-member", "module '%.*s' has no member '%.*s'", (int)module->name->length, module->name->chars, (int)name->length, name->chars);
		return false;
	}

	*out = entry->value;
	cache->receiver = module;
	cache->generation = module->members->generation;
	cache->entry = entry;
	return true;
}

static void emit_push_immediate(jit_asm_t *a, value_type_t type, uint64_t payload) {
	jit_emit_mov32_reg_imm32(a, JIT_RAX, (uint32_t)type);
	jit_emit_mov32_mem_reg(a, JIT_R14, 0, JIT_RAX);
	jit_emit_mov_reg_imm64(a, JIT_RCX, payload);
	jit_emit_mov_mem_reg(a, JIT_R14, 8, JIT_RCX);
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

static void emit_op_constant(jit_asm_t *a, const chunk_t *chunk, uint16_t const_index) {
	value_t *slot;

	slot = &chunk->constants[const_index];
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)slot);
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_RAX, 0);
	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_RAX, 8);
	jit_emit_mov_mem_reg(a, JIT_R14, 0, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, 8, JIT_RDX);
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

/* Returns the fixup site of the trailing jmp to the shared epilogue,
   for the caller to patch once the epilogue's real offset is known. */
static size_t emit_op_return(jit_asm_t *a, const jit_reg_alloc_t *ra) {
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_reg(a, JIT_RSI, JIT_R13);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)vm_close_upvalues_from);
	emit_call(a, ra);

	// The result value is still sitting at the top of our own operand
	// stack ([r14-16]/[r14-8]), untouched by the call above. r14
	// itself is callee-saved, and vm_close_upvalues_from() never
	// touches stack *contents*, only vm->open_upvalues.
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -16);
	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_R14, -8);

	// New stack_top is frame->slots - 1: call_value() (the only place
	// that ever invokes jit_entry, see jit_dispatch.c) never pops the
	// callee value off the stack before setting frame->slots, so slot
	// frame->slots-1 always holds that still-live callee value, one
	// past the last argument's own slot below it. exactly what gets
	// overwritten with the return value here, mirroring OP_RETURN's
	// interpreted handler for every real (non-top-level) call.
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

/* count is a compile-time-known bytecode operand, so every source/dest
   displacement below is a constant. no runtime loop needed to
   replicate OP_DUP_N's "read fresh each iteration" interpreted
   semantics (see chunk.h's own comment on OP_DUP_N for why that
   wording matters: it's what makes [a,b] -> [a,b,a,b] rather than
   something else for a duplicated block deeper than one slot). */
static void emit_op_dup_n(jit_asm_t *a, uint8_t count) {
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

/* If slot is one of the (up to 3) register-allocated locals (see
   select_register_locals()), its bound register already holds its
   current payload. push a value_t built from it directly, no memory
   access at all. For a scalar binding (VAL_INT/VAL_FLOAT/VAL_BOOL) the
   tag is a compile-time-known constant (ra->type[i], from
   chunk->local_reg_types[slot]): register-eligibility (compiler.c's
   note_local_reg_slot()) guarantees this slot's type never changes for
   the whole function.

   A VAL_OBJ binding is different: paweli's type_is_assignable() lets
   `null` flow into a variable of any declared type, so an object-typed
   local's *logical* tag genuinely does vary between VAL_NULL and
   VAL_OBJ across its lifetime, even though its register-cache
   eligibility (unlike a scalar's, see disqualify_local_reg_slot()'s own
   comment in compiler.c) is never revoked for this. Reconstructed here
   at runtime instead, from the one bit of information a bare payload
   register can carry unambiguously: 0 means null (value_null() always
   writes payload 0, and a live heap object's address is never 0), any
   other value means a real object (value_obj() only ever wraps a real
   allocation). This is the one and only place that reconstruction has
   to happen: every other opcode already consumes this local's value
   only after it's been materialized onto the VM stack right here, with
   no awareness that its source was register-cached at all. */
static void emit_op_get_local(jit_asm_t *a, uint8_t slot, const jit_reg_alloc_t *ra) {
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

/* The mirror image: if slot is register-allocated, just read TOS's
   payload half into its bound register. the tag half is never
   written anywhere (it's a fixed constant already correct in memory
   from wherever this slot's tag was last baked. see emit_prologue()
   for a parameter, or simply never read at all for a non-parameter
   slot until its own first OP_SET_LOCAL, matching plain memory-resident
   locals' existing definite-assignment-before-use behavior). */
static void emit_op_set_local(jit_asm_t *a, uint8_t slot, const jit_reg_alloc_t *ra) {
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

/* OP_EQUAL/OP_NOT_EQUAL: value_equals() (src/runtime/value.c) never
   allocates and never fails, so this is real inlined-machine-code
   territory per the file comment's categories even though it's
   implemented as a call. no frame->ip write, no GC check, nothing a
   safepoint needs. Passes pointers rather than the two 16-byte value_t
   structs by value, sidestepping having to hand-reconstruct the System
   V struct-classification ABI rules for a two-eightbyte-INTEGER-class
   argument; the values already live at known stack addresses anyway. */
static void emit_op_comparison_bool(jit_asm_t *a, const jit_reg_alloc_t *ra, bool (*helper)(const value_t *, const value_t *)) {
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RDI, 2 * (int32_t)sizeof(value_t));
	jit_emit_mov_reg_reg(a, JIT_RSI, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RSI, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)helper);
	emit_call(a, ra);

	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_mov32_reg_imm32(a, JIT_RCX, (uint32_t)VAL_BOOL);
	jit_emit_mov32_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t), JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, -2 * (int32_t)sizeof(value_t) + 8, JIT_RAX);
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

/* Leaves 1 in result_reg if the value_t at [r14-16] is falsey
   (value_is_falsey()'s own definition: null, or bool false), else 0.
   Reads only the single `boolean` byte for a VAL_BOOL operand rather
   than a wider load, since only that one byte of value_t's union is
   ever actually written for a bool value (see value_bool()). the
   rest is uninitialized padding, same as in plain interpreted C. */
static void emit_compute_falsey(jit_asm_t *a, jit_reg_t result_reg) {
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

static void emit_op_not(jit_asm_t *a) {
	emit_compute_falsey(a, JIT_RAX);
	jit_emit_mov32_reg_imm32(a, JIT_RCX, (uint32_t)VAL_BOOL);
	jit_emit_mov32_mem_reg(a, JIT_R14, -16, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, -8, JIT_RAX);
}

static void emit_op_negate(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
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

	// IEEE-754 double negation is exactly "flip the sign bit"; this is
	// how GCC/Clang themselves compile unary minus on a double, not a
	// JIT-specific shortcut.
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, -8);
	jit_emit_mov_reg_imm64(a, JIT_RDX, 0x8000000000000000ULL);
	jit_emit_xor_reg_reg(a, JIT_RCX, JIT_RDX);
	jit_emit_mov_mem_reg(a, JIT_R14, -8, JIT_RCX);
	float_done = jit_emit_jmp(a);

	jit_patch_rel32(a, not_float_site, a->count);
	emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_error_negate_non_numeric);
	emit_call(a, ra);
	jit_emit_mov32_reg_imm32(a, JIT_RAX, 0);
	fixup_list_push(epilogue_fixups, jit_emit_jmp(a));

	jit_patch_rel32(a, int_done, a->count);
	jit_patch_rel32(a, float_done, a->count);
}

static void emit_op_bit_not(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
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
	emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_error_bit_not_non_int);
	emit_call(a, ra);
	jit_emit_mov32_reg_imm32(a, JIT_RAX, 0);
	fixup_list_push(epilogue_fixups, jit_emit_jmp(a));

	jit_patch_rel32(a, done_site, a->count);
}

/* The only case in which vm_binary_op()/vm_bitwise_op() can allocate:
   binary_op()'s OP_ADD branch on two OBJ_STRING operands calls
   object_new_string_concat() (vm.c's binary_op(), the string-concat
   branch right after the two operands are popped). bitwise_op() never
   allocates under any opcode. Keep this in lockstep with that branch
   in vm.c -- if it ever grows another allocating case, this predicate
   must grow with it, or emit_call_binary_slow_path's GC safepoint below
   silently stops covering it again. */
static bool jit_binary_op_may_allocate(uint8_t opcode, bool bitwise) {
	return !bitwise && opcode == OP_ADD;
}

/* Shared slow-path tail for every arithmetic/comparison/bitwise/shift
   opcode's fast-path miss: sync r14 to vm->stack_top (the helper reads
   operands via peek()/pop() against that field, not our cached
   register), write frame->ip (a real error safepoint. binary_op()/
   bitwise_op() can call vm_runtime_error() even though neither
   allocates), call the helper, reload r14, and bail to the shared
   epilogue with al already holding the helper's own false/0 on
   failure. no extra "mov al,0" needed, it's already correct.

   binary_op()'s OP_ADD-on-strings case is the one exception to "neither
   allocates" (see jit_binary_op_may_allocate() above), so this is also
   a GC safepoint exactly when that predicate says so -- same shape as
   emit_gc_check()'s other 6 call sites: check right before the call
   that might allocate, so mark_roots() sees a consistent stack/frame
   if a collection actually runs. */
static void emit_call_binary_slow_path(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, uint8_t opcode, bool bitwise, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	uint64_t helper_addr;

	emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_mem_reg(a, JIT_R12, offsetof(vm_t, stack_top), JIT_R14);
	// The GC check (when it applies) must run before any of the
	// argument/callee registers below are loaded: emit_gc_check() uses
	// rax/rcx/rdx/rdi as its own scratch (and rdi/rax again for its
	// internal call to gc_collect()), so running it any later would
	// clobber whatever this function just staged for its own call --
	// exactly the wild-jump bug this ordering avoids.
	if (jit_binary_op_may_allocate(opcode, bitwise)) {
		emit_gc_check(a, ra, offset);
	}
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov32_reg_imm32(a, JIT_RSI, opcode);
	helper_addr = bitwise ? (uint64_t)(uintptr_t)vm_bitwise_op : (uint64_t)(uintptr_t)vm_binary_op;
	jit_emit_mov_reg_imm64(a, JIT_RAX, helper_addr);
	emit_call(a, ra);
	jit_emit_mov_reg_mem(a, JIT_R14, JIT_R12, offsetof(vm_t, stack_top));
	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));
}

/* Both branches below (int and float) write their result to the same
   slot layout: tag at [r14-2*sizeof(value_t)], payload at
   [r14-2*sizeof(value_t)+8], then r14 -= sizeof(value_t) (two operands
   popped, one result pushed. net -1 slot). */
static void emit_op_arith_or_cmp(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
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

	// Both operands VAL_INT failed. check both VAL_FLOAT before giving
	// up to the slow path. Mixed int/float and anything else keeps
	// falling through to vm_binary_op() unchanged, exactly like before
	// this branch existed: it already handles promotion correctly in C.
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
		// Relational ops use the reversed-operand pattern GCC/Clang
		// themselves emit for `double` comparisons: UCOMISD sets
		// ZF=PF=CF=1 on an unordered (NaN-involved) compare, and
		// SETA/SETAE are false whenever CF=1. so comparing the
		// *second* value to seta/setae against the first gives exactly
		// IEEE-754's "any relational comparison against NaN is false"
		// for free, without a separate parity (PF) check. (OP_EQUAL/
		// OP_NOT_EQUAL don't go through this function at all.
		// emit_op_comparison_bool() already handles them via
		// value_equals(), type-independent. so there's no == / !=
		// case here needing the extra parity-flag handling those two
		// alone would require.)
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

/* OP_MODULO never gets a float fast path here (x86 has no hardware
   float-remainder instruction. even a "fast" path would still need a
   libc fmod() call, so int-tag failure goes straight to the slow path
   for it, same as before this function had any float awareness at
   all). OP_DIVIDE, on the other hand, needs neither a float check for
   the fast path itself nor a zero-divisor guard: IEEE-754 float
   division by zero produces inf/-inf/NaN, not a trap. DIVSD computes
   exactly what vm_binary_op()'s native C `a / b` on doubles already
   does, so there's nothing to check for. */
static void emit_op_div_mod(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
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

static void emit_op_bitwise_and_or_xor(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
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

static void emit_op_shift(jit_asm_t *a, const chunk_t *chunk, uint8_t opcode, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
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

	// Shift amount goes in RCX regardless (SHL/SAR's CL operand is
	// implicit in the encoding), which conveniently doubles as the
	// register the unsigned range check below reads.
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

/* Returns the fixup site of the (forward-only) conditional jump, for
   the caller to record against OP_JUMP_IF_FALSE's own target bytecode
   offset. Does not pop the condition value, matching peek(vm,0) in the
   interpreted handler. */
static size_t emit_op_jump_if_false(jit_asm_t *a) {
	emit_compute_falsey(a, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	return jit_emit_jcc(a, JIT_CC_NE);
}

/* globals is function->globals (see object.h's own comment: set once
   by compiler_compile(), fixed for the function's whole lifetime), a
   compile-time-known constant address, same treatment as OP_CONSTANT's
   baked constant-pool slot addresses. which is exactly what makes
   this call site cacheable at all: unlike OP_GET_PROPERTY below, there
   is no runtime receiver to also check, only whether `globals`'s own
   backing array might have moved since the last successful lookup here
   (table_set()'s adjust_capacity(), see table.h's own comment on
   table_t.generation).

   Fast path: cache->entry != NULL (something was cached before) and
   cache->generation matches the table's live generation means the
   cached table_entry_t* is still exactly where it was, so its value
   can be read directly. no call, no re-hash, no probe. Any mismatch
   (never cached yet, or the table resized since) falls through to a
   real jit_helper_get_global_cached() call, which does the actual
   lookup (and can still fail with the same vm_runtime_error() as
   always) and refreshes the cache cell for next time. This is a real
   two-sided branch with a fully-correct fallback, not a speculative
   guess needing unwinding, so it fits the JIT's no-speculation design
   the same way every other fast path in this file does: the cache
   existing at all only ever changes how fast a correct answer is
   found, never whether it's correct.

   emit_write_frame_ip() only runs on the slow path now, unlike before
   this cache existed (it used to be unconditional). safe because
   frame->ip is only ever read in response to vm_runtime_error(), which
   the fast path can never call (a cache hit means the value's already
   known good), and every other opcode that *can* fail still writes its
   own frame->ip immediately before doing so, same as always. A stale
   frame->ip left over from a fast-path GET_GLOBAL is simply never
   observed by anything. */
static void emit_op_get_global(jit_asm_t *a, const chunk_t *chunk, table_t *globals, uint16_t const_index, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_table_cache_t *cache) {
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

	emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RSI, (uint64_t)(uintptr_t)globals);
	jit_emit_mov_reg_imm64(a, JIT_RDX, (uint64_t)(uintptr_t)name);
	jit_emit_mov_reg_reg(a, JIT_RCX, JIT_R14);
	jit_emit_mov_reg_imm64(a, JIT_R8, (uint64_t)(uintptr_t)cache);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_get_global_cached);
	emit_call(a, ra);
	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));

	jit_patch_rel32(a, done_site, a->count);
}

/* Write-side mirror of emit_op_get_global() above, reusing the exact
   same jit_table_cache_t shape and hit/miss test (a separate cache cell
   per site, never shared with any OP_GET_GLOBAL site for the same
   name). OP_SET_GLOBAL is an expression like OP_SET_LOCAL: it leaves
   its value on the VM stack rather than popping it, so the fast path
   only ever reads TOS (at R14-16/R14-8, never adjusting R14) and writes
   it straight through the cached entry. no call at all on a hit,
   same as the read side. The slow path passes the address of that same
   TOS slot as jit_helper_set_global_cached()'s value_t* argument. */
static void emit_op_set_global(jit_asm_t *a, const chunk_t *chunk, table_t *globals, uint16_t const_index, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_table_cache_t *cache) {
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

	emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RSI, (uint64_t)(uintptr_t)globals);
	jit_emit_mov_reg_imm64(a, JIT_RDX, (uint64_t)(uintptr_t)name);
	jit_emit_mov_reg_reg(a, JIT_RCX, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RCX, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_imm64(a, JIT_R8, (uint64_t)(uintptr_t)cache);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_set_global_cached);
	emit_call(a, ra);
	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));

	jit_patch_rel32(a, done_site, a->count);
}

/* The explicit stack-value-array overflow check here (vm->stack_top,
   i.e. r14, must stay below vm->stack + vm->stack_size slots) has no
   analogue anywhere else in this file: call_value() already caps
   *frame* recursion depth via vm->max_frames on every call regardless
   of JIT status, but nothing else guards the value stack's own
   capacity for JIT-generated code the way run()'s per-instruction
   check already does for every interpreted instruction. An entirely
   JIT'd, deeply recursive call chain (each frame contributing its own
   locals/temporaries) could otherwise overrun vm->stack's backing
   allocation well before ever tripping the frame-count limit.
   It is checked at the two points in a JIT'd function where a whole new
   frame's worth of stack usage is about to be committed (emit_op_call()
   and emit_op_try_call() below), which is where that risk actually
   concentrates. Returns the fixup site to jump to on overflow; caller
   must patch it to a block that reports vm::stack-overflow and bails. */
static size_t emit_stack_overflow_check(jit_asm_t *a) {
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R12, offsetof(vm_t, stack));
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R12, offsetof(vm_t, stack_size));
	jit_emit_imul_reg_reg_imm32(a, JIT_RCX, JIT_RCX, (int32_t)sizeof(value_t));
	jit_emit_add_reg_reg(a, JIT_RAX, JIT_RCX);
	jit_emit_cmp_reg_reg(a, JIT_R14, JIT_RAX);
	return jit_emit_jcc(a, JIT_CC_AE);
}

/* The callee and its arg_count arguments are already on the stack
   (pushed by whatever JIT'd code produced them), matching interpreted
   OP_CALL's own precondition. see vm_jit_call()'s own comment
   (src/runtime/vm.c) for what it does once called.

   The value-stack overflow check and GC safepoint below are unrelated
   to call-site caching and stay exactly as they always have.
   They are unconditional, before any dispatch, whether the fast or slow path
   below ends up handling this call.

   Fast path (see jit_call_cache_t's own comment for why no
   staleness/generation check is needed here, unlike the table caches):
   the callee's tag/pointer are checked against call_cache; on a match,
   this inlines exactly what call_value()'s OBJ_FUNCTION/OBJ_CLOSURE
   cases do in C. the frame-count overflow check (never inlined into
   JIT'd code before this), computing the new call_frame_t's address
   (same arithmetic emit_prologue() already uses for its own current-
   frame pointer), writing its five fields, incrementing frame_count,
   then calls the cached function's jit_entry directly. Any mismatch
   (different callee, frame-count already at the limit, or simply never
   cached yet) falls through to the exact same jit_helper_call_cached()
   dispatch this file always used, just with a cache to refresh on
   success instead of jit_helper_call_cached's plain predecessor. */
static void emit_op_call(jit_asm_t *a, const chunk_t *chunk, size_t offset, uint8_t arg_count, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_call_cache_t *call_cache) {
	int32_t callee_disp;
	size_t overflow_site;
	size_t miss_tag;
	size_t miss_identity;
	size_t miss_frame_overflow;
	size_t slow_offset;
	size_t fast_done_site;
	size_t done_site;

	emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_mem_reg(a, JIT_R12, offsetof(vm_t, stack_top), JIT_R14);
	overflow_site = emit_stack_overflow_check(a);

	// Can allocate: the callee is arbitrary paweli code (a native, or a
	// function/closure whose own body reaches an allocating opcode), so
	// this is a GC safepoint, like OP_ARRAY/OP_STRUCT/OP_CLOSURE/
	// OP_GET_INDEX/OP_TRY_CALL already are. Without it, a function whose
	// whole runtime is one JIT'd loop would allocate into a heap that
	// never gets the vm.c threshold check run() performs between
	// instructions. the interpreted loop triggers a collection about
	// once a megabyte; the compiled equivalent would not until control
	// returned to run(), ballooning the heap instead. gc.h's safety
	// invariant ("gc_collect() only ever at well-known points") requires
	// the exact equivalent check here, and the sync-r14-then-collect
	// shape of emit_gc_check() keeps mark_roots() correct (it scans
	// vm->stack[0..stack_top); callee and args are still in it, above
	// frame->slots, and r14 is callee-saved so gc_collect preserves it).
	// Must run here, before callee_disp/miss_tag/etc. start loading rax
	// for their own purposes below: emit_gc_check() uses rax/rcx/rdx/rdi
	// as scratch, so running it any later would clobber values this
	// function is still staging for its own two calls further down.
	emit_gc_check(a, ra, offset);

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

	// frame = vm->frames + frame_count * sizeof(call_frame_t). same
	// shape emit_prologue() already uses for its own current-frame
	// pointer, just without that one's "-1" (this frame doesn't exist
	// yet; frame_count is incremented further down, after every field
	// is written, matching call_value()'s own ordering).
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
	emit_call(a, ra);
	jit_emit_mov_reg_mem(a, JIT_R14, JIT_R12, offsetof(vm_t, stack_top));
	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));
	fast_done_site = jit_emit_jmp(a);

	slow_offset = a->count;
	jit_patch_rel32(a, miss_tag, slow_offset);
	jit_patch_rel32(a, miss_identity, slow_offset);
	jit_patch_rel32(a, miss_frame_overflow, slow_offset);

	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov32_reg_imm32(a, JIT_RSI, arg_count);
	jit_emit_mov_reg_imm64(a, JIT_RDX, (uint64_t)(uintptr_t)call_cache);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_call_cached);
	emit_call(a, ra);
	jit_emit_mov_reg_mem(a, JIT_R14, JIT_R12, offsetof(vm_t, stack_top));
	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));
	done_site = jit_emit_jmp(a);

	jit_patch_rel32(a, overflow_site, a->count);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_error_stack_overflow);
	emit_call(a, ra);
	jit_emit_mov32_reg_imm32(a, JIT_RAX, 0);
	fixup_list_push(epilogue_fixups, jit_emit_jmp(a));

	jit_patch_rel32(a, fast_done_site, a->count);
	jit_patch_rel32(a, done_site, a->count);
}

/* container is at [r14-32], index at [r14-16] (2 popped, 1 pushed,
   net -1 slot). The common case (an array container, an int index,
   in bounds) is inlined as a direct 16-byte read through
   array->values[index], so the overwhelmingly typical index costs a few
   tag/bounds checks and one load instead of a full C-helper call. The
   fast path can never allocate (array reads don't; the slow path's
   string case builds a single-char string) and can never error, so it
   needs neither a GC check nor a frame->ip write. both are emitted
   only on the slow path, where the helper actually requires them. Same
   shape as every other fast path in this file: the slow path stays the
   authoritative implementation of the opcode's full semantics (string
   indexing, out-of-bounds, wrong types), the fast path only shortcuts
   the case it can prove safe. Only scratch registers are touched
   (rax/rcx/rdx/rsi/r8. no call is made, so none of emit_call()'s
   spill machinery applies), never the register-locals r15/r10/r11 or
   the machine-state registers rbx/r12/r13/r14. The bounds check
   mirrors vm_get_index()'s exact `index < 0 || (size_t)index >=
   array->count`: the sign test uses JIT_CC_L (jl) after test. The TEST
   instruction clears OF, so jl trips exactly on SF, and the count
   compare is unsigned (jae) since count is a size_t. */
static void emit_op_get_index(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
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

	// Slow path. unchanged from before this fast path existed.
	// jit_helper_get_index() can allocate (a single-char string result
	// from indexing a string), so this is both a GC safepoint
	// (emit_gc_check(), run before any of the call's own arg/rax setup
	// below so its scratch-register use can't clobber them) and an
	// error safepoint (frame->ip write). out is passed as container's
	// own slot address, safe to reuse as the write target since the
	// helper's by-value copies of *container/*index_value happen (as
	// normal C argument evaluation) before it can write through out,
	// and nothing else reads that slot again in between.
	emit_write_frame_ip(a, chunk, next_offset);
	emit_gc_check(a, ra, offset);

	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_reg(a, JIT_RSI, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RSI, 2 * (int32_t)sizeof(value_t));
	jit_emit_mov_reg_reg(a, JIT_RDX, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RDX, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_reg(a, JIT_RCX, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RCX, 2 * (int32_t)sizeof(value_t));
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_get_index);
	emit_call(a, ra);

	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));

	jit_patch_rel32(a, done_site, a->count);
}

/* container at [r14-48], index at [r14-32], value at [r14-16] (3
   popped, 1 pushed. net -2 slots). Same fast-path structure as
   emit_op_get_index() above: array + in-bounds int inlined as a direct
   16-byte write through array->values[index], everything else (string
   target, non-array, non-int index, out-of-bounds) falls through to
   jit_helper_set_index(), the authoritative implementation. The fast
   path neither allocates nor errors, so it needs no GC check and no
   frame->ip write; the slow path keeps the frame->ip write (the helper
   can report an error, so frame->ip must be current), though no
   emit_gc_check(). vm_set_index() never allocates, exactly as before.
   Only scratch registers are touched (rax/rcx/rdx/rsi/r8), never the
   register-locals r15/r10/r11 or rbx/r12/r13/r14. The result is the
   stored value itself, written to container's own slot (the same target
   the slow path's out uses), then r14 -= 32. */
static void emit_op_set_index(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
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

	// Slow path. authoritative OP_SET_INDEX semantics (string target,
	// non-array, non-int index, out-of-bounds), unchanged from before.
	// Never allocates (vm_set_index() only ever errors or writes into an
	// existing array slot), so this is an error safepoint only, no
	// emit_gc_check(). out is passed as container's own slot address,
	// same reuse-as-write-target reasoning as emit_op_get_index() above.
	emit_write_frame_ip(a, chunk, next_offset);

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
	emit_call(a, ra);

	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));
	jit_emit_sub_reg_imm32(a, JIT_R14, 2 * (int32_t)sizeof(value_t));

	jit_patch_rel32(a, done_site, a->count);
}

/* No type or bounds check. field_index is a compile-time constant
   the compiler only ever emits after confirming the object on the
   stack is an instance of exactly the struct type that field belongs
   to (see run()'s own OP_GET_FIELD/OP_SET_FIELD comment, vm.c). The
   instance pointer is the struct's own value_t payload (the union's
   obj_t* member). */
static void emit_op_get_field(jit_asm_t *a, uint8_t field_index) {
	int32_t disp;

	disp = (int32_t)field_index * (int32_t)sizeof(value_t);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_R14, -(int32_t)sizeof(value_t) + 8);
	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RAX, offsetof(obj_struct_t, fields));
	jit_emit_mov_reg_mem(a, JIT_RCX, JIT_RAX, disp);
	jit_emit_mov_reg_mem(a, JIT_RDX, JIT_RAX, disp + 8);
	jit_emit_mov_mem_reg(a, JIT_R14, -(int32_t)sizeof(value_t), JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, -(int32_t)sizeof(value_t) + 8, JIT_RDX);
}

static void emit_op_set_field(jit_asm_t *a, uint8_t field_index) {
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

/* object_new_array()/object_array_push() never fail (this codebase
   doesn't check allocation failure anywhere. see jit_asm.c's emit8()
   comment), so unlike every other allocating opcode this file handles,
   there's no epilogue_fixups bailout path needed here, matching
   run()'s own OP_ARRAY case.

   The new array's pointer has to survive the per-element
   object_array_push() calls below. r15 used to be free callee-saved
   scratch when this was first written, but any of r15/r10/r11 may now
   be one of the JIT's own register-allocated locals for the whole
   function (select_register_locals()). and even a caller-saved one
   (r10/r11) doesn't help here, since emit_call() only preserves *its
   own* bound-register contents around a call, not arbitrary scratch
   values a caller stuffed into the same registers beforehand. Held on
   the native x86 stack instead (sub rsp,16 rather than 8, to keep the
   16-byte call-site alignment emit_prologue() already establishes;
   only the low 8 bytes are ever used). */
static void emit_op_array(jit_asm_t *a, const chunk_t *chunk, size_t offset, uint8_t count, size_t next_offset, const jit_reg_alloc_t *ra) {
	int32_t src_base;
	int i;

	emit_write_frame_ip(a, chunk, next_offset);
	// Covers both object_new_array() below and every object_array_push()
	// call in the loop: must run before any of them starts loading
	// rdi/rax, since emit_gc_check() uses those as its own scratch (see
	// its own comment) and running it any later would clobber a
	// just-staged callee address or argument.
	emit_gc_check(a, ra, offset);

	jit_emit_mov_reg_mem(a, JIT_RDI, JIT_R12, offsetof(vm_t, heap));
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)object_new_array);
	emit_call(a, ra);
	jit_emit_sub_reg_imm32(a, JIT_RSP, 16);
	jit_emit_mov_mem_reg(a, JIT_RSP, 0, JIT_RAX);

	src_base = -(int32_t)count * (int32_t)sizeof(value_t);
	for (i = 0; i < count; i++) {
		int32_t disp = src_base + i * (int32_t)sizeof(value_t);

		jit_emit_mov_reg_mem(a, JIT_RDI, JIT_R12, offsetof(vm_t, heap));
		jit_emit_mov_reg_mem(a, JIT_RSI, JIT_RSP, 0);
		jit_emit_mov_reg_mem(a, JIT_RDX, JIT_R14, disp);
		jit_emit_mov_reg_mem(a, JIT_RCX, JIT_R14, disp + 8);
		jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)object_array_push);
		emit_call(a, ra);
	}

	jit_emit_mov_reg_mem(a, JIT_RAX, JIT_RSP, 0);
	jit_emit_add_reg_imm32(a, JIT_RSP, 16);

	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)count * (int32_t)sizeof(value_t));
	jit_emit_mov32_reg_imm32(a, JIT_RCX, (uint32_t)VAL_OBJ);
	jit_emit_mov32_mem_reg(a, JIT_R14, 0, JIT_RCX);
	jit_emit_mov_mem_reg(a, JIT_R14, 8, JIT_RAX);
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

/* object_new_struct() never fails either. Unlike the array case, a
   struct's fields array is fixed-size (never grows), so each field is
   just a direct memory write, matching run()'s own OP_STRUCT case
   (`instance->fields[i] = ...`). No per-field helper call needed.

   The new struct's pointer has to survive past the point rax gets
   overwritten with its own `fields` pointer for the write loop below,
   but no register is guaranteed free scratch for that here: r15/r10/r11
   may each be the JIT's own register-allocated locals for the whole
   function (select_register_locals()), so this file's own call-site
   scratch registers can no longer assume any of them are free. This is
   the exact bug class already found and fixed once for this same
   function. Held on the native x86 stack instead, mirroring
   emit_op_array()'s own stash above for the identical reason. */
static void emit_op_struct(jit_asm_t *a, const chunk_t *chunk, size_t offset, uint8_t count, obj_string_t *type_name, size_t next_offset, const jit_reg_alloc_t *ra) {
	int32_t src_base;
	int i;

	emit_write_frame_ip(a, chunk, next_offset);
	emit_gc_check(a, ra, offset);

	jit_emit_mov_reg_mem(a, JIT_RDI, JIT_R12, offsetof(vm_t, heap));
	jit_emit_mov_reg_imm64(a, JIT_RSI, (uint64_t)(uintptr_t)type_name);
	jit_emit_mov32_reg_imm32(a, JIT_RDX, count);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)object_new_struct);
	emit_call(a, ra);

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

/* vm_make_closure() never fails and returns value_t by value. a
   16-byte, two-INTEGER-eightbyte struct, so per the System V ABI it
   comes back in rax:rdx directly, no out-pointer needed (unlike every
   fallible helper in this file, which can't also use rax for a bool
   success/fail signal). function and the descriptor bytes are both
   compile-time-known constants (see vm_make_closure()'s own comment,
   vm.c, on why a baked address into this chunk's own code array is
   valid here the same way OP_CONSTANT's baked constant-pool addresses
   are); frame (rbx) is the one genuinely runtime value needed. */
static void emit_op_closure(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, const jit_reg_alloc_t *ra) {
	obj_function_t *function;
	uint8_t upvalue_count;
	const uint8_t *descriptors;

	function = (obj_function_t *)value_as_obj(chunk->constants[(uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2])]);
	upvalue_count = chunk->code[offset + 3];
	descriptors = &chunk->code[offset + 4];

	emit_write_frame_ip(a, chunk, next_offset);
	emit_gc_check(a, ra, offset);

	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_reg(a, JIT_RSI, JIT_RBX);
	jit_emit_mov_reg_imm64(a, JIT_RDX, (uint64_t)(uintptr_t)function);
	jit_emit_mov_reg_imm64(a, JIT_RCX, (uint64_t)(uintptr_t)descriptors);
	jit_emit_mov32_reg_imm32(a, JIT_R8, upvalue_count);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)vm_make_closure);
	emit_call(a, ra);

	jit_emit_mov_mem_reg(a, JIT_R14, 0, JIT_RAX);
	jit_emit_mov_mem_reg(a, JIT_R14, 8, JIT_RDX);
	jit_emit_add_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

static void emit_op_get_upvalue(jit_asm_t *a, uint8_t slot) {
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

static void emit_op_set_upvalue(jit_asm_t *a, uint8_t slot) {
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

static void emit_op_close_upvalue(jit_asm_t *a, const jit_reg_alloc_t *ra) {
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_reg(a, JIT_RSI, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RSI, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)vm_close_upvalues_from);
	emit_call(a, ra);
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
}

static void emit_op_throw(jit_asm_t *a, const chunk_t *chunk, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	emit_write_frame_ip(a, chunk, next_offset);

	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_mem(a, JIT_RSI, JIT_R14, -8);
	jit_emit_sub_reg_imm32(a, JIT_R14, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_throw);
	emit_call(a, ra);

	jit_emit_mov32_reg_imm32(a, JIT_RAX, 0);
	fixup_list_push(epilogue_fixups, jit_emit_jmp(a));
}

/* name is baked at compile time (same treatment as OP_GET_GLOBAL's own
   constant-pool string). object is popped from the stack and its own
   old slot is reused as out, same reasoning as emit_op_get_index()/
   emit_op_get_global() above. Never allocates (a pure table lookup),
   but can fail (not a module, undefined member).

   Caching this needs one more check than OP_GET_GLOBAL: there's no
   compile-time-fixed table here, only whatever module value the
   receiver happens to be at this call site (usually the same module
   every time in practice, `stdlib.push`, but not guaranteed), so
   the fast path has to confirm the *current* receiver still matches
   the one the cache was resolved against before trusting
   cache->generation at all. The receiver's own tag is checked first
   (VAL_OBJ) so a non-object value's payload bits can never be
   misread as coincidentally matching a cached module pointer. a
   real, if astronomically unlikely, unsoundness this design
   deliberately doesn't allow even as a theoretical edge case. */
static void emit_op_get_property(jit_asm_t *a, const chunk_t *chunk, obj_string_t *name, size_t next_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra, jit_table_cache_t *cache) {
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

	emit_write_frame_ip(a, chunk, next_offset);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RSI, (uint64_t)(uintptr_t)name);
	jit_emit_mov_reg_reg(a, JIT_RDX, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RDX, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_reg(a, JIT_RCX, JIT_R14);
	jit_emit_sub_reg_imm32(a, JIT_RCX, (int32_t)sizeof(value_t));
	jit_emit_mov_reg_imm64(a, JIT_R8, (uint64_t)(uintptr_t)cache);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_get_property_cached);
	emit_call(a, ra);

	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	fixup_list_push(epilogue_fixups, jit_emit_jcc(a, JIT_CC_E));

	jit_patch_rel32(a, done_site, a->count);
}

/* vm_try_call() (src/runtime/vm.c) does the entire "attempt the call,
   unwind and build the error value on failure" job in one shot. see
   its own comment for the full contract. so this opcode's JIT
   translation is a single call plus a branch on the result, same shape
   as emit_op_get_property() above. The 0-arg try-block closure is
   already on the stack, matching vm_try_call()'s own precondition
   (identical to what OP_CALL already requires). Needs the same
   stack-overflow guard as emit_op_call() (see emit_stack_overflow_check()'s
   own comment) since vm_try_call() can just as well drive an entirely
   JIT'd, arbitrarily deep reentrant call chain, and a GC check since its
   failure path allocates the error-message string.

   Returns the fixup site of the (forward-only) branch taken on
   failure, for the caller to record against this instruction's own
   jump-offset operand (target_bytecode_offset). mirroring
   emit_op_jump_if_false()'s contract exactly, since that's structurally
   what this is: on success, fall through with the result already on
   the stack; on failure, jump to the catch handler with the error
   message already on the stack.

   That failure jump is genuinely special among every jump in this
   file: it's the one place a jump target's own bytecode offset can be
   a register-allocated local's decl_end (the optional `catch(string
   name)` binding) *and* still needs the corresponding sync to actually
   run on arrival, unlike every other jump that might coincidentally
   share a bytecode offset with some earlier declaration's decl_end
   (most commonly a loop's own back-edge, see the main translation
   loop's own comment on its shared decl_end sync). The general,
   shared-per-bytecode-offset sync deliberately does NOT handle this
   case (it only fires for ordinary fall-through into that offset), so
   this function emits its own dedicated one: patches the failure jump
   to land here directly (no forward fixup needed, this address is
   already known), syncs any bound register whose decl_end equals
   target_bytecode_offset from the freshly-caught message now sitting
   at [r14-8], then falls into a second, ordinary forward jump that
   *does* need the caller's usual fixup, landing on the shared,
   sync-free entry point for target_bytecode_offset. */
static size_t emit_op_try_call(jit_asm_t *a, const chunk_t *chunk, size_t offset, size_t next_offset, size_t target_bytecode_offset, fixup_list_t *epilogue_fixups, const jit_reg_alloc_t *ra) {
	size_t overflow_site;
	size_t ok_site;
	size_t catch_site;
	size_t catch_sync_jump;
	int ri;

	emit_write_frame_ip(a, chunk, next_offset);
	emit_gc_check(a, ra, offset);
	overflow_site = emit_stack_overflow_check(a);

	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)vm_try_call);
	emit_call(a, ra);
	jit_emit_mov_reg_mem(a, JIT_R14, JIT_R12, offsetof(vm_t, stack_top));
	jit_emit_movzx8_reg_reg(a, JIT_RAX, JIT_RAX);
	jit_emit_test_reg_reg(a, JIT_RAX, JIT_RAX);
	catch_site = jit_emit_jcc(a, JIT_CC_E);
	ok_site = jit_emit_jmp(a);

	jit_patch_rel32(a, overflow_site, a->count);
	jit_emit_mov_reg_reg(a, JIT_RDI, JIT_R12);
	jit_emit_mov_reg_imm64(a, JIT_RAX, (uint64_t)(uintptr_t)jit_helper_error_stack_overflow);
	emit_call(a, ra);
	jit_emit_mov32_reg_imm32(a, JIT_RAX, 0);
	fixup_list_push(epilogue_fixups, jit_emit_jmp(a));

	// Catch-entry: the failure jump lands here directly (address already
	// known, no forward fixup needed for this one), syncs, then falls
	// into its own forward jump (needing the caller's usual fixup) to
	// target_bytecode_offset's ordinary, sync-free entry point.
	jit_patch_rel32(a, catch_site, a->count);
	for (ri = 0; ri < ra->count; ri++) {
		if (ra->decl_end[ri] == target_bytecode_offset) {
			jit_emit_mov_reg_mem(a, ra->reg[ri], JIT_R14, -8);
		}
	}
	catch_sync_jump = jit_emit_jmp(a);

	// Success lands here, deliberately *after* the catch-sync block
	// above, not before it: this is the value the try-block itself
	// produced, not a caught error, so it must never run that sync.
	// This is also, unchanged from before the catch-sync block existed,
	// exactly where this function's own generated code ends -- which is
	// where the very next bytecode instruction (OP_JUMP, emitted by
	// compile_try_expr() immediately after OP_TRY_CALL, to skip the
	// catch region on success) gets translated next.
	jit_patch_rel32(a, ok_site, a->count);
	return catch_sync_jump;
}

/* A pre-pass counting how many OP_GET_GLOBAL/OP_GET_PROPERTY sites (into
   *out_lookup_count) and OP_CALL sites (into *out_call_count) this
   function's own bytecode contains, so jit_compile_function() can
   allocate exactly that many jit_table_cache_t/jit_call_cache_t cells
   up front. see those types' own comments for why the counts have to
   be known before any code is emitted rather than growing the arrays
   as sites are found. Same opcode-width-aware scanning shape as
   select_register_locals() below (and the main translation loop's own
   switch): if it ever hits an opcode it doesn't recognize, it just
   stops scanning early rather than guessing, using whatever counts it
   already has. undercounting here can only make jit_compile_function()
   allocate too few cells for a chunk the main loop is about to reject
   anyway (an unrecognized opcode always aborts compilation for the
   whole function), never a valid function too few. */
static void count_cached_lookup_sites(const chunk_t *chunk, size_t *out_lookup_count, size_t *out_call_count) {
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

/* A lightweight pre-pass, run once before the real per-opcode
   translation loop, choosing which register-eligible locals (see
   chunk->local_reg_eligible[], populated by compiler.c) are worth
   keeping in registers for this function's whole body. up to
   JIT_MAX_REG_LOCALS of them, ranked by how often each is accessed
   (OP_GET_LOCAL/OP_SET_LOCAL), assigned to {r15, r10, r11} in that
   order so the hottest slot always lands in r15, the one register with
   zero spill cost under any circumstance (see emit_call()'s own
   comment for why r10/r11 aren't free). Deliberately a flat count, not
   weighted by loop nesting: even that correlates well enough for a
   first cut (see this feature's own plan/design notes), and a wrong
   choice here is never a correctness risk, only a missed optimization
   . every slot index is bounds- and eligibility-checked before being
   counted, so a bug in this scan's own opcode-width tracking could at
   worst pick a suboptimal slot, never an invalid one. Mirrors the main
   loop's own per-opcode width switch below; if it ever hits an opcode
   it doesn't recognize (should never happen for real bytecode. the
   real loop's own default case is what actually rejects unsupported
   opcodes), it just stops scanning early rather than guessing, using
   whatever counts it already has. */
static void select_register_locals(const chunk_t *chunk, jit_reg_alloc_t *out) {
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

		// A VAL_OBJ-typed binding pays real, per-occurrence overhead
		// every time one of these opcodes runs while it's alive
		// (emit_gc_check()'s flush, plus R2a's own caller-saved
		// push/pop if it lands in r10/r11): unlike a scalar binding,
		// whose register-caching is free once won, an object binding's
		// caching cost scales with how often a safepoint happens
		// nearby, not just with how often the binding itself is read.
		// Penalized directly against its own access count here, at the
		// same flat, unweighted granularity access_counts[] itself
		// already uses (this pass has no loop-nesting information
		// either; see this function's own header comment), so a
		// heavily call-adjacent object local can lose its own registry
		// slot to a scalar (or simply drop out entirely, since the
		// ranking below only ever picks a strictly positive count) even
		// when a register would otherwise be free. Deliberately not
		// restricted to opcodes whose safepoint is unconditional at
		// runtime (OP_GET_INDEX's string-indexing slow path, and
		// OP_ADD's string-concat case via jit_binary_op_may_allocate(),
		// are two more real safepoints this scan can't cheaply predict
		// from the opcode alone without a full type analysis);
		// undercounting there only makes the penalty slightly
		// conservative, never wrong.
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
	select_register_locals(chunk, &ra);
	// decl_end is only meaningful (and only ever checked against) for a
	// non-parameter register local. a parameter's bound register's
	// value comes from emit_prologue() loading frame->slots directly,
	// since there's no declaration bytecode for a parameter to speak of.
	// dead_at mirrors it for the opposite end of a VAL_OBJ binding's
	// live window (see jit_reg_alloc_t's own comment); a parameter's
	// scope never explicitly closes before the function itself returns,
	// same (size_t)-1 sentinel as an ordinary decl_end-less binding.
	for (reg_i = 0; reg_i < ra.count; reg_i++) {
		ra.decl_end[reg_i] = ra.slot[reg_i] >= function->arity ? chunk->local_reg_decl_end[ra.slot[reg_i]] : (size_t)-1;
		ra.dead_at[reg_i] = ra.slot[reg_i] >= function->arity ? chunk->local_reg_dead_at[ra.slot[reg_i]] : (size_t)-1;
	}
	// See jit_table_cache_t's/jit_call_cache_t's own comments: both
	// sized to their final counts and allocated before any code is
	// emitted, so the cell addresses baked into that code as
	// compile-time constants (see emit_op_get_global()/
	// emit_op_get_property()/emit_op_call()) stay valid forever, never
	// invalidated by a later realloc the way a growable array would.
	// calloc-zeroing means every cell starts in its "never cached yet"
	// state for free.
	count_cached_lookup_sites(chunk, &lookup_cache_count, &call_cache_count);
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
	// Same accepted unchecked-malloc convention as emit8()'s realloc in
	// jit_asm.c (see its own comment). this project doesn't check
	// allocation failure anywhere.
	native_offset_for_bytecode = malloc((chunk->count + 1) * sizeof(size_t));

	emit_prologue(&buf, &ra, function->arity);

	offset = 0;
	ok = true;
	while (ok && offset < chunk->count) {
		uint8_t opcode;
		uint8_t operand_a;
		uint16_t jump_operand;
		size_t next_offset;
		size_t target_bytecode_offset;

		// native_offset_for_bytecode[offset] must name the address of
		// the FIRST native instruction reached when execution arrives at
		// this bytecode position, whether by ordinary fall-through or by
		// a jump landing here directly -- recorded before anything below
		// is emitted for this offset, same as it always was. Crucially,
		// this is recorded BEFORE the possible decl_end sync below is
		// emitted: any jump landing exactly at this bytecode offset
		// (a loop's own back-edge, most commonly, when a scalar local's
		// declaration is textually the last thing before a loop's own
		// condition check, e.g. `int i = 0; while (...) {...}`) has to
		// land on the *ordinary* translated code for this offset, not
		// on the sync -- a loop back-edge doesn't carry a freshly
		// declared value the way OP_TRY_CALL's catch jump does (see
		// emit_op_try_call()'s own comment for that specific,
		// deliberately separate case), so re-running the sync there
		// would load a stale/garbage value from wherever the loop
		// body's own last pushed-then-popped temporary happened to
		// leave [r14-8], not this local's real value.
		native_offset_for_bytecode[offset] = buf.count;
		opcode = chunk->code[offset];

		switch (opcode) {
			case OP_CONSTANT:
				emit_op_constant(&buf, chunk, (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]));
				offset += 3;
				break;
			case OP_NULL:
				emit_push_immediate(&buf, VAL_NULL, 0);
				offset += 1;
				break;
			case OP_TRUE:
				emit_push_immediate(&buf, VAL_BOOL, 1);
				offset += 1;
				break;
			case OP_FALSE:
				emit_push_immediate(&buf, VAL_BOOL, 0);
				offset += 1;
				break;
			case OP_POP:
				jit_emit_sub_reg_imm32(&buf, JIT_R14, (int32_t)sizeof(value_t));
				offset += 1;
				break;
			case OP_DUP_N:
				emit_op_dup_n(&buf, chunk->code[offset + 1]);
				offset += 2;
				break;
			case OP_GET_LOCAL:
				emit_op_get_local(&buf, chunk->code[offset + 1], &ra);
				offset += 2;
				break;
			case OP_SET_LOCAL:
				emit_op_set_local(&buf, chunk->code[offset + 1], &ra);
				offset += 2;
				break;
			case OP_EQUAL:
				emit_op_comparison_bool(&buf, &ra, jit_helper_equals);
				offset += 1;
				break;
			case OP_NOT_EQUAL:
				emit_op_comparison_bool(&buf, &ra, jit_helper_not_equals);
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
				emit_op_arith_or_cmp(&buf, chunk, opcode, offset, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_DIVIDE:
			case OP_MODULO:
				next_offset = offset + 1;
				emit_op_div_mod(&buf, chunk, opcode, offset, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_BIT_AND:
			case OP_BIT_OR:
			case OP_BIT_XOR:
				next_offset = offset + 1;
				emit_op_bitwise_and_or_xor(&buf, chunk, opcode, offset, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_SHIFT_LEFT:
			case OP_SHIFT_RIGHT:
				next_offset = offset + 1;
				emit_op_shift(&buf, chunk, opcode, offset, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_NOT:
				emit_op_not(&buf);
				offset += 1;
				break;
			case OP_NEGATE:
				next_offset = offset + 1;
				emit_op_negate(&buf, chunk, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_BIT_NOT:
				next_offset = offset + 1;
				emit_op_bit_not(&buf, chunk, next_offset, &epilogue_fixups, &ra);
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
				forward_fixup_list_push(&forward_fixups, emit_op_jump_if_false(&buf), target_bytecode_offset);
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
				emit_op_get_global(&buf, chunk, globals, (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]), next_offset, &epilogue_fixups, &ra, &cache[cache_index]);
				cache_index++;
				offset += 3;
				break;
			case OP_SET_GLOBAL:
				next_offset = offset + 3;
				emit_op_set_global(&buf, chunk, globals, (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]), next_offset, &epilogue_fixups, &ra, &cache[cache_index]);
				cache_index++;
				offset += 3;
				break;
			case OP_CALL:
				next_offset = offset + 2;
				emit_op_call(&buf, chunk, offset, chunk->code[offset + 1], next_offset, &epilogue_fixups, &ra, &call_cache[call_cache_index]);
				call_cache_index++;
				offset += 2;
				break;
			case OP_GET_INDEX:
				next_offset = offset + 1;
				emit_op_get_index(&buf, chunk, offset, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_SET_INDEX:
				next_offset = offset + 1;
				emit_op_set_index(&buf, chunk, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_GET_FIELD:
				emit_op_get_field(&buf, chunk->code[offset + 1]);
				offset += 2;
				break;
			case OP_SET_FIELD:
				emit_op_set_field(&buf, chunk->code[offset + 1]);
				offset += 2;
				break;
			case OP_ARRAY:
				next_offset = offset + 2;
				emit_op_array(&buf, chunk, offset, chunk->code[offset + 1], next_offset, &ra);
				offset += 2;
				break;
			case OP_STRUCT:
				next_offset = offset + 4;
				emit_op_struct(&buf, chunk, offset, chunk->code[offset + 3], (obj_string_t *)value_as_obj(chunk->constants[(uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2])]), next_offset, &ra);
				offset += 4;
				break;
			case OP_CLOSURE:
				operand_a = chunk->code[offset + 3];
				next_offset = offset + 4 + (size_t)operand_a * 2;
				emit_op_closure(&buf, chunk, offset, next_offset, &ra);
				offset = next_offset;
				break;
			case OP_GET_UPVALUE:
				emit_op_get_upvalue(&buf, chunk->code[offset + 1]);
				offset += 2;
				break;
			case OP_SET_UPVALUE:
				emit_op_set_upvalue(&buf, chunk->code[offset + 1]);
				offset += 2;
				break;
			case OP_CLOSE_UPVALUE:
				emit_op_close_upvalue(&buf, &ra);
				offset += 1;
				break;
			case OP_THROW:
				next_offset = offset + 1;
				emit_op_throw(&buf, chunk, next_offset, &epilogue_fixups, &ra);
				offset += 1;
				break;
			case OP_GET_PROPERTY:
				next_offset = offset + 3;
				emit_op_get_property(&buf, chunk, (obj_string_t *)value_as_obj(chunk->constants[(uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2])]), next_offset, &epilogue_fixups, &ra, &cache[cache_index]);
				cache_index++;
				offset += 3;
				break;
			case OP_TRY_CALL:
				operand_a = chunk->code[offset + 1];
				jump_operand = (uint16_t)((operand_a << 8) | chunk->code[offset + 2]);
				next_offset = offset + 3;
				target_bytecode_offset = offset + 3 + jump_operand;
				forward_fixup_list_push(&forward_fixups, emit_op_try_call(&buf, chunk, offset, next_offset, target_bytecode_offset, &epilogue_fixups, &ra), target_bytecode_offset);
				offset += 3;
				break;
			case OP_RETURN:
				fixup_list_push(&epilogue_fixups, emit_op_return(&buf, &ra));
				offset += 1;
				break;
			default:
				ok = false;
				break;
		}

		// A register-allocated non-parameter local's declaration
		// bytecode ends exactly here: sync its bound register from what
		// was just pushed at [r14-8], so every later
		// OP_GET_LOCAL/OP_SET_LOCAL for this slot sees the correct value
		// regardless of what opcode(s) computed the initializer. Real
		// emitted code, not a compile-time-only decision. if this
		// declaration sits inside a loop body, this same instruction
		// naturally re-fires on every iteration via ordinary fall-through
		// (the loop's own back-edge lands *before* this point, at
		// native_offset_for_bytecode[loop_start], and falls through
		// linearly down to here again), keeping the register correct
		// each time, exactly like the interpreter's own memory-based
		// locals already do implicitly. More than one binding can share
		// the same decl_end if two register-allocated locals are
		// declared back-to-back with no intervening opcode.
		//
		// Deliberately NOT also reachable by an arbitrary jump landing
		// directly at this offset (native_offset_for_bytecode[offset]
		// was already recorded above, before this): a loop's own
		// back-edge can coincidentally target this exact bytecode
		// position too (e.g. `int i = 0; while (...) {...}`, where `i`'s
		// decl_end and the while's own loop_start are the same offset
		// with nothing textually between them), and that back-edge must
		// NOT re-run this sync -- it isn't bringing a freshly declared
		// value, just re-checking the condition, and `i`'s register is
		// already correctly up to date via its own OP_SET_LOCAL writes.
		// OP_TRY_CALL's catch handler is the one place a jump genuinely
		// does need this sync re-run on arrival; it gets its own
		// dedicated sync block instead of relying on this shared one
		// (see emit_op_try_call()'s own comment).
		//
		// For a VAL_OBJ-typed binding specifically, this also re-bakes
		// the memory slot's *tag* half back to VAL_OBJ, not just the
		// register's payload. emit_prologue() only bakes it once,
		// unconditionally, before any opcode runs -- but for a
		// non-parameter local that is also the very first thing this
		// function ever pushes (slot 0 with no preceding locals, e.g.
		// `fn f() { Point p = null; ... }`), the initializer expression
		// itself writes to this exact memory before the local is
		// "officially" declared, for the identical reason emit_gc_check's
		// own decl_end guard exists (r14 == this slot's address at that
		// point, since nothing has been pushed yet): `Point p = null;`'s
		// own OP_NULL push writes {VAL_NULL, 0} directly into [r13+0],
		// permanently stomping the prologue's VAL_OBJ tag with VAL_NULL,
		// even once p later holds a real, non-null struct. This is
		// invisible to a plain register read (emit_op_get_local()
		// reconstructs the logical tag from the register's own
		// zero-ness, never from memory), which is why this went
		// unnoticed until a real GC collection: emit_gc_check()'s flush
		// only ever writes the payload half (the tag is documented as
		// "never rewritten there"), so a corrupted memory tag stays
		// corrupted forever unless something fixes it here, and
		// mark_value() silently skips a VAL_NULL-tagged cell regardless
		// of what garbage payload sits next to it -- exactly the
		// "flushed correctly, marked as unreachable anyway" bug this
		// re-bake closes.
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
	emit_epilogue(&buf);

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
