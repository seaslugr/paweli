#include <stdlib.h>

#include "ast.h"

static ast_node_t *ast_alloc(ast_kind_t kind, int line, int column);
static void print_indent(FILE *out, int depth);
static void dump_node(const ast_node_t *node, FILE *out, int depth);

static ast_node_t *ast_alloc(ast_kind_t kind, int line, int column) {
	ast_node_t *node;

	node = malloc(sizeof(ast_node_t));
	node->kind = kind;
	node->line = line;
	node->column = column;
	return node;
}

void ast_list_init(ast_list_t *list) {
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}

void ast_list_push(ast_list_t *list, ast_node_t *node) {
	if (list->count == list->capacity) {
		list->capacity = list->capacity < 8 ? 8 : list->capacity * 2;
		list->items = realloc(list->items, list->capacity * sizeof(ast_node_t *));
	}
	list->items[list->count] = node;
	list->count++;
}

void ast_list_free(ast_list_t *list) {
	size_t i;

	for (i = 0; i < list->count; i++) {
		ast_free(list->items[i]);
	}
	free(list->items);
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}

ast_node_t *ast_new_program(ast_node_t *directive) {
	ast_node_t *node = ast_alloc(AST_PROGRAM, 1, 1);

	node->as.program.directive = directive;
	ast_list_init(&node->as.program.functions);
	return node;
}

ast_node_t *ast_new_module_import(token_t name, int line, int column) {
	ast_node_t *node = ast_alloc(AST_MODULE_IMPORT, line, column);

	node->as.module_import.name = name;
	return node;
}

ast_node_t *ast_new_module_def(token_t name, int line, int column) {
	ast_node_t *node = ast_alloc(AST_MODULE_DEF, line, column);

	node->as.module_def.name = name;
	return node;
}

ast_node_t *ast_new_function_decl(token_t name, bool has_return_type, token_type_t return_type, ast_node_t *body, int line, int column) {
	ast_node_t *node = ast_alloc(AST_FUNCTION_DECL, line, column);

	node->as.function_decl.name = name;
	node->as.function_decl.has_return_type = has_return_type;
	node->as.function_decl.return_type = return_type;
	node->as.function_decl.body = body;
	ast_list_init(&node->as.function_decl.params);
	return node;
}

ast_node_t *ast_new_param(token_type_t type, token_t name, int line, int column) {
	ast_node_t *node = ast_alloc(AST_PARAM, line, column);

	node->as.param.type = type;
	node->as.param.name = name;
	return node;
}

ast_node_t *ast_new_block(int line, int column) {
	ast_node_t *node = ast_alloc(AST_BLOCK, line, column);

	ast_list_init(&node->as.block.statements);
	return node;
}

ast_node_t *ast_new_var_decl(token_type_t type, token_t name, ast_node_t *initializer, int line, int column) {
	ast_node_t *node = ast_alloc(AST_VAR_DECL, line, column);

	node->as.var_decl.type = type;
	node->as.var_decl.name = name;
	node->as.var_decl.initializer = initializer;
	return node;
}

ast_node_t *ast_new_export_stmt(token_t name, int line, int column) {
	ast_node_t *node = ast_alloc(AST_EXPORT_STMT, line, column);

	node->as.export_stmt.name = name;
	return node;
}

ast_node_t *ast_new_if_stmt(ast_node_t *condition, ast_node_t *then_branch, ast_node_t *else_branch, int line, int column) {
	ast_node_t *node = ast_alloc(AST_IF_STMT, line, column);

	node->as.if_stmt.condition = condition;
	node->as.if_stmt.then_branch = then_branch;
	node->as.if_stmt.else_branch = else_branch;
	return node;
}

ast_node_t *ast_new_while_stmt(ast_node_t *condition, ast_node_t *body, int line, int column) {
	ast_node_t *node = ast_alloc(AST_WHILE_STMT, line, column);

	node->as.while_stmt.condition = condition;
	node->as.while_stmt.body = body;
	return node;
}

