#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "parser_internal.h"

static ast_node_t *parse_export_stmt(parser_t *parser);
static ast_node_t *parse_if_stmt(parser_t *parser);
static ast_node_t *parse_while_stmt(parser_t *parser);
static ast_node_t *parse_typed_for_stmt(parser_t *parser, int line, int column);
static ast_node_t *parse_break_stmt(parser_t *parser);
static ast_node_t *parse_continue_stmt(parser_t *parser);
static ast_node_t *parse_return_stmt(parser_t *parser);
static ast_node_t *parse_expr_stmt(parser_t *parser);
static ast_node_t *parse_throw_stmt(parser_t *parser);

ast_node_t *parse_block(parser_t *parser) {
	ast_node_t *block;
	ast_node_t *stmt;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	parser_consume(parser, TOKEN_LBRACE, "expected '{' to start a block");

	block = ast_new_block(line, column);

	while (!parser_check(parser, TOKEN_RBRACE) && !parser_check(parser, TOKEN_EOF)) {
		stmt = parse_statement(parser);
		if (stmt != NULL) {
			ast_list_push(&block->as.block.statements, stmt);
		}
		if (parser->panic_mode) {
			parser_synchronize(parser);
		}
	}

	parser_consume(parser, TOKEN_RBRACE, "expected '}' to close a block");
	return block;
}

/* True at a token that parse_statement() gives its own parse function.
   parse_try_value_block() needs the same test: in a try block used as
   an expression, a keyword statement parses as usual, with its ';'.
   Only a bare expression can be the block's value. */
bool parser_starts_keyword_statement(parser_t *parser) {
	return parser_check_var_decl_start(parser)
		|| parser_check(parser, TOKEN_FN)
		|| parser_check(parser, TOKEN_EXPORT)
		|| parser_check(parser, TOKEN_IF)
		|| parser_check(parser, TOKEN_WHILE)
		|| parser_check(parser, TOKEN_FOR)
		|| parser_check(parser, TOKEN_BREAK)
		|| parser_check(parser, TOKEN_CONTINUE)
		|| parser_check(parser, TOKEN_RETURN)
		|| parser_check(parser, TOKEN_THROW)
		|| parser_check(parser, TOKEN_TRY)
		|| parser_check(parser, TOKEN_LBRACE);
}

ast_node_t *parse_statement(parser_t *parser) {
	if (parser_check_var_decl_start(parser)) {
		return parse_var_decl(parser);
	}
	if (parser_check(parser, TOKEN_FN)) {
		return parse_function_decl(parser);
	}
	if (parser_check(parser, TOKEN_EXPORT)) {
		return parse_export_stmt(parser);
	}
	if (parser_check(parser, TOKEN_IF)) {
		return parse_if_stmt(parser);
	}
	if (parser_check(parser, TOKEN_WHILE)) {
		return parse_while_stmt(parser);
	}
	if (parser_check(parser, TOKEN_FOR)) {
		return parse_for_stmt(parser);
	}
	if (parser_check(parser, TOKEN_BREAK)) {
		return parse_break_stmt(parser);
	}
	if (parser_check(parser, TOKEN_CONTINUE)) {
		return parse_continue_stmt(parser);
	}
	if (parser_check(parser, TOKEN_RETURN)) {
		return parse_return_stmt(parser);
	}
	if (parser_check(parser, TOKEN_THROW)) {
		return parse_throw_stmt(parser);
	}
	if (parser_check(parser, TOKEN_TRY)) {
		return parse_try_expr(parser, false);
	}
	if (parser_check(parser, TOKEN_LBRACE)) {
		return parse_block(parser);
	}
	return parse_expr_stmt(parser);
}

ast_node_t *parse_var_decl(parser_t *parser) {
	type_ref_t type;
	token_t name;
	ast_node_t *initializer;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	type = parse_type_ref(parser);

	parser_consume(parser, TOKEN_IDENTIFIER, "expected a variable name");
	name = parser->previous;

	parser_consume(parser, TOKEN_EQUAL, "expected '=' after variable name");
	initializer = parse_expression(parser);

	parser_consume(parser, TOKEN_SEMICOLON, "expected ';' after variable declaration");

	return ast_new_var_decl(type, name, initializer, line, column);
}

