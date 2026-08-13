#include <stdbool.h>

#include "diagnostics/diagnostic.h"
#include "parser.h"

static void error_at(parser_t *parser, token_t token, const char *code, const char *message);
static void error_at_current(parser_t *parser, const char *code, const char *message);
static void advance(parser_t *parser);
static bool check(parser_t *parser, token_type_t type);
static bool match(parser_t *parser, token_type_t type);
static void consume(parser_t *parser, token_type_t type, const char *message);
static bool check_type_keyword(parser_t *parser);
static void synchronize(parser_t *parser);

static ast_node_t *parse_directive(parser_t *parser);
static ast_node_t *parse_function_decl(parser_t *parser);
static void parse_params(parser_t *parser, ast_node_t *fn_node);
static ast_node_t *parse_block(parser_t *parser);
static ast_node_t *parse_statement(parser_t *parser);
static ast_node_t *parse_var_decl(parser_t *parser);
static ast_node_t *parse_export_stmt(parser_t *parser);
static ast_node_t *parse_if_stmt(parser_t *parser);
static ast_node_t *parse_while_stmt(parser_t *parser);
static ast_node_t *parse_for_stmt(parser_t *parser);
static ast_node_t *parse_return_stmt(parser_t *parser);
static ast_node_t *parse_expr_stmt(parser_t *parser);

static ast_node_t *parse_expression(parser_t *parser);
static ast_node_t *parse_assignment(parser_t *parser);
static ast_node_t *parse_or(parser_t *parser);
static ast_node_t *parse_and(parser_t *parser);
static ast_node_t *parse_equality(parser_t *parser);
static ast_node_t *parse_comparison(parser_t *parser);
static ast_node_t *parse_term(parser_t *parser);
static ast_node_t *parse_factor(parser_t *parser);
static ast_node_t *parse_unary(parser_t *parser);
static ast_node_t *parse_call(parser_t *parser);
static ast_node_t *finish_call(parser_t *parser, ast_node_t *callee);
static ast_node_t *parse_primary(parser_t *parser);

static void error_at(parser_t *parser, token_t token, const char *code, const char *message) {
	const char *actual_code;

	if (parser->panic_mode) {
		return;
	}
	parser->panic_mode = true;
	parser->had_error = true;

	/* A TOKEN_ERROR came from the lexer, not from a parser expectation
	   that failed, so it carries its own "lexer::*" code and its message
	   text lives in token.start/length rather than in `message`. */
	actual_code = (token.type == TOKEN_ERROR && token.error_code != NULL) ? token.error_code : code;

	if (token.type == TOKEN_EOF) {
		diagnostic_emit(DIAG_ERROR, actual_code, parser->filename, parser->source, token.line, token.column, "at end of file: %s", message);
	} else if (token.type == TOKEN_ERROR) {
		diagnostic_emit(DIAG_ERROR, actual_code, parser->filename, parser->source, token.line, token.column, "%.*s", (int)token.length, token.start);
	} else {
		diagnostic_emit(DIAG_ERROR, actual_code, parser->filename, parser->source, token.line, token.column, "at '%.*s': %s", (int)token.length, token.start, message);
	}
}

static void error_at_current(parser_t *parser, const char *code, const char *message) {
	error_at(parser, parser->current, code, message);
}

static void advance(parser_t *parser) {
	parser->previous = parser->current;

	for (;;) {
		parser->current = lexer_next_token(&parser->lexer);
		if (parser->current.type != TOKEN_ERROR) {
			break;
		}
		error_at_current(parser, NULL, "");
	}
}

static bool check(parser_t *parser, token_type_t type) {
	return parser->current.type == type;
}

static bool match(parser_t *parser, token_type_t type) {
	if (!check(parser, type)) {
		return false;
	}
	advance(parser);
	return true;
}

static void consume(parser_t *parser, token_type_t type, const char *message) {
	if (check(parser, type)) {
		advance(parser);
		return;
	}
	error_at_current(parser, "parser::expected-token", message);
}

static bool check_type_keyword(parser_t *parser) {
	return check(parser, TOKEN_INT) || check(parser, TOKEN_FLOAT) || check(parser, TOKEN_STRING) || check(parser, TOKEN_BOOL);
}

/* Skips tokens until it reaches the likely start of the next statement or
   declaration. This stops one syntax error from cascading into a wall
   of follow-on errors. */
