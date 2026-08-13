# Paweli

Paweli is a small, statically-typed, C-like scripting language. Source files compile to bytecode and run on a custom stack-based VM. The syntax stays close to C on purpose: braces, semicolons, explicit types.

---

## Hello world

```paweli
fn main() {
	print("Hello World!");
}
```

Every program needs a `main` function, the same as C. It is the entry point: `paweli` defines every top-level function first, then calls `main()`.

---

## Running a program

```bash
paweli hello.pw
```

`paweli` also has debug flags for inspecting what the compiler produced, see [CLI reference](cli.md).

---

## Where to go next

- [Syntax reference](syntax.md): comments, literals, keywords, operators
- [Language reference](language.md): variables, functions, control flow, modules
- [CLI reference](cli.md): running programs, debug flags, VM limits
