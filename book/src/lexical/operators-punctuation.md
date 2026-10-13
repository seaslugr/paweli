# Operators and Punctuation

## Math: `+` `-` `*` `/` `%`

```paweli
5 + 2; // 7
5 - 2; // 3
5 * 2; // 10
5 / 2; // 2, int / int truncates
5 % 2; // 1
```

## Comparison: `==` `!=` `<` `<=` `>` `>=`

Always produce a `bool`.

## Logical: `&&` `||` `!`

`&&` is "and", `||` is "or", `!` is "not". Both sides must be `bool`.

```paweli
bool a = true;
bool b = false;

a && b; // false, needs both to be true
a || b; // true, only needs one to be true
!a;     // false
```

They short-circuit: in `f() || g()`, `g()` never runs if `f()` already returned `true`. Same idea for `f() && g()` when `f()` returns `false`.

## Bitwise: `&` `|` `^` `~` `<<` `>>`

Int-only, operates on individual bits. Not related to `&&`/`||` despite looking similar.

```paweli
6 & 3;    // 2, AND
6 | 3;    // 7, OR
6 ^ 3;    // 5, XOR
~5;       // -6, flips every bit
1 << 4;   // 16, shift left
256 >> 4; // 16, shift right
```

Watch out: `a & b == c` means `a & (b == c)`, not `(a & b) == c`. `&` `|` `^` bind looser than `==` here. Add parentheses if that's not what you meant!

## Assignment: `=` `+=` `-=` `*=` `...`

```paweli
int x = 10;
x += 5; // x is now 15
```

`arr[f()] += 1` calls `f()` once, not twice. An assignment is also an expression: `y = (x += 1)` sets `y` to `x`'s new value.

## Other operators

- `.` member access: `point.x`
- `[ ]` indexing and array literals: `arr[0]`, `[1, 2, 3]`
- `->` function return type: `fn add() -> int`

## Precedence

Which operator "grabs" its operands first when an expression mixes several, e.g. why `2 + 3 * 4` is `14`, not `20`. Lowest to highest:

```
=  +=  -=  ...          (assignment)
||
&&
|
^
&
==  !=
<  <=  >  >=
<<  >>
+  -
*  /  %
!  -  ~                 (unary)
()  .  [ ]              (call / member / index)
```

When mixing tiers you're not sure about, just add parentheses.

## Punctuation

- `( )` `{ }` `,` `;`: grouping, blocks, separators, statement ends.
- `:` and `..` only appear in `for (int i : a..b)`, the range form of `for`. See [Control Flow](../language/control-flow.md#for).