static void synchronize(parser_t *parser) {
	parser->panic_mode = false;

	while (!check(parser, TOKEN_EOF)) {
		if (parser->previous.type == TOKEN_SEMICOLON) {
			return;
		}

		switch (parser->current.type) {
			case TOKEN_FN:
			case TOKEN_INT:
			case TOKEN_FLOAT:
			case TOKEN_STRING:
			case TOKEN_BOOL:
			case TOKEN_IF:
			case TOKEN_WHILE:
			case TOKEN_FOR:
			case TOKEN_RETURN:
			case TOKEN_EXPORT:
			case TOKEN_RBRACE:
				return;
			default:
				break;
		}

		advance(parser);
	}
}

static ast_node_t *parse_directive(parser_t *parser) {
	token_t name;
	int line;
	int column;

	if (check(parser, TOKEN_MODULE)) {
		line = parser->current.line;
		column = parser->current.column;
		advance(parser);
		consume(parser, TOKEN_STRING_LITERAL, "expected a string literal after '#module'");
		name = parser->previous;
		return ast_new_module_import(name, line, column);
	}

	if (check(parser, TOKEN_MODULEDEF)) {
		line = parser->current.line;
		column = parser->current.column;
		advance(parser);
		consume(parser, TOKEN_STRING_LITERAL, "expected a string literal after '#moduledef'");
		name = parser->previous;
		return ast_new_module_def(name, line, column);
	}

	return NULL;
}

static ast_node_t *parse_function_decl(parser_t *parser) {
	token_t name;
	int line;
	int column;
	bool has_return_type;
	token_type_t return_type;
	ast_node_t *fn_node;

	if (!check(parser, TOKEN_FN)) {
		error_at_current(parser, "parser::expected-function", "expected a function declaration ('fn')");
		/* Guaranteed to consume at least this token before synchronizing:
		   this check runs before anything else has advanced past it, so if
		   it happens to already be one of synchronize()'s own stop-set
		   tokens (return, if, a type keyword, ...), synchronize() would
		   return immediately without moving forward, and the caller would
		   retry parse_function_decl() on the same token forever. */
		advance(parser);
		synchronize(parser);
		return NULL;
	}

	line = parser->current.line;
	column = parser->current.column;
	advance(parser);

	consume(parser, TOKEN_IDENTIFIER, "expected a function name");
	name = parser->previous;

	consume(parser, TOKEN_LPAREN, "expected '(' after function name");

	fn_node = ast_new_function_decl(name, false, TOKEN_EOF, NULL, line, column);

	if (!check(parser, TOKEN_RPAREN)) {
		parse_params(parser, fn_node);
	}

	consume(parser, TOKEN_RPAREN, "expected ')' after parameters");

	has_return_type = false;
	return_type = TOKEN_EOF;
	if (match(parser, TOKEN_ARROW)) {
		if (!check_type_keyword(parser)) {
			error_at_current(parser, "parser::expected-return-type", "expected a return type after '->'");
		} else {
			return_type = parser->current.type;
			has_return_type = true;
			advance(parser);
		}
	}
	fn_node->as.function_decl.has_return_type = has_return_type;
	fn_node->as.function_decl.return_type = return_type;

	fn_node->as.function_decl.body = parse_block(parser);

	return fn_node;
}

static void parse_params(parser_t *parser, ast_node_t *fn_node) {
	token_type_t type;
	token_t name;
	ast_node_t *param;
	int line;
	int column;

	for (;;) {
		if (!check_type_keyword(parser)) {
			error_at_current(parser, "parser::expected-parameter-type", "expected a parameter type");
			return;
		}

		line = parser->current.line;
		column = parser->current.column;
		type = parser->current.type;
		advance(parser);

		consume(parser, TOKEN_IDENTIFIER, "expected a parameter name");
		name = parser->previous;

		param = ast_new_param(type, name, line, column);
		ast_list_push(&fn_node->as.function_decl.params, param);

		if (!match(parser, TOKEN_COMMA)) {
			break;
		}
	}
}

static ast_node_t *parse_block(parser_t *parser) {
	ast_node_t *block;
	ast_node_t *stmt;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	consume(parser, TOKEN_LBRACE, "expected '{' to start a block");

	block = ast_new_block(line, column);

	while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
		stmt = parse_statement(parser);
		if (stmt != NULL) {
			ast_list_push(&block->as.block.statements, stmt);
		}
		if (parser->panic_mode) {
			synchronize(parser);
		}
	}

	consume(parser, TOKEN_RBRACE, "expected '}' to close a block");
	return block;
}

static ast_node_t *parse_statement(parser_t *parser) {
	if (check_type_keyword(parser)) {
		return parse_var_decl(parser);
	}
	if (check(parser, TOKEN_EXPORT)) {
		return parse_export_stmt(parser);
	}
	if (check(parser, TOKEN_IF)) {
		return parse_if_stmt(parser);
	}
	if (check(parser, TOKEN_WHILE)) {
		return parse_while_stmt(parser);
	}
	if (check(parser, TOKEN_FOR)) {
		return parse_for_stmt(parser);
	}
	if (check(parser, TOKEN_RETURN)) {
		return parse_return_stmt(parser);
	}
	if (check(parser, TOKEN_LBRACE)) {
		return parse_block(parser);
	}
	return parse_expr_stmt(parser);
}

