# Language Reference

---

## Types

| Type | Description |
|---|---|
| `int` | 64-bit signed integer |
| `float` | double-precision floating point |
| `string` | text |
| `bool` | `true` or `false` |

Types are static and explicit. A variable or parameter always declares its type; there is no `let` and no inference.

---

## Variables

```paweli
int count = 0;
string name = "sluglang";
float ratio = 0.5;
```

A variable declaration always needs an initializer. There is no bare `int x;` form. Variables are block-scoped: a variable declared inside `{ }` stops existing once that block ends.

```paweli
fn main() {
	int x = 1;
	{
		int x = 2;
		print(x);
	}
	print(x);
}
```

Redeclaring a name inside the *same* block is an error, but an inner block may shadow a name from an outer one, as above. That program prints `2` then `1`.

Assignment is an expression, so it produces a value. It is right-associative:

```paweli
a = b = 5;
```

Only plain variables can currently be assigned to. Assigning to a member expression (`a.b = 5`) is a compile error, since paweli does not have mutable object fields yet.

---

## Functions

```paweli
fn add(int a, int b) -> int {
	return a + b;
}

fn greet(string name) {
	print(name);
}
```

`-> type` declares a return type. Omitting it means the function returns nothing meaningful (its call expression evaluates to `null`). Functions can only be declared at the top level of a file. There is no nesting a `fn` inside another `fn`.

Every program needs a function named `main`, taking no parameters. It's the entry point: `paweli` binds every top-level function first, then calls `main()`.

`return` with no value is the same as `return null;`. Falling off the end of a function without an explicit `return` also returns `null`.

---

## Control flow

### if / else

```paweli
if (n < 0) {
	print("negative");
} else if (n == 0) {
	print("zero");
} else {
	print("positive");
}
```

Branch bodies must be `{ }` blocks. There is no bare single-statement `if` without braces.

### while

```paweli
while (n > 0) {
	print(n);
	n = n - 1;
}
```

### for

C-style three-clause `for`:

```paweli
for (int i = 0; i < 10; i = i + 1) {
	print(i);
}
```

Any of the three clauses can be omitted (`for (;;) { ... }` loops forever). A variable declared in the init clause is scoped to the loop, including its body.

There is no `break` or `continue`. The only way out of a loop early is `return` from the enclosing function.

---

## Modules

```paweli
#moduledef "geometry"
```

declares the current file as a module named `geometry`.

```paweli
#module "geometry"
```

imports it, at the top of another file. Access into an imported module is qualified, e.g. `geometry.area(w, h)`. There is no way to pull names into your own file unqualified.

A function becomes part of a module's public interface with `export`, called inside `main()`:

```paweli
fn area(int w, int h) -> int {
	return w * h;
}

fn main() {
	export area;
}
```

`main()` always runs when a module is imported, so `export` statements run as a side effect of importing.

**User-defined modules do not run yet.** `#module`/`#moduledef`/`export` all parse and compile, but there is no file-based module loader in the VM. Importing your own module and referencing its name at runtime fails with "undefined variable". The built-in `stdlib` module (below) is a separate thing and already works. See [`docs/design.md`](../design.md) for current status.

---

## Standard library

`stdlib` is always available, like `print`. No `#module` is needed. Every function is accessed through it, qualified: `stdlib.abs(-7)`, never a bare `abs(-7)`.

```paweli
print(stdlib.abs(-7));           // 7
print(stdlib.max(3, 9));         // 9
print(stdlib.sqrt(16.0));        // 4
print(stdlib.to_upper("hi"));    // HI
```

| Function | Signature | Description |
|---|---|---|
| `abs` | `(int) -> int` | Absolute value. |
| `max` | `(int, int) -> int` | Larger of the two. |
| `min` | `(int, int) -> int` | Smaller of the two. |
| `sqrt` | `(int \| float) -> float` | Square root. Runtime error on a negative argument. |
| `length` | `(string) -> int` | Byte length of the string. |
| `to_upper` | `(string) -> string` | Uppercased copy. |
| `concat` | `(string, string) -> string` | Concatenation. |
| `to_string` | `(int \| float \| bool) -> string` | Text representation. |
| `to_int` | `(string) -> int` | Parses a leading integer; unparseable input gives `0`. |

Wrong argument count or type is a runtime error, e.g. `stdlib.abs("x")` fails with `abs expects an int argument`, the same way any other runtime error does (see [CLI reference](cli.md)).

`print` also lives in the standard library's source (`src/stdlib/stdlib.c`), but it stays a bare global at the language level. It is not `stdlib.print`.

---

## Not yet implemented

- **Arrays**: `[` `]` are reserved tokens but have no grammar or runtime support.
- **User-defined modules**: `#module`/`#moduledef` parse and compile, but nothing resolves at runtime yet (see above). The built-in `stdlib` module already works.
- **Closures / nested functions**: `fn` is top-level only.
- **Member assignment**: `a.b = x` is rejected at compile time.
- **`break` / `continue`**: don't exist. `return` is the only early exit from a loop.
