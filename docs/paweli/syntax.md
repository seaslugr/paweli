# Syntax Reference

The lexical building blocks of paweli: comments, literals, identifiers, keywords, and operators.

---

## Comments

```paweli
// a single-line comment, runs to the end of the line

/// a multi-line comment
    opens and closes with three slashes,
    everything between is ignored
///
```

There is no `/* */` form. Line comments use `//`, block comments open and close with `///`.

---

## Identifiers

An identifier starts with a letter or underscore, then continues with letters, digits, or underscores: `total`, `_count`, `player2`. Identifiers are case-sensitive.

---

## Keywords

These words are reserved and cannot be used as identifiers:

| Keyword | Meaning |
|---|---|
| `fn` | declares a function |
| `int` | integer type |
| `float` | floating-point type |
| `string` | string type |
| `bool` | boolean type |
| `if` / `else` | conditional branching |
| `while` | condition-checked loop |
| `for` | C-style counted loop |
| `return` | returns from a function |
| `true` / `false` | boolean literals |
| `null` | the null value |
| `export` | marks a function as part of a module's public interface |

---

## Literals

| Kind | Examples | Notes |
|---|---|---|
| Integer | `0`, `42`, `1000` | decimal only, no `0x`/`0o`/`0b` forms |
| Float | `0.0`, `16.5` | must have digits on both sides of the `.` |
| String | `"hello"`, `"line1\nline2"` | double-quoted, see escapes below |
| Boolean | `true`, `false` | |
| Null | `null` | |

There is no negative-number literal syntax. `-7` is the unary `-` operator applied to `7`.

**String escapes:** `\n` `\t` `\r` `\\` `\"` `\0`. Any other character after a backslash is a compile error.

---

## Operators

| Category | Operators |
|---|---|
| Arithmetic | `+` `-` `*` `/` `%` |
| Comparison | `==` `!=` `<` `<=` `>` `>=` |
| Logical | `&&` `\|\|` `!` |
| Assignment | `=` |
| Member access | `.` |
| Function return type | `->` |

Precedence, lowest to highest: assignment, `\|\|`, `&&`, equality (`==` `!=`), comparison (`<` `<=` `>` `>=`), addition (`+` `-`), multiplication (`*` `/` `%`), unary (`!` `-`), call/member access (`()` `.`).

`&&` and `\|\|` short-circuit: the right-hand side is not evaluated if the left side already determines the result.

---

## Punctuation

`( )` `{ }` `,` `;`: grouping, blocks, argument/parameter separators, statement terminators.

`[ ]` are reserved for arrays but have no grammar yet. See [Language reference](language.md#not-yet-implemented).
