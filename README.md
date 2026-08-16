# Paweli
Paweli is a small, statically-typed, C-like programming language. The language runs on a stack-based VM and a custom JIT compiler.

The name comes from the sea slug *Costasiella paweli*.

```paweli
fn main() {
	print("Hello World!");
}
```

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
- Debug flags: `--dump-ast`, `--dump-bytecode`, `--run-tests`

## Performance

Soon.

## Usage

Run a program from source:
```bash
paweli hello.pw
```

Compile a program to a bytecode file:
```bash
paweli build hello.pw hello.paw
paweli hello.paw
```

Compile a program to a native executable:
```bash
paweli build hello.pw --bin hello
./hello
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

See [`docs/paweli/`](docs/paweli/) for full documentation.

## Build From Source
Requirements: a C11 compiler, CMake, Ninja.

Debug build:
```bash
git clone https://github.com/Foox-dev/paweli
cd paweli
make
```
The binary is at `build/debug/paweli`.

Release build:
```bash
make release
```
The binary is at `build/release/paweli`.

Run the tests:
```bash
make test
```

## Platform Support
Paweli runs on Linux only. Windows and macOS support is planned.

## License
Paweli itself is licensed under [GPLv3](LICENSE).

Programs written in paweli are not. The [runtime exception](RUNTIME-EXCEPTION) covers `.paw` files and `--bin` executables. Both embed part of paweli's own runtime. You can write and distribute closed-source paweli programs.
