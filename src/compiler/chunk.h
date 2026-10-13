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
	// OP_SET_GLOBAL <name-constant-index>: stores the top of the stack
	// under this name in the current function's globals table. It is
	// keyed by name like OP_GET_GLOBAL, since a global has no stack slot.
	// The compiler only emits it for a registered global, so there is no
	// "undefined global" case at runtime.
	OP_SET_GLOBAL,

	OP_GET_PROPERTY,

	OP_ARRAY,
	OP_GET_INDEX,
	OP_SET_INDEX,

	// OP_STRUCT <name-constant-index> <field-count>: pops field-count
	// values in declared field order and pushes a new struct instance.
	// OP_GET_FIELD and OP_SET_FIELD <field-index> use an index resolved at
	// compile time, with no name lookup or type check at runtime.
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

	// Int only. A float operand to any of these six is a runtime error
	// (vm::bitwise-type-mismatch). The two shifts also reject an amount
	// outside [0, 63] (vm::shift-amount-out-of-range), which would be
	// undefined behavior in C.
	OP_BIT_AND,
	OP_BIT_OR,
	OP_BIT_XOR,
	OP_SHIFT_LEFT,
	OP_SHIFT_RIGHT,

	OP_NOT,
	OP_NEGATE,
	OP_BIT_NOT,

	// OP_DUP_N <count>: duplicates the top `count` stack values as one
	// block, in order: `[a, b] -> [a, b, a, b]` for count 2. Compound
	// assignment (`arr[i] += x`, `s.f += x`) uses it to read the target's
	// current value without evaluating the object and index twice.
	OP_DUP_N,

	OP_JUMP,
	OP_JUMP_IF_FALSE,
	OP_LOOP,

	// OP_CLOSURE <function-constant-idx> <upvalue-count> [<is_local> <index>]*:
	// the only variable-length instruction. Pushes a new closure over the
	// given compiled function and resolves each upvalue descriptor against
	// the running frame. With is_local set it captures frame->slots[index].
	// Otherwise it reuses frame->closure->upvalues[index], for a closure
	// nested more than one level deep.
	//
	// OP_GET_UPVALUE and OP_SET_UPVALUE <index> read and write
	// *frame->closure->upvalues[index]->location. They only appear in a
	// nested fn's chunk, which always runs as a closure.
	//
	// OP_CLOSE_UPVALUE closes the open upvalue that points at the stack
	// top, then pops it. It replaces OP_POP where a captured local's scope
	// ends. A `return` closes every open upvalue of the frame at once, in
	// the OP_RETURN handler.
	OP_CLOSURE,
	OP_GET_UPVALUE,
	OP_SET_UPVALUE,
	OP_CLOSE_UPVALUE,

	OP_CALL,
	OP_RETURN,

	// OP_TRY_CALL <catch-target, u16 forward offset>: pops a closure with
	// no parameters, compiled from a try block by compile_try_expr(), and
	// calls it reentrantly. On success it pushes the return value and
	// falls through, normally to an OP_JUMP over the catch region. On
	// failure it closes the upvalues of every frame the call pushed,
	// restores frame_count and stack_top, pushes the failure message as a
	// string, and jumps forward by the offset to the catch code.
	OP_TRY_CALL,

	// Pops a string and raises it as a runtime error with code
	// "user::thrown". It propagates like any vm::* error: an enclosing
	// OP_TRY_CALL catches it, or it is reported at the top level.
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

	// Register eligibility per local slot. The compiler fills these in
	// after it compiles the function body (see
	// compiler_note_local_reg_slot() for the rule). The JIT reads them.
	//
	// local_reg_eligible[i] is valid for i < local_slot_count. The other
	// three are valid when it is true:
	// - local_reg_types[i] is VAL_INT, VAL_FLOAT, or VAL_BOOL for a
	//   scalar, and VAL_OBJ for a string, struct, array, or closure.
	// - local_reg_decl_end[i] is the bytecode offset right after the
	//   local's declaration.
	// - local_reg_dead_at[i] is the offset right after its scope-closing
	//   pop, or (size_t)-1 if it lives until the function returns. Only
	//   used for VAL_OBJ (see jit_emit_gc_check()).
	//
	// All are NULL or 0 for a chunk with no locals and for the "<script>"
	// chunk, which the JIT never compiles.
	bool *local_reg_eligible;
	value_type_t *local_reg_types;
	size_t *local_reg_decl_end;
	size_t *local_reg_dead_at;
	int local_slot_count;
} chunk_t;

void chunk_init(chunk_t *chunk);
void chunk_free(chunk_t *chunk);
void chunk_write(chunk_t *chunk, uint8_t byte, int line, int column);
size_t chunk_add_constant(chunk_t *chunk, value_t value);

void chunk_disassemble(const chunk_t *chunk, const char *name, FILE *out);
size_t chunk_disassemble_instruction(const chunk_t *chunk, size_t offset, FILE *out);
