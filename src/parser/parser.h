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
	/* Whole file's null-terminated text, kept only to render source
	   snippets in diagnostics. parser_t does not own it. */
	const char *source;
	bool had_error;
	bool panic_mode;
} parser_t;

void parser_init(parser_t *parser, const char *source, const char *filename);
ast_node_t *parser_parse_program(parser_t *parser);
