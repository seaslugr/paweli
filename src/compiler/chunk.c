#include <stdlib.h>

#include "chunk.h"

static size_t disassemble_simple(const char *name, size_t offset, FILE *out);
static size_t disassemble_byte_operand(const chunk_t *chunk, const char *name, size_t offset, FILE *out);
static size_t disassemble_constant(const chunk_t *chunk, const char *name, size_t offset, FILE *out);
static size_t disassemble_jump(const chunk_t *chunk, const char *name, int sign, size_t offset, FILE *out);
static size_t disassemble_constant_and_byte(const chunk_t *chunk, const char *name, size_t offset, FILE *out);
static size_t disassemble_closure(const chunk_t *chunk, size_t offset, FILE *out);

void chunk_init(chunk_t *chunk) {
	chunk->count = 0;
	chunk->capacity = 0;
	chunk->code = NULL;
	chunk->lines = NULL;
	chunk->columns = NULL;
	chunk->constants = NULL;
	chunk->constants_count = 0;
	chunk->constants_capacity = 0;
	chunk->local_reg_eligible = NULL;
	chunk->local_reg_types = NULL;
	chunk->local_reg_decl_end = NULL;
	chunk->local_slot_count = 0;
}

void chunk_free(chunk_t *chunk) {
	free(chunk->code);
	free(chunk->lines);
	free(chunk->columns);
	free(chunk->constants);
	free(chunk->local_reg_eligible);
	free(chunk->local_reg_decl_end);
	free(chunk->local_reg_types);
	chunk_init(chunk);
}

void chunk_write(chunk_t *chunk, uint8_t byte, int line, int column) {
	if (chunk->count == chunk->capacity) {
		chunk->capacity = chunk->capacity < 8 ? 8 : chunk->capacity * 2;
		chunk->code = realloc(chunk->code, chunk->capacity * sizeof(uint8_t));
		chunk->lines = realloc(chunk->lines, chunk->capacity * sizeof(int));
		chunk->columns = realloc(chunk->columns, chunk->capacity * sizeof(int));
	}
	chunk->code[chunk->count] = byte;
	chunk->lines[chunk->count] = line;
	chunk->columns[chunk->count] = column;
	chunk->count++;
}

size_t chunk_add_constant(chunk_t *chunk, value_t value) {
	if (chunk->constants_count == chunk->constants_capacity) {
		chunk->constants_capacity = chunk->constants_capacity < 8 ? 8 : chunk->constants_capacity * 2;
		chunk->constants = realloc(chunk->constants, chunk->constants_capacity * sizeof(value_t));
	}
	chunk->constants[chunk->constants_count] = value;
	chunk->constants_count++;
	return chunk->constants_count - 1;
}

static size_t disassemble_simple(const char *name, size_t offset, FILE *out) {
	fprintf(out, "%s\n", name);
	return offset + 1;
}

static size_t disassemble_byte_operand(const chunk_t *chunk, const char *name, size_t offset, FILE *out) {
	uint8_t slot;

	slot = chunk->code[offset + 1];
	fprintf(out, "%-16s %4d\n", name, slot);
	return offset + 2;
}

static size_t disassemble_constant(const chunk_t *chunk, const char *name, size_t offset, FILE *out) {
	uint8_t index;

	index = chunk->code[offset + 1];
	fprintf(out, "%-16s %4d '", name, index);
	value_print(chunk->constants[index], out);
	fputs("'\n", out);
	return offset + 2;
}

static size_t disassemble_jump(const chunk_t *chunk, const char *name, int sign, size_t offset, FILE *out) {
	uint16_t jump;

	jump = (uint16_t)(chunk->code[offset + 1] << 8);
	jump = (uint16_t)(jump | chunk->code[offset + 2]);
	fprintf(out, "%-16s %4zu -> %zu\n", name, offset, offset + 3 + (size_t)(sign * jump));
	return offset + 3;
}

// OP_STRUCT's layout: opcode, a constant-pool index (the type name), then
// a plain byte (the field count), the same two-part shape as
// emit_name_op_str() + a following emit_byte() in the compiler.
static size_t disassemble_constant_and_byte(const chunk_t *chunk, const char *name, size_t offset, FILE *out) {
	uint8_t index;
	uint8_t count;

	index = chunk->code[offset + 1];
	count = chunk->code[offset + 2];
	fprintf(out, "%-16s %4d '", name, index);
	value_print(chunk->constants[index], out);
	fprintf(out, "' %d fields\n", count);
	return offset + 3;
}

// OP_CLOSURE is the only variable-length instruction in this format: a
// constant index, an upvalue count, then that many (is_local, index)
// byte pairs, one per upvalue descriptor. See chunk.h's own comment
// on OP_CLOSURE for what each pair means.
static size_t disassemble_closure(const chunk_t *chunk, size_t offset, FILE *out) {
	uint8_t const_index;
	uint8_t upvalue_count;
	uint8_t i;
	size_t pos;
	uint8_t is_local;
	uint8_t index;

	const_index = chunk->code[offset + 1];
	upvalue_count = chunk->code[offset + 2];
	fprintf(out, "%-16s %4d '", "OP_CLOSURE", const_index);
	value_print(chunk->constants[const_index], out);
	fprintf(out, "' %d upvalues\n", upvalue_count);

	pos = offset + 3;
	for (i = 0; i < upvalue_count; i++) {
		is_local = chunk->code[pos];
		index = chunk->code[pos + 1];
		fprintf(out, "%4zu      |                     %s %d\n", pos, is_local ? "local" : "upvalue", index);
		pos += 2;
	}
	return pos;
}

