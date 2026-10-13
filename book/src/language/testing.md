# Testing

```paweli
@test
fn test_addition() {
	assert(1 + 1 == 2);
}

fn main() {
	print("normal program run");
}
```

`@test` marks a function as a test. See [Decorators](decorators.md) for how `@name` works in general. A `@test` function takes no parameters, the same rule as `main()`.

`@test` does nothing on a normal run. To run your tests, use `paweli test`:

```bash
paweli test example.pw
```

This calls every `@test` function in declaration order and skips `main()`. Though, `main()` still has to exist.

Each test prints `running: <name>` before it runs. There is no pass/fail count. A failed `assert()` stops the whole run right away, with the same diagnostic and call-stack trace as any other paweli program. If the run gets to the end of the list, every test passed.

See [CLI Reference](../cli/commands.md) for the subcommand itself.
