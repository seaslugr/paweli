#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "parser_internal.h"

static ast_node_t *parse_struct_literal(parser_t *parser, token_t type_name, bool has_type_name);
static void parse_catch_clause(parser_t *parser, ast_node_t *try_node);
static bool check_compound_assign(parser_t *parser);
static ast_node_t *parse_assignment(parser_t *parser);
static ast_node_t *parse_or(parser_t *parser);
static ast_node_t *parse_and(parser_t *parser);
static ast_node_t *parse_bitor(parser_t *parser);
static ast_node_t *parse_bitxor(parser_t *parser);
static ast_node_t *parse_bitand(parser_t *parser);
static ast_node_t *parse_equality(parser_t *parser);
static ast_node_t *parse_comparison(parser_t *parser);
static ast_node_t *parse_shift(parser_t *parser);
static ast_node_t *parse_term(parser_t *parser);
static ast_node_t *parse_factor(parser_t *parser);
static ast_node_t *parse_unary(parser_t *parser);
static ast_node_t *parse_call(parser_t *parser);
static ast_node_t *finish_call(parser_t *parser, ast_node_t *callee);
static ast_node_t *parse_lambda_expr(parser_t *parser);

/* `TypeName { field: value, ... }`. parse_primary() calls this after
   it consumed an identifier and saw '{'. That sequence can only be a
   struct literal, since every expression statement ends in ';'. It
   also parses the bare form `{ field: value, ... }`, with
   has_type_name false. The compiler then infers the type from context,
   as it does for an empty `[]`. */
static ast_node_t *parse_struct_literal(parser_t *parser, token_t type_name, bool has_type_name) {
	ast_node_t *node;
	token_t field_name;
	ast_node_t *value;
	ast_node_t *field_init;

	node = ast_new_struct_literal(type_name, has_type_name, has_type_name ? type_name.line : parser->current.line, has_type_name ? type_name.column : parser->current.column);

	parser_consume(parser, TOKEN_LBRACE, "expected '{'");

	if (!parser_check(parser, TOKEN_RBRACE)) {
		do {
			parser_consume(parser, TOKEN_IDENTIFIER, "expected a field name");
			field_name = parser->previous;
			parser_consume(parser, TOKEN_COLON, "expected ':' after field name");
			value = parse_expression(parser);
			field_init = ast_new_struct_field_init(field_name, value, field_name.line, field_name.column);
			ast_list_push(&node->as.struct_literal.fields, field_init);
		} while (parser_match(parser, TOKEN_COMMA));
	}

	parser_consume(parser, TOKEN_RBRACE, "expected '}' to close struct literal");

	return node;
}

/* Parses the `(...)` after `catch` and its optional `{ ... }` body
   into try_node. The current token must be TOKEN_CATCH. The grammar is
   `catch(TYPE X [: TYPE fallback]) { body }?`. If X is a bare
   identifier it declares a binding, as in `catch(string message)`.
   Otherwise it is a fallback expression, as in `catch(int 0)`. A
   clause with a binding and no fallback must have a body. A clause
   with a fallback may leave the body out. */
static void parse_catch_clause(parser_t *parser, ast_node_t *try_node) {
	type_ref_t type;
	lexer_t lookahead;
	token_t next;
	bool is_binding;

	parser_advance(parser);
	parser_consume(parser, TOKEN_LPAREN, "expected '(' after 'catch'");

	if (!parser_check_type_ref_start(parser)) {
		parser_error_at_current(parser, "parser::expected-type", "expected a type after 'catch('");
	} else {
		type = parse_type_ref(parser);

		is_binding = false;
		if (parser_check(parser, TOKEN_IDENTIFIER)) {
			lookahead = parser->lexer;
			next = lexer_next_token(&lookahead);
			is_binding = next.type == TOKEN_RPAREN || next.type == TOKEN_COLON;
		}

		if (is_binding) {
			parser_advance(parser);
			try_node->as.try_expr.has_binding = true;
			try_node->as.try_expr.binding_type = type;
			try_node->as.try_expr.binding_name = parser->previous;

			if (parser_match(parser, TOKEN_COLON)) {
				if (!parser_check_type_ref_start(parser)) {
					parser_error_at_current(parser, "parser::expected-type", "expected a type after ':'");
				} else {
					try_node->as.try_expr.has_fallback = true;
					try_node->as.try_expr.fallback_type = parse_type_ref(parser);
					try_node->as.try_expr.fallback_expr = parse_expression(parser);
				}
			}
		} else {
			try_node->as.try_expr.has_fallback = true;
			try_node->as.try_expr.fallback_type = type;
			try_node->as.try_expr.fallback_expr = parse_expression(parser);
		}
	}

	parser_consume(parser, TOKEN_RPAREN, "expected ')' after catch clause");

	if (parser_check(parser, TOKEN_LBRACE)) {
		try_node->as.try_expr.catch_body = parse_block(parser);
	} else if (!try_node->as.try_expr.has_fallback) {
		parser_error_at_current(parser, "parser::expected-catch-body", "a catch with only a binding needs a '{ ... }' body");
	}
}

