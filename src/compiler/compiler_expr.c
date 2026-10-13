#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler_internal.h"

// Builtin int constants, resolved as identifiers in
// compile_identifier_expr() after everything else misses, so a user
// global of the same name wins. paweli has one int type, a signed
// 64-bit value. These only name the range limits of narrower widths.
typedef struct {
	const char *name;
	int64_t value;
} int_limit_const_t;

static const int_limit_const_t g_int_limit_consts[] = {
	{"INT8_MIN", INT8_MIN}, {"INT8_MAX", INT8_MAX},
	{"INT16_MIN", INT16_MIN}, {"INT16_MAX", INT16_MAX},
	{"INT32_MIN", INT32_MIN}, {"INT32_MAX", INT32_MAX},
	{"INT64_MIN", INT64_MIN}, {"INT64_MAX", INT64_MAX},
};

static type_t compile_compound_assign_value(compiler_t *compiler, const ast_node_t *node, type_t target_type, const ast_node_t *target_node);
static uint8_t compound_assign_opcode(token_type_t op);
static type_t check_arithmetic_or_bitwise(compiler_t *compiler, uint8_t opcode, type_t left_type, type_t right_type, const ast_node_t *left_node, const ast_node_t *right_node, int line, int column);
static type_t compile_binary_expr(compiler_t *compiler, const ast_node_t *node);
static type_t compile_logical_expr(compiler_t *compiler, const ast_node_t *node);
static type_t compile_index_expr(compiler_t *compiler, const ast_node_t *node);
static type_t compile_array_literal(compiler_t *compiler, const ast_node_t *node, const type_t *expected);
static bool signature_matches_func_type(const signature_t *sig, const func_type_t *ft);
static const int_limit_const_t *find_int_limit_const(const char *name, size_t name_length);
static int resolve_struct_field(compiler_t *compiler, const ast_node_t *node, const type_t *object_type, const char *name, size_t name_length, const struct_def_t **out_def);

type_t compile_expr_typed(compiler_t *compiler, const ast_node_t *node, const type_t *expected) {
	obj_string_t *string;

	switch (node->kind) {
		case AST_ASSIGN_EXPR:
			return compile_assign_expr(compiler, node);
		case AST_BINARY_EXPR:
			if (node->as.binary_expr.op == TOKEN_AMP_AMP || node->as.binary_expr.op == TOKEN_PIPE_PIPE) {
				return compile_logical_expr(compiler, node);
			}
			return compile_binary_expr(compiler, node);
		case AST_UNARY_EXPR:
			return compile_unary_expr(compiler, node);
		case AST_CALL_EXPR:
			return compile_call_expr(compiler, node);
		case AST_MEMBER_EXPR:
			return compile_member_expr(compiler, node);
		case AST_INDEX_EXPR:
			return compile_index_expr(compiler, node);
		case AST_ARRAY_LITERAL:
			return compile_array_literal(compiler, node, expected);
		case AST_STRUCT_LITERAL:
			return compile_struct_literal(compiler, node, expected);
		case AST_IDENTIFIER_EXPR:
			return compile_identifier_expr(compiler, node, expected);
		case AST_FUNCTION_DECL:
			return compile_lambda_expr(compiler, node);
		case AST_TRY_EXPR:
			return compile_try_expr(compiler, node);
		case AST_INT_LITERAL:
			compiler_emit_constant(compiler, value_int(compiler_parse_int_literal(compiler, node->as.literal.token, node->line, node->column)), node->line, node->column);
			return type_int();
		case AST_FLOAT_LITERAL:
			compiler_emit_constant(compiler, value_float(compiler_parse_float_literal(node->as.literal.token)), node->line, node->column);
			return type_float();
		case AST_STRING_LITERAL:
			string = object_new_string_from_literal(compiler->heap, compiler->strings, node->as.literal.token.start + 1, node->as.literal.token.length - 2);
			compiler_emit_constant(compiler, value_obj((obj_t *)string), node->line, node->column);
			return type_string();
		case AST_BOOL_LITERAL:
			compiler_emit_byte(compiler, node->as.literal.token.type == TOKEN_TRUE ? OP_TRUE : OP_FALSE, node->line, node->column);
			return type_bool();
		case AST_NULL_LITERAL:
			compiler_emit_byte(compiler, OP_NULL, node->line, node->column);
			return type_null();
		default:
			compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-expression", "expected an expression");
			return type_void();
	}
}

/* The shared tail of a compound assignment (`x += y`, `arr[i] &= y`,
   `s.f <<= y`). The caller has already pushed the target's current
   value, and for an index or member target it used OP_DUP_N so the
   sub-expressions run once. This compiles the right-hand side, emits
   the binary opcode, and checks the result against target_type. The
   caller emits the final OP_SET_*. Returns target_type. */
