# Structs

```paweli
struct Point {
	int x;
	int y;
}

fn distance(Point a, Point b) -> float {
	int dx = a.x - b.x;
	int dy = a.y - b.y;
	return stdlib.sqrt(dx * dx + dy * dy);
}

fn main() {
	Point p = Point { x: 1, y: 2 };
	p.x = p.x + 1;
	Point origin = Point { y: 0, x: 0 }; // field order in a literal doesn't matter
	print(distance(p, origin));
}
```

A `struct` groups a fixed set of named, typed fields. Fields are mutable. There is no inheritance and no encapsulation. Using a field that doesn't exist is a compile error. So is a literal for an unknown struct type.

Structs are declared at the top level of a file. They are private to that file unless exported. See [Modules](modules.md) for more.

## Literals

A struct literal looks like `TypeName { field: value, ... }`. It must name every field of the struct, in any order. The one exception is a field with a default:

```paweli
struct Tuning {
	int base;
	float scale = 2.5;
	string label = "default";
}

fn main() {
	Tuning t = Tuning { base: 5 }; // scale and label take their defaults
	Tuning custom = Tuning { base: 5, scale: 9.0 }; // label still takes its default
}
```

Leaving out a field with no default is a compile error. A default is evaluated fresh at each literal that leaves it out. A default can't use the struct's own fields or a local variable.

## Leaving off the type name

You can leave the type name off when the context already implies it:

```paweli
fn offset(Point p) -> Point {
	return { x: p.x + 1, y: p.y + 1 }; // inferred from the declared return type
}

fn main() {
	Point p = { x: 1, y: 2 }; // inferred from the declared type
	p = { x: 10, y: 20 }; // inferred from the assignment target
	Line l = Line { start: { x: 0, y: 0 }, end: { x: 3, y: 4 } }; // inferred from Line's field types
}
```

With no context to infer from, a bare `{ ... }` is a compile error. The compiler asks you for an explicit type name.

## Fields that hold arrays and structs

A field can be an array or another struct type. It can be the struct's own type, or one declared earlier in the same file:

```paweli
struct Node {
	int value;
	Node next;
}

fn main() {
	Node tail = Node { value: 2, next: null }; // null works for any struct type
	Node head = Node { value: 1, next: tail };
	print(head.next.value); // 2
}
```

## Struct functions

A `struct` body can also declare functions. You call them as `TypeName.name(args)`:

```paweli
struct Point {
	int x;
	int y;

	fn origin() -> Point {
		return Point { x: 0, y: 0 };
	}

	fn length(Point self) -> float {
		return stdlib.sqrt(self.x * self.x + self.y * self.y);
	}
}

fn main() {
	Point p = Point.origin();
	print(Point.length(Point { x: 3, y: 4 })); // 5
}
```

`self` is an ordinary parameter. The name has no special meaning. A struct is not a class, so there is no receiver syntax like `p.length()` and no implicit first argument.

Struct functions overload like top-level functions. They live in a namespace that belongs to the struct. So a struct function can share a name with a top-level function, or with a function in another struct.

Struct functions don't support decorators.

## Exporting

`export TypeName;` exports the struct and all of its functions with it. The importing file uses both unqualified: `TypeName { ... }` and `TypeName.name(args)`. See [Modules](modules.md#exporting-structs).
