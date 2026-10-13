# Modules

A module is one file.

```paweli
#moduledef "geometry"
```

This declares the current file as a module named `geometry`. It must be the first line of the file, and a file can only have one.

```paweli
#module "geometry"
```

This imports it from another file. Imports go at the top of the file, after the `#moduledef` line if there is one. A file can have as many imports as it needs.

`geometry` resolves to `geometry.pw` in the same directory as the importing file. There is no search path.

You access a module's functions qualified, like `geometry.area(w, h)`. There is no way to pull function names into your own file unqualified.

## Exporting functions

`export` makes a function part of the module's public interface. It goes inside `main()`:

```paweli
fn area(int w, int h) -> int {
	return w * h;
}

fn main() {
	export area;
}
```

`export` is a compile-time declaration. It never runs. In fact, a module's `main()` never runs at all when the module is imported.

`export area;` must be a direct statement in `main()`'s own body. Inside an `if`, `while`, or `for` it is a compile error. A conditional export would export the function either way, which is misleading, so the compiler rejects it. `export` is also a compile error outside a `#moduledef` file, or outside `main()`.

Every top-level function is private to its own file by default. A helper that `area()` calls stays invisible to the importer unless you export it too. Two files can each have their own `main`, `area`, or anything else with no collision. Only exported names are reachable, and only through the module's name.

You can export an overloaded function. The importer sees every overload and can call each one. See [Functions and Overloading](functions.md#overloading).

## Exporting structs

A `struct` is exported the same way, with `export TypeName;` inside `main()`:

```paweli
struct Point {
	int x;
	int y;
}

fn origin() -> Point {
	return Point { x: 0, y: 0 };
}

fn main() {
	export Point;
	export origin;
}
```

Watch out: an exported struct's name is used unqualified on the importing side. You write `Point { x: 1, y: 2 }`. There is no `geometry.Point { ... }` form. Functions still need the `geometry.` prefix.

An exported struct brings its own functions with it (see [Structs](structs.md#struct-functions)). `export Point;` is enough, there is no separate step. They are unqualified too, so you write `Point.origin()`.

Since struct names are unqualified, they can collide:

- A local struct with the same name as an imported one is a compile error. It does not shadow the imported one.
- If two imported modules export a struct with the same name, using that name is a compile error.

A struct that is never exported stays invisible to an importer, the same as a function that is never exported.

## How imports resolve

The compiler resolves, parses, and fully compiles a `#module` dependency before the file that imports it. It needs the module's exported signatures to type-check every `module.member(...)` call and to pick the right overload.

An import fails with a compile error at the `#module` line, before anything runs, if the imported file:

- fails to parse or compile
- has no `#moduledef` line
- declares a name that doesn't match the name you asked for

## Subdirectories

A module name can include a `/` to import from a subdirectory. `#module "shapes/geometry"` looks for `shapes/geometry.pw` next to the importing file. The local name is still `geometry`, the part after the last `/`.

## Modules that import modules

A module can import other modules. Paths resolve from the file that has the `#module` line. Two files can import the same module with no problem.

An import cycle is a compile error (`compiler::circular-import`). The message shows the whole chain of files.
