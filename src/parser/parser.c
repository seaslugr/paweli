#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "parser_internal.h"

static bool check_type_keyword(parser_t *parser);
static ast_node_t *parse_module_import(parser_t *parser);
static ast_node_t *parse_module_def(parser_t *parser);

void parser_error_at(parser_t *parser, token_t token, const char *code, const char *message) {
	const char *actual_code;

	if (parser->panic_mode) {
		return;
	}
	parser->panic_mode = true;
	parser->had_error = true;

	// A TOKEN_ERROR comes from the lexer. It has its own "lexer::*" code,
	// and its message is in token.start and token.length.
	actual_code = (token.type == TOKEN_ERROR && token.error_code != NULL) ? token.error_code : code;

	if (token.type == TOKEN_EOF) {
		diagnostic_emit(DIAG_ERROR, actual_code, parser->filename, parser->source, token.line, token.column, 0, "at end of file: %s", message);
	} else if (token.type == TOKEN_ERROR) {
		// token.length is the length of the lexer's message, so it cannot set
		// the caret width. One caret at the error position is used.
		diagnostic_emit(DIAG_ERROR, actual_code, parser->filename, parser->source, token.line, token.column, 0, "%.*s", (int)token.length, token.start);
	} else {
		diagnostic_emit(DIAG_ERROR, actual_code, parser->filename, parser->source, token.line, token.column, (int)token.length, "at '%.*s': %s", (int)token.length, token.start, message);
	}
}

void parser_error_at_current(parser_t *parser, const char *code, const char *message) {
	parser_error_at(parser, parser->current, code, message);
}

void parser_advance(parser_t *parser) {
	parser->previous = parser->current;

	for (;;) {
		parser->current = lexer_next_token(&parser->lexer);
		if (parser->current.type != TOKEN_ERROR) {
			break;
		}
		parser_error_at_current(parser, NULL, "");
	}
}

bool parser_check(parser_t *parser, token_type_t type) {
	return parser->current.type == type;
}

bool parser_match(parser_t *parser, token_type_t type) {
	if (!parser_check(parser, type)) {
		return false;
	}
	parser_advance(parser);
	return true;
}

void parser_consume(parser_t *parser, token_type_t type, const char *message) {
	if (parser_check(parser, type)) {
		parser_advance(parser);
		return;
	}
	parser_error_at_current(parser, "parser::expected-token", message);
}

static bool check_type_keyword(parser_t *parser) {
	return parser_check(parser, TOKEN_INT) || parser_check(parser, TOKEN_FLOAT) || parser_check(parser, TOKEN_STRING) || parser_check(parser, TOKEN_BOOL);
}

/* True at a builtin type keyword, an identifier (a struct type name),
   or `fn` (a function type). Use it only where a type is the one thing
   that can start, such as a parameter type or a return type. At the
   start of a statement use parser_check_var_decl_start(). */
bool parser_check_type_ref_start(parser_t *parser) {
	return check_type_keyword(parser) || parser_check(parser, TOKEN_IDENTIFIER) || parser_check(parser, TOKEN_FN);
}

/* The current token must be TOKEN_FN. True if it starts a function
   type (`fn(int)->int`, with no name before the `(`). False if it
   starts a declaration (`fn add(...) {...}`). One token of lookahead
   past `fn` tells them apart. */
bool parser_check_fn_is_type(parser_t *parser) {
	lexer_t lookahead;
	token_t next;

	lookahead = parser->lexer;
	next = lexer_next_token(&lookahead);
	return next.type != TOKEN_IDENTIFIER;
}

/* True if a variable declaration starts here. Used at the start of a
   statement and in a `for` init clause, where an identifier or `fn`
   could also start an expression. `Point p = ...;` must be told apart
   from `foo();` and `x = 5;` before either is parsed.

   A builtin type keyword is always a declaration. For an identifier it
   looks ahead on a copy of the lexer, which is a small plain struct. A
   declaration has the form `IDENTIFIER ( '[' ']' )* IDENTIFIER`.
   Anything else is an expression.

   `fn` followed by `(` is a function-typed declaration
   (`fn(int,int)->int op = add;`). `fn` followed by a name is a nested
   function declaration, which parse_statement() handles. */
