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

	AST_BLOCK,
	AST_VAR_DECL,
	AST_EXPORT_STMT,
	AST_IF_STMT,
	AST_WHILE_STMT,
	AST_FOR_STMT,
	AST_RETURN_STMT,
	AST_EXPR_STMT,

	AST_ASSIGN_EXPR,
	AST_BINARY_EXPR,
	AST_UNARY_EXPR,
	AST_CALL_EXPR,
	AST_MEMBER_EXPR,
	AST_IDENTIFIER_EXPR,
	AST_INT_LITERAL,
	AST_FLOAT_LITERAL,
	AST_STRING_LITERAL,
	AST_BOOL_LITERAL,
	AST_NULL_LITERAL,
} ast_kind_t;

typedef struct {
	ast_node_t **items;
	size_t count;
	size_t capacity;
} ast_list_t;

struct ast_node {
	ast_kind_t kind;
	int line;
	int column;

	union {
		struct {
			ast_node_t *directive;
			ast_list_t functions;
		} program;

		struct {
			token_t name;
		} module_import;

		struct {
			token_t name;
		} module_def;

		struct {
			bool has_return_type;
			token_type_t return_type;
			token_t name;
			ast_list_t params;
			ast_node_t *body;
		} function_decl;

		struct {
			token_type_t type;
			token_t name;
		} param;

		struct {
			ast_list_t statements;
		} block;

		struct {
			token_type_t type;
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

		struct {
			ast_node_t *value;
		} return_stmt;

		struct {
			ast_node_t *expression;
		} expr_stmt;

		struct {
			ast_node_t *target;
			ast_node_t *value;
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
			token_t name;
		} identifier_expr;

		struct {
			token_t token;
		} literal;
	} as;
};

void ast_list_init(ast_list_t *list);
void ast_list_push(ast_list_t *list, ast_node_t *node);
void ast_list_free(ast_list_t *list);

ast_node_t *ast_new_program(ast_node_t *directive);
ast_node_t *ast_new_module_import(token_t name, int line, int column);
ast_node_t *ast_new_module_def(token_t name, int line, int column);
ast_node_t *ast_new_function_decl(token_t name, bool has_return_type, token_type_t return_type, ast_node_t *body, int line, int column);
ast_node_t *ast_new_param(token_type_t type, token_t name, int line, int column);
ast_node_t *ast_new_block(int line, int column);
ast_node_t *ast_new_var_decl(token_type_t type, token_t name, ast_node_t *initializer, int line, int column);
ast_node_t *ast_new_export_stmt(token_t name, int line, int column);
ast_node_t *ast_new_if_stmt(ast_node_t *condition, ast_node_t *then_branch, ast_node_t *else_branch, int line, int column);
ast_node_t *ast_new_while_stmt(ast_node_t *condition, ast_node_t *body, int line, int column);
ast_node_t *ast_new_for_stmt(ast_node_t *initializer, ast_node_t *condition, ast_node_t *increment, ast_node_t *body, int line, int column);
ast_node_t *ast_new_return_stmt(ast_node_t *value, int line, int column);
ast_node_t *ast_new_expr_stmt(ast_node_t *expression, int line, int column);
ast_node_t *ast_new_assign_expr(ast_node_t *target, ast_node_t *value, int line, int column);
ast_node_t *ast_new_binary_expr(token_type_t op, ast_node_t *left, ast_node_t *right, int line, int column);
ast_node_t *ast_new_unary_expr(token_type_t op, ast_node_t *operand, int line, int column);
ast_node_t *ast_new_call_expr(ast_node_t *callee, int line, int column);
ast_node_t *ast_new_member_expr(ast_node_t *object, token_t name, int line, int column);
ast_node_t *ast_new_identifier_expr(token_t name, int line, int column);
ast_node_t *ast_new_literal(ast_kind_t kind, token_t token, int line, int column);

void ast_free(ast_node_t *node);
void ast_dump(const ast_node_t *node, FILE *out);
