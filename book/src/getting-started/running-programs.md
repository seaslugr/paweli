# Running Programs

`paweli` has three ways to get from source to a running program.

## Run source directly

```bash
paweli hello.pw
```

The first way to run programs is directly. This parses, compiles, and runs `hello.pw` in one step and is the fastest way to iterate.

## Compile to bytecode package (paw)

```bash
paweli build hello.pw hello.paw
paweli hello.paw
```

`paweli build` compiles a `.pw` file to a `.paw` bytecode package without running it. Running a `.paw` file skips straight to execution, with no parsing or compiling step. Though, you still need a proper `paweli` installation to run it.

## Build a native executable

```bash
paweli build hello.pw --bin hello
./hello
```

`--bin` produces a single, directly-runnable file. A copy of the `paweli` binary is built with the compiled program appended to it, so it carries its own runtime with it. No `paweli` install is needed to run the result.

`paweli` also has debug flags for inspecting what the compiler produced (`--dump-ast`, `--dump-bytecode`, `--jit-stats`), and settings for the VM's stack size and call-frame depth. See the [CLI Reference](../cli/index.md) for the full list.
