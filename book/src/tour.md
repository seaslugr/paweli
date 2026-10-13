# A Tour of Paweli

A quick walk through the language, building up one example at a time. This isn't the full reference, see [Lexical Structure](lexical/index.md) and [The Language](language/index.md) for that. Think of it as a five-minute look at the shape of paweli before reading those chapters in full.

## Variables and a loop

```paweli
fn main() {
	int total = 0;
	for (int i : 1..10) {
		total = total + i;
	}
	print(total); // 55
}
```

`int` declares a variable's type up front. There's no `let` and no inference. `for (int i : 1..10)` counts `i` from 1 to 10. `print` writes whatever you hand it, followed by a newline.

## Pulling logic into a function

The sum above is useful enough to want twice, so give it a name:

```paweli
fn sum_up_to(int n) -> int {
	int total = 0;
	for (int i : 1..n) {
		total = total + i;
	}
	return total;
}

fn main() {
	print(sum_up_to(10)); // 55
	print(sum_up_to(100)); // 5050
}
```

`-> int` says what `sum_up_to` returns. Every paweli program needs a `main`. `sum_up_to` is just an ordinary function that `main` happens to call.

## A struct with its own functions

```paweli
struct Point {
	int x;
	int y;

	fn length(Point self) -> float {
		return stdlib.sqrt(self.x * self.x + self.y * self.y);
	}
}

fn main() {
	Point[] points = [Point { x: 3, y: 4 }, Point { x: 6, y: 8 }];

	for (Point p : points) {
		print(Point.length(p)); // 5, then 10
	}
}
```

`struct Point` bundles `x` and `y` together. `fn length` lives inside the struct body and is called as `Point.length(p)`, not `p.length()` as a struct is not a class and there's no receiver syntax. `self` here is just an ordinary parameter. The `for (Point p : points)` loop walks the array one `Point` at a time, the same way it would walk an `int[]`.

## Where to go from here

[Lexical Structure](lexical/index.md) covers the pieces (comments, keywords, literals, operators) from the ground up. [The Language](language/index.md) covers everything above in full, plus arrays, strings, modules, closures, and error handling.
