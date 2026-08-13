; Fallback — anything more specific below overrides this.
(identifier) @variable

; Keywords
[
  "fn"
  "return"
  "export"
] @keyword

"if" @keyword.control.conditional
"else" @keyword.control.conditional
"while" @keyword.control.repeat
"for" @keyword.control.repeat

"#module" @keyword.control.import
"#moduledef" @keyword.control.import

; Types
[
  "int"
  "float"
  "string"
  "bool"
] @type.builtin

; Literals
(int_literal) @constant.numeric.integer
(float_literal) @constant.numeric.float
(boolean_literal) @constant.builtin.boolean
(null_literal) @constant.builtin
(string_literal) @string
(escape_sequence) @string.escape

; Comments
(line_comment) @comment.line
(block_comment) @comment.block

; Functions and calls
(function_declaration name: (identifier) @function)
(call_expression function: (identifier) @function.call)
(call_expression function: (member_expression property: (identifier) @function.call))

; Parameters
(parameter name: (identifier) @variable.parameter)

; Member access
(member_expression property: (identifier) @property)

; Operators
[
  "+"
  "-"
  "*"
  "/"
  "%"
  "=="
  "!="
  "<"
  "<="
  ">"
  ">="
  "&&"
  "||"
  "!"
  "="
  "->"
] @operator

; Punctuation
[
  "("
  ")"
  "{"
  "}"
] @punctuation.bracket

[
  ","
  ";"
  "."
] @punctuation.delimiter
