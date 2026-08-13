#pragma once

#include "token.h"

typedef struct {
	const char *start;
	const char *current;
	int line;
	int column;
	int token_line;
	int token_column;
} lexer_t;

void lexer_init(lexer_t *lexer, const char *source);
token_t lexer_next_token(lexer_t *lexer);
