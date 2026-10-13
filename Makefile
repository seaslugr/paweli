SHELL := /usr/bin/env bash

# Installation destinations. Override PREFIX (and DESTDIR) for distro
# packaging; MANDIR defaults under PREFIX.
PREFIX ?= /usr/local
MANDIR ?= $(PREFIX)/share/man

# Which built binary `install` copies (default: the Debug build that
# `make` produces).
PAWELI ?= build/debug/paweli
MANPAGES := docs/man/paweli.1 docs/man/paweli-syntax.7

.PHONY: all build run debug relasserts reldebug release windows test-windows clean count test speedtest man install

# Default: local debug build, outputs to build/debug/.
all: build

build:
	cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
	cmake --build build/debug

run: build
	./build/debug/paweli

debug: build

# Full speed + assertions on.
relasserts:
	cmake -S . -B build/relasserts -G Ninja -DCMAKE_BUILD_TYPE=RelWithAsserts
	cmake --build build/relasserts

# Full speed + debug symbols.
reldebug:
	cmake -S . -B build/reldebug -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
	cmake --build build/reldebug

release:
	cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build/release

# Cross-compiles a Release paweli.exe for 64-bit Windows with MinGW-w64.
windows:
	cmake -S . -B build/windows -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DCMAKE_BUILD_TYPE=Release
	cmake --build build/windows

# Runs the test suite against build/windows/paweli.exe under wine.
# winedbg is disabled so a crash returns an exit code instead of
# opening a dialog.
test-windows: windows
	WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" PAWELI=$(CURDIR)/build/windows/paweli.exe PAWELI_RUNNER=wine PAWELI_EXE_SUFFIX=.exe ./scripts/run_tests.sh
	WINEDEBUG=-all WINEDLLOVERRIDES="winedbg.exe=d" wine build/windows/jit_selftest.exe

clean:
	rm -rf build

count:
	./scripts/linecount.sh

test: build
	./scripts/run_tests.sh

# Compares .pw source, .paw bytecode, --bin native exe, and a hand-written
# C equivalent across bench/*, using hyperfine. Needs a Release build
# (optimized, no sanitizers) to give paweli a fair shot against C.
speedtest: release
	./scripts/speedtest.sh

# Renders every docs/man page with groff and parses them for errors; -z
# silences normal output so a non-zero exit is purely a syntax problem.
# Requires groff (only for development; the manpages ship pre-rendered).
man:
	mkdir -p build/man
	for page in $(MANPAGES); do \
		groff -man -Tutf8 -z $$page || { echo "man: $$page failed"; exit 1; }; \
	done
	for page in $(MANPAGES); do \
		groff -man -Tutf8 -c $$page > build/man/$$(basename $$page).txt; \
	done

install: build
	install -Dm755 $(PAWELI) $(DESTDIR)$(PREFIX)/bin/paweli
	install -Dm644 docs/man/paweli.1 $(DESTDIR)$(MANDIR)/man1/paweli.1
	install -Dm644 docs/man/paweli-syntax.7 $(DESTDIR)$(MANDIR)/man7/paweli-syntax.7

install-release: release
	install -Dm755 build/release/paweli $(DESTDIR)$(PREFIX)/bin/paweli
	install -Dm644 docs/man/paweli.1 $(DESTDIR)$(MANDIR)/man1/paweli.1
	install -Dm644 docs/man/paweli-syntax.7 $(DESTDIR)$(MANDIR)/man7/paweli-syntax.7
