# Comments, Identifiers, and Keywords

## Comments

```paweli
// a single-line comment, runs to the end of the line

/// a multi-line comment
    opens and closes with three slashes,
    everything between is ignored
///
```

There is no `/* */` form like C. Line comments use `//`, block comments open and close with `///`.

## Identifiers

An identifier starts with a letter or underscore, then continues with letters, digits, or underscores: `total`, `_count`, `player2`. Identifiers are case-sensitive.

## Keywords

These words are reserved and cannot be used as identifiers:

- `fn`: declares a function
- `int`, `float`, `string`, `bool`: the four base types
- `if` / `else`: conditional branching
- `while`: condition-checked loop
- `for`: C-style counted loop
- `break`: exits the innermost loop immediately
- `continue`: skips to the next iteration of the innermost loop
- `return`: returns from a function
- `true` / `false`: boolean literals
- `null`: the null value
- `export`: marks a function as part of a module's public interface
- `struct`: declares a struct type
- `try` / `catch` / `throw`: error handling, see [Error Handling](../language/error-handling.md)