static ast_node_t *parse_export_stmt(parser_t *parser) {
	token_t name;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);

	parser_consume(parser, TOKEN_IDENTIFIER, "expected a name after 'export'");
	name = parser->previous;

	parser_consume(parser, TOKEN_SEMICOLON, "expected ';' after export statement");

	return ast_new_export_stmt(name, line, column);
}

static ast_node_t *parse_if_stmt(parser_t *parser) {
	ast_node_t *condition;
	ast_node_t *then_branch;
	ast_node_t *else_branch;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);

	parser_consume(parser, TOKEN_LPAREN, "expected '(' after 'if'");
	condition = parse_expression(parser);
	parser_consume(parser, TOKEN_RPAREN, "expected ')' after if condition");

	then_branch = parse_block(parser);

	else_branch = NULL;
	if (parser_match(parser, TOKEN_ELSE)) {
		if (parser_check(parser, TOKEN_IF)) {
			else_branch = parse_if_stmt(parser);
		} else {
			else_branch = parse_block(parser);
		}
	}

	return ast_new_if_stmt(condition, then_branch, else_branch, line, column);
}

static ast_node_t *parse_while_stmt(parser_t *parser) {
	ast_node_t *condition;
	ast_node_t *body;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);

	parser_consume(parser, TOKEN_LPAREN, "expected '(' after 'while'");
	condition = parse_expression(parser);
	parser_consume(parser, TOKEN_RPAREN, "expected ')' after while condition");

	body = parse_block(parser);

	return ast_new_while_stmt(condition, body, line, column);
}

ast_node_t *parse_for_stmt(parser_t *parser) {
	ast_node_t *initializer;
	ast_node_t *condition;
	ast_node_t *increment;
	ast_node_t *body;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);

	parser_consume(parser, TOKEN_LPAREN, "expected '(' after 'for'");

	// A declared type here can start the three-clause form
	// (`for (int i = 0; ...)`) or the range and foreach forms
	// (`for (int i : 0..10)`, `for (int x : nums)`). They only differ
	// after the variable name, so parse_typed_for_stmt() handles all
	// three.
	if (parser_check_var_decl_start(parser)) {
		return parse_typed_for_stmt(parser, line, column);
	}

	if (parser_match(parser, TOKEN_SEMICOLON)) {
		initializer = NULL;
	} else {
		initializer = parse_expr_stmt(parser);
	}

	condition = NULL;
	if (!parser_check(parser, TOKEN_SEMICOLON)) {
		condition = parse_expression(parser);
	}
	parser_consume(parser, TOKEN_SEMICOLON, "expected ';' after for-loop condition");

	increment = NULL;
	if (!parser_check(parser, TOKEN_RPAREN)) {
		increment = parse_expression(parser);
	}
	parser_consume(parser, TOKEN_RPAREN, "expected ')' after for-loop clauses");

	body = parse_block(parser);

	return ast_new_for_stmt(initializer, condition, increment, body, line, column);
}

/* Parses every `for` form that starts with a declared type. The
   three-clause form is `for (T name = init; cond; incr)`. The range
   form `for (T name : start..end)` is rewritten here into the AST of a
   three-clause loop with a hidden end variable. The array and string
   form `for (T name : expr)` becomes an AST_FOREACH_STMT, because the
   element type of expr is only known to the compiler (see
   compile_foreach_stmt()). */