ast_node_t *ast_new_for_stmt(ast_node_t *initializer, ast_node_t *condition, ast_node_t *increment, ast_node_t *body, int line, int column) {
	ast_node_t *node = ast_alloc(AST_FOR_STMT, line, column);

	node->as.for_stmt.initializer = initializer;
	node->as.for_stmt.condition = condition;
	node->as.for_stmt.increment = increment;
	node->as.for_stmt.body = body;
	return node;
}

ast_node_t *ast_new_return_stmt(ast_node_t *value, int line, int column) {
	ast_node_t *node = ast_alloc(AST_RETURN_STMT, line, column);

	node->as.return_stmt.value = value;
	return node;
}

ast_node_t *ast_new_expr_stmt(ast_node_t *expression, int line, int column) {
	ast_node_t *node = ast_alloc(AST_EXPR_STMT, line, column);

	node->as.expr_stmt.expression = expression;
	return node;
}

ast_node_t *ast_new_assign_expr(ast_node_t *target, ast_node_t *value, int line, int column) {
	ast_node_t *node = ast_alloc(AST_ASSIGN_EXPR, line, column);

	node->as.assign_expr.target = target;
	node->as.assign_expr.value = value;
	return node;
}

ast_node_t *ast_new_binary_expr(token_type_t op, ast_node_t *left, ast_node_t *right, int line, int column) {
	ast_node_t *node = ast_alloc(AST_BINARY_EXPR, line, column);

	node->as.binary_expr.op = op;
	node->as.binary_expr.left = left;
	node->as.binary_expr.right = right;
	return node;
}

ast_node_t *ast_new_unary_expr(token_type_t op, ast_node_t *operand, int line, int column) {
	ast_node_t *node = ast_alloc(AST_UNARY_EXPR, line, column);

	node->as.unary_expr.op = op;
	node->as.unary_expr.operand = operand;
	return node;
}

ast_node_t *ast_new_call_expr(ast_node_t *callee, int line, int column) {
	ast_node_t *node = ast_alloc(AST_CALL_EXPR, line, column);

	node->as.call_expr.callee = callee;
	ast_list_init(&node->as.call_expr.args);
	return node;
}

ast_node_t *ast_new_member_expr(ast_node_t *object, token_t name, int line, int column) {
	ast_node_t *node = ast_alloc(AST_MEMBER_EXPR, line, column);

	node->as.member_expr.object = object;
	node->as.member_expr.name = name;
	return node;
}

ast_node_t *ast_new_identifier_expr(token_t name, int line, int column) {
	ast_node_t *node = ast_alloc(AST_IDENTIFIER_EXPR, line, column);

	node->as.identifier_expr.name = name;
	return node;
}

ast_node_t *ast_new_literal(ast_kind_t kind, token_t token, int line, int column) {
	ast_node_t *node = ast_alloc(kind, line, column);

	node->as.literal.token = token;
	return node;
}

void ast_free(ast_node_t *node) {
	if (node == NULL) {
		return;
	}

	switch (node->kind) {
		case AST_PROGRAM:
			ast_free(node->as.program.directive);
			ast_list_free(&node->as.program.functions);
			break;
		case AST_MODULE_IMPORT:
		case AST_MODULE_DEF:
			break;
		case AST_FUNCTION_DECL:
			ast_list_free(&node->as.function_decl.params);
			ast_free(node->as.function_decl.body);
			break;
		case AST_PARAM:
			break;
		case AST_BLOCK:
			ast_list_free(&node->as.block.statements);
			break;
		case AST_VAR_DECL:
			ast_free(node->as.var_decl.initializer);
			break;
		case AST_EXPORT_STMT:
			break;
		case AST_IF_STMT:
			ast_free(node->as.if_stmt.condition);
			ast_free(node->as.if_stmt.then_branch);
			ast_free(node->as.if_stmt.else_branch);
			break;
		case AST_WHILE_STMT:
			ast_free(node->as.while_stmt.condition);
			ast_free(node->as.while_stmt.body);
			break;
		case AST_FOR_STMT:
			ast_free(node->as.for_stmt.initializer);
			ast_free(node->as.for_stmt.condition);
			ast_free(node->as.for_stmt.increment);
			ast_free(node->as.for_stmt.body);
			break;
		case AST_RETURN_STMT:
			ast_free(node->as.return_stmt.value);
			break;
		case AST_EXPR_STMT:
			ast_free(node->as.expr_stmt.expression);
			break;
		case AST_ASSIGN_EXPR:
			ast_free(node->as.assign_expr.target);
			ast_free(node->as.assign_expr.value);
			break;
		case AST_BINARY_EXPR:
			ast_free(node->as.binary_expr.left);
			ast_free(node->as.binary_expr.right);
			break;
		case AST_UNARY_EXPR:
			ast_free(node->as.unary_expr.operand);
			break;
		case AST_CALL_EXPR:
			ast_free(node->as.call_expr.callee);
			ast_list_free(&node->as.call_expr.args);
			break;
		case AST_MEMBER_EXPR:
			ast_free(node->as.member_expr.object);
			break;
		case AST_IDENTIFIER_EXPR:
		case AST_INT_LITERAL:
		case AST_FLOAT_LITERAL:
		case AST_STRING_LITERAL:
		case AST_BOOL_LITERAL:
		case AST_NULL_LITERAL:
			break;
	}

	free(node);
}

