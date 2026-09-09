#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostics/diagnostic.h"
#include "parser.h"

static void error_at(parser_t *parser, token_t token, const char *code, const char *message);
static void error_at_current(parser_t *parser, const char *code, const char *message);
static void advance(parser_t *parser);
static bool check(parser_t *parser, token_type_t type);
static bool match(parser_t *parser, token_type_t type);
static void consume(parser_t *parser, token_type_t type, const char *message);
static bool check_type_keyword(parser_t *parser);
static bool check_type_ref_start(parser_t *parser);
static bool check_fn_is_type(parser_t *parser);
static bool check_var_decl_start(parser_t *parser);
static type_ref_t parse_type_ref(parser_t *parser);
static void synchronize(parser_t *parser);

static ast_node_t *parse_module_import(parser_t *parser);
static ast_node_t *parse_module_def(parser_t *parser);
static ast_node_t *parse_function_decl(parser_t *parser);
static void parse_params(parser_t *parser, ast_node_t *fn_node);
static ast_node_t *parse_struct_decl(parser_t *parser);
static ast_node_t *parse_decorator(parser_t *parser);
static ast_node_t *parse_struct_literal(parser_t *parser, token_t type_name, bool has_type_name);
static bool starts_keyword_statement(parser_t *parser);
static ast_node_t *parse_block(parser_t *parser);
static ast_node_t *parse_statement(parser_t *parser);
static ast_node_t *parse_var_decl(parser_t *parser);
static ast_node_t *parse_export_stmt(parser_t *parser);
static ast_node_t *parse_if_stmt(parser_t *parser);
static ast_node_t *parse_while_stmt(parser_t *parser);
static ast_node_t *parse_for_stmt(parser_t *parser);
static ast_node_t *parse_typed_for_stmt(parser_t *parser, int line, int column);
static token_t synthetic_token(token_type_t type, const char *text, int line, int column);
static ast_node_t *parse_break_stmt(parser_t *parser);
static ast_node_t *parse_continue_stmt(parser_t *parser);
static ast_node_t *parse_return_stmt(parser_t *parser);
static ast_node_t *parse_expr_stmt(parser_t *parser);
static ast_node_t *parse_throw_stmt(parser_t *parser);
static ast_node_t *parse_try_expr(parser_t *parser, bool require_value);
static ast_node_t *parse_try_value_block(parser_t *parser);
static void parse_catch_clause(parser_t *parser, ast_node_t *try_node);

static ast_node_t *parse_expression(parser_t *parser);
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
static ast_node_t *parse_primary(parser_t *parser);