bool parser_check_var_decl_start(parser_t *parser) {
	lexer_t lookahead;
	token_t next;

	if (check_type_keyword(parser)) {
		return true;
	}

	if (parser_check(parser, TOKEN_FN)) {
		return parser_check_fn_is_type(parser);
	}

	if (!parser_check(parser, TOKEN_IDENTIFIER)) {
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

/* Call it after parser_check_type_ref_start() or
   parser_check_var_decl_start() passed. Consumes the base type, then
   any `[]` suffixes. */
type_ref_t parse_type_ref(parser_t *parser) {
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
		parser_advance(parser);
		parser_consume(parser, TOKEN_LPAREN, "expected '(' after 'fn' in a function type");

		param_types = NULL;
		param_count = 0;
		param_capacity = 0;

		if (!parser_check(parser, TOKEN_RPAREN)) {
			do {
				if (!parser_check_type_ref_start(parser)) {
					parser_error_at_current(parser, "parser::expected-type", "expected a type");
					break;
				}
				if (param_count == param_capacity) {
					param_capacity = param_capacity < 4 ? 4 : param_capacity * 2;
					param_types = realloc(param_types, param_capacity * sizeof(type_ref_t));
				}
				param_types[param_count++] = parse_type_ref(parser);
			} while (parser_match(parser, TOKEN_COMMA));
		}

		parser_consume(parser, TOKEN_RPAREN, "expected ')' after function type parameters");

		function_ref = malloc(sizeof(function_type_ref_t));
		function_ref->param_types = param_types;
		function_ref->param_count = param_count;
		function_ref->has_return_type = false;
		function_ref->return_type = (type_ref_t){TOKEN_EOF, false, {0}, NULL};

		if (parser_match(parser, TOKEN_ARROW)) {
			function_ref->return_type = parse_type_ref(parser);
			function_ref->has_return_type = true;
		}

		type.function_ref = function_ref;
	} else {
		parser_advance(parser);
	}

	while (parser_match(parser, TOKEN_LBRACKET)) {
		parser_consume(parser, TOKEN_RBRACKET, "expected ']' after '[' in an array type");
		type.array_depth++;
	}

	return type;
}

/* Skips tokens up to the likely start of the next statement or
   declaration, so one syntax error does not cause a run of follow-on
   errors. */
void parser_synchronize(parser_t *parser) {
	parser->panic_mode = false;

	while (!parser_check(parser, TOKEN_EOF)) {
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

		parser_advance(parser);
	}
}

static ast_node_t *parse_module_import(parser_t *parser) {
	token_t name;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);
	parser_consume(parser, TOKEN_STRING_LITERAL, "expected a string literal after '#module'");
	name = parser->previous;
	return ast_new_module_import(name, line, column);
}

static ast_node_t *parse_module_def(parser_t *parser) {
	token_t name;
	int line;
	int column;

	line = parser->current.line;
	column = parser->current.column;
	parser_advance(parser);
	parser_consume(parser, TOKEN_STRING_LITERAL, "expected a string literal after '#moduledef'");
	name = parser->previous;
	return ast_new_module_def(name, line, column);
}

token_t parser_synthetic_token(token_type_t type, const char *text, int line, int column) {
	token_t token;

	token.type = type;
	token.start = text;
	token.length = strlen(text);
	token.line = line;
	token.column = column;
	token.error_code = NULL;

	return token;
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

	parser_advance(parser);
}

ast_node_t *parser_parse_program(parser_t *parser) {
	ast_node_t *program;
	ast_node_t *item;
	ast_list_t decorators;

	program = ast_new_program();

	if (parser_check(parser, TOKEN_MODULEDEF)) {
		program->as.program.module_def = parse_module_def(parser);
	}

	while (parser_check(parser, TOKEN_MODULE)) {
		ast_list_push(&program->as.program.imports, parse_module_import(parser));
	}

	// A second '#moduledef', or one after a '#module' import. Report it,
	// then parse and discard it so the rest of the file still parses.
	if (parser_check(parser, TOKEN_MODULEDEF)) {
		parser_error_at_current(parser, "parser::misplaced-moduledef", "'#moduledef' must be the first line of the file, before any '#module' imports, and only once");
		ast_free(parse_module_def(parser));
		parser_synchronize(parser);
	}

	while (!parser_check(parser, TOKEN_EOF)) {
		ast_list_init(&decorators);
		while (parser_check(parser, TOKEN_AT)) {
			ast_list_push(&decorators, parse_decorator(parser));
		}

		if (decorators.count > 0 && !parser_check(parser, TOKEN_FN)) {
			parser_error_at_current(parser, "parser::decorators-require-function", "decorators can only be used on function declarations");
			ast_list_free(&decorators);
			ast_list_init(&decorators);
		}

		if (parser_check(parser, TOKEN_STRUCT)) {
			item = parse_struct_decl(parser);
			if (item != NULL) {
				ast_list_push(&program->as.program.structs, item);
			}
			ast_list_free(&decorators);
		} else if (parser_check_var_decl_start(parser)) {
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
			parser_synchronize(parser);
		}
	}

	return program;
}