static type_t compile_compound_assign_value(compiler_t *compiler, const ast_node_t *node, type_t target_type, const ast_node_t *target_node) {
	type_t value_type;
	uint8_t opcode;
	type_t result_type;

	value_type = compile_expr_typed(compiler, node->as.assign_expr.value, NULL);
	opcode = compound_assign_opcode(node->as.assign_expr.op);
	result_type = check_arithmetic_or_bitwise(compiler, opcode, target_type, value_type, target_node, node->as.assign_expr.value, node->line, node->column);
	compiler_emit_byte(compiler, opcode, node->line, node->column);

	if (!type_is_assignable(target_type, result_type)) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot assign a %s result back into a %s target", type_to_string(result_type), type_to_string(target_type));
	}

	return target_type;
}

type_t compile_assign_expr(compiler_t *compiler, const ast_node_t *node) {
	int slot;
	type_t target_type;
	type_t value_type;
	type_t object_type;
	type_t index_type;
	type_t element_expected;
	const ast_node_t *target;
	int field_index;
	const struct_def_t *def;
	bool is_compound;

	target = node->as.assign_expr.target;
	is_compound = node->as.assign_expr.op != TOKEN_EQUAL;

	if (target->kind == AST_IDENTIFIER_EXPR) {
		slot = compiler_resolve_local(compiler, target->as.identifier_expr.name);
		if (slot == -1) {
			int upvalue = compiler_resolve_upvalue(compiler, target->as.identifier_expr.name);

			if (upvalue != -1) {
				target_type = compiler->upvalues[upvalue].type;
				if (is_compound) {
					compiler_emit_bytes(compiler, OP_GET_UPVALUE, (uint8_t)upvalue, node->line, node->column);
					compile_compound_assign_value(compiler, node, target_type, target);
				} else {
					value_type = compile_expr_typed(compiler, node->as.assign_expr.value, &target_type);
					if (!type_is_assignable(target_type, value_type)) {
						compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot assign a %s value to a %s variable", type_to_string(value_type), type_to_string(target_type));
					}
				}
				compiler_emit_bytes(compiler, OP_SET_UPVALUE, (uint8_t)upvalue, node->line, node->column);
				return target_type;
			}

			{
				const global_def_t *global_def = compiler_global_registry_find(compiler->globals, target->as.identifier_expr.name.start, target->as.identifier_expr.name.length);

				if (global_def != NULL) {
					target_type = global_def->type;
					if (is_compound) {
						compiler_emit_name_op_str(compiler, OP_GET_GLOBAL, target->as.identifier_expr.name.start, target->as.identifier_expr.name.length, node->line, node->column);
						compile_compound_assign_value(compiler, node, target_type, target);
					} else {
						value_type = compile_expr_typed(compiler, node->as.assign_expr.value, &target_type);
						if (!type_is_assignable(target_type, value_type)) {
							compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot assign a %s value to a %s variable", type_to_string(value_type), type_to_string(target_type));
						}
					}
					compiler_emit_name_op_str(compiler, OP_SET_GLOBAL, target->as.identifier_expr.name.start, target->as.identifier_expr.name.length, node->line, node->column);
					return target_type;
				}
			}

			compiler_error(compiler, node->line, node->column, 0, "compiler::undeclared-assignment-target", "cannot assign to an undeclared or non-local variable");
			compile_expr_typed(compiler, node->as.assign_expr.value, NULL);
			return type_void();
		}
		target_type = compiler->locals[slot].type;
		if (is_compound) {
			compiler_emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)slot, node->line, node->column);
			compile_compound_assign_value(compiler, node, target_type, target);
		} else {
			value_type = compile_expr_typed(compiler, node->as.assign_expr.value, &target_type);
			if (!type_is_assignable(target_type, value_type)) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot assign a %s value to a %s variable", type_to_string(value_type), type_to_string(target_type));
			}
			// `x = null;` disqualifies a register-cached scalar, like a `null`
			// initializer (see compiler_disqualify_local_reg_slot()). A compound
			// assignment never yields TYPE_NULL, so only this branch checks.
			if (value_type.kind == TYPE_NULL) {
				value_type_t vtype;

				if (compiler_register_cacheable_value_type(target_type, &vtype) && vtype != VAL_OBJ) {
					compiler_disqualify_local_reg_slot(compiler, slot);
				}
			}
		}
		compiler_emit_bytes(compiler, OP_SET_LOCAL, (uint8_t)slot, node->line, node->column);
		return target_type;
	}

	if (target->kind == AST_INDEX_EXPR) {
		object_type = compile_expr_typed(compiler, target->as.index_expr.object, NULL);

		if (type_equals(object_type, type_string())) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::string-immutable", "cannot assign into a string index; strings are immutable");
		} else if (object_type.array_depth == 0) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::not-indexable", "'[]' assignment only works on arrays, got %s", type_to_string(object_type));
		}

		element_expected = type_int();
		index_type = compile_expr_typed(compiler, target->as.index_expr.index, &element_expected);
		if (!type_equals(index_type, type_int())) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::index-not-integer", "array index must be an int");
		}

		element_expected = type_element_of(object_type);

		if (is_compound) {
			// [object, index] -> [object, index, object, index] -> [object, index, current]
			compiler_emit_bytes(compiler, OP_DUP_N, 2, node->line, node->column);
			compiler_emit_byte(compiler, OP_GET_INDEX, node->line, node->column);
			value_type = compile_compound_assign_value(compiler, node, element_expected, target);
		} else if (object_type.array_depth > 0) {
			value_type = compile_expr_typed(compiler, node->as.assign_expr.value, &element_expected);
			if (!type_is_assignable(element_expected, value_type)) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot assign a %s into an array of %s", type_to_string(value_type), type_to_string(element_expected));
			}
		} else {
			value_type = compile_expr_typed(compiler, node->as.assign_expr.value, NULL);
		}

		compiler_emit_byte(compiler, OP_SET_INDEX, node->line, node->column);
		return value_type;
	}

	if (target->kind == AST_MEMBER_EXPR) {
		object_type = compile_expr_typed(compiler, target->as.member_expr.object, NULL);

		if (object_type.kind != TYPE_STRUCT || object_type.array_depth > 0) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-member-assignment", "'.' assignment only works on struct fields");
			compile_expr_typed(compiler, node->as.assign_expr.value, NULL);
			return type_void();
		}

		field_index = resolve_struct_field(compiler, target, &object_type, target->as.member_expr.name.start, target->as.member_expr.name.length, &def);
		if (field_index < 0) {
			compile_expr_typed(compiler, node->as.assign_expr.value, NULL);
			return type_void();
		}

		target_type = def->fields[field_index].type;

		if (is_compound) {
			// [object] -> [object, object] -> [object, current]
			compiler_emit_bytes(compiler, OP_DUP_N, 1, node->line, node->column);
			compiler_emit_bytes(compiler, OP_GET_FIELD, (uint8_t)field_index, node->line, node->column);
			compile_compound_assign_value(compiler, node, target_type, target);
		} else {
			value_type = compile_expr_typed(compiler, node->as.assign_expr.value, &target_type);
			if (!type_is_assignable(target_type, value_type)) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot assign a %s value to field '%.*s' of type %s", type_to_string(value_type), (int)def->fields[field_index].name_length, def->fields[field_index].name, type_to_string(target_type));
			}
		}

		compiler_emit_bytes(compiler, OP_SET_FIELD, (uint8_t)field_index, node->line, node->column);
		return target_type;
	}

	compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-member-assignment", "assignment to member expressions is not supported yet");
	compile_expr_typed(compiler, node->as.assign_expr.value, NULL);
	return type_void();
}