static void error_at(parser_t *parser, token_t token, const char *code, const char *message) {
	const char *actual_code;

	if (parser->panic_mode) {
		return;
	}
	parser->panic_mode = true;
	parser->had_error = true;

	// A TOKEN_ERROR came from the lexer, not from a parser expectation
	// that failed, so it carries its own "lexer::*" code and its message
	// text lives in token.start/length rather than in `message`.
	actual_code = (token.type == TOKEN_ERROR && token.error_code != NULL) ? token.error_code : code;

	if (token.type == TOKEN_EOF) {
		diagnostic_emit(DIAG_ERROR, actual_code, parser->filename, parser->source, token.line, token.column, 0, "at end of file: %s", message);
	} else if (token.type == TOKEN_ERROR) {
		// token.length here is the lexer message's length, not a source
		// span (see error_token() in lexer.c), so it can't drive the
		// caret width; a single caret at the error's source position is
		// all that's meaningful.
		diagnostic_emit(DIAG_ERROR, actual_code, parser->filename, parser->source, token.line, token.column, 0, "%.*s", (int)token.length, token.start);
	} else {
		diagnostic_emit(DIAG_ERROR, actual_code, parser->filename, parser->source, token.line, token.column, (int)token.length, "at '%.*s': %s", (int)token.length, token.start, message);
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

/* A builtin type keyword, a bare identifier (a struct type name), or
   `fn` (a function type). Safe to use anywhere a type is the only thing
   that could start (parameter types, return types after '->'): those
   positions are never ambiguous with anything else paweli's grammar
   allows there, unlike a bare identifier or `fn` at a *statement's*
   own start, see check_var_decl_start() below. */
static bool check_type_ref_start(parser_t *parser) {
	return check_type_keyword(parser) || check(parser, TOKEN_IDENTIFIER) || check(parser, TOKEN_FN);
}

/* Assumes the current token is TOKEN_FN. True if it starts a function
   *type* (`fn(int)->int`, no name between `fn` and `(`), false if it
   starts a *declaration* instead (`fn add(...) {...}`, a nested fn
   statement, or, in parse_struct_decl(), a struct method). Shared by
   check_var_decl_start() (a statement's own start) and
   parse_struct_decl()'s field-vs-method dispatch, which face the exact
   same ambiguity: only a one-token lookahead past `fn` tells them apart. */
static bool check_fn_is_type(parser_t *parser) {
	lexer_t lookahead;
	token_t next;

	lookahead = parser->lexer;
	next = lexer_next_token(&lookahead);
	return next.type != TOKEN_IDENTIFIER;
}

/* Like check_type_ref_start(), but for the two positions where a bare
   identifier (or `fn`) is ALSO ambiguous with an ordinary
   expression-statement (`parse_statement`'s dispatch, and the
   equivalent spot in `parse_for_stmt`): `Point p = ...;` (a var decl)
   has to be told apart from `foo();` or `x = 5;` (expression
   statements) before either has been parsed. A builtin keyword is
   unambiguous on its own. For a bare identifier, look ahead past any
   `[]` suffixes (zero or more; a nested array type like `Point[][] m`
   is still a declaration) without disturbing real parser state.
   lexer_t is a small, plain, copyable struct, so copying it and
   driving the copy is a cheap, side-effect-free way to peek past the
   current token. A declaration here always looks like
   `IDENTIFIER ( '[' ']' )* IDENTIFIER`; anything else (a lone
   identifier, a call, an index, an assignment, ...) is not a
   declaration.

   `fn` needs its own one-token lookahead, for a different ambiguity:
   `fn` directly followed by `(` is a function-*typed* var-decl
   (`fn(int,int)->int op = add;`), but `fn` followed by an identifier is
   the existing nested-function-*declaration* statement
   (`fn add(...) {...}`), parse_statement()'s own TOKEN_FN case, which
   this function must not shadow. */
static bool check_var_decl_start(parser_t *parser) {
	lexer_t lookahead;
	token_t next;

	if (check_type_keyword(parser)) {
		return true;
	}

	if (check(parser, TOKEN_FN)) {
		return check_fn_is_type(parser);
	}

	if (!check(parser, TOKEN_IDENTIFIER)) {
		return false;
	}

	lookahead = parser->lexer;
	next = lexer_next_token(&lookahead);

	while (next.type == TOKEN_LBRACKET) {
		next = lexer_next_token(&lookahead);
		if (next.type != TOKEN_RBRACKET) {
			return false;
		}
		next = lexer_next_token(&lookahead);
	}

	return next.type == TOKEN_IDENTIFIER;
}

/* Assumes check_type_ref_start() (or check_var_decl_start()) already
   passed. Consumes the base type, a keyword or a struct name, then
   an optional `[]` suffix for "array of" that type. */
static type_ref_t parse_type_ref(parser_t *parser) {
	type_ref_t type;
	function_type_ref_t *function_ref;
	type_ref_t *param_types;
	size_t param_count;
	size_t param_capacity;

	type.base_type = parser->current.type;
	type.array_depth = 0;
	type.function_ref = NULL;
	if (parser->current.type == TOKEN_IDENTIFIER) {
		type.struct_name = parser->current;
	}

	if (parser->current.type == TOKEN_FN) {
		advance(parser);
		consume(parser, TOKEN_LPAREN, "expected '(' after 'fn' in a function type");

		param_types = NULL;
		param_count = 0;
		param_capacity = 0;

		if (!check(parser, TOKEN_RPAREN)) {
			do {
				if (!check_type_ref_start(parser)) {
					error_at_current(parser, "parser::expected-type", "expected a type");
					break;
				}
				if (param_count == param_capacity) {
					param_capacity = param_capacity < 4 ? 4 : param_capacity * 2;
					param_types = realloc(param_types, param_capacity * sizeof(type_ref_t));
				}
				param_types[param_count++] = parse_type_ref(parser);
			} while (match(parser, TOKEN_COMMA));
		}

		consume(parser, TOKEN_RPAREN, "expected ')' after function type parameters");

		function_ref = malloc(sizeof(function_type_ref_t));
		function_ref->param_types = param_types;
		function_ref->param_count = param_count;
		function_ref->has_return_type = false;
		function_ref->return_type = (type_ref_t){TOKEN_EOF, false, {0}, NULL};

		if (match(parser, TOKEN_ARROW)) {
			function_ref->return_type = parse_type_ref(parser);
			function_ref->has_return_type = true;
		}

		type.function_ref = function_ref;
	} else {
		advance(parser);
	}

	while (match(parser, TOKEN_LBRACKET)) {
		consume(parser, TOKEN_RBRACKET, "expected ']' after '[' in an array type");
		type.array_depth++;
	}

	return type;
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
			case TOKEN_STRUCT:
			case TOKEN_INT:
			case TOKEN_FLOAT:
			case TOKEN_STRING:
			case TOKEN_BOOL:
			case TOKEN_IF:
			case TOKEN_WHILE:
			case TOKEN_FOR:
			case TOKEN_RETURN:
			case TOKEN_EXPORT:
			case TOKEN_TRY:
			case TOKEN_THROW:
			case TOKEN_RBRACE:
				return;
			default:
				break;
		}

		advance(parser);
	}
}

