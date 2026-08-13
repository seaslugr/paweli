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

Parse and compile errors are reported as `file:line: error: message` (parse errors also include a column: `file:line:column: error: message`) and stop before the program runs.

Runtime errors print the failing message, then a call-stack trace from the point of failure outward:

```
undefined variable 'stdlib'
example.pw:10: in main()
example.pw:1: in <script>
```

`<script>` is the synthetic top-level frame that defines every function and calls `main()`. It is not a function you wrote.

Two distinct stack-related errors exist:

- **`stack overflow`**: the value stack exceeded `--stack-size`.
- **`call stack overflow`**: call nesting exceeded `--max-frames`, usually from unbounded recursion.
