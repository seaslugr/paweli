# Todo

- **Compiled bytecode distribution format** (JAR-style): serialize compiled chunks to disk so programs can run without recompiling from source, and so a program can be distributed as one bundle instead of loose `.pw` files. Deferred until the module system is real. Bundling only matters once a program can span multiple files. See the "Modules" section of `docs/paweli/language.md` for current module status.