static ast_node_t *parse_module_import(parser_t *parser) {
	token_t name;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	advance(parser);
	consume(parser, TOKEN_STRING_LITERAL, "expected a string literal after '#module'");
	name = parser->previous;
	return ast_new_module_import(name, line, column);
}

static ast_node_t *parse_module_def(parser_t *parser) {
	token_t name;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	advance(parser);
	consume(parser, TOKEN_STRING_LITERAL, "expected a string literal after '#moduledef'");
	name = parser->previous;
	return ast_new_module_def(name, line, column);
}

static ast_node_t *parse_function_decl(parser_t *parser) {
	token_t name;
	int line;
	int column;
	bool has_return_type;
	type_ref_t return_type;
	ast_node_t *fn_node;

	if (!check(parser, TOKEN_FN)) {
		error_at_current(parser, "parser::expected-function", "expected a function declaration ('fn')");
		// Guaranteed to consume at least this token before synchronizing:
		// this check runs before anything else has advanced past it, so if
		// it happens to already be one of synchronize()'s own stop-set
		// tokens (return, if, a type keyword, ...), synchronize() would
		// return immediately without moving forward, and the caller would
		// retry parse_function_decl() on the same token forever.
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

	fn_node = ast_new_function_decl(name, false, (type_ref_t){TOKEN_EOF, false, {0}, NULL}, NULL, line, column);

	if (!check(parser, TOKEN_RPAREN)) {
		parse_params(parser, fn_node);
	}

	consume(parser, TOKEN_RPAREN, "expected ')' after parameters");

	has_return_type = false;
	return_type = (type_ref_t){TOKEN_EOF, false, {0}, NULL};
	if (match(parser, TOKEN_ARROW)) {
		if (!check_type_ref_start(parser)) {
			error_at_current(parser, "parser::expected-return-type", "expected a return type after '->'");
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

static void parse_params(parser_t *parser, ast_node_t *fn_node) {
	type_ref_t type;
	token_t name;
	ast_node_t *param;
	int line;
	int column;

	for (;;) {
		if (!check_type_ref_start(parser)) {
			error_at_current(parser, "parser::expected-parameter-type", "expected a parameter type");
			return;
		}

		line = parser->current.line;
		column = parser->current.column;
		type = parse_type_ref(parser);

		consume(parser, TOKEN_IDENTIFIER, "expected a parameter name");
		name = parser->previous;

		param = ast_new_param(type, name, NULL, line, column);
		ast_list_push(&fn_node->as.function_decl.params, param);

		if (!match(parser, TOKEN_COMMA)) {
			break;
		}
	}
}

/* `@name` or `@name("string literal")`, only ever consumed at the top
   level, right before a `fn` (see parser_parse_program()). There is no
   grammar for one anywhere else, including inside a function body, so a
   `@` there just falls through to parse_primary()'s generic
   "expected an expression" error. */
static ast_node_t *parse_decorator(parser_t *parser) {
	int line;
	int column;
	token_t name;
	ast_node_t *arg;

	line = parser->current.line;
	column = parser->current.column;
	advance(parser);

	consume(parser, TOKEN_IDENTIFIER, "expected a decorator name after '@'");
	name = parser->previous;

	arg = NULL;
	if (match(parser, TOKEN_LPAREN)) {
		consume(parser, TOKEN_STRING_LITERAL, "expected a string literal argument");
		arg = ast_new_literal(AST_STRING_LITERAL, parser->previous, parser->previous.line, parser->previous.column);
		consume(parser, TOKEN_RPAREN, "expected ')' after decorator argument");
	}

	return ast_new_decorator(name, arg, line, column);
}

/* `struct Name { type field (= expr)? ; ... }`. A field without `= expr`
   is required at every construction site; one with a default may be
   omitted from a struct literal, falling back to that expression. */
static ast_node_t *parse_struct_decl(parser_t *parser) {
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
	advance(parser);

	consume(parser, TOKEN_IDENTIFIER, "expected a struct name");
	name = parser->previous;

	node = ast_new_struct_decl(name, line, column);

	consume(parser, TOKEN_LBRACE, "expected '{' after struct name");

	while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
		if (check(parser, TOKEN_FN) && !check_fn_is_type(parser)) {
			ast_list_push(&node->as.struct_decl.functions, parse_function_decl(parser));
			continue;
		}

		if (!check_type_ref_start(parser)) {
			error_at_current(parser, "parser::expected-field-type", "expected a field type");
			break;
		}

		field_line = parser->current.line;
		field_column = parser->current.column;
		field_type = parse_type_ref(parser);

		consume(parser, TOKEN_IDENTIFIER, "expected a field name");
		field_name = parser->previous;

		default_value = NULL;
		if (match(parser, TOKEN_EQUAL)) {
			default_value = parse_expression(parser);
		}

		consume(parser, TOKEN_SEMICOLON, "expected ';' after struct field");

		field = ast_new_param(field_type, field_name, default_value, field_line, field_column);
		ast_list_push(&node->as.struct_decl.fields, field);
	}

	consume(parser, TOKEN_RBRACE, "expected '}' to close struct");

	return node;
}

/* `TypeName { field: value, ... }`, called from parse_primary once an
   identifier has already been consumed and '{' seen right after it.
   That is the only context that sequence can mean, since paweli requires a ';'
   after every expression statement, so an identifier immediately
   followed by '{' can never be "identifier statement, then a block
   statement" split across two statements. Also handles the bare
   `{ field: value, ... }` form (has_type_name false), called directly
   from parse_primary on a bare '{' in expression position: the type is
   left for the compiler to infer from context (compile_struct_literal),
   the same way an empty array literal `[]` already borrows its element
   type from context instead of spelling it out. */
static ast_node_t *parse_struct_literal(parser_t *parser, token_t type_name, bool has_type_name) {
	ast_node_t *node;
	token_t field_name;
	ast_node_t *value;
	ast_node_t *field_init;

	node = ast_new_struct_literal(type_name, has_type_name, has_type_name ? type_name.line : parser->current.line, has_type_name ? type_name.column : parser->current.column);

	consume(parser, TOKEN_LBRACE, "expected '{'");

	if (!check(parser, TOKEN_RBRACE)) {
		do {
			consume(parser, TOKEN_IDENTIFIER, "expected a field name");
			field_name = parser->previous;
			consume(parser, TOKEN_COLON, "expected ':' after field name");
			value = parse_expression(parser);
			field_init = ast_new_struct_field_init(field_name, value, field_name.line, field_name.column);
			ast_list_push(&node->as.struct_literal.fields, field_init);
		} while (match(parser, TOKEN_COMMA));
	}

	consume(parser, TOKEN_RBRACE, "expected '}' to close struct literal");

	return node;
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

/* The set of tokens parse_statement() dispatches to a dedicated parse_*
   function for, rather than falling through to parse_expr_stmt(). Kept
   as its own check (rather than duplicated) because parse_try_value_block()
   needs the exact same distinction: inside a try-block used in
   expression position, every *keyword-led* statement still parses (and
   still needs its own ';' where it always did) exactly like it does
   anywhere else. Only a bare expression gets the special "no ';'
   before the closing brace means this is the block's value" treatment. */
static bool starts_keyword_statement(parser_t *parser) {
	return check_var_decl_start(parser)
		|| check(parser, TOKEN_FN)
		|| check(parser, TOKEN_EXPORT)
		|| check(parser, TOKEN_IF)
		|| check(parser, TOKEN_WHILE)
		|| check(parser, TOKEN_FOR)
		|| check(parser, TOKEN_BREAK)
		|| check(parser, TOKEN_CONTINUE)
		|| check(parser, TOKEN_RETURN)
		|| check(parser, TOKEN_THROW)
		|| check(parser, TOKEN_TRY)
		|| check(parser, TOKEN_LBRACE);
}

static ast_node_t *parse_statement(parser_t *parser) {
	if (check_var_decl_start(parser)) {
		return parse_var_decl(parser);
	}
	if (check(parser, TOKEN_FN)) {
		return parse_function_decl(parser);
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
	if (check(parser, TOKEN_BREAK)) {
		return parse_break_stmt(parser);
	}
	if (check(parser, TOKEN_CONTINUE)) {
		return parse_continue_stmt(parser);
	}
	if (check(parser, TOKEN_RETURN)) {
		return parse_return_stmt(parser);
	}
	if (check(parser, TOKEN_THROW)) {
		return parse_throw_stmt(parser);
	}
	if (check(parser, TOKEN_TRY)) {
		return parse_try_expr(parser, false);
	}
	if (check(parser, TOKEN_LBRACE)) {
		return parse_block(parser);
	}
	return parse_expr_stmt(parser);
}

static ast_node_t *parse_var_decl(parser_t *parser) {
	type_ref_t type;
	token_t name;
	ast_node_t *initializer;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	type = parse_type_ref(parser);

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

	// A type keyword here is ambiguous between the classic three-clause
	// form (`for (int i = 0; ...)`) and the range/foreach forms
	// (`for (int i : 0..10)` / `for (int x : nums)`), which only
	// diverge after the variable's name. parse_typed_for_stmt() handles
	// all three once past that point, since only it can tell them apart.
	if (check_var_decl_start(parser)) {
		return parse_typed_for_stmt(parser, line, column);
	}

	if (match(parser, TOKEN_SEMICOLON)) {
		initializer = NULL;
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

static token_t synthetic_token(token_type_t type, const char *text, int line, int column) {
	token_t token;

	token.type = type;
	token.start = text;
	token.length = strlen(text);
	token.line = line;
	token.column = column;
	token.error_code = NULL;

	return token;
}

/* Handles every for-loop form that starts with a declared type: the
   classic `for (T name = init; cond; incr)`, the range form
   `for (T name : start..end)` (desugared here into exactly the AST a
   hand-written three-clause loop with a hidden end-bound variable would
   produce, reusing compile_for_stmt() unchanged), and the array/string
   form `for (T name : expr)` (a real AST_FOREACH_STMT, since expr's
   element type is not known until it is type-checked, see
   compile_foreach_stmt() in compiler.c). */
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

	consume(parser, TOKEN_IDENTIFIER, "expected a variable name");
	name = parser->previous;

	if (match(parser, TOKEN_COLON)) {
		ast_node_t *first_expr;

		first_expr = parse_expression(parser);

		if (match(parser, TOKEN_DOT_DOT)) {
			ast_node_t *end_expr;
			ast_node_t *range_end_decl;
			ast_node_t *for_node;
			ast_node_t *wrapper;
			token_t range_end_name;
			token_t one_literal;

			end_expr = parse_expression(parser);
			consume(parser, TOKEN_RPAREN, "expected ')' after for-loop range");
			body = parse_block(parser);

			// { T $for_range_end = end; for (T name = start; name <= $for_range_end; name = name + 1) body }
			range_end_name = synthetic_token(TOKEN_IDENTIFIER, "$for_range_end", decl_line, decl_column);
			one_literal = synthetic_token(TOKEN_INT_LITERAL, "1", decl_line, decl_column);

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

		consume(parser, TOKEN_RPAREN, "expected ')' after for-loop array expression");
		body = parse_block(parser);

		return ast_new_foreach_stmt(type, name, first_expr, body, line, column);
	}

	consume(parser, TOKEN_EQUAL, "expected '=' or ':' after for-loop variable name");
	initializer = ast_new_var_decl(type, name, parse_expression(parser), decl_line, decl_column);
	consume(parser, TOKEN_SEMICOLON, "expected ';' after variable declaration");

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

static ast_node_t *parse_break_stmt(parser_t *parser) {
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	advance(parser);

	consume(parser, TOKEN_SEMICOLON, "expected ';' after 'break'");

	return ast_new_break_stmt(line, column);
}

static ast_node_t *parse_continue_stmt(parser_t *parser) {
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	advance(parser);

	consume(parser, TOKEN_SEMICOLON, "expected ';' after 'continue'");

	return ast_new_continue_stmt(line, column);
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

static ast_node_t *parse_throw_stmt(parser_t *parser) {
	ast_node_t *value;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	advance(parser);

	value = parse_expression(parser);
	consume(parser, TOKEN_SEMICOLON, "expected ';' after 'throw'");

	return ast_new_throw_stmt(value, line, column);
}

/* The `(...)` after `catch`, plus its optional `{ ... }` body, filling
   in try_node's own fields. Assumes the current token is TOKEN_CATCH.
   The grammar is `catch(TYPE X [: TYPE fallback]) { body }?`, where X
   is either a bare identifier (declares a new binding, `catch(string
   message)`) or a general expression (a fallback value, `catch(int
   0)`), distinguished the same way a var_decl's declared type is
   told apart from an ordinary identifier expression everywhere else in
   this grammar: TYPE directly followed by a bare identifier always
   means "declare a new name of this type", never "read an existing
   variable". A catch clause with only a binding (no fallback) must
   have a body, since otherwise it would do nothing at all; one with a
   fallback may omit the body. */
static void parse_catch_clause(parser_t *parser, ast_node_t *try_node) {
	type_ref_t type;
	lexer_t lookahead;
	token_t next;
	bool is_binding;

	advance(parser);
	consume(parser, TOKEN_LPAREN, "expected '(' after 'catch'");

	if (!check_type_ref_start(parser)) {
		error_at_current(parser, "parser::expected-type", "expected a type after 'catch('");
	} else {
		type = parse_type_ref(parser);

		is_binding = false;
		if (check(parser, TOKEN_IDENTIFIER)) {
			lookahead = parser->lexer;
			next = lexer_next_token(&lookahead);
			is_binding = next.type == TOKEN_RPAREN || next.type == TOKEN_COLON;
		}

		if (is_binding) {
			advance(parser);
			try_node->as.try_expr.has_binding = true;
			try_node->as.try_expr.binding_type = type;
			try_node->as.try_expr.binding_name = parser->previous;

			if (match(parser, TOKEN_COLON)) {
				if (!check_type_ref_start(parser)) {
					error_at_current(parser, "parser::expected-type", "expected a type after ':'");
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

	consume(parser, TOKEN_RPAREN, "expected ')' after catch clause");

	if (check(parser, TOKEN_LBRACE)) {
		try_node->as.try_expr.catch_body = parse_block(parser);
	} else if (!try_node->as.try_expr.has_fallback) {
		error_at_current(parser, "parser::expected-catch-body", "a catch with only a binding needs a '{ ... }' body");
	}
}

/* Like parse_block(), but for a try-block used in EXPRESSION position:
   every keyword-led statement (starts_keyword_statement()) parses
   exactly like it does anywhere else, ';' included. The one
   exception is a bare trailing expression with no ';' immediately
   before the closing '}', which becomes this block's implicit `return`
   (the block's value). This is the only place in the whole grammar a
   statement's ';' is ever optional. */
static ast_node_t *parse_try_value_block(parser_t *parser) {
	ast_node_t *block;
	ast_node_t *stmt;
	ast_node_t *expr;

	block = ast_new_block(parser->current.line, parser->current.column);

	consume(parser, TOKEN_LBRACE, "expected '{' to begin a try block");

	while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
		if (parser->panic_mode) {
			synchronize(parser);
			continue;
		}

		if (starts_keyword_statement(parser)) {
			stmt = parse_statement(parser);
			ast_list_push(&block->as.block.statements, stmt);
			continue;
		}

		expr = parse_expression(parser);

		if (check(parser, TOKEN_RBRACE)) {
			stmt = ast_new_return_stmt(expr, expr->line, expr->column);
			ast_list_push(&block->as.block.statements, stmt);
			break;
		}

		consume(parser, TOKEN_SEMICOLON, "expected ';' after expression statement");
		stmt = ast_new_expr_stmt(expr, expr->line, expr->column);
		ast_list_push(&block->as.block.statements, stmt);
	}

	consume(parser, TOKEN_RBRACE, "expected '}' to close a try block");

	return block;
}

/* `try { ... } catch(...) { ... }`, in either position: require_value
   is false from parse_statement() (an ordinary block, ';'-terminated
   statements throughout, the try's own result, if any, discarded),
   true from parse_primary() (parse_try_value_block()'s trailing-
   expression convention, and a fallback becomes mandatory since the
   whole point is producing a value on both the success and failure
   paths). Also reached, pre-built, from parse_call()'s postfix loop for
   the terse `expr catch(TYPE fallback)` spelling. That one calls
   parse_catch_clause() directly instead of going through here, since
   its try_body is synthesized from a single already-parsed expression
   rather than a `{ }` block. */
static ast_node_t *parse_try_expr(parser_t *parser, bool require_value) {
	int line;
	int column;
	ast_node_t *try_body;
	ast_node_t *try_node;

	line = parser->current.line;
	column = parser->current.column;
	advance(parser);

	try_body = require_value ? parse_try_value_block(parser) : parse_block(parser);
	try_node = ast_new_try_expr(try_body, line, column);

	if (!check(parser, TOKEN_CATCH)) {
		error_at_current(parser, "parser::expected-catch", "expected 'catch' after a try block");
		return try_node;
	}

	parse_catch_clause(parser, try_node);

	if (require_value && !try_node->as.try_expr.has_fallback) {
		error_at_current(parser, "parser::try-expression-needs-fallback", "a try used as an expression needs 'catch(... : TYPE fallback)' to provide a value if it fails");
	}

	return try_node;
}

static ast_node_t *parse_expression(parser_t *parser) {
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

	if (check(parser, TOKEN_EQUAL) || check_compound_assign(parser)) {
		equals = parser->current;
		advance(parser);

		value = parse_assignment(parser);

		if (expr->kind != AST_IDENTIFIER_EXPR && expr->kind != AST_MEMBER_EXPR && expr->kind != AST_INDEX_EXPR) {
			error_at(parser, equals, "parser::invalid-assignment-target", "invalid assignment target");
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

	expr = parse_bitor(parser);

	while (check(parser, TOKEN_AMP_AMP)) {
		op = parser->current;
		advance(parser);
		right = parse_bitor(parser);
		expr = ast_new_binary_expr(op.type, expr, right, op.line, op.column);
	}

	return expr;
}

/* Bitwise `|`, `^`, `&`, in ascending precedence (`|` loosest, `&`
   tightest), matching C's own ladder. All three sit strictly between the
   logical operators (`&&`/`||`, which short-circuit and are unrelated
   despite the similar-looking tokens) and equality, so `a & b == c`
   parses as `a & (b == c)`, not `(a & b) == c`. */
static ast_node_t *parse_bitor(parser_t *parser) {
	ast_node_t *expr;
	token_t op;
	ast_node_t *right;

	expr = parse_bitxor(parser);

	while (check(parser, TOKEN_PIPE)) {
		op = parser->current;
		advance(parser);
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

	while (check(parser, TOKEN_CARET)) {
		op = parser->current;
		advance(parser);
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

	while (check(parser, TOKEN_AMP)) {
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

	expr = parse_shift(parser);

	while (check(parser, TOKEN_LESS) || check(parser, TOKEN_LESS_EQUAL) || check(parser, TOKEN_GREATER) || check(parser, TOKEN_GREATER_EQUAL)) {
		op = parser->current;
		advance(parser);
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

	while (check(parser, TOKEN_LESS_LESS) || check(parser, TOKEN_GREATER_GREATER)) {
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

	if (check(parser, TOKEN_BANG) || check(parser, TOKEN_MINUS) || check(parser, TOKEN_TILDE)) {
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
		} else if (check(parser, TOKEN_LBRACKET)) {
			ast_node_t *index;

			line = parser->current.line;
			column = parser->current.column;
			advance(parser);
			index = parse_expression(parser);
			consume(parser, TOKEN_RBRACKET, "expected ']' after array index");
			expr = ast_new_index_expr(expr, index, line, column);
		} else if (check(parser, TOKEN_CATCH)) {
			// `expr catch(TYPE fallback)`: pure sugar over the general
			// try/catch mechanism. It synthesizes a one-statement
			// try-block (`return expr;`) so the compiler only ever has
			// one AST_TRY_EXPR shape to handle, regardless of spelling.
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

	if (!check(parser, TOKEN_RPAREN)) {
		do {
			arg = parse_expression(parser);
			ast_list_push(&call->as.call_expr.args, arg);
		} while (match(parser, TOKEN_COMMA));
	}

	consume(parser, TOKEN_RPAREN, "expected ')' after arguments");

	return call;
}

/* `fn(TYPE NAME, ...) -> TYPE { block }`, a lambda literal in expression
   position, mirrors parse_function_decl() (same param/return-type/body
   shape, reusing parse_params() unchanged) except there is no name to
   consume: the callee position TOKEN_FN was checked, not consumed, by
   parse_primary() before calling this. The resulting AST_FUNCTION_DECL
   node carries a synthetic empty name (matching how a bare `{ ... }`
   struct literal already gets a synthetic empty type-name token from
   parse_primary()) so compiler.c can tell a lambda's node apart from a
   real declaration's on sight, though in practice it never needs to:
   the two are only ever reached through different compile_* entry
   points (compile_statement vs. compile_expr_typed), never confused. */
static ast_node_t *parse_lambda_expr(parser_t *parser) {
	token_t fn_token;
	token_t empty_name;
	ast_node_t *lambda;
	bool has_return_type;
	type_ref_t return_type;

	fn_token = parser->current;
	advance(parser);
	consume(parser, TOKEN_LPAREN, "expected '(' after 'fn'");

	empty_name = synthetic_token(TOKEN_FN, "", fn_token.line, fn_token.column);
	lambda = ast_new_function_decl(empty_name, false, (type_ref_t){TOKEN_EOF, false, {0}, NULL}, NULL, fn_token.line, fn_token.column);

	if (!check(parser, TOKEN_RPAREN)) {
		parse_params(parser, lambda);
	}
	consume(parser, TOKEN_RPAREN, "expected ')' after lambda parameters");

	has_return_type = false;
	return_type = (type_ref_t){TOKEN_EOF, false, {0}, NULL};
	if (match(parser, TOKEN_ARROW)) {
		if (!check_type_ref_start(parser)) {
			error_at_current(parser, "parser::expected-return-type", "expected a return type after '->'");
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
		token_t name = parser->previous;

		if (check(parser, TOKEN_LBRACE)) {
			return parse_struct_literal(parser, name, true);
		}
		return ast_new_identifier_expr(name, name.line, name.column);
	}
	if (match(parser, TOKEN_LPAREN)) {
		expr = parse_expression(parser);
		consume(parser, TOKEN_RPAREN, "expected ')' after expression");
		return expr;
	}
	if (check(parser, TOKEN_LBRACE)) {
		token_t empty_name = synthetic_token(TOKEN_LBRACE, "", parser->current.line, parser->current.column);
		return parse_struct_literal(parser, empty_name, false);
	}
	if (check(parser, TOKEN_LBRACKET)) {
		ast_node_t *array;
		ast_node_t *element;

		array = ast_new_array_literal(parser->current.line, parser->current.column);
		advance(parser);

		if (!check(parser, TOKEN_RBRACKET)) {
			do {
				element = parse_expression(parser);
				ast_list_push(&array->as.array_literal.elements, element);
			} while (match(parser, TOKEN_COMMA));
		}

		consume(parser, TOKEN_RBRACKET, "expected ']' after array elements");
		return array;
	}
	if (check(parser, TOKEN_FN)) {
		return parse_lambda_expr(parser);
	}
	if (check(parser, TOKEN_TRY)) {
		return parse_try_expr(parser, true);
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
	ast_node_t *program;
	ast_node_t *item;
	ast_list_t decorators;

	program = ast_new_program();

	if (check(parser, TOKEN_MODULEDEF)) {
		program->as.program.module_def = parse_module_def(parser);
	}

	while (check(parser, TOKEN_MODULE)) {
		ast_list_push(&program->as.program.imports, parse_module_import(parser));
	}

	// A second '#moduledef', or one after any '#module' import, is caught
	// here rather than by the two checks above ever re-triggering (they
	// each only run once/in a fixed loop position): report it, then parse
	// and discard it so the rest of the file still gets parsed instead of
	// cascading into item-parser garbage.
	if (check(parser, TOKEN_MODULEDEF)) {
		error_at_current(parser, "parser::misplaced-moduledef", "'#moduledef' must be the first line of the file, before any '#module' imports, and only once");
		ast_free(parse_module_def(parser));
		synchronize(parser);
	}

	while (!check(parser, TOKEN_EOF)) {
		ast_list_init(&decorators);
		while (check(parser, TOKEN_AT)) {
			ast_list_push(&decorators, parse_decorator(parser));
		}

		if (decorators.count > 0 && !check(parser, TOKEN_FN)) {
			error_at_current(parser, "parser::decorators-require-function", "decorators can only be used on function declarations");
			ast_list_free(&decorators);
			ast_list_init(&decorators);
		}

		if (check(parser, TOKEN_STRUCT)) {
			item = parse_struct_decl(parser);
			if (item != NULL) {
				ast_list_push(&program->as.program.structs, item);
			}
			ast_list_free(&decorators);
		} else if (check_var_decl_start(parser)) {
			item = parse_var_decl(parser);
			if (item != NULL) {
				ast_list_push(&program->as.program.globals, item);
			}
			ast_list_free(&decorators);
		} else {
			item = parse_function_decl(parser);
			if (item != NULL) {
				item->as.function_decl.decorators = decorators;
				ast_list_push(&program->as.program.functions, item);
			} else {
				ast_list_free(&decorators);
			}
		}
		if (parser->panic_mode) {
			synchronize(parser);
		}
	}

	return program;
}