/* Like parse_block(), for a try block used as an expression. A keyword
   statement parses as usual, with its ';'. A bare expression right
   before the closing '}' with no ';' becomes the block's value, as an
   implicit `return`. This is the only place in the grammar where a ';'
   is optional. */
ast_node_t *parse_try_value_block(parser_t *parser) {
	ast_node_t *block;
	ast_node_t *stmt;
	ast_node_t *expr;

	block = ast_new_block(parser->current.line, parser->current.column);

	parser_consume(parser, TOKEN_LBRACE, "expected '{' to begin a try block");

	while (!parser_check(parser, TOKEN_RBRACE) && !parser_check(parser, TOKEN_EOF)) {
		if (parser->panic_mode) {
			parser_synchronize(parser);
			continue;
		}

		if (parser_starts_keyword_statement(parser)) {
			stmt = parse_statement(parser);
			ast_list_push(&block->as.block.statements, stmt);
			continue;
		}

		expr = parse_expression(parser);

		if (parser_check(parser, TOKEN_RBRACE)) {
			stmt = ast_new_return_stmt(expr, expr->line, expr->column);
			ast_list_push(&block->as.block.statements, stmt);
			break;
		}

		parser_consume(parser, TOKEN_SEMICOLON, "expected ';' after expression statement");
		stmt = ast_new_expr_stmt(expr, expr->line, expr->column);
		ast_list_push(&block->as.block.statements, stmt);
	}

	parser_consume(parser, TOKEN_RBRACE, "expected '}' to close a try block");

	return block;
}

/* `try { ... } catch(...) { ... }`. require_value is false when called
   from parse_statement(): the block is ordinary and its value is
   discarded. It is true when called from parse_primary(): the block
   uses the trailing-expression rule and a fallback is required, since
   both paths must produce a value. The postfix form
   `expr catch(TYPE fallback)` does not come through here.
   parse_call() builds its node and calls parse_catch_clause(). */
ast_node_t *parse_try_expr(parser_t *parser, bool require_value) {
	int line;
	int column;
	ast_node_t *try_body;
	ast_node_t *try_node;

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);

	try_body = require_value ? parse_try_value_block(parser) : parse_block(parser);
	try_node = ast_new_try_expr(try_body, line, column);

	if (!parser_check(parser, TOKEN_CATCH)) {
		parser_error_at_current(parser, "parser::expected-catch", "expected 'catch' after a try block");
		return try_node;
	}

	parse_catch_clause(parser, try_node);

	if (require_value && !try_node->as.try_expr.has_fallback) {
		parser_error_at_current(parser, "parser::try-expression-needs-fallback", "a try used as an expression needs 'catch(... : TYPE fallback)' to provide a value if it fails");
	}

	return try_node;
}

ast_node_t *parse_expression(parser_t *parser) {
	return parse_assignment(parser);
}

static bool check_compound_assign(parser_t *parser) {
	switch (parser->current.type) {
		case TOKEN_PLUS_EQUAL:
		case TOKEN_MINUS_EQUAL:
		case TOKEN_STAR_EQUAL:
		case TOKEN_SLASH_EQUAL:
		case TOKEN_PERCENT_EQUAL:
		case TOKEN_AMP_EQUAL:
		case TOKEN_PIPE_EQUAL:
		case TOKEN_CARET_EQUAL:
		case TOKEN_LESS_LESS_EQUAL:
		case TOKEN_GREATER_GREATER_EQUAL:
			return true;
		default:
			return false;
	}
}

static ast_node_t *parse_assignment(parser_t *parser) {
	ast_node_t *expr;
	ast_node_t *value;
	token_t equals;

	expr = parse_or(parser);

	if (parser_check(parser, TOKEN_EQUAL) || check_compound_assign(parser)) {
		equals = parser->current;
		parser_advance(parser);

		value = parse_assignment(parser);

		if (expr->kind != AST_IDENTIFIER_EXPR && expr->kind != AST_MEMBER_EXPR && expr->kind != AST_INDEX_EXPR) {
			parser_error_at(parser, equals, "parser::invalid-assignment-target", "invalid assignment target");
			ast_free(value);
			return expr;
		}

		return ast_new_assign_expr(expr, value, equals.type, equals.line, equals.column);
	}

	return expr;
}

