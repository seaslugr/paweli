# CLI Reference

```bash
paweli [--dump-ast] [--dump-bytecode] [--run-tests] [--stack-size N] [--max-frames N] <file.pw|file.paw>
paweli build <file.pw> [output.paw] [--strip-source] [--run-tests]
paweli build <file.pw> --bin <name> [--strip-source] [--run-tests]
```

`paweli <file>` runs a program. Given a `.pw` source file, it parses, compiles, and runs it. Given a `.paw` file (a compiled bytecode container, detected by its magic bytes regardless of extension), it skips straight to running the already-compiled program, with no parsing or compiling step at all. Exit code is `0` on success, `1` if parsing, compiling, or running failed.

`paweli build` compiles a `.pw` file (and the `#module` it depends on, if any) without running it, for distributing a program as one file instead of loose source. Two output kinds:

- **`.paw`** (default): a compiled bytecode container. Output path is an optional second argument; defaults to the input path with its `.pw` extension replaced by `.paw`. Still needs `paweli` installed to run (`paweli out.paw`).
- **Native executable** (`--bin <name>`): a single, directly-runnable file (`./name`, no `paweli` involved), a copy of the currently running `paweli` binary with the compiled program appended to it, so it carries its own runtime with it. See [Native executables](#native-executables) below.

See [Modules](language.md#modules) for `#module`/`#moduledef`.

---

## Flags

### Run (`paweli <file>`)

| Flag | Description |
|---|---|
| `--dump-ast` | Prints the parsed AST to stdout before compiling. Only meaningful for a `.pw` file; has no effect (with a warning) on a `.paw` file or a built executable, since there is no source to parse. |
| `--dump-bytecode` | Prints disassembled bytecode for every compiled function to stdout, before running. Works for a `.pw` file, a `.paw` file, and a built executable. |
| `--run-tests` | Runs every `@test`-tagged top-level function instead of `main()` (`main()` still has to exist, it just isn't called). See [Decorators](language.md#decorators). Only meaningful for a `.pw` file; a `.paw` file's or built executable's run mode was already fixed at `build` time (with a warning if passed anyway). |
| `--stack-size N` | Sets the VM's value stack size, in slots. Default `4096`, minimum `16`. |
| `--max-frames N` | Sets the maximum call-frame depth (how deep function calls can nest, including recursion). Default `256`, minimum `1`. |

Both `--dump-ast` and `--dump-bytecode` still run the program afterward, unless parsing or compiling failed. `--stack-size`/`--max-frames`/`--dump-bytecode` all work the same way on a built executable, passed directly to it (`./myprogram --stack-size 8192`). It takes no other arguments, since the program it runs is fixed.

### Build (`paweli build <file.pw>`)

| Flag | Description |
|---|---|
| `[output.paw]` | Optional second positional argument: where to write the compiled `.paw` file. Defaults to the input path with its `.pw` extension replaced by `.paw` (or `.paw` appended, if the input doesn't end in `.pw`). Not used with `--bin`, which names its own output. |
| `--bin <name>` | Build a native executable named `name` instead of a `.paw` file. See [Native executables](#native-executables). |
| `--strip-source` | Omits the embedded source text from the output. Produces a smaller file; a runtime error from the result still prints its `[code] file:line: message` and call stack, just without the source-line snippet under it. Source is embedded by default. |
| `--run-tests` | Bakes `--run-tests` mode into the compiled program: running the result runs every `@test`-tagged function instead of `main()`, the same as passing `--run-tests` at run time would for the equivalent `.pw` file. |

A build bundles the entry program and its one `#module` dependency (if any) into a single output; a `#moduledef` file can never itself have a `#module` import, so there is never more than one to bundle. Compile-time-only diagnostics, like `@deprecated`'s warning, are printed during `build`, not when the result is later run.

---

## Native executables

```bash
paweli build hello.pw --bin hello
./hello
```

`--bin` produces a self-contained, directly-runnable file: a copy of the `paweli` binary that ran the `build` command, with the compiled program appended after it, followed by a small trailer recording where that program starts. On startup, every `paweli` invocation checks its own executable file for that trailer before doing anything else; an ordinary `paweli` binary (nothing appended) never matches, so this adds no visible behavior to normal use. `paw_is_self_extracting()`/`paw_build_executable()` in `src/compiler/paw.c` are the implementation.

This means the executable still carries the whole paweli runtime (VM, garbage collector) inside it, not just the compiled program. It is not an ahead-of-time-compiled native binary the way a C or Rust program is, and its size and startup cost reflect that (a few hundred KB to a few MB, depending on which `paweli` build produced it, plus whatever the compiled program itself adds). What it buys is a real, standalone file: no `paweli` install needed to run it, no separate `.paw` file to keep track of.

Which `paweli` binary you run `build --bin` from matters: a Debug build (AddressSanitizer, `-fanalyzer` overhead, unstripped symbols) makes a correspondingly large, slow executable; a Release build makes a small, fast one. There is no cross-compilation. The executable a given `paweli` binary produces only runs on the same OS/architecture that binary itself does. Currently ELF (Linux) only.

---

## Errors

Every diagnostic, from the lexer, parser, compiler, or VM, is reported in one shared format:

```
[code] file:line:col: error: message
```

`code` is a stable, namespaced slug (`parser::expected-token`, `compiler::duplicate-local`, `vm::division-by-zero`, ...) that identifies the kind of error independent of the message text. The stage prefix (`lexer::`, `parser::`, `compiler::`, `vm::`) tells you which stage raised it. `col` is omitted for the handful of diagnostics that don't have a precise source column (some compiler codegen-limit errors, and all VM runtime errors, whose bytecode only tracks a line per instruction): those print as `[code] file:line: error: message`.

Most diagnostics are `error`, which stop the program from compiling or running; a handful are `warning` (currently just `compiler::deprecated-call`, see [Decorators](language.md#decorators)), which print the same way but never stop compilation.

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

---

## Environment variables

| Variable | Effect |
|---|---|
| `NO_COLOR` | Set to any non-empty value to disable colored diagnostic output. Color is otherwise on automatically when stderr is a terminal. |
| `PAWELI_GC_STRESS` | Set to any non-empty value to run a full garbage-collection cycle before every single instruction, instead of only when the heap grows past its threshold. Much slower; meant for shaking out GC correctness bugs (a missed root shows up as an immediate crash instead of a rare one), especially combined with a debug build's AddressSanitizer. |
| `PAWELI_GC_DEBUG` | Set to any non-empty value to print each collection's byte counts (before, after, and the threshold that triggered it) to stderr. |
