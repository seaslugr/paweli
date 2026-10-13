# CLI Reference

```bash
paweli [--dump-ast] [--dump-bytecode] [--jit-stats] [--stack-size N] [--max-frames N] <file.pw|file.paw>
paweli init [name]
paweli run [file.pw] [options]
paweli test [file.pw] [options]
paweli build <file.pw> [output.paw] [--strip-source] [--run-tests]
paweli build <file.pw> --bin <name> [--strip-source] [--run-tests]
paweli build [--bin <name>] [--strip-source] [--run-tests]
paweli --version
```

## `paweli <file>`

Runs a program. Given a `.pw` source file, it parses, compiles, and runs it. Given a `.paw` file, it skips straight to running, with no parsing or compiling step. A `.paw` file is detected by its magic bytes, so the extension doesn't matter.

The exit code is `0` on success, and `1` if parsing, compiling, or running failed.

## `paweli init`

Scaffolds a project: a `project.pawproj` manifest and a starter `main.pw`. See [Projects](projects.md).

## `paweli run` and `paweli test`

`paweli run file.pw` does the same thing as `paweli file.pw`. Given no filename, it reads `entry` out of `project.pawproj` in the current directory and runs that.

`paweli test` is the same, except it runs every `@test` function and skips `main()`. This is the only way to run tests on a source file.

Neither one writes an output file. See [Projects](projects.md).

## `paweli build`

Compiles a `.pw` file and every `#module` it depends on, without running it. Use it to distribute a program as one file. There are two kinds of output:

- **`.paw`** (default): a compiled bytecode package. The output path is an optional second argument. It defaults to the input path with `.pw` replaced by `.paw`. You still need `paweli` installed to run it (`paweli out.paw`).
- **Native executable** (`--bin <name>`): a single file you run directly (`./name`). It is a copy of the `paweli` binary with the compiled program appended, so it carries its own runtime. See [Native Executables](native-executables.md).

Given no filename, `paweli build` reads `project.pawproj` from the current directory and writes its output under `build/`. See [Projects](projects.md).

See [Modules](../language/modules.md) for `#module`/`#moduledef`.

## In this chapter

- [Commands and Flags](commands.md)
- [Projects](projects.md)
- [Native Executables](native-executables.md)
- [Diagnostics](diagnostics.md)
