#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "lexer/token.h"

typedef struct ast_node ast_node_t;

typedef enum {
	AST_PROGRAM,

	AST_MODULE_IMPORT,
	AST_MODULE_DEF,

	AST_FUNCTION_DECL,
	AST_PARAM,
	AST_DECORATOR,

	AST_BLOCK,
	AST_VAR_DECL,
	AST_EXPORT_STMT,
	AST_IF_STMT,
	AST_WHILE_STMT,
	AST_FOR_STMT,
	AST_FOREACH_STMT,
	AST_BREAK_STMT,
	AST_CONTINUE_STMT,
	AST_RETURN_STMT,
	AST_EXPR_STMT,
	AST_THROW_STMT,

	AST_ASSIGN_EXPR,
	AST_BINARY_EXPR,
	AST_UNARY_EXPR,
	AST_CALL_EXPR,
	AST_MEMBER_EXPR,
	AST_INDEX_EXPR,
	AST_IDENTIFIER_EXPR,
	AST_INT_LITERAL,
	AST_FLOAT_LITERAL,
	AST_STRING_LITERAL,
	AST_BOOL_LITERAL,
	AST_NULL_LITERAL,
	AST_ARRAY_LITERAL,
	AST_TRY_EXPR,

	AST_STRUCT_DECL,
	AST_STRUCT_LITERAL,
	AST_STRUCT_FIELD_INIT,
} ast_kind_t;

typedef struct {
	ast_node_t **items;
	size_t count;
	size_t capacity;
} ast_list_t;

// A declared type: one of the base type keywords (TOKEN_INT, TOKEN_FLOAT,
// TOKEN_STRING, TOKEN_BOOL), TOKEN_IDENTIFIER naming a struct (in which
// case struct_name holds the identifier token), or TOKEN_FN naming a
// function type (in which case function_ref holds its param/return
// shape), optionally suffixed with one or more `[]` for "array of" that
// type (e.g. `int[]`, `Point[][]`, `fn(int)->int[]`). array_depth counts
// the trailing `[]` pairs, so `int[][]` is `int` with array_depth 2.
// `[]` always binds to the nearest preceding type (the `fn(...) -> T`
// return type, when one is written), so a nested array is always a
// depth count. There is still no way to spell "array of function
// type", which would need a grouping syntax the grammar lacks.
typedef struct type_ref type_ref_t;
typedef struct function_type_ref function_type_ref_t;

struct type_ref {
	token_type_t base_type;
	size_t array_depth;
	token_t struct_name;
	// Non-NULL iff base_type == TOKEN_FN. Heap-allocated (ast_free()
	// frees it) rather than embedded, since type_ref_t appears by value
	// all over the AST (params, var_decl, struct fields, ...) and most
	// of those never need it.
	function_type_ref_t *function_ref;
};

// `fn(param_types...) -> return_type`, or `fn(param_types...)` with
// has_return_type false. param_types holds bare types only, no names.
// this describes a type, not a parameter list (see AST_FUNCTION_DECL's
// own `params`, an ast_list_t of AST_PARAM nodes, for that). Recursive
// by construction: a function type's own return type can itself be a
// function type (`fn() -> fn(int) -> int`, a factory returning a
// factory), which is why return_type is a plain type_ref_t and not
// restricted to a base type.
struct function_type_ref {
	type_ref_t *param_types;
	size_t param_count;
	bool has_return_type;
	type_ref_t return_type;
};

struct ast_node {
	ast_kind_t kind;
	int line;
	int column;

	union {
		struct {
			// At most one, must precede every `imports` entry (enforced by
			// the parser, see parser_parse_program()). NULL for a file with
			// no `#moduledef`.
			ast_node_t *module_def;
			// Zero or more AST_MODULE_IMPORT nodes, one per `#module` line.
			// A moduledef file can carry its own imports now (that's what
			// makes a transitive import chain possible), unlike the old
			// single-directive design.
			ast_list_t imports;
			ast_list_t functions;
			ast_list_t structs;
			// Top-level global variable declarations (AST_VAR_DECL nodes,
			// the exact same node a local `type name = expr;` produces;
			// see var_decl below; the node itself carries no
			// function-body-specific assumptions). compiler.c's
			// register_globals() walks this before collect_signatures()
			// so a function body can forward-reference a global declared
			// later in the same file, the same guarantee functions
			// already give each other.
			ast_list_t globals;
		} program;

		struct {
			token_t name;
		} module_import;

		struct {
			token_t name;
		} module_def;

