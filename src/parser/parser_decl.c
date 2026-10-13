#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "parser_internal.h"

ast_node_t *parse_function_decl(parser_t *parser) {
	token_t name;
	int line;
	int column;
	bool has_return_type;
	type_ref_t return_type;
	ast_node_t *fn_node;

	if (!parser_check(parser, TOKEN_FN)) {
		parser_error_at_current(parser, "parser::expected-function", "expected a function declaration ('fn')");
		// Consume this token before synchronizing. If it is one of the tokens
		// parser_synchronize() stops at, synchronize would return at once, and
		// the caller would retry parse_function_decl() on the same token
		// forever.
		parser_advance(parser);
		parser_synchronize(parser);
		return NULL;
	}

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);

	parser_consume(parser, TOKEN_IDENTIFIER, "expected a function name");
	name = parser->previous;

	parser_consume(parser, TOKEN_LPAREN, "expected '(' after function name");

	fn_node = ast_new_function_decl(name, false, (type_ref_t){TOKEN_EOF, false, {0}, NULL}, NULL, line, column);

	if (!parser_check(parser, TOKEN_RPAREN)) {
		parse_params(parser, fn_node);
	}

	parser_consume(parser, TOKEN_RPAREN, "expected ')' after parameters");

	has_return_type = false;
	return_type = (type_ref_t){TOKEN_EOF, false, {0}, NULL};
	if (parser_match(parser, TOKEN_ARROW)) {
		if (!parser_check_type_ref_start(parser)) {
			parser_error_at_current(parser, "parser::expected-return-type", "expected a return type after '->'");
		} else {
			return_type = parse_type_ref(parser);
			has_return_type = true;
		}
	}
	fn_node->as.function_decl.has_return_type = has_return_type;
	fn_node->as.function_decl.return_type = return_type;

	fn_node->as.function_decl.body = parse_block(parser);

	return fn_node;
}

void parse_params(parser_t *parser, ast_node_t *fn_node) {
	type_ref_t type;
	token_t name;
	ast_node_t *param;
	int line;
	int column;

	for (;;) {
		if (!parser_check_type_ref_start(parser)) {
			parser_error_at_current(parser, "parser::expected-parameter-type", "expected a parameter type");
			return;
		}

		line = parser->current.line;
		column = parser->current.column;
		type = parse_type_ref(parser);

		parser_consume(parser, TOKEN_IDENTIFIER, "expected a parameter name");
		name = parser->previous;

		param = ast_new_param(type, name, NULL, line, column);
		ast_list_push(&fn_node->as.function_decl.params, param);

		if (!parser_match(parser, TOKEN_COMMA)) {
			break;
		}
	}
}

/* `@name` or `@name("string literal")`. Only parsed at the top level,
   right before a `fn` (see parser_parse_program()). Anywhere else a
   `@` gives the "expected an expression" error from parse_primary(). */
ast_node_t *parse_decorator(parser_t *parser) {
	int line;
	int column;
	token_t name;
	ast_node_t *arg;

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);

	parser_consume(parser, TOKEN_IDENTIFIER, "expected a decorator name after '@'");
	name = parser->previous;

	arg = NULL;
	if (parser_match(parser, TOKEN_LPAREN)) {
		parser_consume(parser, TOKEN_STRING_LITERAL, "expected a string literal argument");
		arg = ast_new_literal(AST_STRING_LITERAL, parser->previous, parser->previous.line, parser->previous.column);
		parser_consume(parser, TOKEN_RPAREN, "expected ')' after decorator argument");
	}

	return ast_new_decorator(name, arg, line, column);
}

/* `struct Name { type field (= expr)? ; ... }`. A field with no
   default must appear in every struct literal. A field with a default
   may be left out. */
ast_node_t *parse_struct_decl(parser_t *parser) {
	token_t name;
	ast_node_t *node;
	type_ref_t field_type;
	token_t field_name;
	ast_node_t *default_value;
	ast_node_t *field;
	int line;
	int column;
	int field_line;
	int field_column;

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);

	parser_consume(parser, TOKEN_IDENTIFIER, "expected a struct name");
	name = parser->previous;

	node = ast_new_struct_decl(name, line, column);

	parser_consume(parser, TOKEN_LBRACE, "expected '{' after struct name");

	while (!parser_check(parser, TOKEN_RBRACE) && !parser_check(parser, TOKEN_EOF)) {
		if (parser_check(parser, TOKEN_FN) && !parser_check_fn_is_type(parser)) {
			ast_list_push(&node->as.struct_decl.functions, parse_function_decl(parser));
			continue;
		}

		if (!parser_check_type_ref_start(parser)) {
			parser_error_at_current(parser, "parser::expected-field-type", "expected a field type");
			break;
		}

		field_line = parser->current.line;
		field_column = parser->current.column;
		field_type = parse_type_ref(parser);

		parser_consume(parser, TOKEN_IDENTIFIER, "expected a field name");
		field_name = parser->previous;

		default_value = NULL;
		if (parser_match(parser, TOKEN_EQUAL)) {
			default_value = parse_expression(parser);
		}

		parser_consume(parser, TOKEN_SEMICOLON, "expected ';' after struct field");

		field = ast_new_param(field_type, field_name, default_value, field_line, field_column);
		ast_list_push(&node->as.struct_decl.fields, field);
	}

	parser_consume(parser, TOKEN_RBRACE, "expected '}' to close struct");

	return node;
}
