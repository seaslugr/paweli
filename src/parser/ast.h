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

// A declared type. base_type is a type keyword (TOKEN_INT,
// TOKEN_FLOAT, TOKEN_STRING, TOKEN_BOOL), TOKEN_IDENTIFIER for a
// struct (struct_name holds the name), or TOKEN_FN for a function type
// (function_ref holds its shape). array_depth counts trailing `[]`
// pairs: `int[][]` is `int` with depth 2. `[]` binds to the nearest
// type before it, which in a function type is the return type. So an
// array of functions cannot be written.
typedef struct type_ref type_ref_t;
typedef struct function_type_ref function_type_ref_t;

struct type_ref {
	token_type_t base_type;
	size_t array_depth;
	token_t struct_name;
	// Non-NULL only when base_type == TOKEN_FN. Heap-allocated, since
	// type_ref_t is stored by value all over the AST and most uses do not
	// need it. ast_free() frees it.
	function_type_ref_t *function_ref;
};

// `fn(param_types...) -> return_type`, or `fn(param_types...)` with
// has_return_type false. param_types holds types only, with no names.
// The return type can be a function type, as in
// `fn() -> fn(int) -> int`.
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
			// At most one, and it comes before every import (see
			// parser_parse_program()). NULL for a file with no `#moduledef`.
			ast_node_t *module_def;
			// One AST_MODULE_IMPORT node per `#module` line. A moduledef file can
			// have imports too.
			ast_list_t imports;
			ast_list_t functions;
			ast_list_t structs;
			// Top-level global variable declarations. They are AST_VAR_DECL nodes,
			// the same node a local declaration uses. The compiler registers them
			// before it collects signatures, so a function can use a global
			// declared later in the file.
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
			// AST_DECORATOR nodes. Only a top-level fn can have any (see
			// parser_parse_program()).
			ast_list_t decorators;
		} function_decl;

		// Used for function parameters, where default_value is NULL, and for
		// struct fields, where it is the `= expr` if the field has one.
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

		// `for (type name : array_expr) body`, over the elements of an array
		// or the characters of a string. The range form is rewritten into a
		// for_stmt at parse time. This form needs its own node, since the
		// element type is only known to the compiler (see
		// compile_foreach_stmt()).
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
			// TOKEN_EQUAL for `=`, or one of the ten TOKEN_*_EQUAL compound
			// tokens. compile_assign_expr() emits a read-modify-write sequence for
			// those. Rewriting `arr[f()] += 1` at parse time would call f() twice.
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

		// Both spellings: the postfix `expr catch(TYPE fallback)` and the full
		// `try { ... } catch(...) { ... }`. The postfix form has a try_body of
		// one implicit `return expr;`, no binding, and a NULL catch_body.
		//
		// try_body is an AST_BLOCK. As an expression, its last statement may
		// be a bare expression with no `;`, parsed as an implicit `return`.
		//
		// has_binding and has_fallback are independent. A statement-form
		// try/catch usually has a binding and no fallback. The postfix form
		// has a fallback and no binding. catch_body is NULL only for the
		// postfix form.
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

		// fields are AST_PARAM nodes. functions are AST_FUNCTION_DECL nodes
		// with an empty decorators list. A struct function is called only as
		// `TypeName.name(args)` (see compile_struct_call()).
		struct {
			token_t name;
			ast_list_t fields;
			ast_list_t functions;
		} struct_decl;

		// `TypeName { field: value, ... }`, or a bare `{ field: value, ... }`
		// with has_type_name false. The bare form takes its type from context:
		// a declared variable type, a return type, an assignment target, a
		// call argument with one candidate overload, an array literal's
		// element type, or the field type of an enclosing struct literal. A
		// field that is not listed takes its declared default. If it has none,
		// that is a compile error.
		struct {
			token_t type_name;
			bool has_type_name;
			ast_list_t fields;
		} struct_literal;

		struct {
			token_t name;
			ast_node_t *value;
		} struct_field_init;

		// `@name` or `@name("...")` on a top-level function. arg is NULL or an
		// AST_STRING_LITERAL node.
		struct {
			token_t name;
			ast_node_t *arg;
		} decorator;
	} as;
};

void ast_list_init(ast_list_t *list);
void ast_list_push(ast_list_t *list, ast_node_t *node);
void ast_list_free(ast_list_t *list);

ast_node_t *ast_new_program();
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

/* The binding, fallback, and catch_body fields start as false or NULL.
   The caller sets the ones that apply. */
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
