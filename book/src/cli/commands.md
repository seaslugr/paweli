# Commands and Flags

## Run (`paweli <file>` / `paweli run [file.pw]`)

| Flag | Description |
|---|---|
| `--dump-ast` | Prints the parsed AST to stdout before compiling. Only works on a `.pw` file. On a `.paw` file or a built executable there is no source to parse, so it prints a warning and does nothing. |
| `--dump-bytecode` | Prints the disassembled bytecode of every compiled function to stdout, before running. Works on a `.pw` file, a `.paw` file, and a built executable. |
| `--jit-stats` | Prints each function's JIT state (`compiled`, `failed`, or `not compiled`) and its hotness counter to stdout, after the run finishes. Works on a `.pw` file, a `.paw` file, and a built executable. |
| `--stack-size N` | Sets the size of the VM's value stack, in slots. Default `4096`, minimum `16`. |
| `--max-frames N` | Sets the maximum call-frame depth. This is how deep function calls can nest, including recursion. Default `256`, minimum `1`. |

`--dump-ast` and `--dump-bytecode` still run the program afterward, unless parsing or compiling failed.

`--stack-size`, `--max-frames`, `--dump-bytecode`, and `--jit-stats` also work on a built executable. Pass them directly to it: `./myprogram --stack-size 8192`.

`paweli run [file.pw]` is the same thing as `paweli <file>`. The difference is what happens with no filename. `paweli <file>` needs one. `paweli run` falls back to `project.pawproj` in the current directory (see [Projects](projects.md)).

## Test (`paweli test [file.pw]`)

The same as `run`, with the same flags and the same project fallback. The one difference: it runs every `@test` top-level function and skips `main()`. `main()` still has to exist. See [Testing](../language/testing.md).

`paweli test` only works on a `.pw` file. A `.paw` file or a built executable already has its run mode fixed at `build` time, so `test` prints a warning there.

## Init (`paweli init [name]`)

Scaffolds a `project.pawproj` manifest and a starter `main.pw`. See [Projects](projects.md).

- With a `name`, it creates a directory of that name and scaffolds into it. It is fine if the directory already exists. The manifest's `name` field is set to `name`.
- Without one, it scaffolds into the current directory. The manifest's `name` field defaults to the directory's name.

It refuses to run if a manifest is already there.

## Build (`paweli build <file.pw>`)

| Flag | Description |
|---|---|
| `[output.paw]` | Optional second positional argument. Says where to write the compiled `.paw` file. Defaults to the input path with `.pw` replaced by `.paw`. If the input doesn't end in `.pw`, `.paw` is appended. Unused with `--bin`, which names its own output. |
| `--bin <name>` | Builds a native executable named `name` in place of a `.paw` file. On Windows the output is `name.exe`. See [Native Executables](native-executables.md). |
| `--strip-source` | Leaves the source text out of the output, which makes the file smaller. A runtime error from the result still prints its `[code] file:line: message` and call stack. It only loses the source-line snippet under it. Source is embedded by default. |
| `--run-tests` | Bakes test mode into the compiled program. Running the result runs every `@test` function and skips `main()`, like `paweli test` does for a `.pw` file. This flag only exists on `build`. For a single run, use `paweli test`. |

A build bundles the entry program and every module it depends on, directly or through other modules, into a single output.

Compile-time diagnostics, like `@deprecated`'s warning, print during `build`. They don't print again when you run the result.

With no `<file.pw>` argument, `build` reads `project.pawproj` from the current directory (see [Projects](projects.md)). Its output goes under `build/`. Every flag above still applies.

## Version (`paweli --version` / `paweli -v`)

Prints the installed `paweli` version (`paweli 0.1.0`) and exits. No file argument needed.

## No arguments (`paweli`)

Running `paweli` with no file and no subcommand prints a help banner: the version, the usage forms above, two example invocations, the run flags, and a pointer to this reference. The exit code is still `1`, the same as any other missing-argument error.
