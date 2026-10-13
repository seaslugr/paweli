#pragma once

#include <stdbool.h>

#include "ast.h"
#include "lexer/lexer.h"
#include "lexer/token.h"

typedef struct {
	lexer_t lexer;
	token_t previous;
	token_t current;
	const char *filename;
	// The whole file as null-terminated text, for diagnostic snippets.
	// Not owned.
	const char *source;
	bool had_error;
	bool panic_mode;
} parser_t;

void parser_init(parser_t *parser, const char *source, const char *filename);
ast_node_t *parser_parse_program(parser_t *parser);
