# Diagnostics

## Errors

Every diagnostic uses one shared format, whether it comes from the lexer, parser, compiler, or VM:

```
[code] file:line:col: error: message
```

`code` is a stable, namespaced slug like `parser::expected-token`, `compiler::duplicate-local`, or `vm::division-by-zero`. It identifies the kind of error, so it stays the same even if the message text changes. The prefix (`lexer::`, `parser::`, `compiler::`, `vm::`) tells you which stage raised it.

A few diagnostics have no exact column, so `col` is left out and they print as `[code] file:line: error: message`. That covers some compiler codegen-limit errors and all VM runtime errors, since bytecode only tracks a line per instruction.

## Warnings

Most diagnostics are an `error`, which stops the program from compiling or running. A few are a `warning`. A warning prints the same way and never stops compilation. Currently the only one is `compiler::deprecated-call`, see [Decorators](../language/decorators.md).

## Compile errors

Parse and compile errors stop before the program runs. Both stages try to report more than one error per run, so a single run can show you several separate problems at once.

## Runtime errors

A runtime error prints the failing message with its `[code] file:line: error: message` header, then a call-stack trace from the point of failure outward:

```
[vm::undefined-variable] example.pw:10: error: undefined variable 'stdlib'
example.pw:10: in main()
example.pw:1: in <script>
```

`<script>` is the top-level frame that defines every function and calls `main()`. paweli adds it for you.

There are two different stack errors:

- **`vm::stack-overflow`** (`stack overflow`): the value stack went past `--stack-size`.
- **`vm::call-stack-overflow`** (`call stack overflow`): calls nested deeper than `--max-frames`. This is usually unbounded recursion.

## Environment variables

| Variable | Effect |
|---|---|
| `NO_COLOR` | Set to any non-empty value to turn off colored diagnostic output. Otherwise color is on automatically when stderr is a terminal. |
| `PAWELI_GC_STRESS` | Set to any non-empty value to run a full garbage collection before every single instruction. Normally one only runs when the heap grows past its threshold. This is much slower. It is meant for finding GC bugs, since a missed root crashes right away instead of once in a while. It works best with a debug build's AddressSanitizer. |
| `PAWELI_GC_DEBUG` | Set to any non-empty value to print each collection's byte counts to stderr: before, after, and the threshold that triggered it. |