		struct {
			bool has_return_type;
			type_ref_t return_type;
			token_t name;
			ast_list_t params;
			ast_node_t *body;
			// Top-level-only (see parser_parse_program()): a nested fn's
			// function_decl always has an empty list, since parse_statement()
			// never looks for a leading '@'. Items are AST_DECORATOR nodes.
			ast_list_t decorators;
		} function_decl;

		// Reused for both function parameters (default_value always NULL)
		// and struct fields (default_value set when the field declares
		// `= expr`, NULL when the field is required at construction).
		struct {
			type_ref_t type;
			token_t name;
			ast_node_t *default_value;
		} param;

		struct {
			ast_list_t statements;
		} block;

		struct {
			type_ref_t type;
			token_t name;
			ast_node_t *initializer;
		} var_decl;

		struct {
			token_t name;
		} export_stmt;

		struct {
			ast_node_t *condition;
			ast_node_t *then_branch;
			ast_node_t *else_branch;
		} if_stmt;

		struct {
			ast_node_t *condition;
			ast_node_t *body;
		} while_stmt;

		struct {
			ast_node_t *initializer;
			ast_node_t *condition;
			ast_node_t *increment;
			ast_node_t *body;
		} for_stmt;

		// `for (type name : array_expr) body`, iterating array_expr's
		// elements (or a string's characters). Unlike the range form
		// (`for (type name : a..b)`), which desugars entirely at parse
		// time into an ordinary for_stmt, this needs a real AST node:
		// array_expr's element type is not known until it is
		// type-checked, so the compiler has to do this desugaring
		// itself, see compile_foreach_stmt() in compiler.c.
		struct {
			type_ref_t type;
			token_t name;
			ast_node_t *array_expr;
			ast_node_t *body;
		} foreach_stmt;

		struct {
			ast_node_t *value;
		} return_stmt;

		struct {
			ast_node_t *expression;
		} expr_stmt;

		struct {
			ast_node_t *value;
		} throw_stmt;

		struct {
			ast_node_t *target;
			ast_node_t *value;
			// TOKEN_EQUAL for plain `=`; one of the ten TOKEN_*_EQUAL
			// compound-assignment tokens (TOKEN_PLUS_EQUAL, ...) otherwise.
			// compiler.c's compile_assign_expr() branches on this per
			// target kind to emit a read-modify-write sequence instead of
			// desugaring at parse time, which would double-evaluate an
			// index/member target's sub-expressions (e.g. `arr[f()] += 1`
			// calling f() twice).
			token_type_t op;
		} assign_expr;

		struct {
			token_type_t op;
			ast_node_t *left;
			ast_node_t *right;
		} binary_expr;

		struct {
			token_type_t op;
			ast_node_t *operand;
		} unary_expr;

		struct {
			ast_node_t *callee;
			ast_list_t args;
		} call_expr;

		struct {
			ast_node_t *object;
			token_t name;
		} member_expr;

		struct {
			ast_node_t *object;
			ast_node_t *index;
		} index_expr;

		struct {
			token_t name;
		} identifier_expr;

		struct {
			token_t token;
		} literal;

		struct {
			ast_list_t elements;
		} array_literal;

		// Covers both spellings: the terse postfix `expr catch(TYPE
		// fallback)` and the full `try { ... } catch(...) { ... }` block
		// The former parses directly into this same shape
		// (try_body synthesized as a one-statement block holding an
		// implicit `return expr;`, has_binding false, catch_body NULL),
		// so the compiler only ever has one shape to handle.
		//
		// try_body is an AST_BLOCK. In expression position, its last
		// statement, if written as a bare expression with no trailing
		// `;`, is parsed as an implicit `return`. This is the one place in the
		// grammar a semicolon is ever optional, see parse_primary()'s
		// TOKEN_TRY case.
		//
		// has_binding/has_fallback are independent: a statement-position
		// try/catch commonly has a binding and no fallback (nothing
		// consumes the value); the terse postfix form always has a
		// fallback and no binding; the full "unified" form has both.
		// catch_body is NULL only for the postfix form; every other
		// combination requires at least an empty `{ }`, since a catch
		// clause with neither a fallback nor a body would do nothing.
		struct {
			ast_node_t *try_body;

			bool has_binding;
			type_ref_t binding_type;
			token_t binding_name;

			bool has_fallback;
			type_ref_t fallback_type;
			ast_node_t *fallback_expr;

			ast_node_t *catch_body;
		} try_expr;

		// Fields are AST_PARAM nodes (type, name, optional default_value).
		// functions are AST_FUNCTION_DECL nodes, same shape a top-level or
		// nested fn already uses (their own `decorators` list is always
		// empty here; parse_decorator() is only ever invoked from
		// parser_parse_program()'s own loop, never from here). Called only
		// as `TypeName.name(args)`; see compiler.c's compile_struct_call().
		struct {
			token_t name;
			ast_list_t fields;
			ast_list_t functions;
		} struct_decl;

