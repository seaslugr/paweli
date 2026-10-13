#pragma once

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostics/diagnostic.h"
#include "parser.h"

void parser_error_at(parser_t *parser, token_t token, const char *code, const char *message);
void parser_error_at_current(parser_t *parser, const char *code, const char *message);
void parser_advance(parser_t *parser);
bool parser_check(parser_t *parser, token_type_t type);
bool parser_match(parser_t *parser, token_type_t type);
void parser_consume(parser_t *parser, token_type_t type, const char *message);
bool parser_check_type_ref_start(parser_t *parser);
bool parser_check_fn_is_type(parser_t *parser);
bool parser_check_var_decl_start(parser_t *parser);
type_ref_t parse_type_ref(parser_t *parser);
void parser_synchronize(parser_t *parser);
token_t parser_synthetic_token(token_type_t type, const char *text, int line, int column);

ast_node_t *parse_function_decl(parser_t *parser);
void parse_params(parser_t *parser, ast_node_t *fn_node);
ast_node_t *parse_decorator(parser_t *parser);
ast_node_t *parse_struct_decl(parser_t *parser);

ast_node_t *parse_block(parser_t *parser);
bool parser_starts_keyword_statement(parser_t *parser);
ast_node_t *parse_statement(parser_t *parser);
ast_node_t *parse_var_decl(parser_t *parser);
ast_node_t *parse_for_stmt(parser_t *parser);

ast_node_t *parse_try_value_block(parser_t *parser);
ast_node_t *parse_try_expr(parser_t *parser, bool require_value);
ast_node_t *parse_expression(parser_t *parser);
ast_node_t *parse_primary(parser_t *parser);