/* Maps a compound-assignment token to its binary opcode. It is only
   called with one of the ten TOKEN_*_EQUAL tokens. */
static uint8_t compound_assign_opcode(token_type_t op) {
	switch (op) {
		case TOKEN_PLUS_EQUAL: return OP_ADD;
		case TOKEN_MINUS_EQUAL: return OP_SUBTRACT;
		case TOKEN_STAR_EQUAL: return OP_MULTIPLY;
		case TOKEN_SLASH_EQUAL: return OP_DIVIDE;
		case TOKEN_PERCENT_EQUAL: return OP_MODULO;
		case TOKEN_AMP_EQUAL: return OP_BIT_AND;
		case TOKEN_PIPE_EQUAL: return OP_BIT_OR;
		case TOKEN_CARET_EQUAL: return OP_BIT_XOR;
		case TOKEN_LESS_LESS_EQUAL: return OP_SHIFT_LEFT;
		case TOKEN_GREATER_GREATER_EQUAL: return OP_SHIFT_RIGHT;
		default: return OP_ADD;
	}
}

/* Type-checks an arithmetic, bitwise, or shift operation and returns
   the result type. A mismatch is reported at the position of the
   operand that is wrong. line and column are only used for modulo on a
   float, which belongs to neither operand. Shared by
   compile_binary_expr() and the compound-assignment branches of
   compile_assign_expr(). */
static type_t check_arithmetic_or_bitwise(compiler_t *compiler, uint8_t opcode, type_t left_type, type_t right_type, const ast_node_t *left_node, const ast_node_t *right_node, int line, int column) {
	bool is_bitwise;

	// Bitwise and shift operators take ints only. A float operand is a
	// type error.
	is_bitwise = opcode == OP_BIT_AND || opcode == OP_BIT_OR || opcode == OP_BIT_XOR || opcode == OP_SHIFT_LEFT || opcode == OP_SHIFT_RIGHT;
	if (is_bitwise) {
		if (!type_equals(left_type, type_int())) {
			compiler_error(compiler, left_node->line, left_node->column, 0, "compiler::type-mismatch", "left operand must be int, got %s", type_to_string(left_type));
		}
		if (!type_equals(right_type, type_int())) {
			compiler_error(compiler, right_node->line, right_node->column, 0, "compiler::type-mismatch", "right operand must be int, got %s", type_to_string(right_type));
		}
		return type_int();
	}

	// `+` on two strings concatenates, like OP_ADD in binary_op() (vm.c).
	if (opcode == OP_ADD && type_equals(left_type, type_string()) && type_equals(right_type, type_string())) {
		return type_string();
	}

	if (!type_is_numeric(left_type)) {
		compiler_error(compiler, left_node->line, left_node->column, 0, "compiler::type-mismatch", "left operand must be int or float, got %s", type_to_string(left_type));
	}
	if (!type_is_numeric(right_type)) {
		compiler_error(compiler, right_node->line, right_node->column, 0, "compiler::type-mismatch", "right operand must be int or float, got %s", type_to_string(right_type));
	}

	if (opcode == OP_MODULO && (left_type.kind == TYPE_FLOAT || right_type.kind == TYPE_FLOAT)) {
		compiler_error(compiler, line, column, 0, "compiler::type-mismatch", "modulo requires int operands, got %s and %s", type_to_string(left_type), type_to_string(right_type));
		return type_int();
	}

	// Arithmetic gives int if both sides are int and float if either is
	// float, like binary_op() (vm.c).
	if (left_type.kind == TYPE_FLOAT || right_type.kind == TYPE_FLOAT) {
		return type_float();
	}
	return type_int();
}

