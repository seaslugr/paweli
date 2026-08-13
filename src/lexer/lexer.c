#include <stdbool.h>
#include <string.h>

#include "lexer.h"

typedef struct {
	const char *text;
	token_type_t type;
} keyword_t;

static bool is_at_end(lexer_t *lexer);
static char advance(lexer_t *lexer);
static char peek(lexer_t *lexer);
static char peek_next(lexer_t *lexer);
static char peek_next2(lexer_t *lexer);
static bool match(lexer_t *lexer, char expected);
static bool is_digit(char c);
static bool is_alpha(char c);
static void begin_token(lexer_t *lexer);
static token_t make_token(lexer_t *lexer, token_type_t type);
static token_t error_token(lexer_t *lexer, const char *code, const char *message);
static bool skip_line_comment(lexer_t *lexer);
static bool skip_block_comment(lexer_t *lexer);
static bool skip_whitespace(lexer_t *lexer);
static token_type_t identifier_type(lexer_t *lexer);
static token_t scan_identifier(lexer_t *lexer);
static token_t scan_number(lexer_t *lexer);
static token_t scan_string(lexer_t *lexer);
static token_t scan_directive(lexer_t *lexer);

static inline bool is_at_end(lexer_t *lexer) {
	return *lexer->current == '\0';
}

static inline char advance(lexer_t *lexer) {
	char c;

	c = *lexer->current;
	lexer->current++;

	if (c == '\n') {
		lexer->line++;
		lexer->column = 1;
	} else {
		lexer->column++;
	}

	return c;
}

static inline char peek(lexer_t *lexer) {
	return *lexer->current;
}

static inline char peek_next(lexer_t *lexer) {
	if (is_at_end(lexer)) {
		return '\0';
	}
	return lexer->current[1];
}

static inline char peek_next2(lexer_t *lexer) {
	if (is_at_end(lexer) || lexer->current[1] == '\0') {
		return '\0';
	}
	return lexer->current[2];
}

static inline bool match(lexer_t *lexer, char expected) {
	if (is_at_end(lexer) || *lexer->current != expected) {
		return false;
	}
	advance(lexer);
	return true;
}

static inline bool is_digit(char c) {
	return c >= '0' && c <= '9';
}

