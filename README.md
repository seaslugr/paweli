# Paweli Language

**Paweli** is a statically-typed, C-like programming language that runs on a stack-based VM and a custom JIT compiler.

The name **Paweli** comes from the sea slug [`Costasiella paweli`](https://en.seaslug.world/species/costasiella_paweli).

```paweli
fn main() {
	print("Hello World!");
}
```

Book can be found [`Here`](https://seaslugr.github.io/paweli/).

## Features

- Statically typed
- Structs with struct-scoped functions
- First-class functions, closures, and lambdas
- `try`/`catch`/`throw` error handling
- A small standard library (`stdlib.*`)
- A module system for multi-file programs
- Decorators (`@test`, `@deprecated`)
- A mark-sweep garbage collector
- A JIT compiler for hot loops and functions
- Debug/Config flags: `--dump-ast`, `--dump-bytecode`, `--jit-stats`

## Performance

Soon.

## Usage

Run a program from source:
```bash
paweli hello.pw
```

Scaffold and run a project (see [`projects.md`](book/src/cli/projects.md)):
```bash
paweli init myproject
cd myproject
paweli run
```

Compile a program to a packaged bytecode file:
```bash
paweli build hello.pw hello.paw
paweli hello.paw # this requires the Paweli runtime to be installed
```

Compile a program to a native executable:
```bash
paweli build hello.pw --bin hello
./hello # doesnt require Paweli to run!
```

Example program:
```paweli
struct Point {
	int x;
	int y;

	fn length(Point self) -> float {
		return stdlib.sqrt(self.x * self.x + self.y * self.y);
	}
}

fn divide(int a, int b) -> int {
	if (b == 0) {
		throw "division by zero";
	}
	return a / b;
}

fn main() {
	Point p = Point { x: 3, y: 4 };
	print(Point.length(p));

	int result = divide(10, 0) catch(int -1);
	print(result);
}
```

See [`book/src/`](book/src/) for full documentation, or run `mdbook serve book` to browse it locally.

## Build From Source
Requirements: a C11 compiler, CMake, Ninja.

Debug build:
```bash
git clone https://github.com/seaslugr/paweli
cd paweli
make # Debug/dev release. Placed in 'build/debug/'
```

Release build:
```bash
make release # Placed in 'build/release/'
```

Run the test suite:
```bash
make test
```

## Platform Support
Paweli supports x86_64 Linux and x86_64 Windows. macOS and ARM support is planned in the future.

The Windows build is cross-compiled from Linux with MinGW-w64: `make windows` writes `build/windows/paweli.exe`.

## License
Paweli itself is licensed under [GPLv3](LICENSE).

Programs written in paweli are not. The [runtime exception](RUNTIME-EXCEPTION) covers `.paw` files and `--bin` executables. Both embed part of paweli's own runtime. You can write and distribute closed-source paweli programs.