static type_t compile_binary_expr(compiler_t *compiler, const ast_node_t *node) {
	type_t left_type;
	type_t right_type;
	uint8_t opcode;
	bool is_equality;
	bool is_comparison;

	left_type = compile_expr_typed(compiler, node->as.binary_expr.left, NULL);
	right_type = compile_expr_typed(compiler, node->as.binary_expr.right, NULL);

	is_equality = node->as.binary_expr.op == TOKEN_EQUAL_EQUAL || node->as.binary_expr.op == TOKEN_BANG_EQUAL;

	switch (node->as.binary_expr.op) {
		case TOKEN_PLUS: opcode = OP_ADD; break;
		case TOKEN_MINUS: opcode = OP_SUBTRACT; break;
		case TOKEN_STAR: opcode = OP_MULTIPLY; break;
		case TOKEN_SLASH: opcode = OP_DIVIDE; break;
		case TOKEN_PERCENT: opcode = OP_MODULO; break;
		case TOKEN_EQUAL_EQUAL: opcode = OP_EQUAL; break;
		case TOKEN_BANG_EQUAL: opcode = OP_NOT_EQUAL; break;
		case TOKEN_LESS: opcode = OP_LESS; break;
		case TOKEN_LESS_EQUAL: opcode = OP_LESS_EQUAL; break;
		case TOKEN_GREATER: opcode = OP_GREATER; break;
		case TOKEN_GREATER_EQUAL: opcode = OP_GREATER_EQUAL; break;
		case TOKEN_AMP: opcode = OP_BIT_AND; break;
		case TOKEN_PIPE: opcode = OP_BIT_OR; break;
		case TOKEN_CARET: opcode = OP_BIT_XOR; break;
		case TOKEN_LESS_LESS: opcode = OP_SHIFT_LEFT; break;
		case TOKEN_GREATER_GREATER: opcode = OP_SHIFT_RIGHT; break;
		default:
			compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-binary-operator", "unsupported binary operator");
			compiler_emit_byte(compiler, OP_EQUAL, node->line, node->column);
			return type_bool();
	}

	compiler_emit_byte(compiler, opcode, node->line, node->column);

	if (is_equality) {
		bool numeric_pair = type_is_numeric(left_type) && type_is_numeric(right_type);
		bool either_null = left_type.kind == TYPE_NULL || right_type.kind == TYPE_NULL;
		if (!type_equals(left_type, right_type) && !numeric_pair && !either_null) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "cannot compare %s and %s for equality", type_to_string(left_type), type_to_string(right_type));
		}
		return type_bool();
	}

	is_comparison = opcode == OP_LESS || opcode == OP_LESS_EQUAL || opcode == OP_GREATER || opcode == OP_GREATER_EQUAL;
	if (is_comparison) {
		if (!type_is_numeric(left_type)) {
			compiler_error(compiler, node->as.binary_expr.left->line, node->as.binary_expr.left->column, 0, "compiler::type-mismatch", "left operand must be int or float, got %s", type_to_string(left_type));
		}
		if (!type_is_numeric(right_type)) {
			compiler_error(compiler, node->as.binary_expr.right->line, node->as.binary_expr.right->column, 0, "compiler::type-mismatch", "right operand must be int or float, got %s", type_to_string(right_type));
		}
		return type_bool();
	}

	// Everything left is arithmetic, bitwise, or shift.
	return check_arithmetic_or_bitwise(compiler, opcode, left_type, right_type, node->as.binary_expr.left, node->as.binary_expr.right, node->line, node->column);
}

/* && and || short-circuit with jumps. The right side does not run once
   the left side decides the result. Both operands must be bool. The
   language has no truthy coercion. */
