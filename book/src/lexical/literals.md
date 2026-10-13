# Literals

- Integer: `0`, `42`, `1000`. Decimal only. Signed 64-bit value. A literal outside `INT64_MIN`..`INT64_MAX` is a compile error.
- Float: `0.0`, `16.5`. Must have digits on both sides of the `.`. A 64-bit IEEE 754 double.
- String: `"hello"`, `"line1\nline2"`. Double-quoted, see escapes at the end.
- Boolean: `true`, `false`.
- Null: is `null`. (what else would it even be?).

**Int max constants**: `INT8_MIN`/`INT8_MAX`, `INT16_MIN`/`INT16_MAX`, `INT32_MIN`/`INT32_MAX`, and `INT64_MIN`/`INT64_MAX` are builtin `int` constants for their respective range boundaries.

**String escapes**: `\n` `\t` `\r` `\\` `\"` `\0`. Any other character after a backslash will result in a compile error.
