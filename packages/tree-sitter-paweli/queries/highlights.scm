; Fallback. Anything more specific below overrides this.
(identifier) @variable

; Keywords
[
  "fn"
  "return"
  "export"
  "struct"
] @keyword

"if" @keyword.control.conditional
"else" @keyword.control.conditional
"while" @keyword.control.repeat
"for" @keyword.control.repeat
"break" @keyword.control.repeat
"continue" @keyword.control.repeat

"#module" @keyword.control.import
"#moduledef" @keyword.control.import

; Decorators
"@" @attribute
(decorator name: (identifier) @attribute)

; Types
[
  "int"
  "float"
  "string"
  "bool"
] @type.builtin

; A struct's own declared name, and every place a struct type name (not
; a builtin keyword, which @type.builtin above already covers) can be
; written: a field/parameter/variable's type, a return type, or a struct
; literal's type.
(struct_declaration name: (identifier) @type)
(struct_field type: (identifier) @type)
(parameter type: (identifier) @type)
(variable_declaration type: (identifier) @type)
(function_declaration return_type: (identifier) @type)
(struct_literal type: (identifier) @type)

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

; Member access. `.` accesses either a module (stdlib, or an imported
; module's local binding) or a struct field. The two are syntactically
; identical, and telling them apart needs the type information this
; query doesn't have. `stdlib` specifically is always recognizable by name, so it
; still gets its own color; an arbitrary imported module's name is not
; distinguishable from a struct-typed variable and just falls back to
; the plain @variable rule at the top of this file, same as any other
; identifier.
((identifier) @module
  (#eq? @module "stdlib"))
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
  ".."
] @operator

; Punctuation
[
  "("
  ")"
  "{"
  "}"
  "["
  "]"
] @punctuation.bracket

[
  ","
  ";"
  "."
  ":"
] @punctuation.delimiter