static type_t compile_logical_expr(compiler_t *compiler, const ast_node_t *node) {
	size_t jump;
	size_t end_jump;
	type_t bool_hint;
	type_t left_type;
	type_t right_type;
	bool is_and;

	bool_hint = type_bool();
	is_and = node->as.binary_expr.op == TOKEN_AMP_AMP;

	left_type = compile_expr_typed(compiler, node->as.binary_expr.left, &bool_hint);
	if (!type_equals(left_type, type_bool())) {
		compiler_error(compiler, node->as.binary_expr.left->line, node->as.binary_expr.left->column, 0, "compiler::type-mismatch", "left operand of '%s' must be bool, got %s", is_and ? "&&" : "||", type_to_string(left_type));
	}

	if (is_and) {
		jump = compiler_emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
		compiler_emit_byte(compiler, OP_POP, node->line, node->column);
		right_type = compile_expr_typed(compiler, node->as.binary_expr.right, &bool_hint);
		compiler_patch_jump(compiler, jump, node->line);
	} else {
		jump = compiler_emit_jump(compiler, OP_JUMP_IF_FALSE, node->line, node->column);
		end_jump = compiler_emit_jump(compiler, OP_JUMP, node->line, node->column);
		compiler_patch_jump(compiler, jump, node->line);
		compiler_emit_byte(compiler, OP_POP, node->line, node->column);
		right_type = compile_expr_typed(compiler, node->as.binary_expr.right, &bool_hint);
		compiler_patch_jump(compiler, end_jump, node->line);
	}

	if (!type_equals(right_type, type_bool())) {
		compiler_error(compiler, node->as.binary_expr.right->line, node->as.binary_expr.right->column, 0, "compiler::type-mismatch", "right operand of '%s' must be bool, got %s", is_and ? "&&" : "||", type_to_string(right_type));
	}

	return type_bool();
}

type_t compile_unary_expr(compiler_t *compiler, const ast_node_t *node) {
	type_t operand_type;
	const ast_node_t *operand;

	operand = node->as.unary_expr.operand;

	// Fold '-' into an int literal at compile time. This is the only way
	// to write INT64_MIN. See compiler_parse_negated_int_literal().
	if (node->as.unary_expr.op == TOKEN_MINUS && operand->kind == AST_INT_LITERAL) {
		compiler_emit_constant(compiler, value_int(compiler_parse_negated_int_literal(compiler, operand->as.literal.token, node->line, node->column)), node->line, node->column);
		return type_int();
	}

	operand_type = compile_expr_typed(compiler, operand, NULL);

	switch (node->as.unary_expr.op) {
		case TOKEN_MINUS:
			if (!type_is_numeric(operand_type)) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "unary '-' requires an int or float operand, got %s", type_to_string(operand_type));
			}
			compiler_emit_byte(compiler, OP_NEGATE, node->line, node->column);
			return type_is_numeric(operand_type) ? operand_type : type_int();
		case TOKEN_BANG:
			if (!type_equals(operand_type, type_bool())) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "unary '!' requires a bool operand, got %s", type_to_string(operand_type));
			}
			compiler_emit_byte(compiler, OP_NOT, node->line, node->column);
			return type_bool();
		case TOKEN_TILDE:
			if (!type_equals(operand_type, type_int())) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "unary '~' requires an int operand, got %s", type_to_string(operand_type));
			}
			compiler_emit_byte(compiler, OP_BIT_NOT, node->line, node->column);
			return type_int();
		default:
			compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-unary-operator", "unsupported unary operator");
			compiler_emit_byte(compiler, OP_NOT, node->line, node->column);
			return type_bool();
	}
}

static type_t compile_index_expr(compiler_t *compiler, const ast_node_t *node) {
	type_t object_type;
	type_t index_type;
	type_t element_expected;

	element_expected = type_int();
	object_type = compile_expr_typed(compiler, node->as.index_expr.object, NULL);
	index_type = compile_expr_typed(compiler, node->as.index_expr.index, &element_expected);

	if (!type_equals(index_type, type_int())) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::index-not-integer", "array/string index must be an int");
	}

	compiler_emit_byte(compiler, OP_GET_INDEX, node->line, node->column);

	if (object_type.array_depth > 0) {
		return type_element_of(object_type);
	}
	if (type_equals(object_type, type_string())) {
		return type_string();
	}

	compiler_error(compiler, node->line, node->column, 0, "compiler::not-indexable", "'[]' access only works on arrays and strings, got %s", type_to_string(object_type));
	return type_int();
}

static type_t compile_array_literal(compiler_t *compiler, const ast_node_t *node, const type_t *expected) {
	size_t i;
	size_t count;
	type_t element_type;
	type_t element_expected;
	type_t next_type;
	bool have_element_expected;
	const ast_node_t *element_node;

	count = node->as.array_literal.elements.count;

	have_element_expected = expected != NULL && expected->array_depth > 0;
	element_expected = have_element_expected ? type_element_of(*expected) : type_int();

	if (count > UINT8_MAX) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::too-many-elements", "an array literal can have at most 255 elements");
		count = 0;
	}

	if (count == 0) {
		compiler_emit_bytes(compiler, OP_ARRAY, 0, node->line, node->column);
		if (!have_element_expected) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::ambiguous-empty-array", "cannot infer the element type of an empty array literal here");
		}
		return type_array_of(element_expected);
	}

	element_node = node->as.array_literal.elements.items[0];
	element_type = compile_expr_typed(compiler, element_node, have_element_expected ? &element_expected : NULL);

	for (i = 1; i < count; i++) {
		element_node = node->as.array_literal.elements.items[i];
		next_type = compile_expr_typed(compiler, element_node, &element_type);
		if (!type_equals(next_type, element_type)) {
			compiler_error(compiler, element_node->line, element_node->column, 0, "compiler::array-literal-type-mismatch", "array literal elements must all have the same type");
		}
	}

	compiler_emit_bytes(compiler, OP_ARRAY, (uint8_t)count, node->line, node->column);
	return type_array_of(element_type);
}

