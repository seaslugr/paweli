# Control Flow

Every condition below (`if`, `while`, `for`'s middle clause) must be a `bool` expression. There is no generic "truthy" coercion for other types. The same rule applies to `&&`, `||`, and `!`'s operands, and to `assert`'s first argument.

## if / else

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

## while

```paweli
while (n > 0) {
	print(n);
	n = n - 1;
}
```

This is pretty self explanitory. Same rules as an if statment.

## for

C-style three-clause `for`:

```paweli
for (int i = 0; i < 10; i = i + 1) {
	print(i);
}
```

Any of the three clauses can be omitted (`for (;;) { ... }` loops forever). A variable declared in the init clause is scoped to the loop, including its body.

However, we have two shorter forms that cover the two things a three-clause `for` is used for almost every time: counting through a range, and walking an array:

```paweli
for (int i : 0..9) {
	print(i); // 0 through 9
}

int[] nums = [10, 20, 30];
for (int n : nums) {
	print(n); // 10, then 20, then 30
}
```

`a..b` counts up from `a` to `b`, both inclusive, in steps of 1; if `a > b`, that simply means zero iterations rather than an error. The declared type on the left of `:` must match: `int` for a range, or an array's own element type for `for (T x : array)`. A string works the same way as an array of its characters: `for (string c : "abc")` binds `c` to `"a"`, then `"b"`, then `"c"`.

The range form is just a more ergonomic form of the three-clause form above it. `for (int i : 0..9)` compiles to exactly the same bytecode as writing the range and increment out by hand. The array/string form evaluates its right-hand side exactly once, before the first iteration, so `for (int x : some_function())` does not call `some_function()` again on every pass.

## break / continue

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

`break` exits the innermost `while` or `for` immediately. `continue` skips the rest of the innermost loop's current iteration and moves on to the next one: for `while`, that means re-checking the condition. For `for`, that means running the increment clause first, then checking the condition. Either one correctly leaves any locals declared since the loop started, including ones in a nested block, so a value from an earlier iteration never leaks into what runs after the loop.
