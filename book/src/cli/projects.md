# Projects

```bash
paweli init myapp
cd myapp
paweli run
```

## Creating a project

`paweli init [name]` writes two files: `project.pawproj` (a manifest) and `main.pw` (a starter program).

- With a `name`, it creates a directory of that name first and scaffolds into it. It is fine if the directory already exists.
- Without one, it scaffolds into the current directory. The manifest's `name` field defaults to the directory's name.

Either way, it refuses to run if `project.pawproj` already exists there. It never overwrites one. `main.pw` is only written if it doesn't exist yet.

## The manifest

`project.pawproj` looks like this:

```paweli
string name = "myapp";
string version = "0.1.0";
string entry = "main.pw";
```

It uses paweli's own syntax, so there is no separate format like TOML to learn. Though, it is not a paweli program. Only `string field = "value";` declarations are recognized, in any order. All three fields (`name`, `version`, `entry`) are required. `//` and `///...///` comments work the same as anywhere else.

There is no `[dependencies]`-style section, and you don't list your files in the manifest at all. A project is whatever `entry` and its `#module` imports reach, the same as for a single-file program. See [Modules](../language/modules.md).

Fetching or versioning someone else's code isn't supported yet.

## Running and testing

With no filename, `paweli run` and `paweli test` read `entry` out of `project.pawproj` in the current directory and run it. It is the same as `paweli run main.pw` or `paweli test main.pw`. Neither one writes an output file. See [Commands and Flags](commands.md) for the difference between them.

## Building

With no filename, `paweli build` reads `entry` out of `project.pawproj` and compiles it, the same as `paweli build main.pw`. The only difference is where the output goes: `build/<name>.paw`, or `build/<name>` for `--bin`. `build/` is created automatically.

All of `build`'s usual flags (`--bin`, `--strip-source`, `--run-tests`) still work. `--bin` still needs its own explicit name.

## With a filename

Project mode only turns on when you give no filename. `paweli build <file.pw>`, `paweli run <file.pw>`, and `paweli test <file.pw>` never look for a manifest.