/* True if sig's parameter and return types match ft exactly. Used to
   pick the overload a bare function name means as a value. */
static bool signature_matches_func_type(const signature_t *sig, const func_type_t *ft) {
	size_t i;

	if (sig->param_count != ft->param_count || sig->has_return_type != ft->has_return_type) {
		return false;
	}
	for (i = 0; i < sig->param_count; i++) {
		if (!type_equals(sig->param_types[i], ft->param_types[i])) {
			return false;
		}
	}
	if (sig->has_return_type && !type_equals(sig->return_type, ft->return_type)) {
		return false;
	}
	return true;
}

/* A bare name that resolved to an overload set, used as a value. One
   overload needs no context. With several, expected must be a
   TYPE_FUNCTION that matches exactly one, the same rule an empty `[]`
   or a bare `{ ... }` uses. Emits OP_GET_GLOBAL <mangled_name>. The
   function object is already in the file's globals under that name. */
type_t compile_function_value_ref(compiler_t *compiler, const ast_node_t *node, const overload_set_t *set, const type_t *expected) {
	const signature_t *sig;
	size_t i;
	size_t match_count;
	func_type_t *ft;

	sig = NULL;
	if (set->overload_count == 1) {
		sig = &set->overloads[0];
	} else if (expected != NULL && expected->kind == TYPE_FUNCTION) {
		match_count = 0;
		for (i = 0; i < set->overload_count; i++) {
			if (signature_matches_func_type(&set->overloads[i], expected->func_type)) {
				sig = &set->overloads[i];
				match_count++;
			}
		}
		if (match_count != 1) {
			sig = NULL;
		}
	}

	if (sig == NULL) {
		compiler_error(compiler, node->line, node->column, (int)node->as.identifier_expr.name.length, "compiler::ambiguous-function-reference", "'%.*s' has multiple overloads; a known function type is needed here to tell which one this refers to", (int)node->as.identifier_expr.name.length, node->as.identifier_expr.name.start);
		return type_void();
	}

	compiler_emit_name_op_str(compiler, OP_GET_GLOBAL, sig->mangled_name, strlen(sig->mangled_name), node->line, node->column);

	ft = type_new_func_type();
	ft->param_count = sig->param_count;
	ft->param_types = sig->param_count > 0 ? malloc(sig->param_count * sizeof(type_t)) : NULL;
	for (i = 0; i < sig->param_count; i++) {
		ft->param_types[i] = sig->param_types[i];
	}
	ft->has_return_type = sig->has_return_type;
	ft->return_type = sig->has_return_type ? sig->return_type : type_void();

	return type_function(ft);
}

static const int_limit_const_t *find_int_limit_const(const char *name, size_t name_length) {
	size_t i;

	for (i = 0; i < sizeof(g_int_limit_consts) / sizeof(g_int_limit_consts[0]); i++) {
		if (strlen(g_int_limit_consts[i].name) == name_length && memcmp(g_int_limit_consts[i].name, name, name_length) == 0) {
			return &g_int_limit_consts[i];
		}
	}
	return NULL;
}

type_t compile_identifier_expr(compiler_t *compiler, const ast_node_t *node, const type_t *expected) {
	int slot;
	int upvalue;
	token_t name;
	overload_set_t *set;

	name = node->as.identifier_expr.name;

	slot = compiler_resolve_local(compiler, name);
	if (slot != -1) {
		compiler_emit_bytes(compiler, OP_GET_LOCAL, (uint8_t)slot, node->line, node->column);
		return compiler->locals[slot].type;
	}

	upvalue = compiler_resolve_upvalue(compiler, name);
	if (upvalue != -1) {
		compiler_emit_bytes(compiler, OP_GET_UPVALUE, (uint8_t)upvalue, node->line, node->column);
		return compiler->upvalues[upvalue].type;
	}

	set = signature_table_find_set(compiler->file_signatures, name.start, name.length);
	if (set != NULL) {
		return compile_function_value_ref(compiler, node, set, expected);
	}

	if (name.length == 6 && memcmp(name.start, "stdlib", 6) == 0) {
		compiler_error(compiler, node->line, node->column, 6, "compiler::module-used-as-value", "'stdlib' is a module; access a member and call it");
		return type_void();
	}
	if (compiler_find_imported_module(compiler->imported_modules, compiler->imported_module_count, name.start, name.length) != NULL) {
		compiler_error(compiler, node->line, node->column, (int)name.length, "compiler::module-used-as-value", "'%.*s' is a module; access a member and call it", (int)name.length, name.start);
		return type_void();
	}

	{
		const global_def_t *global_def = compiler_global_registry_find(compiler->globals, name.start, name.length);
		if (global_def != NULL) {
			compiler_emit_name_op_str(compiler, OP_GET_GLOBAL, name.start, name.length, node->line, node->column);
			return global_def->type;
		}
	}

	{
		const int_limit_const_t *limit_const = find_int_limit_const(name.start, name.length);
		if (limit_const != NULL) {
			compiler_emit_constant(compiler, value_int(limit_const->value), node->line, node->column);
			return type_int();
		}
	}

	compiler_error(compiler, node->line, node->column, (int)name.length, "compiler::undefined-variable", "undefined variable '%.*s'", (int)name.length, name.start);
	return type_void();
}