		// `TypeName { field: value, ... }`, or a bare `{ field: value, ... }`
		// (has_type_name false, type_name unset) when the type is meant to
		// be inferred from context: a var_decl's declared type, a
		// return's declared type, an assignment's target, a call argument
		// with only one candidate overload, an array literal's element
		// type, or an enclosing struct literal's own field type. Fields
		// not listed here fall back to their struct declaration's default
		// (a compile error if the struct declares no default for that
		// field).
		struct {
			token_t type_name;
			bool has_type_name;
			ast_list_t fields;
		} struct_literal;

		struct {
			token_t name;
			ast_node_t *value;
		} struct_field_init;

		// `@name` or `@name("...")`, only ever attached to a top-level
		// function_decl's own decorators list. arg is NULL, or an
		// AST_STRING_LITERAL node holding the optional message.
		struct {
			token_t name;
			ast_node_t *arg;
		} decorator;
	} as;
};

void ast_list_init(ast_list_t *list);
void ast_list_push(ast_list_t *list, ast_node_t *node);
void ast_list_free(ast_list_t *list);

ast_node_t *ast_new_program(void);
ast_node_t *ast_new_module_import(token_t name, int line, int column);
ast_node_t *ast_new_module_def(token_t name, int line, int column);
ast_node_t *ast_new_function_decl(token_t name, bool has_return_type, type_ref_t return_type, ast_node_t *body, int line, int column);
ast_node_t *ast_new_param(type_ref_t type, token_t name, ast_node_t *default_value, int line, int column);
ast_node_t *ast_new_block(int line, int column);
ast_node_t *ast_new_var_decl(type_ref_t type, token_t name, ast_node_t *initializer, int line, int column);
ast_node_t *ast_new_export_stmt(token_t name, int line, int column);
ast_node_t *ast_new_if_stmt(ast_node_t *condition, ast_node_t *then_branch, ast_node_t *else_branch, int line, int column);
ast_node_t *ast_new_while_stmt(ast_node_t *condition, ast_node_t *body, int line, int column);
ast_node_t *ast_new_for_stmt(ast_node_t *initializer, ast_node_t *condition, ast_node_t *increment, ast_node_t *body, int line, int column);
ast_node_t *ast_new_foreach_stmt(type_ref_t type, token_t name, ast_node_t *array_expr, ast_node_t *body, int line, int column);
ast_node_t *ast_new_break_stmt(int line, int column);
ast_node_t *ast_new_continue_stmt(int line, int column);
ast_node_t *ast_new_return_stmt(ast_node_t *value, int line, int column);
ast_node_t *ast_new_expr_stmt(ast_node_t *expression, int line, int column);
ast_node_t *ast_new_throw_stmt(ast_node_t *value, int line, int column);

// try_expr's has_binding/binding_type/binding_name/has_fallback/
// fallback_type/fallback_expr/catch_body all default to
// false/unset/NULL; the caller fills in whichever apply, the same
// pattern ast_new_function_decl()'s caller already uses for
// has_return_type/return_type.
ast_node_t *ast_new_try_expr(ast_node_t *try_body, int line, int column);
ast_node_t *ast_new_assign_expr(ast_node_t *target, ast_node_t *value, token_type_t op, int line, int column);
ast_node_t *ast_new_binary_expr(token_type_t op, ast_node_t *left, ast_node_t *right, int line, int column);
ast_node_t *ast_new_unary_expr(token_type_t op, ast_node_t *operand, int line, int column);
ast_node_t *ast_new_call_expr(ast_node_t *callee, int line, int column);
ast_node_t *ast_new_member_expr(ast_node_t *object, token_t name, int line, int column);
ast_node_t *ast_new_index_expr(ast_node_t *object, ast_node_t *index, int line, int column);
ast_node_t *ast_new_identifier_expr(token_t name, int line, int column);
ast_node_t *ast_new_literal(ast_kind_t kind, token_t token, int line, int column);
ast_node_t *ast_new_array_literal(int line, int column);
ast_node_t *ast_new_struct_decl(token_t name, int line, int column);
ast_node_t *ast_new_struct_literal(token_t type_name, bool has_type_name, int line, int column);
ast_node_t *ast_new_struct_field_init(token_t name, ast_node_t *value, int line, int column);
ast_node_t *ast_new_decorator(token_t name, ast_node_t *arg, int line, int column);

void ast_free(ast_node_t *node);
void ast_dump(const ast_node_t *node, FILE *out);