static ast_node_t *parse_var_decl(parser_t *parser) {
	token_type_t type;
	token_t name;
	ast_node_t *initializer;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	type = parser->current.type;
	advance(parser);

	consume(parser, TOKEN_IDENTIFIER, "expected a variable name");
	name = parser->previous;

	consume(parser, TOKEN_EQUAL, "expected '=' after variable name");
	initializer = parse_expression(parser);

	consume(parser, TOKEN_SEMICOLON, "expected ';' after variable declaration");

	return ast_new_var_decl(type, name, initializer, line, column);
}

static ast_node_t *parse_export_stmt(parser_t *parser) {
	token_t name;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	advance(parser);

	consume(parser, TOKEN_IDENTIFIER, "expected a name after 'export'");
	name = parser->previous;

	consume(parser, TOKEN_SEMICOLON, "expected ';' after export statement");

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
	advance(parser);

	consume(parser, TOKEN_LPAREN, "expected '(' after 'if'");
	condition = parse_expression(parser);
	consume(parser, TOKEN_RPAREN, "expected ')' after if condition");

	then_branch = parse_block(parser);

	else_branch = NULL;
	if (match(parser, TOKEN_ELSE)) {
		if (check(parser, TOKEN_IF)) {
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
	advance(parser);

	consume(parser, TOKEN_LPAREN, "expected '(' after 'while'");
	condition = parse_expression(parser);
	consume(parser, TOKEN_RPAREN, "expected ')' after while condition");

	body = parse_block(parser);

	return ast_new_while_stmt(condition, body, line, column);
}

static ast_node_t *parse_for_stmt(parser_t *parser) {
	ast_node_t *initializer;
	ast_node_t *condition;
	ast_node_t *increment;
	ast_node_t *body;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	advance(parser);

	consume(parser, TOKEN_LPAREN, "expected '(' after 'for'");

	if (match(parser, TOKEN_SEMICOLON)) {
		initializer = NULL;
	} else if (check_type_keyword(parser)) {
		initializer = parse_var_decl(parser);
	} else {
		initializer = parse_expr_stmt(parser);
	}

	condition = NULL;
	if (!check(parser, TOKEN_SEMICOLON)) {
		condition = parse_expression(parser);
	}
	consume(parser, TOKEN_SEMICOLON, "expected ';' after for-loop condition");

	increment = NULL;
	if (!check(parser, TOKEN_RPAREN)) {
		increment = parse_expression(parser);
	}
	consume(parser, TOKEN_RPAREN, "expected ')' after for-loop clauses");

	body = parse_block(parser);

	return ast_new_for_stmt(initializer, condition, increment, body, line, column);
}

static ast_node_t *parse_return_stmt(parser_t *parser) {
	ast_node_t *value;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	advance(parser);

	value = NULL;
	if (!check(parser, TOKEN_SEMICOLON)) {
		value = parse_expression(parser);
	}

	consume(parser, TOKEN_SEMICOLON, "expected ';' after return statement");

	return ast_new_return_stmt(value, line, column);
}

static ast_node_t *parse_expr_stmt(parser_t *parser) {
	ast_node_t *expr;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;

	expr = parse_expression(parser);
	consume(parser, TOKEN_SEMICOLON, "expected ';' after expression");

	return ast_new_expr_stmt(expr, line, column);
}

static ast_node_t *parse_expression(parser_t *parser) {
	return parse_assignment(parser);
}

static ast_node_t *parse_assignment(parser_t *parser) {
	ast_node_t *expr;
	ast_node_t *value;
	token_t equals;

	expr = parse_or(parser);

	if (check(parser, TOKEN_EQUAL)) {
		equals = parser->current;
		advance(parser);

		value = parse_assignment(parser);

		if (expr->kind != AST_IDENTIFIER_EXPR && expr->kind != AST_MEMBER_EXPR) {
			error_at(parser, equals, "parser::invalid-assignment-target", "invalid assignment target");
			ast_free(value);
			return expr;
		}

		return ast_new_assign_expr(expr, value, equals.line, equals.column);
	}

	return expr;
}

static ast_node_t *parse_or(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_and(parser);

	while (check(parser, TOKEN_PIPE_PIPE)) {
		op = parser->current;
		advance(parser);
		right = parse_and(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

static ast_node_t *parse_and(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_equality(parser);

	while (check(parser, TOKEN_AMP_AMP)) {
		op = parser->current;
		advance(parser);
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

	while (check(parser, TOKEN_EQUAL_EQUAL) || check(parser, TOKEN_BANG_EQUAL)) {
		op = parser->current;
		advance(parser);
		right = parse_comparison(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

static ast_node_t *parse_comparison(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_term(parser);

	while (check(parser, TOKEN_LESS) || check(parser, TOKEN_LESS_EQUAL) || check(parser, TOKEN_GREATER) || check(parser, TOKEN_GREATER_EQUAL)) {
		op = parser->current;
		advance(parser);
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

	while (check(parser, TOKEN_PLUS) || check(parser, TOKEN_MINUS)) {
		op = parser->current;
		advance(parser);
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

	while (check(parser, TOKEN_STAR) || check(parser, TOKEN_SLASH) || check(parser, TOKEN_PERCENT)) {
		op = parser->current;
		advance(parser);
		right = parse_unary(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

static ast_node_t *parse_unary(parser_t *parser) {
	token_t op;
	ast_node_t *operand;

	if (check(parser, TOKEN_BANG) || check(parser, TOKEN_MINUS)) {
		op = parser->current;
		advance(parser);
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
		if (match(parser, TOKEN_LPAREN)) {
			expr = finish_call(parser, expr);
		} else if (check(parser, TOKEN_DOT)) {
			line = parser->current.line;
			column = parser->current.column;
			advance(parser);
			consume(parser, TOKEN_IDENTIFIER, "expected a property name after '.'");
			name = parser->previous;
			expr = ast_new_member_expr(expr, name, line, column);
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

	if (!check(parser, TOKEN_RPAREN)) {
		do {
			arg = parse_expression(parser);
			ast_list_push(&call->as.call_expr.args, arg);
		} while (match(parser, TOKEN_COMMA));
	}

	consume(parser, TOKEN_RPAREN, "expected ')' after arguments");

	return call;
}

static ast_node_t *parse_primary(parser_t *parser) {
	ast_node_t *expr;
	token_t token;

	if (match(parser, TOKEN_INT_LITERAL)) {
		return ast_new_literal(AST_INT_LITERAL, parser->previous, parser->previous.line, parser->previous.column);
	}
	if (match(parser, TOKEN_FLOAT_LITERAL)) {
		return ast_new_literal(AST_FLOAT_LITERAL, parser->previous, parser->previous.line, parser->previous.column);
	}
	if (match(parser, TOKEN_STRING_LITERAL)) {
		return ast_new_literal(AST_STRING_LITERAL, parser->previous, parser->previous.line, parser->previous.column);
	}
	if (check(parser, TOKEN_TRUE) || check(parser, TOKEN_FALSE)) {
		token = parser->current;
		advance(parser);
		return ast_new_literal(AST_BOOL_LITERAL, token, token.line, token.column);
	}
	if (match(parser, TOKEN_NULL)) {
		return ast_new_literal(AST_NULL_LITERAL, parser->previous, parser->previous.line, parser->previous.column);
	}
	if (match(parser, TOKEN_IDENTIFIER)) {
		return ast_new_identifier_expr(parser->previous, parser->previous.line, parser->previous.column);
	}
	if (match(parser, TOKEN_LPAREN)) {
		expr = parse_expression(parser);
		consume(parser, TOKEN_RPAREN, "expected ')' after expression");
		return expr;
	}

	error_at_current(parser, "parser::expected-expression", "expected an expression");
	advance(parser);
	return ast_new_literal(AST_NULL_LITERAL, parser->previous, parser->previous.line, parser->previous.column);
}

void parser_init(parser_t *parser, const char *source, const char *filename) {
	lexer_init(&parser->lexer, source);
	parser->filename = filename;
	parser->source = source;
	parser->had_error = false;
	parser->panic_mode = false;

	parser->current.type = TOKEN_EOF;
	parser->current.start = source;
	parser->current.length = 0;
	parser->current.line = 1;
	parser->current.column = 1;
	parser->current.error_code = NULL;
	parser->previous = parser->current;

	advance(parser);
}

ast_node_t *parser_parse_program(parser_t *parser) {
	ast_node_t *directive;
	ast_node_t *program;
	ast_node_t *function;

	directive = parse_directive(parser);
	program = ast_new_program(directive);

	while (!check(parser, TOKEN_EOF)) {
		function = parse_function_decl(parser);
		if (function != NULL) {
			ast_list_push(&program->as.program.functions, function);
		}
		if (parser->panic_mode) {
			synchronize(parser);
		}
	}

	return program;
}