/* Finds node's member name in object_type's struct fields. Reports
   "unknown field" and returns -1 if there is none. object_type must be
   a non-array TYPE_STRUCT. */
static int resolve_struct_field(compiler_t *compiler, const ast_node_t *node, const type_t *object_type, const char *name, size_t name_length, const struct_def_t **out_def) {
	int index;

	*out_def = object_type->struct_def;
	index = struct_def_find_field(object_type->struct_def, name, name_length);
	if (index < 0) {
		compiler_error(compiler, node->as.member_expr.name.line, node->as.member_expr.name.column, (int)name_length, "compiler::unknown-field", "'%.*s' has no field '%.*s'", (int)object_type->struct_def->name_length, object_type->struct_def->name, (int)name_length, name);
	}
	return index;
}

/* `object.name` outside a call. On a struct value it reads a field. On
   `stdlib` or an imported module it is an error: a module member can
   only be called. The object's identifier text is checked directly and
   never compiled, since a module is not a runtime value. */
type_t compile_member_expr(compiler_t *compiler, const ast_node_t *node) {
	const ast_node_t *object_node;
	token_t object_name;
	type_t object_type;
	int field_index;
	const struct_def_t *def;
	struct_def_t *type_def;

	object_node = node->as.member_expr.object;

	if (object_node->kind == AST_IDENTIFIER_EXPR) {
		object_name = object_node->as.identifier_expr.name;

		if (object_name.length == 6 && memcmp(object_name.start, "stdlib", 6) == 0) {
			compiler_error(compiler, node->as.member_expr.name.line, node->as.member_expr.name.column, (int)node->as.member_expr.name.length, "compiler::unsupported-member-access", "cannot use 'stdlib.%.*s' without calling it", (int)node->as.member_expr.name.length, node->as.member_expr.name.start);
			return type_void();
		}

		// A struct type name used bare, as in `Point.origin` with no call. A
		// struct function can only be called, so this is the same error.
		type_def = struct_registry_find(compiler->structs, object_name.start, object_name.length);
		if (type_def == NULL) {
			size_t m;
			for (m = 0; m < compiler->imported_module_count; m++) {
				struct_def_t *imported_type_def = struct_registry_find(compiler->imported_modules[m]->exported_structs, object_name.start, object_name.length);
				if (imported_type_def != NULL && imported_type_def->is_exported) {
					type_def = imported_type_def;
					break;
				}
			}
		}
		if (type_def != NULL) {
			compiler_error(compiler, node->as.member_expr.name.line, node->as.member_expr.name.column, (int)node->as.member_expr.name.length, "compiler::unsupported-member-access", "cannot use '%.*s.%.*s' without calling it", (int)object_name.length, object_name.start, (int)node->as.member_expr.name.length, node->as.member_expr.name.start);
			return type_void();
		}

		if (compiler_find_imported_module(compiler->imported_modules, compiler->imported_module_count, object_name.start, object_name.length) != NULL) {
			compiler_error(compiler, node->as.member_expr.name.line, node->as.member_expr.name.column, (int)node->as.member_expr.name.length, "compiler::unsupported-member-access", "cannot use '%.*s.%.*s' without calling it", (int)object_name.length, object_name.start, (int)node->as.member_expr.name.length, node->as.member_expr.name.start);
			return type_void();
		}
	}

	object_type = compile_expr_typed(compiler, object_node, NULL);

	if (object_type.kind != TYPE_STRUCT || object_type.array_depth > 0) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::unsupported-member-access", "'.' only works on struct fields, or a module (which must be called)");
		return type_void();
	}

	field_index = resolve_struct_field(compiler, node, &object_type, node->as.member_expr.name.start, node->as.member_expr.name.length, &def);
	if (field_index < 0) {
		return type_void();
	}

	compiler_emit_bytes(compiler, OP_GET_FIELD, (uint8_t)field_index, node->line, node->column);
	return def->fields[field_index].type;
}

/* `TypeName { field: value, ... }`. Fields are compiled in declared
   order, whatever order the literal uses. OP_STRUCT pops them in that
   order, and OP_GET_FIELD and OP_SET_FIELD use the same index. An
   omitted field takes its declared default, compiled again here. A
   default cannot use locals or sibling fields, so it compiles the same
   everywhere. */