static ast_node_t *parse_or(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_and(parser);

	while (parser_check(parser, TOKEN_PIPE_PIPE)) {
		op = parser->current;
		parser_advance(parser);
		right = parse_and(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

static ast_node_t *parse_and(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_bitor(parser);

	while (parser_check(parser, TOKEN_AMP_AMP)) {
		op = parser->current;
		parser_advance(parser);
		right = parse_bitor(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

/* Bitwise `|`, `^`, `&`, from loosest to tightest, as in C. All three
   bind looser than equality, so `a & b == c` parses as
   `a & (b == c)`. */
static ast_node_t *parse_bitor(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_bitxor(parser);

	while (parser_check(parser, TOKEN_PIPE)) {
		op = parser->current;
		parser_advance(parser);
		right = parse_bitxor(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

static ast_node_t *parse_bitxor(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_bitand(parser);

	while (parser_check(parser, TOKEN_CARET)) {
		op = parser->current;
		parser_advance(parser);
		right = parse_bitand(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

static ast_node_t *parse_bitand(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_equality(parser);

	while (parser_check(parser, TOKEN_AMP)) {
		op = parser->current;
		parser_advance(parser);
		right = parse_equality(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

static ast_node_t *parse_equality(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_comparison(parser);

	while (parser_check(parser, TOKEN_EQUAL_EQUAL) || parser_check(parser, TOKEN_BANG_EQUAL)) {
		op = parser->current;
		parser_advance(parser);
		right = parse_comparison(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

static ast_node_t *parse_comparison(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_shift(parser);

	while (parser_check(parser, TOKEN_LESS) || parser_check(parser, TOKEN_LESS_EQUAL) || parser_check(parser, TOKEN_GREATER) || parser_check(parser, TOKEN_GREATER_EQUAL)) {
		op = parser->current;
		parser_advance(parser);
		right = parse_shift(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

static ast_node_t *parse_shift(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_term(parser);

	while (parser_check(parser, TOKEN_LESS_LESS) || parser_check(parser, TOKEN_GREATER_GREATER)) {
		op = parser->current;
		parser_advance(parser);
		right = parse_term(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

static ast_node_t *parse_term(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_factor(parser);

	while (parser_check(parser, TOKEN_PLUS) || parser_check(parser, TOKEN_MINUS)) {
		op = parser->current;
		parser_advance(parser);
		right = parse_factor(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

static ast_node_t *parse_factor(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_unary(parser);

	while (parser_check(parser, TOKEN_STAR) || parser_check(parser, TOKEN_SLASH) || parser_check(parser, TOKEN_PERCENT)) {
		op = parser->current;
		parser_advance(parser);
		right = parse_unary(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

static ast_node_t *parse_unary(parser_t *parser) {
	token_t op;
	ast_node_t *operand;

	if (parser_check(parser, TOKEN_BANG) || parser_check(parser, TOKEN_MINUS) || parser_check(parser, TOKEN_TILDE)) {
		op = parser->current;
		parser_advance(parser);
		operand = parse_unary(parser);
		return ast_new_unary_expr(op.type, operand, op.line, op.column);
	}

	return parse_call(parser);
}

static ast_node_t *parse_call(parser_t *parser) {
	ast_node_t *expr;
	token_t name;
	int line;
	int column;

	expr = parse_primary(parser);

	for (;;) {
		if (parser_match(parser, TOKEN_LPAREN)) {
			expr = finish_call(parser, expr);
		} else if (parser_check(parser, TOKEN_DOT)) {
			line = parser->current.line;
			column = parser->current.column;
			parser_advance(parser);
			parser_consume(parser, TOKEN_IDENTIFIER, "expected a property name after '.'");
			name = parser->previous;
			expr = ast_new_member_expr(expr, name, line, column);
		} else if (parser_check(parser, TOKEN_LBRACKET)) {
			ast_node_t *index;

			line = parser->current.line;
			column = parser->current.column;
			parser_advance(parser);
			index = parse_expression(parser);
			parser_consume(parser, TOKEN_RBRACKET, "expected ']' after array index");
			expr = ast_new_index_expr(expr, index, line, column);
		} else if (parser_check(parser, TOKEN_CATCH)) {
			// `expr catch(TYPE fallback)` is sugar. It builds a try block with one
			// statement, `return expr;`, so the compiler sees one AST_TRY_EXPR
			// shape.
			ast_node_t *try_body;
			ast_node_t *return_stmt;
			ast_node_t *try_node;

			try_body = ast_new_block(expr->line, expr->column);
			return_stmt = ast_new_return_stmt(expr, expr->line, expr->column);
			ast_list_push(&try_body->as.block.statements, return_stmt);

			try_node = ast_new_try_expr(try_body, parser->current.line, parser->current.column);
			parse_catch_clause(parser, try_node);
			expr = try_node;
		} else {
			break;
		}
	}

	return expr;
}

static ast_node_t *finish_call(parser_t *parser, ast_node_t *callee) {
	ast_node_t *call;
	ast_node_t *arg;
	int line;
	int column;

	line = parser->previous.line;
	column = parser->previous.column;

	call = ast_new_call_expr(callee, line, column);

	if (!parser_check(parser, TOKEN_RPAREN)) {
		do {
			arg = parse_expression(parser);
			ast_list_push(&call->as.call_expr.args, arg);
		} while (parser_match(parser, TOKEN_COMMA));
	}

	parser_consume(parser, TOKEN_RPAREN, "expected ')' after arguments");

	return call;
}

/* `fn(TYPE NAME, ...) -> TYPE { block }`, a lambda. It has the shape
   of parse_function_decl() with no name to consume. parse_primary()
   checked TOKEN_FN and did not consume it. The AST_FUNCTION_DECL node
   gets an empty name. */
static ast_node_t *parse_lambda_expr(parser_t *parser) {
	token_t fn_token;
	token_t empty_name;
	ast_node_t *lambda;
	bool has_return_type;
	type_ref_t return_type;

	fn_token = parser->current;
	parser_advance(parser);
	parser_consume(parser, TOKEN_LPAREN, "expected '(' after 'fn'");

	empty_name = parser_synthetic_token(TOKEN_FN, "", fn_token.line, fn_token.column);
	lambda = ast_new_function_decl(empty_name, false, (type_ref_t){TOKEN_EOF, false, {0}, NULL}, NULL, fn_token.line, fn_token.column);

	if (!parser_check(parser, TOKEN_RPAREN)) {
		parse_params(parser, lambda);
	}
	parser_consume(parser, TOKEN_RPAREN, "expected ')' after lambda parameters");

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
	lambda->as.function_decl.has_return_type = has_return_type;
	lambda->as.function_decl.return_type = return_type;

	lambda->as.function_decl.body = parse_block(parser);

	return lambda;
}

ast_node_t *parse_primary(parser_t *parser) {
	ast_node_t *expr;
	token_t token;

	if (parser_match(parser, TOKEN_INT_LITERAL)) {
		return ast_new_literal(AST_INT_LITERAL, parser->previous, parser->previous.line, parser->previous.column);
	}
	if (parser_match(parser, TOKEN_FLOAT_LITERAL)) {
		return ast_new_literal(AST_FLOAT_LITERAL, parser->previous, parser->previous.line, parser->previous.column);
	}
	if (parser_match(parser, TOKEN_STRING_LITERAL)) {
		return ast_new_literal(AST_STRING_LITERAL, parser->previous, parser->previous.line, parser->previous.column);
	}
	if (parser_check(parser, TOKEN_TRUE) || parser_check(parser, TOKEN_FALSE)) {
		token = parser->current;
		parser_advance(parser);
		return ast_new_literal(AST_BOOL_LITERAL, token, token.line, token.column);
	}
	if (parser_match(parser, TOKEN_NULL)) {
		return ast_new_literal(AST_NULL_LITERAL, parser->previous, parser->previous.line, parser->previous.column);
	}
	if (parser_match(parser, TOKEN_IDENTIFIER)) {
		token_t name = parser->previous;

		if (parser_check(parser, TOKEN_LBRACE)) {
			return parse_struct_literal(parser, name, true);
		}
		return ast_new_identifier_expr(name, name.line, name.column);
	}
	if (parser_match(parser, TOKEN_LPAREN)) {
		expr = parse_expression(parser);
		parser_consume(parser, TOKEN_RPAREN, "expected ')' after expression");
		return expr;
	}
	if (parser_check(parser, TOKEN_LBRACE)) {
		token_t empty_name = parser_synthetic_token(TOKEN_LBRACE, "", parser->current.line, parser->current.column);
		return parse_struct_literal(parser, empty_name, false);
	}
	if (parser_check(parser, TOKEN_LBRACKET)) {
		ast_node_t *array;
		ast_node_t *element;

		array = ast_new_array_literal(parser->current.line, parser->current.column);
		parser_advance(parser);

		if (!parser_check(parser, TOKEN_RBRACKET)) {
			do {
				element = parse_expression(parser);
				ast_list_push(&array->as.array_literal.elements, element);
			} while (parser_match(parser, TOKEN_COMMA));
		}

		parser_consume(parser, TOKEN_RBRACKET, "expected ']' after array elements");
		return array;
	}
	if (parser_check(parser, TOKEN_FN)) {
		return parse_lambda_expr(parser);
	}
	if (parser_check(parser, TOKEN_TRY)) {
		return parse_try_expr(parser, true);
	}

	parser_error_at_current(parser, "parser::expected-expression", "expected an expression");
	parser_advance(parser);
	return ast_new_literal(AST_NULL_LITERAL, parser->previous, parser->previous.line, parser->previous.column);
}
