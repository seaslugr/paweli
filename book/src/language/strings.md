# Strings

```paweli
string greeting = "hello";
```

A string literal is double-quoted. See [Literals](../lexical/literals.md) for the escape sequences it can accept.

## Indexing

Strings can be indexed like arraysy: `"hello"[0]` gives `"h"`. Strings are immutable, unlike arrays, assigning into a string index (`s[0] = "x"`) is a runtime error.

## Iterating

`for (string c : "abc")` walks a string one character at a time, binding `c` to `"a"`, then `"b"`, then `"c"`. See [Control Flow](control-flow.md#for) for more.

## Equality

`==` on two strings compares their actual contents byte for byte: `stdlib.concat("a", "b") == "ab"` is `true`, even though the left side is a freshly built string and the right side is a literal, two different objects on the heap. This is different from arrays and structs, which compare by identity (see [Structs](structs.md)). A string is the one heap-allocated type in paweli with value equality.

## Concatenation

`"a" + "b"` concatenates, giving `"ab"`. `stdlib.concat(a, b)` does the same thing as a plain function call. See [Standard Library](standard-library.md) for `concat`, `split`, `to_upper`, `to_string`, `to_int`, and `length`.

## Memory

Strings live on the garbage-collected heap, the same as arrays, functions, modules, and struct instances. See [Types and Variables](types-and-variables.md#memory).
