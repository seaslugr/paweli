# Decorators

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

`@name` attaches a decorator to a top-level `fn`. It goes directly above the function. A decorator can take one string literal: `@name("some text")`. You can stack more than one.

Decorators are resolved at compile time. They don't wrap the function in a runtime object, and they don't change how you call it. If you know Python's decorators, these are much simpler.

A decorator only works on a top-level function. On a `struct`, or on a `fn` nested inside another function, it is a compile error. An unknown decorator name is a compile error too.

Currently, there are two decorators.

## `@deprecated`

Warns at every call site that resolves to the decorated overload. The message is optional.

```
[compiler::deprecated-call] example.pw:5:9: warning: 'old_way' is deprecated: use new_way instead
```

This is a warning. The program still compiles and runs the same as it does without the decorator.

If a module exports a deprecated function, the files that import it get the warning at their own call sites too.

## `@test`

Marks a function as a test. `paweli test` runs it. See [Testing](testing.md) for the full workflow.
