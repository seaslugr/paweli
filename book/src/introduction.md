# Introduction

`paweli` is a small, statically-typed, C-like programming language that runs on a stack-based VM and a custom JIT compiler.

```paweli
struct Point {
	int x;
	int y;

	fn length(Point self) -> float {
		return stdlib.sqrt(self.x * self.x + self.y * self.y);
	}
}

fn main() {
	Point p = Point { x: 3, y: 4 };
	print(Point.length(p));
}
```

## Platform support

`paweli` runs on x86_64 Linux and x86_64 Windows. macOS and ARM support is planned.

## License

`paweli` itself is licensed under GPLv3. However, programs written in `paweli` are not. The runtime exception covers `.paw` files and `--bin` executables, so you can write and distribute closed-source `paweli` programs.
