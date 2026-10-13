# Error Handling

Every runtime failure carries a plain-text message. That covers failures from the VM (division by zero, an out-of-bounds index, and so on) and a `throw` from your own code.

If nothing catches the failure, paweli prints a diagnostic and a call-stack trace, then exits nonzero. `catch` and `try`/`catch` let your program catch the failure and keep running.

## throw

```paweli
fn divide(int a, int b) -> int {
	if (b == 0) {
		throw "division by zero";
	}
	return a / b;
}
```

`throw` always takes a `string`. There is no typed error payload. It's just a message.

## Postfix catch

The simplest way to catch a failure is postfix `catch`. It's an expression that gives a typed fallback:

```paweli
int result = divide(10, 0) catch(int -1); // -1
int ok = divide(10, 2) catch(int -1); // 5
```

`catch(TYPE fallback)` is explicitly typed, like every other declaration in paweli. Nothing is inferred from the surrounding expression.

## try / catch

For anything beyond "just give me a default", use the full `try { } catch(...) { }` block. It can bind the caught message, give a fallback, or both:

```paweli
// Binding only, used as a statement. The block's value (if any) is discarded.
try {
	print(divide(1, 0));
} catch(string message) {
	print(message); // "division by zero"
}

// Binding AND a fallback, used as an expression.
int result = try {
	divide(10, 0)
} catch(string message : int -99) {
	print(message);
};
```

A `try` block works as a statement or as an expression:

- As an expression, its last line is a bare expression with no `;`. That is the block's value. This is the only place in the whole grammar where a `;` is optional.
- As a statement, every line inside ends with `;`, including the last one. Any value it produces is thrown away.

## The catch clause

- `catch(string message)`: a `TYPE` followed by a bare identifier declares a binding.
- `catch(int -1)`: a `TYPE` followed by anything else is a fallback expression.
- `catch(string message : int -99)`: both, joined with `:`.

The `{ ... }` body is required when there is no fallback, since the clause has to do *something*. With a fallback, the body is optional.

A caught message is always a `string`, so the binding's type must be `string`. The compiler checks this.

## Nesting

`try`/`catch` nests freely. A `throw` inside a `try` is caught the same way a VM failure is:

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

## Limitation

Watch out: `return`, `break`, and `continue` don't cross a `try` block. Using one inside a `try` block to leave the function or loop around it is a compile error.

This is because a `try` block compiles to an anonymous closure. A `break` inside it has no loop of its own to break out of. A lambda has the same rule. It can't `break` out of a loop it was passed into either.