size_t chunk_disassemble_instruction(const chunk_t *chunk, size_t offset, FILE *out) {
	uint8_t instruction;

	fprintf(out, "%4zu ", offset);
	if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
		fputs("   | ", out);
	} else {
		fprintf(out, "%4d ", chunk->lines[offset]);
	}

	instruction = chunk->code[offset];

	switch (instruction) {
		case OP_CONSTANT:
			return disassemble_constant(chunk, "OP_CONSTANT", offset, out);
		case OP_NULL:
			return disassemble_simple("OP_NULL", offset, out);
		case OP_TRUE:
			return disassemble_simple("OP_TRUE", offset, out);
		case OP_FALSE:
			return disassemble_simple("OP_FALSE", offset, out);
		case OP_POP:
			return disassemble_simple("OP_POP", offset, out);
		case OP_GET_LOCAL:
			return disassemble_byte_operand(chunk, "OP_GET_LOCAL", offset, out);
		case OP_SET_LOCAL:
			return disassemble_byte_operand(chunk, "OP_SET_LOCAL", offset, out);
		case OP_GET_GLOBAL:
			return disassemble_constant(chunk, "OP_GET_GLOBAL", offset, out);
		case OP_DEFINE_GLOBAL:
			return disassemble_constant(chunk, "OP_DEFINE_GLOBAL", offset, out);
		case OP_SET_GLOBAL:
			return disassemble_constant(chunk, "OP_SET_GLOBAL", offset, out);
		case OP_GET_PROPERTY:
			return disassemble_constant(chunk, "OP_GET_PROPERTY", offset, out);
		case OP_ARRAY:
			return disassemble_byte_operand(chunk, "OP_ARRAY", offset, out);
		case OP_GET_INDEX:
			return disassemble_simple("OP_GET_INDEX", offset, out);
		case OP_SET_INDEX:
			return disassemble_simple("OP_SET_INDEX", offset, out);
		case OP_STRUCT:
			return disassemble_constant_and_byte(chunk, "OP_STRUCT", offset, out);
		case OP_GET_FIELD:
			return disassemble_byte_operand(chunk, "OP_GET_FIELD", offset, out);
		case OP_SET_FIELD:
			return disassemble_byte_operand(chunk, "OP_SET_FIELD", offset, out);
		case OP_EQUAL:
			return disassemble_simple("OP_EQUAL", offset, out);
		case OP_NOT_EQUAL:
			return disassemble_simple("OP_NOT_EQUAL", offset, out);
		case OP_LESS:
			return disassemble_simple("OP_LESS", offset, out);
		case OP_LESS_EQUAL:
			return disassemble_simple("OP_LESS_EQUAL", offset, out);
		case OP_GREATER:
			return disassemble_simple("OP_GREATER", offset, out);
		case OP_GREATER_EQUAL:
			return disassemble_simple("OP_GREATER_EQUAL", offset, out);
		case OP_ADD:
			return disassemble_simple("OP_ADD", offset, out);
		case OP_SUBTRACT:
			return disassemble_simple("OP_SUBTRACT", offset, out);
		case OP_MULTIPLY:
			return disassemble_simple("OP_MULTIPLY", offset, out);
		case OP_DIVIDE:
			return disassemble_simple("OP_DIVIDE", offset, out);
		case OP_MODULO:
			return disassemble_simple("OP_MODULO", offset, out);
		case OP_BIT_AND:
			return disassemble_simple("OP_BIT_AND", offset, out);
		case OP_BIT_OR:
			return disassemble_simple("OP_BIT_OR", offset, out);
		case OP_BIT_XOR:
			return disassemble_simple("OP_BIT_XOR", offset, out);
		case OP_SHIFT_LEFT:
			return disassemble_simple("OP_SHIFT_LEFT", offset, out);
		case OP_SHIFT_RIGHT:
			return disassemble_simple("OP_SHIFT_RIGHT", offset, out);
		case OP_NOT:
			return disassemble_simple("OP_NOT", offset, out);
		case OP_NEGATE:
			return disassemble_simple("OP_NEGATE", offset, out);
		case OP_BIT_NOT:
			return disassemble_simple("OP_BIT_NOT", offset, out);
		case OP_DUP_N:
			return disassemble_byte_operand(chunk, "OP_DUP_N", offset, out);
		case OP_JUMP:
			return disassemble_jump(chunk, "OP_JUMP", 1, offset, out);
		case OP_JUMP_IF_FALSE:
			return disassemble_jump(chunk, "OP_JUMP_IF_FALSE", 1, offset, out);
		case OP_LOOP:
			return disassemble_jump(chunk, "OP_LOOP", -1, offset, out);
		case OP_CLOSURE:
			return disassemble_closure(chunk, offset, out);
		case OP_GET_UPVALUE:
			return disassemble_byte_operand(chunk, "OP_GET_UPVALUE", offset, out);
		case OP_SET_UPVALUE:
			return disassemble_byte_operand(chunk, "OP_SET_UPVALUE", offset, out);
		case OP_CLOSE_UPVALUE:
			return disassemble_simple("OP_CLOSE_UPVALUE", offset, out);
		case OP_CALL:
			return disassemble_byte_operand(chunk, "OP_CALL", offset, out);
		case OP_RETURN:
			return disassemble_simple("OP_RETURN", offset, out);
		case OP_TRY_CALL:
			return disassemble_jump(chunk, "OP_TRY_CALL", 1, offset, out);
		case OP_THROW:
			return disassemble_simple("OP_THROW", offset, out);
	}

	fprintf(out, "unknown opcode %d\n", instruction);
	return offset + 1;
}

void chunk_disassemble(const chunk_t *chunk, const char *name, FILE *out) {
	size_t offset;

	fprintf(out, "== %s ==\n", name);

	offset = 0;
	while (offset < chunk->count) {
		offset = chunk_disassemble_instruction(chunk, offset, out);
	}
}