static inline bool is_alpha(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static void begin_token(lexer_t *lexer) {
	lexer->start = lexer->current;
	lexer->token_line = lexer->line;
	lexer->token_column = lexer->column;
}

static token_t make_token(lexer_t *lexer, token_type_t type) {
	token_t token;

	token.type = type;
	token.start = lexer->start;
	token.length = (size_t)(lexer->current - lexer->start);
	token.line = lexer->token_line;
	token.column = lexer->token_column;
	token.error_code = NULL;
	return token;
}

static token_t error_token(lexer_t *lexer, const char *code, const char *message) {
	token_t token;

	token.type = TOKEN_ERROR;
	token.start = message;
	token.length = strlen(message);
	token.line = lexer->token_line;
	token.column = lexer->token_column;
	token.error_code = code;
	return token;
}

/* Stops right before the newline that ends the comment. */
static bool skip_line_comment(lexer_t *lexer) {
	while (peek(lexer) != '\n' && !is_at_end(lexer)) {
		advance(lexer);
	}
	return true;
}

/* Assumes the lexer has not consumed the opening "///" yet. Returns false
   if EOF appears before a closing "///". */
static bool skip_block_comment(lexer_t *lexer) {
	advance(lexer);
	advance(lexer);
	advance(lexer);

	while (!(peek(lexer) == '/' && peek_next(lexer) == '/' && peek_next2(lexer) == '/')) {
		if (is_at_end(lexer)) {
			return false;
		}
		advance(lexer);
	}

	advance(lexer);
	advance(lexer);
	advance(lexer);
	return true;
}

/* Returns false when it finds an unterminated block comment. */
static bool skip_whitespace(lexer_t *lexer) {
	for (;;) {
		char c = peek(lexer);

		switch (c) {
			case ' ':
			case '\t':
			case '\r':
			case '\n':
				advance(lexer);
				break;
			case '/':
				if (peek_next(lexer) != '/') {
					return true;
				}
				if (peek_next2(lexer) == '/') {
					if (!skip_block_comment(lexer)) {
						return false;
					}
				} else {
					skip_line_comment(lexer);
				}
				break;
			default:
				return true;
		}
	}
}

static const keyword_t keywords[] = {
	{"fn", TOKEN_FN},
	{"int", TOKEN_INT},
	{"float", TOKEN_FLOAT},
	{"string", TOKEN_STRING},
	{"bool", TOKEN_BOOL},
	{"if", TOKEN_IF},
	{"else", TOKEN_ELSE},
	{"while", TOKEN_WHILE},
	{"for", TOKEN_FOR},
	{"return", TOKEN_RETURN},
	{"true", TOKEN_TRUE},
	{"false", TOKEN_FALSE},
	{"null", TOKEN_NULL},
	{"export", TOKEN_EXPORT},
};

static token_type_t identifier_type(lexer_t *lexer) {
	size_t length;
	size_t count;
	size_t i;

	length = (size_t)(lexer->current - lexer->start);
	count = sizeof(keywords) / sizeof(keywords[0]);

	for (i = 0; i < count; i++) {
		if (strlen(keywords[i].text) == length && memcmp(lexer->start, keywords[i].text, length) == 0) {
			return keywords[i].type;
		}
	}

	return TOKEN_IDENTIFIER;
}

static token_t scan_identifier(lexer_t *lexer) {
	while (is_alpha(peek(lexer)) || is_digit(peek(lexer))) {
		advance(lexer);
	}
	return make_token(lexer, identifier_type(lexer));
}

static token_t scan_number(lexer_t *lexer) {
	while (is_digit(peek(lexer))) {
		advance(lexer);
	}

	if (peek(lexer) == '.' && is_digit(peek_next(lexer))) {
		advance(lexer);
		while (is_digit(peek(lexer))) {
			advance(lexer);
		}
		return make_token(lexer, TOKEN_FLOAT_LITERAL);
	}

	return make_token(lexer, TOKEN_INT_LITERAL);
}

/* Validates escape sequences while scanning, so it can find the closing
   quote correctly. It does not decode them. Decoding happens later, when
   the compiler turns this token into a heap string. */
static token_t scan_string(lexer_t *lexer) {
	char escaped;

	while (peek(lexer) != '"') {
		if (is_at_end(lexer) || peek(lexer) == '\n') {
			return error_token(lexer, "lexer::unterminated-string", "unterminated string literal");
		}

		if (peek(lexer) == '\\') {
			advance(lexer);
			if (is_at_end(lexer)) {
				return error_token(lexer, "lexer::unterminated-string", "unterminated string literal");
			}
			escaped = peek(lexer);
			if (escaped != 'n' && escaped != 't' && escaped != 'r' && escaped != '\\' && escaped != '"' && escaped != '0') {
				return error_token(lexer, "lexer::invalid-escape-sequence", "invalid escape sequence in string literal");
			}
		}

		advance(lexer);
	}

	advance(lexer);
	return make_token(lexer, TOKEN_STRING_LITERAL);
}

/* Assumes the lexer has already consumed the leading '#'. No whitespace
   can appear between '#' and the directive name. */
static token_t scan_directive(lexer_t *lexer) {
	const char *word_start;
	size_t length;

	if (!is_alpha(peek(lexer))) {
		return error_token(lexer, "lexer::directive-missing-name", "expected 'module' or 'moduledef' after '#'");
	}

	word_start = lexer->current;
	while (is_alpha(peek(lexer)) || is_digit(peek(lexer))) {
		advance(lexer);
	}

	length = (size_t)(lexer->current - word_start);

	if (length == 6 && memcmp(word_start, "module", 6) == 0) {
		return make_token(lexer, TOKEN_MODULE);
	}
	if (length == 9 && memcmp(word_start, "moduledef", 9) == 0) {
		return make_token(lexer, TOKEN_MODULEDEF);
	}

	return error_token(lexer, "lexer::unknown-directive", "unknown directive (expected #module or #moduledef)");
}

/* source must be null-terminated and must outlive the lexer. Tokens are
   spans into that buffer, so they point directly into it instead of
   copying from it. */
void lexer_init(lexer_t *lexer, const char *source) {
	lexer->start = source;
	lexer->current = source;
	lexer->line = 1;
	lexer->column = 1;
	lexer->token_line = 1;
	lexer->token_column = 1;
}

token_t lexer_next_token(lexer_t *lexer) {
	char c;

	if (!skip_whitespace(lexer)) {
		begin_token(lexer);
		return error_token(lexer, "lexer::unterminated-block-comment", "unterminated block comment");
	}

	begin_token(lexer);

	if (is_at_end(lexer)) {
		return make_token(lexer, TOKEN_EOF);
	}

	c = advance(lexer);

	if (is_alpha(c)) {
		return scan_identifier(lexer);
	}
	if (is_digit(c)) {
		return scan_number(lexer);
	}

	switch (c) {
		case '(':
			return make_token(lexer, TOKEN_LPAREN);
		case ')':
			return make_token(lexer, TOKEN_RPAREN);
		case '{':
			return make_token(lexer, TOKEN_LBRACE);
		case '}':
			return make_token(lexer, TOKEN_RBRACE);
		case '[':
			return make_token(lexer, TOKEN_LBRACKET);
		case ']':
			return make_token(lexer, TOKEN_RBRACKET);
		case ',':
			return make_token(lexer, TOKEN_COMMA);
		case ';':
			return make_token(lexer, TOKEN_SEMICOLON);
		case '.':
			return make_token(lexer, TOKEN_DOT);
		case '+':
			return make_token(lexer, TOKEN_PLUS);
		case '-':
			return make_token(lexer, match(lexer, '>') ? TOKEN_ARROW : TOKEN_MINUS);
		case '*':
			return make_token(lexer, TOKEN_STAR);
		case '%':
			return make_token(lexer, TOKEN_PERCENT);
		case '/':
			return make_token(lexer, TOKEN_SLASH);
		case '!':
			return make_token(lexer, match(lexer, '=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
		case '=':
			return make_token(lexer, match(lexer, '=') ? TOKEN_EQUAL_EQUAL : TOKEN_EQUAL);
		case '<':
			return make_token(lexer, match(lexer, '=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
		case '>':
			return make_token(lexer, match(lexer, '=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
		case '&':
			if (match(lexer, '&')) {
				return make_token(lexer, TOKEN_AMP_AMP);
			}
			return error_token(lexer, "lexer::unexpected-character", "unexpected character '&'");
		case '|':
			if (match(lexer, '|')) {
				return make_token(lexer, TOKEN_PIPE_PIPE);
			}
			return error_token(lexer, "lexer::unexpected-character", "unexpected character '|'");
		case '"':
			return scan_string(lexer);
		case '#':
			return scan_directive(lexer);
	}

	return error_token(lexer, "lexer::unexpected-character", "unexpected character");
}
