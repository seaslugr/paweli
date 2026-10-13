# Native Executables

```bash
paweli build hello.pw --bin hello
./hello
```

`--bin` produces a single file that you run directly. No `paweli` install is needed to run it, and there is no separate `.paw` file to keep track of.

## How it works

The output is a copy of the `paweli` binary that ran the `build` command. The compiled program is appended after it, followed by a small trailer that records where the program starts.

On startup, every `paweli` binary checks its own file for that trailer before it does anything else. An ordinary `paweli` binary has nothing appended, so it never matches and behaves as normal.

## What you get

The executable carries the whole paweli runtime inside it (the VM and the garbage collector), plus your compiled program. It is not ahead-of-time compiled to machine code like a C or Rust program is.

Its size and startup cost show that. Expect a few hundred KB to a few MB, depending on which `paweli` build produced it, plus whatever your program adds.

## Things to know

The `paweli` binary you run `build --bin` from matters. A Debug build (AddressSanitizer, `-fanalyzer` overhead, unstripped symbols) makes a large, slow executable. A Release build makes a small, fast one.

There is no cross-compilation. The executable only runs on the same OS and architecture as the `paweli` binary that built it. Currently, that means ELF (Linux) only.