type_t compile_struct_literal(compiler_t *compiler, const ast_node_t *node, const type_t *expected) {
	const struct_def_t *def;
	size_t lit_count;
	size_t i;
	size_t j;
	int field_index;
	const ast_node_t *field_init;
	const ast_node_t *earlier;
	const ast_node_t *matched;
	type_t value_type;

	lit_count = node->as.struct_literal.fields.count;

	if (node->as.struct_literal.has_type_name) {
		def = struct_registry_find(compiler->structs, node->as.struct_literal.type_name.start, node->as.struct_literal.type_name.length);
		if (def == NULL) {
			size_t m;
			size_t match_count = 0;
			for (m = 0; m < compiler->imported_module_count; m++) {
				struct_def_t *candidate = struct_registry_find(compiler->imported_modules[m]->exported_structs, node->as.struct_literal.type_name.start, node->as.struct_literal.type_name.length);
				if (candidate != NULL && candidate->is_exported) {
					def = candidate;
					match_count++;
				}
			}
			if (match_count > 1) {
				compiler_error(compiler, node->line, node->column, (int)node->as.struct_literal.type_name.length, "compiler::ambiguous-struct-name", "'%.*s' is exported by more than one imported module", (int)node->as.struct_literal.type_name.length, node->as.struct_literal.type_name.start);
				return type_void();
			}
		}
		if (def == NULL) {
			compiler_error(compiler, node->line, node->column, (int)node->as.struct_literal.type_name.length, "compiler::unknown-type", "unknown struct type '%.*s'", (int)node->as.struct_literal.type_name.length, node->as.struct_literal.type_name.start);
			return type_void();
		}
	} else {
		// A bare `{ ... }` takes its type from context, like an empty `[]`.
		if (expected == NULL || expected->kind != TYPE_STRUCT || expected->array_depth > 0) {
			compiler_error(compiler, node->line, node->column, 0, "compiler::ambiguous-struct-literal", "cannot infer the struct type of '{ ... }' here; give an explicit type name");
			return type_void();
		}
		def = expected->struct_def;
	}

	// Check for unknown and duplicate field names first. Nothing is
	// emitted here. The values are compiled below in declared order.
	for (i = 0; i < lit_count; i++) {
		field_init = node->as.struct_literal.fields.items[i];
		field_index = struct_def_find_field(def, field_init->as.struct_field_init.name.start, field_init->as.struct_field_init.name.length);
		if (field_index < 0) {
			compiler_error(compiler, field_init->line, field_init->column, (int)field_init->as.struct_field_init.name.length, "compiler::unknown-field", "'%.*s' has no field '%.*s'", (int)def->name_length, def->name, (int)field_init->as.struct_field_init.name.length, field_init->as.struct_field_init.name.start);
			continue;
		}
		for (j = 0; j < i; j++) {
			earlier = node->as.struct_literal.fields.items[j];
			if (compiler_identifiers_equal(earlier->as.struct_field_init.name, field_init->as.struct_field_init.name)) {
				compiler_error(compiler, field_init->line, field_init->column, (int)field_init->as.struct_field_init.name.length, "compiler::duplicate-field", "field '%.*s' is set more than once", (int)field_init->as.struct_field_init.name.length, field_init->as.struct_field_init.name.start);
				break;
			}
		}
	}

	if (def->field_count > UINT8_MAX) {
		compiler_error(compiler, node->line, node->column, 0, "compiler::too-many-fields", "a struct can have at most 255 fields");
	}

	for (i = 0; i < def->field_count; i++) {
		matched = NULL;
		for (j = 0; j < lit_count; j++) {
			field_init = node->as.struct_literal.fields.items[j];
			if (field_init->as.struct_field_init.name.length == def->fields[i].name_length && memcmp(field_init->as.struct_field_init.name.start, def->fields[i].name, def->fields[i].name_length) == 0) {
				matched = field_init;
				break;
			}
		}

		if (matched != NULL) {
			value_type = compile_expr_typed(compiler, matched->as.struct_field_init.value, &def->fields[i].type);
			if (!type_is_assignable(def->fields[i].type, value_type)) {
				compiler_error(compiler, matched->line, matched->column, (int)matched->as.struct_field_init.name.length, "compiler::type-mismatch", "field '%.*s' expects %s, got %s", (int)def->fields[i].name_length, def->fields[i].name, type_to_string(def->fields[i].type), type_to_string(value_type));
			}
		} else if (def->fields[i].default_value != NULL) {
			value_type = compile_expr_typed(compiler, def->fields[i].default_value, &def->fields[i].type);
			if (!type_is_assignable(def->fields[i].type, value_type)) {
				compiler_error(compiler, node->line, node->column, 0, "compiler::type-mismatch", "default value for field '%.*s' must be %s, got %s", (int)def->fields[i].name_length, def->fields[i].name, type_to_string(def->fields[i].type), type_to_string(value_type));
			}
		} else {
			compiler_error(compiler, node->line, node->column, 0, "compiler::missing-field", "struct literal for '%.*s' is missing required field '%.*s'", (int)def->name_length, def->name, (int)def->fields[i].name_length, def->fields[i].name);
			compiler_emit_byte(compiler, OP_NULL, node->line, node->column);
		}
	}

	compiler_emit_name_op_str(compiler, OP_STRUCT, def->name, def->name_length, node->line, node->column);
	compiler_emit_byte(compiler, (uint8_t)def->field_count, node->line, node->column);

	return type_struct(def);
}
