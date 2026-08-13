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

	OP_GET_PROPERTY,

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
	OP_NOT,
	OP_NEGATE,

	OP_JUMP,
	OP_JUMP_IF_FALSE,
	OP_LOOP,

	OP_CALL,
	OP_RETURN,
} opcode_t;

typedef struct {
	size_t count;
	size_t capacity;
	uint8_t *code;
	int *lines;

	value_t *constants;
	size_t constants_count;
	size_t constants_capacity;
} chunk_t;

void chunk_init(chunk_t *chunk);
void chunk_free(chunk_t *chunk);
void chunk_write(chunk_t *chunk, uint8_t byte, int line);
size_t chunk_add_constant(chunk_t *chunk, value_t value);

void chunk_disassemble(const chunk_t *chunk, const char *name, FILE *out);
size_t chunk_disassemble_instruction(const chunk_t *chunk, size_t offset, FILE *out);
