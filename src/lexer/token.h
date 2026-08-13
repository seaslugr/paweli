#pragma once

#include <stddef.h>

typedef enum {
	TOKEN_IDENTIFIER,
	TOKEN_INT_LITERAL,
	TOKEN_FLOAT_LITERAL,
	TOKEN_STRING_LITERAL,

	TOKEN_FN,
	TOKEN_INT,
	TOKEN_FLOAT,
	TOKEN_STRING,
	TOKEN_BOOL,
	TOKEN_IF,
	TOKEN_ELSE,
	TOKEN_WHILE,
	TOKEN_FOR,
	TOKEN_RETURN,
	TOKEN_TRUE,
	TOKEN_FALSE,
	TOKEN_NULL,
	TOKEN_EXPORT,

	TOKEN_MODULE,
	TOKEN_MODULEDEF,

	TOKEN_LPAREN,
	TOKEN_RPAREN,
	TOKEN_LBRACE,
	TOKEN_RBRACE,
	TOKEN_LBRACKET,
	TOKEN_RBRACKET,
	TOKEN_COMMA,
	TOKEN_SEMICOLON,
	TOKEN_DOT,

	TOKEN_PLUS,
	TOKEN_MINUS,
	TOKEN_STAR,
	TOKEN_SLASH,
	TOKEN_PERCENT,
	TOKEN_EQUAL,
	TOKEN_EQUAL_EQUAL,
	TOKEN_BANG,
	TOKEN_BANG_EQUAL,
	TOKEN_LESS,
	TOKEN_LESS_EQUAL,
	TOKEN_GREATER,
	TOKEN_GREATER_EQUAL,
	TOKEN_AMP_AMP,
	TOKEN_PIPE_PIPE,
	TOKEN_ARROW,

	TOKEN_ERROR,
	TOKEN_EOF,
} token_type_t;

typedef struct {
	token_type_t type;
	const char *start;
	size_t length;
	int line;
	int column;
	/* Only meaningful when type == TOKEN_ERROR. One of the "lexer::*"
	   diagnostic codes in diagnostic.h; lets the parser report the
	   lexer's own error under the shared diagnostic format. */
	const char *error_code;
} token_t;

const char *token_type_name(token_type_t type);
