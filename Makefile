SHELL := /usr/bin/env bash

.PHONY: all build run debug relasserts reldebug release clean count test

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

clean:
	rm -rf build

count:
	./scripts/linecount.sh

test: build
	./scripts/run_tests.sh
