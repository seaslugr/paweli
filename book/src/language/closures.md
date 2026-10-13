# Closures and Function Values

## Nested functions

```paweli
fn make_counter() -> int {
	int count = 0;

	fn increment() -> int {
		count = count + 1;
		return count;
	}

	print(increment()); // 1
	print(increment()); // 2
	return increment(); // 3
}
```

A `fn` can also be a statement inside another function's body. A nested fn can read and write any local from the scopes around it, including parameters.

It captures by reference. `increment` above uses the same `count` on every call. It never gets its own copy. Two nested fns in the same scope that use the same local share it too, so a change through one is visible through the other.

A nested fn can call itself, since its own name is visible inside its body. Though, it can't call a sibling that is declared after it. Top-level fns are hoisted and nested fns are not. Calling a sibling too early is an "undefined function" error, the same as using any other local before its declaration.

You can store a nested fn in a variable, pass it as an argument, or return it. That's the next section.

## Function values

A function type is written `fn(ParamType, ...) -> ReturnType`. Leave off the `-> ReturnType` for a function that returns nothing:

```paweli
fn add(int a, int b) -> int {
	return a + b;
}

fn main() {
	fn(int, int) -> int op = add; // add's value, no call happens here
	print(op(2, 3)); // 5, calling through the variable
}
```

A bare function name is that function's value. A name followed directly by `(` is still an ordinary call.

If the name is overloaded, the compiler needs to know which overload you mean. It takes that from a known function type in the context, like a variable's declared type, a parameter type, or a return type. An empty `[]` and a bare `{ ... }` get their type the same way. With one overload, no context is needed. With several overloads and no context, the bare name is a compile error.

### Callbacks

```paweli
fn apply_twice(fn(int) -> int f, int x) -> int {
	return f(f(x));
}

fn double(int x) -> int {
	return x * 2;
}

fn main() {
	print(apply_twice(double, 5)); // 20
}
```

### Returning a closure

A nested fn is a function value like any other, so a function can hand it back to its caller:

```paweli
fn make_adder(int amount) -> fn(int) -> int {
	fn adder(int x) -> int {
		return x + amount;
	}
	return adder;
}

fn main() {
	fn(int) -> int add5 = make_adder(5);
	print(add5(10)); // 15
}
```

### Lambdas

A lambda is an anonymous function written as an expression. You mostly use one inline, as a call argument. It has the same shape as a declaration without the name, `fn(TYPE NAME, ...) -> TYPE { ... }`:

```paweli
fn main() {
	print(apply_twice(fn(int x) -> int { return x + 1; }, 10)); // 12
}
```

A lambda is a real closure. It follows the same capture-by-reference rules as a nested fn.

### In structs

A struct field can hold a function value too. A struct function can call a function that it gets through a field or a parameter. A function value is nothing special once it is stored somewhere. It behaves like any other value with a declared type.

## No arrays of functions

Currently, there is no way to write an array of functions. `fn(int) -> int[]` means "a function that returns `int[]`". `[]` always binds to the nearest type before it, and there is no grouping syntax to change that.

Every place you write a type (a variable, a parameter, a return type, a struct field) uses this same grammar. So you can't declare an array of function values anywhere right now.
