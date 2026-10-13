# Standard Library

`stdlib` is always available, no `#module` needed. You access every function through it, qualified: `stdlib.abs(-7)`. A bare `abs(-7)` does not work.

```paweli
print(stdlib.abs(-7)); // 7
print(stdlib.max(3, 9)); // 9
print(stdlib.sqrt(16.0)); // 4
print(stdlib.to_upper("hi")); // HI
```

## Functions

- `abs(int) -> int`: absolute value.
- `max(int, int) -> int`: larger of the two.
- `min(int, int) -> int`: smaller of the two.
- `sqrt(int | float) -> float`: square root. Runtime error on a negative argument.
- `length(string) -> int`: byte length of a string. Also accepts an array and returns its element count.
- `to_upper(string) -> string`: uppercased copy.
- `concat(string, string) -> string`: concatenation.
- `split(string, string) -> string[]`: splits on every occurrence of the delimiter. Runs of the delimiter collapse, so no empty tokens come out. Runtime error on an empty delimiter.
- `to_string(int | float | bool) -> string`: text representation.
- `to_int(string) -> int`: parses a leading integer. Input that can't be parsed gives `0`.
- `push(array, value)`: appends `value` to the end of `array`, in place.
- `pop(array) -> value`: removes and returns the last element of an array. Runtime error on an empty array.
- `sort(T[], fn(T,T)->bool)`: sorts an array in place, stably. The function you pass takes `(a, b)` and reports whether `a` belongs before `b`. See [Function Values](closures.md#function-values).
- `read_line() -> string`: reads one line from stdin, without the trailing newline. Gives an empty string on EOF.

A wrong argument count or type is a compile error. `stdlib.abs("x")` fails to compile, the same way a call to your own function with the wrong argument types does (see [Functions and Overloading](functions.md#overloading)).

Every stdlib function still checks its arguments at runtime too, as a backstop. A program that compiled should never hit that path.

## Builtins

Two functions are bare globals: `print` and `assert`. Both are always available, the same as `stdlib` itself. There is no `stdlib.print` or `stdlib.assert`.

`print(value)` writes a text representation of `value` to stdout, followed by a newline. It accepts any type.

`assert(condition)` and `assert(condition, message)` check a `bool` condition. A false condition is a runtime error: `assertion failed`, or `assertion failed: <message>` with the two-argument form. This is how [`@test`](testing.md) functions check themselves. A failing `assert()` stops the run with a diagnostic.
