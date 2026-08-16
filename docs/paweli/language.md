# Language Reference

---

## Types

| Type | Description |
|---|---|
| `int` | 64-bit signed integer |
| `float` | double-precision floating point |
| `string` | text |
| `bool` | `true` or `false` |

Types are static and explicit. A variable or parameter always declares its type; there is no `let` and no inference. Every declaration, assignment, return, operator, and function call is type-checked at compile time; a mismatch is always a compile error, never something that only shows up at runtime.

`null` is the one exception: it is compatible with any declared type, since paweli has no separate nullable-type annotation. `int x = null;` compiles.

Any base type can be suffixed with `[]` to declare an array of that type: `int[]`, `string[]`. `[]` may be repeated to nest arrays: `int[][]` is an array of `int[]` (a 2D grid), `int[][][]` a 3D one, and so on.

A `struct` declaration introduces its own named type, usable anywhere a base type is (a variable, a parameter, a return type, an array element type); see [Structs](#structs).

---

## Memory

Strings, functions, modules, arrays, and struct instances live on a garbage-collected heap. Nothing needs manual allocation or freeing; a mark-sweep collector runs automatically during execution whenever the heap grows past a threshold, and frees anything no longer reachable from a running variable, global, or function.

`int`, `float`, and `bool` values do not touch the heap at all; they live directly on the VM's stack.

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

Plain variables, array elements, and struct fields can be assigned to (see [Structs](#structs)).

Ten compound assignment operators, `+= -= *= /= %= &= |= ^= <<= >>=`, combine an operator with `=`: `x += 1;` means "add 1 to `x` and store the result back in `x`", the same as `x = x + 1;` except that an index or member target's sub-expressions are only evaluated once (`arr[f()] += 1` calls `f()` exactly once, not twice). See [Operators](syntax.md#operators) for the exact type rules, including `&= |= ^= <<= >>=` and paweli's bitwise operators generally.

---

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
	print(counter);   // 2
}
```

There is no separate keyword: a declaration's position (top level vs. inside a function body) is what makes it a global. Reading or writing one from inside a function needs no special syntax either. A bare name that isn't a local or a captured upvalue in the current scope falls through to the global of that name, the same way an unqualified function call already falls through to the top-level function of that name. A local declared with the same name as a global shadows it for the rest of that scope, with no way to reach the global from inside it:

```paweli
int x = 1;

fn shadow() -> int {
	int x = 2;
	return x;   // 2, the local; the global x is not reachable here
}
```

A global's initializer runs once, before `main()` is called, in file order: it may call any function declared anywhere in the file, and may reference any *earlier* global, but not one declared later in the file.

```paweli
int base = 10;
int derived = base * 2;   // fine, base is declared above this line
```

Every global is **private to its own file**, the same default a top-level function or struct already has (see [Modules](#modules)): there is currently no way to `export` a global across a `#module` import. Two unrelated files can each freely declare their own `counter` with no collision.

---

## Arrays

```paweli
int[] nums = [1, 2, 3];
string[] names = ["alice", "bob"];
```

An array literal is a comma-separated list of expressions inside `[ ]`. Elements are read and written by index, zero-based:

```paweli
print(nums[0]);   // 1
nums[1] = 20;
```

Reading or writing past the end, or with a negative index, is a runtime error. Indexing anything other than an array or a string, or indexing with anything other than an `int`, is also a runtime error.

Strings can also be indexed, read-only: `"hello"[0]` gives `"h"`, a one-character string (paweli has no separate character type). Assigning into a string index (`s[0] = "x"`) is a runtime error; strings are immutable.

Arrays live on the garbage-collected heap, the same as strings. `stdlib.length` reports an array's element count the same way it reports a string's byte length:

```paweli
print(stdlib.length(nums));   // 3
```

`stdlib.push` and `stdlib.pop` grow and shrink an array in place:

```paweli
int[] stack = [];
stdlib.push(stack, 10);
stdlib.push(stack, 20);
int top = stdlib.pop(stack);   // 20, stack now has one element
```

`stdlib.pop` on an empty array is a runtime error.

Array literals are type-checked: every element must have the same type, and `int[] nums = ["not an int"];` is a compile error. An empty literal (`[]`) has no elements to infer a type from, so it needs a known array type from context (a `var_decl`'s declared type, an assignment's target, a `return`, or a call argument where only one overload could possibly match); `int[] nums = [];` works, but `[]` on its own with no such context is a compile error.

### Nested arrays

Repeating `[]` nests arrays: `int[][]` is an array whose elements are themselves `int[]`. Each level is type-checked independently, so `[[1, 2], [3, 4]]` is an `int[][]`, `grid[1][2]` reads one cell, and `grid[1]` yields the whole inner `int[]` row:

```paweli
int[][] grid = [[1, 2, 3], [4, 5, 6]];
print(grid[1][2]);    // 6
int[] row = grid[0];  // [1, 2, 3]
grid[1][1] = 99;
```

Rows are ordinary arrays, so they need not have the same length (a "jagged" grid):

```paweli
int[][] jagged = [[1], [2, 3], [4, 5, 6]];
```

`for (T x : grid)` iterates a nested array's *elements*, so the loop variable's type is one level shallower. `for (int[] row : grid)` over an `int[][]` binds each inner row:

```paweli
for (int[] row : grid) {
    int total = 0;
    for (int v : row) {
        total = total + v;
    }
    print(total);
}
```

Element types are checked one level at a time: `int[][] grid = [1, 2, 3];` is a compile error (`int[][]` holds `int[]`, not `int`), as is `int[][] grid = [[1, 2], ["a", "b"]];` (its second element is a `string[]`). An empty `[]` still borrows its type from context, so `int[][] grid = [];` works, and an inner `[]` inherits the outer element type: `int[][] x = [[], [1, 2]];`.

There is still no way to spell "array of function type" (`fn(int) -> int[]` is a function returning an `int[]`, not an array of functions); `[]` always binds to the nearest preceding type, so nesting only ever adds depth to the *innermost* base type.

---

## Structs

```paweli
struct Point {
	int x;
	int y;
}

fn distance(Point a, Point b) -> float {
	int dx = a.x - b.x;
	int dy = a.y - b.y;
	return stdlib.sqrt(dx * dx + dy * dy);
}

fn main() {
	Point p = Point { x: 1, y: 2 };
	p.x = p.x + 1;
	Point origin = Point { y: 0, x: 0 };   // field order in a literal doesn't matter
	print(distance(p, origin));
}
```

A `struct` declares a plain, mutable, C-like value shape: named fields, each with a type, one per line. There is no inheritance, no encapsulation, no methods, and no automatic structural equality; a struct is closer to a C `struct` than to a class. Structs can only be declared at the top level of a file, and are private to that file unless explicitly exported: see [Modules](#modules).

A struct literal, `TypeName { field: value, ... }`, must name every field the struct declares, in any order, unless the field has a declared default:

```paweli
struct Tuning {
	int base;
	float scale = 2.5;
	string label = "default";
}

fn main() {
	Tuning t = Tuning { base: 5 };                 // scale and label take their defaults
	Tuning custom = Tuning { base: 5, scale: 9.0 }; // label still takes its default
}
```

A field without `= expr` is required at every construction site; leaving it out is a compile error. A field's default is evaluated fresh at each literal that omits it; it cannot reference the struct's own fields or any local variable.

The type name can be left off, `{ field: value, ... }`, whenever it can be inferred from context: a `var_decl`'s declared type, an assignment's target, a `return`, a call argument (when only one overload could take it), an array literal's element type, or an enclosing struct literal's own field type. The same idea an empty array literal `[]` already uses to borrow its element type from context, extended to structs:

```paweli
fn offset(Point p) -> Point {
	return { x: p.x + 1, y: p.y + 1 };   // inferred from the declared return type
}

fn main() {
	Point p = { x: 1, y: 2 };            // inferred from the var_decl
	p = { x: 10, y: 20 };                // inferred from the assignment target
	Line l = Line { start: { x: 0, y: 0 }, end: { x: 3, y: 4 } };   // inferred from Line's field types
}
```

With nothing in context to infer from, e.g. `print({ x: 1 })`, a bare `{ ... }` is a compile error asking for an explicit type name.

`.` reads and writes a field:

```paweli
Point p = Point { x: 1, y: 2 };
p.x = p.x + 1;
```

Referencing an undeclared field, or constructing a literal for an unknown struct type, is a compile error. Struct values are compared and assigned by identity, exactly like arrays: `==` is true only when both sides are the same instance, not when their fields happen to match.

A struct field can be an array, or another struct type, including the struct's own type, or one declared earlier in the same file. A struct may only reference itself or an earlier struct as a field's type, never one declared later in the file:

```paweli
struct Node {
	int value;
	Node next;   // fine: a struct field may reference its own type
}

fn main() {
	Node tail = Node { value: 2, next: null };      // null unifies with any struct type
	Node head = Node { value: 1, next: tail };
	print(head.next.value);   // 2
}
```

A `struct` body can also declare functions, called as `TypeName.name(args)`:

```paweli
struct Point {
	int x;
	int y;

	fn origin() -> Point {
		return Point { x: 0, y: 0 };
	}

	fn length(Point self) -> float {
		return stdlib.sqrt(self.x * self.x + self.y * self.y);
	}
}

fn main() {
	Point p = Point.origin();
	print(Point.length(Point { x: 3, y: 4 }));   // 5
}
```

There is exactly one way to call a struct function: `TypeName.name(args)`. `self` in the example above is an ordinary parameter, written like any other. There is no receiver syntax (`p.length()`), no implicit first argument, and no special meaning attached to the name `self` at all; it's purely a convention. A struct's functions overload the same way top-level functions do, and live in their own namespace scoped to that one struct: two different structs (or a struct and a top-level function) can freely share a function name with no collision. Struct functions do not support decorators.

Exporting a struct (`export TypeName;`) exports its whole function table along with it. There is no separate export step for struct functions. On the importing side they're referenced unqualified, `TypeName.name(args)`, the same way the imported struct type itself already is (not `module.TypeName.name(args)`).

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

`-> type` declares a return type. Omitting it means the function returns nothing meaningful (its call expression evaluates to `null`). Most functions are declared at the top level of a file; a `fn` can also nest inside another `fn`'s body, which is how closures work, see [Closures / nested functions](#closures--nested-functions).

Every program needs a function named `main`, taking no parameters. It's the entry point: `paweli` binds every top-level function first, then calls `main()`.

`return` with no value is the same as `return null;`. Falling off the end of a function without an explicit `return` also returns `null`. A function with no declared `-> type` cannot `return` a value at all (`return expr;` is a compile error there); only `return;` or falling off the end are allowed. This is checked per `return` statement, not by proving every path returns something of the right type: a function declared `-> int` that falls off the end without an explicit `return` still compiles, and returns `null` at runtime rather than an `int`.

---

## Overloading

Multiple functions can share a name as long as their parameter lists differ, either in count or in type:

```paweli
fn add(int a, int b) -> int {
	return a + b;
}

fn add(float a, float b) -> float {
	return a + b;
}

fn add(string a, string b) -> string {
	return stdlib.concat(a, b);
}

fn main() {
	print(add(1, 2));         // int overload
	print(add(1.5, 2.5));     // float overload
	print(add("a", "b"));     // string overload
}
```

Resolution happens entirely at compile time, from the static types of the arguments at the call site: `add("a", "b")` is wired directly to the `string` overload, not dispatched at runtime. Calling with arguments that no overload's parameters match exactly is a compile error, not a runtime one.

Two functions with identical parameter types but different return types are not a valid overload pair; declaring both is a compile error. Overload resolution never considers the return type or how the result is used, only the arguments.

A function exported from a module can be overloaded the same way; the importer sees every overload and resolves calls into it exactly like a local call (see [Modules](#modules)).

---

## Decorators

```paweli
@deprecated("use new_way instead")
fn old_way(int x) -> int {
	return x * 2;
}

@test
fn test_addition() {
	assert(1 + 1 == 2);
}
```

`@name` (optionally `@name("a string literal")`) attaches a decorator to a top-level `fn` declaration, immediately above it. More than one can stack. Decorators are resolved entirely at compile time: neither one wraps the function in a runtime object or changes how it's called, unlike Python's decorators. A decorator on anything other than a top-level function (a `struct`, or a nested `fn` inside another function's body) is a compile error; an unknown decorator name is too.

**`@deprecated`** warns at every call site that resolves to the decorated overload, with an optional message:

```
[compiler::deprecated-call] example.pw:5:9: warning: 'old_way' is deprecated: use new_way instead
```

This is a warning: the program still compiles and runs exactly as it would without the decorator. A decorated overload exported from a module still warns at the importing file's own call sites, not just within the file that declared it.

**`@test`** marks a function as a test. A `@test` function must take no parameters (the same rule `main()` follows). It has no effect on an ordinary run; a new CLI flag, `--run-tests`, calls every `@test`-tagged function in declaration order instead of `main()` (which still has to exist, it just isn't called):

```bash
paweli --run-tests example.pw
```

Each test prints `running: <name>` before it runs. There's no separate pass/fail bookkeeping: an `assert()` failure inside a test aborts the whole run immediately, with the same diagnostic and call-stack trace any other paweli program would produce; reaching the end of the list with nothing aborting is what "every test passed" means. See [CLI reference](cli.md) for the flag itself.

---

## Control flow

Every condition below (`if`, `while`, `for`'s middle clause) must be a `bool` expression. There is no generic "truthy" coercion for other types; the same rule applies to `&&`, `||`, and `!`'s operands, and to `assert`'s first argument.

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

Two shorter forms cover the two things a three-clause `for` is used for almost every time: counting through a range, and walking an array:

```paweli
for (int i : 0..10) {
	print(i);          // 0 through 9
}

int[] nums = [10, 20, 30];
for (int n : nums) {
	print(n);           // 10, then 20, then 30
}
```

`a..b` counts up from `a` (inclusive) to `b` (exclusive) in steps of 1; if `a >= b`, that simply means zero iterations rather than an error. The declared type on the left of `:` must match: `int` for a range, or an array's own element type for `for (T x : array)`. A string works the same way as an array of its characters: `for (string c : "abc")` binds `c` to `"a"`, then `"b"`, then `"c"`.

The range form is pure sugar for the three-clause form above it (`for (int i : 0..10)` compiles to exactly the same bytecode as writing the range and increment out by hand); the array/string form evaluates its right-hand side exactly once, before the first iteration, so `for (int x : some_function())` does not call `some_function()` again on every pass.

### break / continue

```paweli
for (int i = 0; i < 10; i = i + 1) {
	if (i == 3) {
		continue;
	}
	if (i == 7) {
		break;
	}
	print(i);
}
```

`break` exits the innermost `while` or `for` immediately. `continue` skips the rest of the innermost loop's current iteration and moves on to the next one: for `while`, that means re-checking the condition; for `for`, that means running the increment clause first, then checking the condition. Both are a compile error outside a loop. Either one correctly leaves any locals declared since the loop started, including ones in a nested block, so a value from an earlier iteration never leaks into what runs after the loop.

---

## Modules

```paweli
#moduledef "geometry"
```

declares the current file as a module named `geometry`. A file has at most one directive, and it must be the first line: `#moduledef` and `#module` cannot both appear, and neither can appear anywhere else.

```paweli
#module "geometry"
```

imports it, at the top of another file. `geometry` resolves to `geometry.pw` in the same directory as the importing file; there is no search path. Access into an imported module is qualified, e.g. `geometry.area(w, h)`. There is no way to pull names into your own file unqualified.

A function becomes part of a module's public interface with `export`, declared inside `main()`:

```paweli
fn area(int w, int h) -> int {
	return w * h;
}

fn main() {
	export area;
}
```

`export` is a purely compile-time declaration, not something that runs: a module's `main()` never actually executes when the module is imported. `export area;` must be a direct statement in `main()`'s own body, not nested inside an `if`/`while`/`for`; since it doesn't run, a conditional export would be misleading (the function ends up exported either way), so it is rejected outright instead. `export` is a compile error anywhere outside a `#moduledef` file, or outside `main()`.

Every top-level function is private to its own file by default, including across an import: a helper `area()` calls but never `export`s stays invisible to whatever imported it, and two unrelated files can each freely have their own `main`, `area`, or anything else without colliding. Only what gets `export`ed is reachable, and only through the module's own name. An overloaded function can be exported; the importer sees and can call every overload (see [Overloading](#overloading)).

A `struct` type can be exported the same way, `export TypeName;` inside `main()`:

```paweli
struct Point {
	int x;
	int y;
}

fn origin() -> Point {
	return Point { x: 0, y: 0 };
}

fn main() {
	export Point;
	export origin;
}
```

Unlike a function, an exported struct's name is used *unqualified* on the importing side, `Point { x: 1, y: 2 }` rather than `geometry.Point { ... }`, even though the module itself is still accessed qualified for everything else. This is a deliberate choice: a file can only ever have one `#module` dependency (see below), so there is never more than one imported struct registry an unqualified name could mean, and a local struct declared with the same name as an imported one is a compile error rather than silently shadowing it. A struct that is declared in a `#moduledef` file but never `export`ed stays just as invisible to an importer as an unexported function does. An exported struct's own functions (see [Structs](#structs)) travel with it automatically. `export Point;` is enough; there is no separate step. They are referenced unqualified too, `Point.origin()` not `geometry.Point.origin()`.

A `#module` dependency is resolved, parsed, and fully compiled before the file that imports it: the compiler needs the module's exported signatures in hand to type-check and overload-resolve every `module.member(...)` call. Importing a file that fails to parse, fails to compile, has no `#moduledef` directive, or declares a name that does not match what was asked for is a compile error at the `#module` line, before anything runs.

A module name can include a `/` to import from a subdirectory, e.g. `#module "shapes/geometry"` looks for `shapes/geometry.pw` next to the importing file; the local binding is still just `geometry` (the part after the last `/`), matching what code that uses it writes.

A `#moduledef` file can never itself have a `#module` import (a file has at most one directive), so a module dependency graph is always exactly one file deep. There is no cycle to worry about as a result.

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
| `length` | `(string) -> int` | Byte length of a string. Also accepts an array, returning its element count. |
| `to_upper` | `(string) -> string` | Uppercased copy. |
| `concat` | `(string, string) -> string` | Concatenation. |
| `split` | `(string, string) -> string[]` | Splits on every occurrence of the delimiter. Runs of the delimiter collapse, so no empty tokens come out. Runtime error on an empty delimiter. |
| `to_string` | `(int \| float \| bool) -> string` | Text representation. |
| `to_int` | `(string) -> int` | Parses a leading integer; unparseable input gives `0`. |
| `push` | `(array, value)` | Appends `value` to the end of `array`, in place. |
| `pop` | `(array) -> value` | Removes and returns an array's last element. Runtime error on an empty array. |
| `sort` | `(T[], fn(T,T)->bool)` | Sorts an array in place, stably, using the given comparator: `less_than(a, b)` should report whether `a` belongs before `b`. See [Function values](#function-values). |
| `read_line` | `() -> string` | Reads one line from stdin, without the trailing newline. An empty string on EOF. |

Wrong argument count or type is a compile error, e.g. `stdlib.abs("x")` fails to compile, the same way calling a user function with the wrong argument types does (see [Overloading](#overloading)). Every native still checks its arguments at runtime too, as a backstop; that path should never actually trigger for a program that compiled successfully.

`print` also lives in the standard library's source (`src/stdlib/stdlib.c`), but it stays a bare global at the language level. It is not `stdlib.print`.

---

## Closures / nested functions

```paweli
fn make_counter() -> int {
	int count = 0;

	fn increment() -> int {
		count = count + 1;
		return count;
	}

	print(increment());   // 1
	print(increment());   // 2
	return increment();    // 3
}
```

`fn` can also appear as a statement inside another function's body. A nested fn can read and write any local (including a parameter) from every scope it's nested inside, by reference: `increment` above shares the same `count` across every call, not a fresh copy taken when it was declared. Two nested fns declared in the same enclosing scope that both reference the same local share that capture too; a mutation through either one is visible through the other.

A nested fn can call itself (its own name is visible inside its own body, for recursion), but two nested fns declared as siblings cannot call each other: a nested fn's name is not hoisted the way a top-level fn's is, so a forward reference to a not-yet-declared sibling is an ordinary "undefined function" error, the same as referencing any other not-yet-declared local would be.

A nested fn's value can be stored in a variable, passed as an argument, or returned from a function. See [Function values](#function-values) below.

---

## Function values

A function's type is written `fn(ParamType, ...) -> ReturnType`, or `fn(ParamType, ...)` with no `-> type` for one that returns nothing:

```paweli
fn add(int a, int b) -> int {
	return a + b;
}

fn main() {
	fn(int, int) -> int op = add;   // add's value, not a call
	print(op(2, 3));                 // 5, calling through the variable
}
```

A bare function name, everywhere except immediately followed by `(` (which is still an ordinary call), is that function's value. If the name is overloaded, which overload it refers to is resolved the same way an empty array literal `[]` or a bare struct literal `{ ... }` already infer their type: from a known function type in context (a `var_decl`'s declared type, a parameter's declared type, a return type, ...). With exactly one overload there is nothing to infer, so no context is needed; with more than one and no context to resolve it, referencing the bare name is a compile error.

Passing a function as a callback:

```paweli
fn apply_twice(fn(int) -> int f, int x) -> int {
	return f(f(x));
}

fn double(int x) -> int {
	return x * 2;
}

fn main() {
	print(apply_twice(double, 5));   // 20
}
```

Returning a closure to the caller: a nested fn (see [Closures / nested functions](#closures--nested-functions) above) is a function value like any other, so it can be handed back instead of only ever being called from inside the scope that declared it:

```paweli
fn make_adder(int amount) -> fn(int) -> int {
	fn adder(int x) -> int {
		return x + amount;
	}
	return adder;
}

fn main() {
	fn(int) -> int add5 = make_adder(5);
	print(add5(10));   // 15
}
```

A lambda literal is an anonymous function used directly as an expression, most often inline as a call argument, with the same `fn(TYPE NAME, ...) -> TYPE { ... }` shape a declaration uses, minus the name:

```paweli
fn main() {
	print(apply_twice(fn(int x) -> int { return x + 1; }, 10));   // 12
}
```

It's a real closure exactly like a nested fn statement is (same capture-by-reference rules), just written and used in one place instead of declared and referenced by name.

A struct field can hold a function value too, and a struct function itself can call a function passed to it in a field or a parameter. There's nothing special about a function value once it's stored somewhere; it behaves like any other value of a declared type.

One real gap: there is no way to write "array of function type" (`fn(int) -> int[]` parses as "a function returning `int[]`", not "an array of `fn(int) -> int` values"; `[]` always binds to the nearest preceding type, and there is no grouping syntax to override that). Every position a type is written (a `var_decl`, a parameter, a return type, a struct field) uses this same grammar, so an array of function values has no way to be declared at all right now, not just in one specific position.

---

## Error handling

Every runtime failure, whether a VM-triggered one (division by zero, an out-of-bounds index, ...) or a `throw` from your own code, carries a plain-text message. Left uncaught, it prints a diagnostic and a call-stack trace and exits nonzero, exactly the same as before this feature existed. `catch` and `try`/`catch` let a program intercept that failure instead and keep running.

`throw` raises one:

```paweli
fn divide(int a, int b) -> int {
	if (b == 0) {
		throw "division by zero";
	}
	return a / b;
}
```

`throw` always takes a `string`. There's no structured/typed error payload, just a message.

The simplest way to catch one is postfix `catch`, an expression that supplies a typed fallback:

```paweli
int result = divide(10, 0) catch(int -1);   // -1
int ok = divide(10, 2) catch(int -1);       // 5
```

`catch(TYPE fallback)` is explicitly typed, like every other paweli declaration. There's no inference from the surrounding expression.

For anything beyond "just give me a default," a full `try { } catch(...) { }` block form binds the caught message, supplies a fallback, or both:

```paweli
// Binding only, used as a statement. The block's value (if any) is discarded.
try {
	print(divide(1, 0));
} catch(string message) {
	print(message);   // "division by zero"
}

// Binding AND a fallback, used as an expression.
int result = try {
	divide(10, 0)
} catch(string message : int -99) {
	print(message);
};
```

A `try` block used as an **expression** works like a lambda or nested fn's body: its trailing bare expression, with no `;`, is its value. This is the only place in the whole grammar a statement's `;` is optional. Used as a **statement**, every line inside (including the last) is an ordinary semicolon-terminated statement, and any value it would have produced is discarded.

`catch(...)`'s grammar: a `TYPE` followed by a bare identifier declares a binding (`catch(string message)`); a `TYPE` followed by anything else is a fallback expression (`catch(int -1)`). The two can combine with `:`, as in `catch(string message : int -99)`. A `{ ... }` body is required when there's no fallback (the catch clause has to do *something*); it's optional when there is one. A caught message is always a `string`. The binding's declared type must be `string`, checked at compile time.

`try`/`catch` nests freely, and a `throw` from inside a `try` is caught by it exactly like a VM-triggered failure would be:

```paweli
int outer = try {
	int inner = try {
		divide(1, 0)
	} catch(int -1) {
	};
	divide(inner, 0)
} catch(int -2) {
};
```

**One real limitation**: a `try` block compiles to an anonymous closure, invoked the same way a native calling back into paweli code already works. That means `return`, `break`, and `continue` do not cross a `try` block's boundary. Instead they apply to the (nonexistent) enclosing loop/function of the try-closure itself, which triggers a compile error rather than a jump to the real enclosing function or loop. This mirrors how a lambda already can't `break` out of a loop it was merely passed into.
