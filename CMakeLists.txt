cmake_minimum_required(VERSION 3.25)

project(paweli LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CONFIGURATION_TYPES "Debug;RelWithAsserts;RelWithDebInfo;Release" CACHE STRING "" FORCE)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "" FORCE)
endif()

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache found, using it to speed up rebuilds: ${CCACHE_PROGRAM}")
endif()

set(PAWELI_LIB_SOURCES
    src/diagnostics/diagnostic.c

    src/lexer/lexer.c
    src/lexer/token.c

    src/parser/ast.c
    src/parser/parser.c

    src/compiler/chunk.c
    src/compiler/compiler.c
    src/compiler/type.c
    src/compiler/signature.c
    src/compiler/native_signatures.c
    src/compiler/paw.c

    src/platform/exe.c

    src/jit/jit_asm.c
    src/jit/jit_exec_mem.c
    src/jit/jit_compile.c
    src/jit/jit_dispatch.c

    src/runtime/value.c
    src/runtime/object.c
    src/runtime/table.c
    src/runtime/gc.c
    src/runtime/module.c
    src/runtime/vm.c

    src/stdlib/stdlib.c
    src/stdlib/math.c
    src/stdlib/string.c
    src/stdlib/convert.c
    src/stdlib/array.c
    src/stdlib/io.c
)

set(PAWELI_WARNING_OPTIONS
    -Wall
    -Wextra
    -Wno-unused-parameter
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Debug>:-fanalyzer>
    $<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-omit-frame-pointer>
    $<$<CONFIG:RelWithAsserts>:-g -Ofast>
    $<$<CONFIG:RelWithDebInfo>:-g -DNDEBUG -Ofast>
    $<$<CONFIG:Release>:-DNDEBUG -Ofast -Werror>
)

set(PAWELI_SANITIZE_LINK_OPTIONS
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    $<$<CONFIG:RelWithDebInfo>:-s>
    $<$<CONFIG:Release>:-s>
)

add_executable(paweli src/main.c ${PAWELI_LIB_SOURCES})
target_include_directories(paweli PRIVATE src)
target_compile_options(paweli PRIVATE ${PAWELI_WARNING_OPTIONS})
target_link_options(paweli PRIVATE ${PAWELI_SANITIZE_LINK_OPTIONS})

if(UNIX)
    target_link_libraries(paweli PRIVATE m)
endif()

# Standalone C harness for the JIT subsystem (src/jit/), which -- unlike
# every other feature in this codebase -- isn't reachable end-to-end
# through a .pw script until src/jit/jit_dispatch.c wires hotness-driven
# compilation into call_value() (see docs/todo.md's JIT phase plan).
# Drives jit_compile_function()/jit_entry directly against a hand-built
# chunk_t, the same way scripts/run_tests.sh drives paweli against
# tests/*.pw.
add_executable(jit_selftest tests/jit/jit_selftest.c ${PAWELI_LIB_SOURCES})
target_include_directories(jit_selftest PRIVATE src)
target_compile_options(jit_selftest PRIVATE ${PAWELI_WARNING_OPTIONS})
target_link_options(jit_selftest PRIVATE ${PAWELI_SANITIZE_LINK_OPTIONS})

if(UNIX)
    target_link_libraries(jit_selftest PRIVATE m)
endif()
