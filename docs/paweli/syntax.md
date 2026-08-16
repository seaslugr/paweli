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
| `break` | exits the innermost loop immediately |
| `continue` | skips to the next iteration of the innermost loop |
| `return` | returns from a function |
| `true` / `false` | boolean literals |
| `null` | the null value |
| `export` | marks a function as part of a module's public interface |
| `struct` | declares a struct type |

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
| Bitwise | `&` `\|` `^` `~` `<<` `>>` |
| Comparison | `==` `!=` `<` `<=` `>` `>=` |
| Logical | `&&` `\|\|` `!` |
| Assignment | `=` `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` |
| Member access | `.` |
| Indexing | `[ ]` |
| Function return type | `->` |

Precedence, lowest to highest: assignment, `\|\|`, `&&`, `\|`, `^`, `&`, equality (`==` `!=`), comparison (`<` `<=` `>` `>=`), shift (`<<` `>>`), addition (`+` `-`), multiplication (`*` `/` `%`), unary (`!` `-` `~`), call/member/index access (`()` `.` `[ ]`).

`&&` and `\|\|` short-circuit: the right-hand side is not evaluated if the left side already determines the result.

Bitwise `&` `\|` `^` `~` `<<` `>>` are unrelated to the similar-looking logical `&&`/`\|\|`: they operate on the two's-complement bits of an `int` (never `float`, and there is no other integer width to pick between), and sit at their own precedence tier between the logical operators and equality, following C's own long-standing (if famously surprising) placement, so `a & b == c` parses as `a & (b == c)`, not `(a & b) == c`. Parenthesize explicitly if that's not what's meant. A shift amount outside `0..63` is a runtime error rather than the undefined behavior a raw C `<<`/`>>` would produce for the same input; a right shift of a negative left-hand operand is arithmetic (sign-preserving), matching this project's target platforms.

A compound assignment operator, `target op= value`, is exactly equivalent to `target = target op value` *semantically*, but not by literally expanding to that: for an index or member target (`arr[i] += 1`, `s.f += 1`), the object and index/field sub-expressions are only evaluated once, not once to read the current value and again to write the new one. `arr[f()] += 1` calls `f()` exactly once. Like plain `=`, a compound assignment is itself an expression, producing the value that was stored (`y = (x += 1)` sets `y` to `x`'s new value). The right-hand side's type follows the same rules the matching binary operator would (`+=` etc. promote int/float the way `+` does; `&=` etc. require both sides `int` the way `&` does), and the computed result has to still fit the target's own declared type, the same check plain assignment already makes.

---

## Punctuation

`( )` `{ }` `,` `;`: grouping, blocks, argument/parameter separators, statement terminators.

`[ ]`: array literals (`[1, 2, 3]`) and indexing (`arr[i]`). See [Language reference](language.md#arrays).

`:` and `..` only appear in one place, the two shorter `for` forms: `for (T name : a..b)` (a range) and `for (T name : expr)` (an array or string). Neither is a general-purpose operator; `..` in particular has no meaning outside that one syntactic position. See [Language reference](language.md#for).
