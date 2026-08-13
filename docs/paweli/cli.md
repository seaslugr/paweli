# CLI Reference

```bash
paweli [--dump-ast] [--dump-bytecode] [--stack-size N] [--max-frames N] <file.pw>
```

`paweli` reads a single source file, compiles it, and runs it. Exit code is `0` on success, `1` if parsing, compiling, or running the program failed.

---

## Flags

| Flag | Description |
|---|---|
| `--dump-ast` | Prints the parsed AST to stdout before compiling. Useful for debugging the parser. |
| `--dump-bytecode` | Prints disassembled bytecode for every compiled function to stdout, before running. Useful for debugging the compiler. |
| `--stack-size N` | Sets the VM's value stack size, in slots. Default `4096`, minimum `16`. |
| `--max-frames N` | Sets the maximum call-frame depth (how deep function calls can nest, including recursion). Default `256`, minimum `1`. |

Both `--dump-ast` and `--dump-bytecode` still run the program afterward, unless parsing or compiling failed.

---

## Errors

Every diagnostic, from the lexer, parser, compiler, or VM, is reported in one shared format:

```
[code] file:line:col: error: message
```

`code` is a stable, namespaced slug (`parser::expected-token`, `compiler::duplicate-local`, `vm::division-by-zero`, ...) that identifies the kind of error independent of the message text. The stage prefix (`lexer::`, `parser::`, `compiler::`, `vm::`) tells you which stage raised it. `col` is omitted for the handful of diagnostics that don't have a precise source column (some compiler codegen-limit errors, and all VM runtime errors, whose bytecode only tracks a line per instruction): those print as `[code] file:line: error: message`.

Parse and compile errors stop before the program runs. Both stages try to collect and report more than one error per run instead of stopping at the first one, so a single invocation can surface several independent problems at once.

Runtime errors print the failing message with its own `[code] file:line: error: message` header, then a call-stack trace from the point of failure outward:

```
[vm::undefined-variable] example.pw:10: error: undefined variable 'stdlib'
example.pw:10: in main()
example.pw:1: in <script>
```

`<script>` is the synthetic top-level frame that defines every function and calls `main()`. It is not a function you wrote.

Two distinct stack-related errors exist:

- **`vm::stack-overflow`** (`stack overflow`): the value stack exceeded `--stack-size`.
- **`vm::call-stack-overflow`** (`call stack overflow`): call nesting exceeded `--max-frames`, usually from unbounded recursion.
