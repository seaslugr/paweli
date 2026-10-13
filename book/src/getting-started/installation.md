# Installation

Currently, `paweli` is "build from source" only.

## Build Requirements

- A C11 compiler (gcc, MSVC, etc)
- CMake 3.25 or newer
- Ninja 1.10 or newer

## Building & Installing

```bash
git clone https://github.com/Foox-dev/paweli
cd paweli
sudo make install-release
```

This copies the `paweli` binary to `/usr/local/bin/paweli`, and installs its man pages (`paweli(1)`, `paweli-syntax(7)`) under `/usr/local/share/man`.

Use `install-release`, not plain `install`. Plain `make install` builds and installs the Debug build at `build/debug/paweli`, which is not what you want outside of developing paweli itself.

Override `PREFIX` (default `/usr/local`) or `DESTDIR`, e.g. for packaging:

```bash
make install-release PREFIX=/usr DESTDIR=/tmp/pkgroot
```

## Checking the install

Lastly, to check if the install is correct, you can check the version with:

```bash
paweli --version
```

This prints the installed version and build type (`paweli 0.1.0 (release)`).
You are now ready to start writing programs in `paweli`!
