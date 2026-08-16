/**
 * @file Tree-sitter grammar for paweli
 * @see docs/paweli/syntax.md and docs/paweli/language.md in the paweli repo
 */

function commaSep1(rule) {
	return seq(rule, repeat(seq(",", rule)));
}

function commaSep(rule) {
	return optional(commaSep1(rule));
}

module.exports = grammar({
	name: "paweli",

	extras: ($) => [/\s/, $.line_comment, $.block_comment],

	word: ($) => $.identifier,

	rules: {
		source_file: ($) => repeat($._top_level_item),

		_top_level_item: ($) => choice($.module_import, $.module_def, $.struct_declaration, $.variable_declaration, $.function_declaration),

		struct_declaration: ($) =>
			seq("struct", field("name", $.identifier), "{", repeat($.struct_field), "}"),

		struct_field: ($) =>
			seq(field("type", $._type), field("name", $.identifier), optional(seq("=", field("default", $._expression))), ";"),

		module_import: ($) => seq("#module", field("name", $.string_literal), optional(";")),
		module_def: ($) => seq("#moduledef", field("name", $.string_literal), optional(";")),

		function_declaration: ($) =>
			seq(
				"fn",
				field("name", $.identifier),
				field("parameters", $.parameter_list),
				optional(seq("->", field("return_type", $._type))),
				field("body", $.block),
			),

		parameter_list: ($) => seq("(", commaSep($.parameter), ")"),
		parameter: ($) => seq(field("type", $._type), field("name", $.identifier)),

		_type: ($) => seq(choice("int", "float", "string", "bool", $.identifier), optional("[]")),

		block: ($) => seq("{", repeat($._statement), "}"),

		_statement: ($) =>
			choice(
				$.variable_declaration,
				$.if_statement,
				$.while_statement,
				$.for_statement,
				$.break_statement,
				$.continue_statement,
				$.return_statement,
				$.export_statement,
				$.expression_statement,
				$.block,
			),

		variable_declaration: ($) =>
			seq(field("type", $._type), field("name", $.identifier), "=", field("value", $._expression), ";"),

		if_statement: ($) =>
			seq(
				"if",
				"(",
				field("condition", $._expression),
				")",
				field("consequence", $.block),
				optional(seq("else", field("alternative", choice($.if_statement, $.block)))),
			),

		while_statement: ($) =>
			seq("while", "(", field("condition", $._expression), ")", field("body", $.block)),

		// Three for-loop forms share the opening "for (": the classic
		// three-clause one, a range (`type name : a..b`), and an
		// array/string one (`type name : expr`). The last two both start
		// "type name :" and only diverge on whether ".." follows, which a
		// single token of lookahead after the shared expression resolves.
		for_statement: ($) =>
			seq("for", "(", choice($.for_range_clause, $.for_each_clause, $.for_classic_clause), ")", field("body", $.block)),

		for_classic_clause: ($) =>
			seq(
				field("init", choice($.variable_declaration, seq(optional($._expression), ";"))),
				field("condition", optional($._expression)),
				";",
				field("update", optional($._expression)),
			),

		for_range_clause: ($) =>
			seq(
				field("type", $._type),
				field("name", $.identifier),
				":",
				field("start", $._expression),
				"..",
				field("end", $._expression),
			),

		for_each_clause: ($) =>
			seq(field("type", $._type), field("name", $.identifier), ":", field("iterable", $._expression)),

		break_statement: ($) => seq("break", ";"),
		continue_statement: ($) => seq("continue", ";"),

		return_statement: ($) => seq("return", optional(field("value", $._expression)), ";"),

		export_statement: ($) => seq("export", field("name", $.identifier), ";"),

		expression_statement: ($) => seq($._expression, ";"),

		_expression: ($) =>
			choice(
				$.assignment_expression,
				$.binary_expression,
				$.unary_expression,
				$.call_expression,
				$.member_expression,
				$.index_expression,
				$.array_literal,
				$.struct_literal,
				$.parenthesized_expression,
				$.identifier,
				$.int_literal,
				$.float_literal,
				$.string_literal,
				$.boolean_literal,
				$.null_literal,
			),

		parenthesized_expression: ($) => seq("(", $._expression, ")"),

		array_literal: ($) => seq("[", commaSep($._expression), "]"),

		struct_literal: ($) =>
			prec(8, seq(optional(field("type", $.identifier)), "{", commaSep($.struct_field_init), "}")),

		struct_field_init: ($) => seq(field("name", $.identifier), ":", field("value", $._expression)),

		assignment_expression: ($) =>
			prec.right(0, seq(field("left", choice($.identifier, $.index_expression, $.member_expression)), "=", field("right", $._expression))),

		binary_expression: ($) =>
			choice(
				prec.left(1, seq(field("left", $._expression), field("operator", "||"), field("right", $._expression))),
				prec.left(2, seq(field("left", $._expression), field("operator", "&&"), field("right", $._expression))),
				prec.left(
					3,
					seq(field("left", $._expression), field("operator", choice("==", "!=")), field("right", $._expression)),
				),
				prec.left(
					4,
					seq(
						field("left", $._expression),
						field("operator", choice("<", "<=", ">", ">=")),
						field("right", $._expression),
					),
				),
				prec.left(
					5,
					seq(field("left", $._expression), field("operator", choice("+", "-")), field("right", $._expression)),
				),
				prec.left(
					6,
					seq(
						field("left", $._expression),
						field("operator", choice("*", "/", "%")),
						field("right", $._expression),
					),
				),
			),

		unary_expression: ($) => prec(7, seq(field("operator", choice("!", "-")), field("operand", $._expression))),

		call_expression: ($) =>
			prec(8, seq(field("function", $._expression), field("arguments", $.argument_list))),

		argument_list: ($) => seq("(", commaSep($._expression), ")"),

		member_expression: ($) =>
			prec(8, seq(field("object", $._expression), ".", field("property", $.identifier))),

		index_expression: ($) =>
			prec(8, seq(field("object", $._expression), "[", field("index", $._expression), "]")),

		int_literal: ($) => /\d+/,
		float_literal: ($) => /\d+\.\d+/,

		string_literal: ($) => seq('"', repeat(choice($.escape_sequence, /[^"\\\n]+/)), '"'),
		escape_sequence: ($) => token.immediate(/\\[ntr\\"0]/),

		boolean_literal: ($) => choice("true", "false"),
		null_literal: ($) => "null",

		identifier: ($) => /[A-Za-z_][A-Za-z0-9_]*/,

		line_comment: ($) => token(seq("//", /[^\n]*/)),

		block_comment: ($) =>
			token(seq("///", repeat(choice(/[^\/]/, /\/[^\/]/, /\/\/[^\/]/)), "///")),
	},
});
