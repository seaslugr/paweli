#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_internal.h"

static uint64_t parse_int_literal_magnitude(token_t token, bool *out_overflow);

chunk_t *compiler_current_chunk(compiler_t *compiler) {
	return &compiler->function->chunk;
}

void compiler_error(compiler_t *compiler, int line, int column, int length, const char *code, const char *format, ...) {
	va_list args;
	char message[512];

	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	diagnostic_emit(DIAG_ERROR, code, compiler->filename, compiler->source, line, column, length, "%s", message);
	compiler->had_error = true;
}

/* Like compiler_error(), but does not set had_error. A warning never
   fails compilation. Used by @deprecated. */
void compiler_warning(compiler_t *compiler, int line, int column, int length, const char *code, const char *format, ...) {
	va_list args;
	char message[512];

	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	diagnostic_emit(DIAG_WARNING, code, compiler->filename, compiler->source, line, column, length, "%s", message);
}

void compiler_emit_byte(compiler_t *compiler, uint8_t byte, int line, int column) {
	chunk_write(compiler_current_chunk(compiler), byte, line, column);
}

void compiler_emit_bytes(compiler_t *compiler, uint8_t b1, uint8_t b2, int line, int column) {
	compiler_emit_byte(compiler, b1, line, column);
	compiler_emit_byte(compiler, b2, line, column);
}

size_t compiler_emit_jump(compiler_t *compiler, uint8_t instruction, int line, int column) {
	compiler_emit_byte(compiler, instruction, line, column);
	compiler_emit_byte(compiler, 0xff, line, column);
	compiler_emit_byte(compiler, 0xff, line, column);
	return compiler_current_chunk(compiler)->count - 2;
}

void compiler_patch_jump(compiler_t *compiler, size_t offset, int line) {
	size_t jump;

	jump = compiler_current_chunk(compiler)->count - offset - 2;

	if (jump > UINT16_MAX) {
		compiler_error(compiler, line, 0, 0, "compiler::jump-too-large", "too much code to jump over");
		return;
	}

	compiler_current_chunk(compiler)->code[offset] = (uint8_t)((jump >> 8) & 0xff);
	compiler_current_chunk(compiler)->code[offset + 1] = (uint8_t)(jump & 0xff);
}

void compiler_emit_loop(compiler_t *compiler, size_t loop_start, int line, int column) {
	size_t offset;
	size_t jump;

	compiler_emit_byte(compiler, OP_LOOP, line, column);

	offset = compiler_current_chunk(compiler)->count + 2;
	jump = offset - loop_start;

	if (jump > UINT16_MAX) {
		compiler_error(compiler, line, 0, 0, "compiler::loop-too-large", "loop body too large");
		jump = 0;
	}

	compiler_emit_byte(compiler, (uint8_t)((jump >> 8) & 0xff), line, column);
	compiler_emit_byte(compiler, (uint8_t)(jump & 0xff), line, column);
}

void compiler_emit_constant(compiler_t *compiler, value_t value, int line, int column) {
	size_t index;

	index = chunk_add_constant(compiler_current_chunk(compiler), value);
	if (index > UINT16_MAX) {
		compiler_error(compiler, line, 0, 0, "compiler::too-many-constants", "too many constants in one function");
		return;
	}

	compiler_emit_byte(compiler, OP_CONSTANT, line, column);
	compiler_emit_bytes(compiler, (uint8_t)((index >> 8) & 0xff), (uint8_t)(index & 0xff), line, column);
}

void compiler_emit_name_op_str(compiler_t *compiler, uint8_t opcode, const char *name, size_t length, int line, int column) {
	obj_string_t *string;
	size_t index;

	string = object_new_string(compiler->heap, compiler->strings, name, length);
	index = chunk_add_constant(compiler_current_chunk(compiler), value_obj((obj_t *)string));
	if (index > UINT16_MAX) {
		compiler_error(compiler, line, 0, 0, "compiler::too-many-constants", "too many constants in one function");
		return;
	}

	compiler_emit_byte(compiler, opcode, line, column);
	compiler_emit_bytes(compiler, (uint8_t)((index >> 8) & 0xff), (uint8_t)(index & 0xff), line, column);
}