static ast_node_t *parse_typed_for_stmt(parser_t *parser, int line, int column) {
	type_ref_t type;
	token_t name;
	int decl_line;
	int decl_column;
	ast_node_t *initializer;
	ast_node_t *condition;
	ast_node_t *increment;
	ast_node_t *body;

	decl_line = parser->current.line;
	decl_column = parser->current.column;
	type = parse_type_ref(parser);

	parser_consume(parser, TOKEN_IDENTIFIER, "expected a variable name");
	name = parser->previous;

	if (parser_match(parser, TOKEN_COLON)) {
		ast_node_t *first_expr;

		first_expr = parse_expression(parser);

		if (parser_match(parser, TOKEN_DOT_DOT)) {
			ast_node_t *end_expr;
			ast_node_t *range_end_decl;
			ast_node_t *for_node;
			ast_node_t *wrapper;
			token_t range_end_name;
			token_t one_literal;

			end_expr = parse_expression(parser);
			parser_consume(parser, TOKEN_RPAREN, "expected ')' after for-loop range");
			body = parse_block(parser);

			// { T $for_range_end = end; for (T name = start; name <= $for_range_end; name = name + 1) body }
			range_end_name = parser_synthetic_token(TOKEN_IDENTIFIER, "$for_range_end", decl_line, decl_column);
			one_literal = parser_synthetic_token(TOKEN_INT_LITERAL, "1", decl_line, decl_column);

			range_end_decl = ast_new_var_decl(type, range_end_name, end_expr, decl_line, decl_column);
			initializer = ast_new_var_decl(type, name, first_expr, decl_line, decl_column);
			condition = ast_new_binary_expr(TOKEN_LESS_EQUAL, ast_new_identifier_expr(name, decl_line, decl_column), ast_new_identifier_expr(range_end_name, decl_line, decl_column), decl_line, decl_column);
			increment = ast_new_assign_expr(
				ast_new_identifier_expr(name, decl_line, decl_column),
				ast_new_binary_expr(TOKEN_PLUS, ast_new_identifier_expr(name, decl_line, decl_column), ast_new_literal(AST_INT_LITERAL, one_literal, decl_line, decl_column), decl_line, decl_column),
				TOKEN_EQUAL, decl_line, decl_column);

			for_node = ast_new_for_stmt(initializer, condition, increment, body, line, column);

			wrapper = ast_new_block(line, column);
			ast_list_push(&wrapper->as.block.statements, range_end_decl);
			ast_list_push(&wrapper->as.block.statements, for_node);

			return wrapper;
		}

		parser_consume(parser, TOKEN_RPAREN, "expected ')' after for-loop array expression");
		body = parse_block(parser);

		return ast_new_foreach_stmt(type, name, first_expr, body, line, column);
	}

	parser_consume(parser, TOKEN_EQUAL, "expected '=' or ':' after for-loop variable name");
	initializer = ast_new_var_decl(type, name, parse_expression(parser), decl_line, decl_column);
	parser_consume(parser, TOKEN_SEMICOLON, "expected ';' after variable declaration");

	condition = NULL;
	if (!parser_check(parser, TOKEN_SEMICOLON)) {
		condition = parse_expression(parser);
	}
	parser_consume(parser, TOKEN_SEMICOLON, "expected ';' after for-loop condition");

	increment = NULL;
	if (!parser_check(parser, TOKEN_RPAREN)) {
		increment = parse_expression(parser);
	}
	parser_consume(parser, TOKEN_RPAREN, "expected ')' after for-loop clauses");

	body = parse_block(parser);

	return ast_new_for_stmt(initializer, condition, increment, body, line, column);
}

static ast_node_t *parse_break_stmt(parser_t *parser) {
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);

	parser_consume(parser, TOKEN_SEMICOLON, "expected ';' after 'break'");

	return ast_new_break_stmt(line, column);
}

static ast_node_t *parse_continue_stmt(parser_t *parser) {
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);

	parser_consume(parser, TOKEN_SEMICOLON, "expected ';' after 'continue'");

	return ast_new_continue_stmt(line, column);
}

static ast_node_t *parse_return_stmt(parser_t *parser) {
	ast_node_t *value;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);

	value = NULL;
	if (!parser_check(parser, TOKEN_SEMICOLON)) {
		value = parse_expression(parser);
	}

	parser_consume(parser, TOKEN_SEMICOLON, "expected ';' after return statement");

	return ast_new_return_stmt(value, line, column);
}

static ast_node_t *parse_expr_stmt(parser_t *parser) {
	ast_node_t *expr;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;

	expr = parse_expression(parser);
	parser_consume(parser, TOKEN_SEMICOLON, "expected ';' after expression");

	return ast_new_expr_stmt(expr, line, column);
}

static ast_node_t *parse_throw_stmt(parser_t *parser) {
	ast_node_t *value;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);

	value = parse_expression(parser);
	parser_consume(parser, TOKEN_SEMICOLON, "expected ';' after 'throw'");

	return ast_new_throw_stmt(value, line, column);
}