static void print_indent(FILE *out, int depth) {
	int i;

	for (i = 0; i < depth; i++) {
		fputs("  ", out);
	}
}

static void dump_node(const ast_node_t *node, FILE *out, int depth) {
	size_t i;

	if (node == NULL) {
		return;
	}

	print_indent(out, depth);

	switch (node->kind) {
		case AST_PROGRAM:
			fprintf(out, "Program (%d:%d)\n", node->line, node->column);
			dump_node(node->as.program.directive, out, depth + 1);
			for (i = 0; i < node->as.program.functions.count; i++) {
				dump_node(node->as.program.functions.items[i], out, depth + 1);
			}
			break;

		case AST_MODULE_IMPORT:
			fprintf(out, "ModuleImport %.*s (%d:%d)\n", (int)node->as.module_import.name.length, node->as.module_import.name.start, node->line, node->column);
			break;

		case AST_MODULE_DEF:
			fprintf(out, "ModuleDef %.*s (%d:%d)\n", (int)node->as.module_def.name.length, node->as.module_def.name.start, node->line, node->column);
			break;

		case AST_FUNCTION_DECL:
			fprintf(out, "FunctionDecl %.*s -> %s (%d:%d)\n",
				(int)node->as.function_decl.name.length, node->as.function_decl.name.start,
				node->as.function_decl.has_return_type ? token_type_name(node->as.function_decl.return_type) : "void",
				node->line, node->column);
			for (i = 0; i < node->as.function_decl.params.count; i++) {
				dump_node(node->as.function_decl.params.items[i], out, depth + 1);
			}
			dump_node(node->as.function_decl.body, out, depth + 1);
			break;

		case AST_PARAM:
			fprintf(out, "Param %s %.*s (%d:%d)\n",
				token_type_name(node->as.param.type),
				(int)node->as.param.name.length, node->as.param.name.start,
				node->line, node->column);
			break;

		case AST_BLOCK:
			fprintf(out, "Block (%d:%d)\n", node->line, node->column);
			for (i = 0; i < node->as.block.statements.count; i++) {
				dump_node(node->as.block.statements.items[i], out, depth + 1);
			}
			break;

		case AST_VAR_DECL:
			fprintf(out, "VarDecl %s %.*s (%d:%d)\n",
				token_type_name(node->as.var_decl.type),
				(int)node->as.var_decl.name.length, node->as.var_decl.name.start,
				node->line, node->column);
			dump_node(node->as.var_decl.initializer, out, depth + 1);
			break;

		case AST_EXPORT_STMT:
			fprintf(out, "ExportStmt %.*s (%d:%d)\n",
				(int)node->as.export_stmt.name.length, node->as.export_stmt.name.start,
				node->line, node->column);
			break;

		case AST_IF_STMT:
			fprintf(out, "IfStmt (%d:%d)\n", node->line, node->column);
			dump_node(node->as.if_stmt.condition, out, depth + 1);
			dump_node(node->as.if_stmt.then_branch, out, depth + 1);
			dump_node(node->as.if_stmt.else_branch, out, depth + 1);
			break;

		case AST_WHILE_STMT:
			fprintf(out, "WhileStmt (%d:%d)\n", node->line, node->column);
			dump_node(node->as.while_stmt.condition, out, depth + 1);
			dump_node(node->as.while_stmt.body, out, depth + 1);
			break;

		case AST_FOR_STMT:
			fprintf(out, "ForStmt (%d:%d)\n", node->line, node->column);
			dump_node(node->as.for_stmt.initializer, out, depth + 1);
			dump_node(node->as.for_stmt.condition, out, depth + 1);
			dump_node(node->as.for_stmt.increment, out, depth + 1);
			dump_node(node->as.for_stmt.body, out, depth + 1);
			break;

		case AST_RETURN_STMT:
			fprintf(out, "ReturnStmt (%d:%d)\n", node->line, node->column);
			dump_node(node->as.return_stmt.value, out, depth + 1);
			break;

		case AST_EXPR_STMT:
			fprintf(out, "ExprStmt (%d:%d)\n", node->line, node->column);
			dump_node(node->as.expr_stmt.expression, out, depth + 1);
			break;

		case AST_ASSIGN_EXPR:
			fprintf(out, "Assign (%d:%d)\n", node->line, node->column);
			dump_node(node->as.assign_expr.target, out, depth + 1);
			dump_node(node->as.assign_expr.value, out, depth + 1);
			break;

		case AST_BINARY_EXPR:
			fprintf(out, "Binary %s (%d:%d)\n", token_type_name(node->as.binary_expr.op), node->line, node->column);
			dump_node(node->as.binary_expr.left, out, depth + 1);
			dump_node(node->as.binary_expr.right, out, depth + 1);
			break;

		case AST_UNARY_EXPR:
			fprintf(out, "Unary %s (%d:%d)\n", token_type_name(node->as.unary_expr.op), node->line, node->column);
			dump_node(node->as.unary_expr.operand, out, depth + 1);
			break;

		case AST_CALL_EXPR:
			fprintf(out, "Call (%d:%d)\n", node->line, node->column);
			dump_node(node->as.call_expr.callee, out, depth + 1);
			for (i = 0; i < node->as.call_expr.args.count; i++) {
				dump_node(node->as.call_expr.args.items[i], out, depth + 1);
			}
			break;

		case AST_MEMBER_EXPR:
			fprintf(out, "Member .%.*s (%d:%d)\n",
				(int)node->as.member_expr.name.length, node->as.member_expr.name.start,
				node->line, node->column);
			dump_node(node->as.member_expr.object, out, depth + 1);
			break;

		case AST_IDENTIFIER_EXPR:
			fprintf(out, "Identifier %.*s (%d:%d)\n",
				(int)node->as.identifier_expr.name.length, node->as.identifier_expr.name.start,
				node->line, node->column);
			break;

		case AST_INT_LITERAL:
			fprintf(out, "Int %.*s (%d:%d)\n", (int)node->as.literal.token.length, node->as.literal.token.start, node->line, node->column);
			break;

		case AST_FLOAT_LITERAL:
			fprintf(out, "Float %.*s (%d:%d)\n", (int)node->as.literal.token.length, node->as.literal.token.start, node->line, node->column);
			break;

		case AST_STRING_LITERAL:
			fprintf(out, "String %.*s (%d:%d)\n", (int)node->as.literal.token.length, node->as.literal.token.start, node->line, node->column);
			break;

		case AST_BOOL_LITERAL:
			fprintf(out, "Bool %.*s (%d:%d)\n", (int)node->as.literal.token.length, node->as.literal.token.start, node->line, node->column);
			break;

		case AST_NULL_LITERAL:
			fprintf(out, "Null (%d:%d)\n", node->line, node->column);
			break;
	}
}

void ast_dump(const ast_node_t *node, FILE *out) {
	dump_node(node, out, 0);
}
