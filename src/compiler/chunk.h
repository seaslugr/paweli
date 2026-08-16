#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "runtime/value.h"

typedef enum {
	OP_CONSTANT,
	OP_NULL,
	OP_TRUE,
	OP_FALSE,
	OP_POP,

	OP_GET_LOCAL,
	OP_SET_LOCAL,
	OP_GET_GLOBAL,
	OP_DEFINE_GLOBAL,
	// OP_SET_GLOBAL <name-constant-index>: pops TOS and stores it under
	// this name in the current function's globals table. Encoded like
	// OP_GET_GLOBAL (name-keyed, via emit_name_op_str()), not like
	// OP_SET_LOCAL (frame-slot-keyed). A global has no fixed stack
	// position. Only ever emitted for a name already registered by
	// register_globals(), so there is no "undefined global" case to
	// handle at runtime.
	OP_SET_GLOBAL,

	OP_GET_PROPERTY,

	OP_ARRAY,
	OP_GET_INDEX,
	OP_SET_INDEX,

	// OP_STRUCT <name-constant-index> <field-count>: pops field-count
	// values (in the struct's declared field order) and pushes a new
	// struct instance. OP_GET_FIELD/OP_SET_FIELD <field-index> access a
	// field by a fixed index resolved at compile time. Unlike
	// OP_GET_PROPERTY (name-based, modules only), a struct's field
	// layout is always known statically, so there is no name lookup or
	// type check left to do at runtime.
	OP_STRUCT,
	OP_GET_FIELD,
	OP_SET_FIELD,

	OP_EQUAL,
	OP_NOT_EQUAL,
	OP_LESS,
	OP_LESS_EQUAL,
	OP_GREATER,
	OP_GREATER_EQUAL,
	OP_ADD,
	OP_SUBTRACT,
	OP_MULTIPLY,
	OP_DIVIDE,
	OP_MODULO,

	// Int-only; a float operand to any of these six is a runtime error
	// (vm::bitwise-type-mismatch) rather than promoted, unlike
	// OP_ADD/OP_SUBTRACT/OP_MULTIPLY/OP_DIVIDE. OP_SHIFT_LEFT/
	// OP_SHIFT_RIGHT additionally reject a shift amount outside
	// [0, 63] (vm::shift-amount-out-of-range) instead of leaving it as
	// undefined behavior in the underlying C shift.
	OP_BIT_AND,
	OP_BIT_OR,
	OP_BIT_XOR,
	OP_SHIFT_LEFT,
	OP_SHIFT_RIGHT,

	OP_NOT,
	OP_NEGATE,
	OP_BIT_NOT,

	// OP_DUP_N <count byte>: duplicates the top `count` stack values as a
	// contiguous block, preserving their order (`[a, b] -> [a, b, a, b]`
	// for count 2). Used by compiler.c's compound-assignment compilation
	// (`arr[i] += x`, `s.f += x`) to read an index/member target's
	// current value without re-evaluating the object/index
	// sub-expressions a second time, which would double any side effect
	// they have.
	OP_DUP_N,

	OP_JUMP,
	OP_JUMP_IF_FALSE,
	OP_LOOP,

	// OP_CLOSURE <function-constant-idx> <upvalue-count> [<is_local> <index>]*:
	// the only variable-length instruction in this chunk format. Pushes a
	// new closure wrapping the given (already fully compiled)
	// obj_function_t, resolving each upvalue descriptor against the
	// *currently executing* frame. is_local true means "capture
	// frame->slots[index]" (a local of the immediately enclosing
	// function), false means "reuse frame->closure->upvalues[index]"
	// (an upvalue of the enclosing function itself, for a closure nested
	// more than one level deep). OP_GET_UPVALUE/OP_SET_UPVALUE
	// <index> read/write *frame->closure->upvalues[index]->location;
	// both are only ever emitted inside a nested fn's own chunk, which
	// can only ever run as a closure, so frame->closure is never NULL
	// there. OP_CLOSE_UPVALUE closes (see obj_upvalue_t) whatever open
	// upvalue points at the current stack top, then pops it. Emitted
	// in place of OP_POP wherever a captured local's scope ends normally
	// (end_scope()/emit_pop_to_depth() in compiler.c); an early exit via
	// `return` instead closes every upvalue still open in the whole
	// frame in one step, from OP_RETURN's own handler in vm.c.
	OP_CLOSURE,
	OP_GET_UPVALUE,
	OP_SET_UPVALUE,
	OP_CLOSE_UPVALUE,

	OP_CALL,
	OP_RETURN,

	// OP_TRY_CALL <catch-target, u16 forward offset>: pops a 0-arg
	// closure (compiled from a try-block's body by compile_try_expr(),
	// the same OP_CLOSURE mechanism a lambda already uses) and calls it
	// reentrantly, the same call_value()-plus-nested-run() pattern
	// vm_call_value() already established for a native calling back
	// into paweli, just invoked from inside the bytecode dispatch loop
	// itself instead of from C. On success: pushes the closure's return
	// value and falls through to the very next instruction (normally an
	// OP_JUMP skipping over the catch region). On failure: closes every
	// upvalue opened by the (possibly many) frames the reentrant call
	// pushed, unwinds vm->frame_count/stack_top back to where they were
	// before this instruction ran, pushes the failure's message as a
	// string, and jumps forward by the offset, landing at the start of
	// the compiled catch-handling code.
	OP_TRY_CALL,

	// Pops a string and raises it as a catchable runtime error (code
	// "user::thrown"), propagating exactly like any other vm::* runtime
	// error: caught by an enclosing OP_TRY_CALL's reentrant run(), or
	// uncaught and reported at the top level the same way a division by
	// zero already is.
	OP_THROW,
} opcode_t;

typedef struct {
	size_t count;
	size_t capacity;
	uint8_t *code;
	int *lines;
	int *columns;

	value_t *constants;
	size_t constants_count;
	size_t constants_capacity;

	// Per-local-slot register-allocation eligibility, populated once by
	// compiler.c at the end of compiling this function's body (see
	// note_local_reg_slot()'s own comment for the eligibility rule) and
	// consumed by src/jit/jit_compile.c. local_reg_eligible[i] is only
	// meaningful when i < local_slot_count; local_reg_types[i]
	// (VAL_INT/VAL_FLOAT/VAL_BOOL) and local_reg_decl_end[i] (the
	// bytecode offset right after this local's own declaration. See
	// compiler_t's own comment on why the JIT needs this, since a
	// declaration is never a dedicated opcode of its own) are only
	// meaningful when local_reg_eligible[i] is true. All NULL/0 for a
	// chunk with no locals at all (or one that was never routed through
	// the finalization step, e.g. the top-level "<script>" chunk, which
	// the JIT never compiles regardless).
	bool *local_reg_eligible;
	value_type_t *local_reg_types;
	size_t *local_reg_decl_end;
	int local_slot_count;
} chunk_t;

void chunk_init(chunk_t *chunk);
void chunk_free(chunk_t *chunk);
void chunk_write(chunk_t *chunk, uint8_t byte, int line, int column);
size_t chunk_add_constant(chunk_t *chunk, value_t value);

void chunk_disassemble(const chunk_t *chunk, const char *name, FILE *out);
size_t chunk_disassemble_instruction(const chunk_t *chunk, size_t offset, FILE *out);
