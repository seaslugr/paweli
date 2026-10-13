# Functions and Overloading

## Functions

```paweli
fn add(int a, int b) -> int {
	return a + b;
}

fn greet(string name) {
	print(name);
}
```

`-> type` declares the return type. Without it, the function returns nothing useful, and a call to it evaluates to `null`.

Most functions sit at the top level of a file. A `fn` can also nest inside another `fn`'s body, which is how closures work. See [Closures and Function Values](closures.md).

Every program needs a function named `main` that takes no parameters. It's the entry point. `paweli` defines every top-level function first, then calls `main()`.

## Returning

`return;` with no value is the same as `return null;`. Falling off the end of a function is the same too.

A function with no `-> type` can't return a value at all. Only `return;` or falling off the end is allowed.

Watch out: the compiler checks each `return` statement on its own. It does not check every path through the function. So a function declared `-> int` that falls off the end still compiles, and it returns `null` at runtime.

## Overloading

Several functions can share a name, as long as their parameter lists differ in count or in type:

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
	print(add(1, 2)); // int overload
	print(add(1.5, 2.5)); // float overload
	print(add("a", "b")); // string overload
}
```

The compiler picks the overload at compile time, from the static types of the arguments. `add("a", "b")` goes straight to the `string` overload. If the arguments don't exactly match any overload, that is a compile error.

Overloads must differ in their parameters. Two functions with the same parameter types and different return types are a compile error. The compiler only looks at the argument types. It never looks at the return type or at how you use the result.

An exported function can be overloaded too. The importer sees every overload, and its calls resolve the same way as a local call. See [Modules](modules.md).