/* Fills in the operand that compiler_emit_name_op_str() reserved. A
   call's callee must sit below its arguments on the stack, but which
   overload it names is only known after the arguments are compiled. So
   the opcode is emitted first and its constant index is patched here,
   like compiler_emit_jump() and compiler_patch_jump(). */
void compiler_patch_name_op(compiler_t *compiler, size_t offset, const char *name, size_t length, int line, int column) {
	obj_string_t *string;
	size_t index;

	string = object_new_string(compiler->heap, compiler->strings, name, length);
	index = chunk_add_constant(compiler_current_chunk(compiler), value_obj((obj_t *)string));
	if (index > UINT16_MAX) {
		compiler_error(compiler, line, column, 0, "compiler::too-many-constants", "too many constants in one function");
		return;
	}

	compiler_current_chunk(compiler)->code[offset + 1] = (uint8_t)((index >> 8) & 0xff);
	compiler_current_chunk(compiler)->code[offset + 2] = (uint8_t)(index & 0xff);
}

/* Token text is not null-terminated, and strtod would read past the
   token into the source that follows. So the text is copied into a
   bounded buffer first.

   The result is an unsigned magnitude. That lets
   compiler_parse_negated_int_literal() accept exactly 2^63, the digits
   of INT64_MIN, which do not fit in an int64_t. */
static uint64_t parse_int_literal_magnitude(token_t token, bool *out_overflow) {
	char buffer[64];
	size_t length;
	uint64_t magnitude;

	length = token.length < sizeof(buffer) - 1 ? token.length : sizeof(buffer) - 1;
	memcpy(buffer, token.start, length);
	buffer[length] = '\0';

	errno = 0;
	magnitude = strtoull(buffer, NULL, 10);
	*out_overflow = errno == ERANGE;
	return magnitude;
}

int64_t compiler_parse_int_literal(compiler_t *compiler, token_t token, int line, int column) {
	bool overflow;
	uint64_t magnitude;

	magnitude = parse_int_literal_magnitude(token, &overflow);
	if (overflow || magnitude > (uint64_t)INT64_MAX) {
		compiler_error(compiler, line, column, (int)token.length, "compiler::int-literal-overflow", "integer literal '%.*s' is too large for a 64-bit int (max %lld)", (int)token.length, token.start, (long long)INT64_MAX);
		return 0;
	}
	return (int64_t)magnitude;
}

/* Folds unary '-' on an int literal at compile time (see
   compile_unary_expr()). Without this INT64_MIN could not be written:
   its digits overflow int64_t before the negation applies. The value
   is computed as 0 - magnitude in unsigned arithmetic and then read as
   signed, which is correct for every magnitude in range. */
int64_t compiler_parse_negated_int_literal(compiler_t *compiler, token_t token, int line, int column) {
	bool overflow;
	uint64_t magnitude;

	magnitude = parse_int_literal_magnitude(token, &overflow);
	if (overflow || magnitude > 9223372036854775808ULL) {
		compiler_error(compiler, line, column, (int)token.length, "compiler::int-literal-overflow", "integer literal '-%.*s' is too small for a 64-bit int (min %lld)", (int)token.length, token.start, (long long)INT64_MIN);
		return 0;
	}
	return (int64_t)(0ULL - magnitude);
}

double compiler_parse_float_literal(token_t token) {
	char buffer[64];
	size_t length;

	length = token.length < sizeof(buffer) - 1 ? token.length : sizeof(buffer) - 1;
	memcpy(buffer, token.start, length);
	buffer[length] = '\0';

	return strtod(buffer, NULL);
}

bool compiler_identifiers_equal(token_t a, token_t b) {
	if (a.length != b.length) {
		return false;
	}
	return memcmp(a.start, b.start, a.length) == 0;
}

/* A name for a synthetic local. Identifiers cannot start with '$', so
   "$for_array" cannot collide with a real one. Used by
   compile_foreach_stmt(). */
token_t compiler_synthetic_token(const char *text) {
	token_t token;

	token.type = TOKEN_IDENTIFIER;
	token.start = text;
	token.length = strlen(text);
	token.line = 0;
	token.column = 0;
	token.error_code = NULL;

	return token;
}
