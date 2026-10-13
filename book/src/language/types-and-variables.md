# Types and Variables

## Types

- `int`: 64-bit signed integer
- `float`: double-precision floating point
- `string`: text (obviously)
- `bool`: `true` or `false`

Types are static and explicit. A variable or parameter always declares its type. Every declaration, assignment, return, operator, and function call is type-checked at compile time and a mismatch is always a compile error.

However, `null` is the one exception: it is compatible with any declared type, since paweli has no separate nullable-type annotation. `int x = null;` compiles.

Any base type can be suffixed with `[]` to declare an array of that type: `int[]`, `string[]`. `[]` may be repeated to nest arrays: `int[][]` is an array of `int[]` (a 2D grid), `int[][][]` a 3D one, and so on.

A `struct` declaration introduces its own named type, usable anywhere a base type is (a variable, a parameter, a return type, an array element type). For more, see [Structs](structs.md).

## Variables

```paweli
int count = 0;
string name = "paweli!!!";
float ratio = 6.9;
```

Variables are block-scoped: a variable declared inside `{ }` stops existing once that block ends.

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
```
2
1
```

Redeclaring a name inside the *same* block is an error, but an inner block may shadow a name from an outer one, as above. That program prints `2` then `1`.

Assignment is an expression, so it produces a value. It is right-associative:

```paweli
a = b = 5;
```

Plain variables, array elements, and struct fields can be assigned to (see [Structs](structs.md)).

Ten compound assignment operators, `+= -= *= /= %= &= |= ^= <<= >>=`, combine an operator with `=`: `x += 1;` means "add 1 to `x` and store the result back in `x`", the same as `x = x + 1;` except that an index or member target's sub-expressions are only evaluated once (`arr[f()] += 1` calls `f()` exactly once). See [Operators and Punctuation](../lexical/operators-punctuation.md) for the exact type rules, including `&= |= ^= <<= >>=` and paweli's bitwise operators generally.

## Global variables

The same `type name = expr;` syntax, written outside any function, declares a global instead of a local:

```paweli
int counter = 0;

fn bump() {
	counter += 1;
}

fn main() {
	bump();
	bump();
	print(counter); // 2
}
```

A declaration's position (top level vs. inside a function body) is what makes defines it as a local variable or a global. Reading or writing one from inside a function needs no special syntax either. A bare name that isn't a local or a captured upvalue in the current scope falls through to the global of that name, the same way an unqualified function call already falls through to the top-level function of that name. A local declared with the same name as a global shadows it for the rest of that scope, with no way to reach the global from inside it:

```paweli
int x = 1;

fn shadow() -> int {
	int x = 2;
	return x; // global x is not reachable here
}
```

A global's initializer runs once, before `main()` is called, in file order: it may call any function declared anywhere in the file, and may reference any *earlier* global, but not one declared later in the file.

```paweli
int base = 10;
int derived = base * 2;   // fine, base is declared above this line
```

Every global is **private to its own file**, the same default a top-level function or struct already has (see [Modules](modules.md)): there is currently no way to `export` a global across a `#module` import. Two unrelated files can each freely declare their own `counter` with no collision.

## Memory

Strings, functions, modules, arrays, and struct instances live on a garbage-collected heap. Nothing needs manual allocation or freeing, as a mark-sweep collector runs automatically during execution whenever the heap grows past a threshold, and frees anything no longer reachable from a running variable, global, or function.

`int`, `float`, and `bool` values do not touch the heap at all; they live directly on the VM's stack.
