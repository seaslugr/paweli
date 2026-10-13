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
    src/parser/parser_decl.c
    src/parser/parser_stmt.c
    src/parser/parser_expr.c

    src/compiler/chunk.c
    src/compiler/compiler.c
    src/compiler/compiler_emit.c
    src/compiler/compiler_scope.c
    src/compiler/compiler_function.c
    src/compiler/compiler_stmt.c
    src/compiler/compiler_expr.c
    src/compiler/compiler_call.c
    src/compiler/compiler_test_runner.c
    src/compiler/type.c
    src/compiler/signature.c
    src/compiler/native_signatures.c

    src/paw/paw_write.c
    src/paw/paw_read.c
    src/paw/paw_exe.c


    src/project/manifest.c

    src/jit/jit_asm.c
    src/jit/jit_exec_mem.c
    src/jit/jit_compile.c
    src/jit/jit_analysis.c
    src/jit/jit_frame.c
    src/jit/jit_helpers.c
    src/jit/jit_ops_stack.c
    src/jit/jit_ops_arith.c
    src/jit/jit_ops_call.c
    src/jit/jit_ops_object.c
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

if(WIN32)
    list(APPEND PAWELI_LIB_SOURCES src/platform/windows.c)
else()
    list(APPEND PAWELI_LIB_SOURCES src/platform/posix.c)
endif()

# MinGW has no AddressSanitizer or UBSan runtime, so a Windows Debug
# build keeps -fanalyzer and drops the sanitizers.
if(WIN32)
    set(PAWELI_SANITIZE_FLAGS "")
else()
    set(PAWELI_SANITIZE_FLAGS -fsanitize=address,undefined)
endif()

set(PAWELI_WARNING_OPTIONS
    -Wall
    -Wextra
    -Wno-unused-parameter
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Debug>:-fanalyzer>
    $<$<CONFIG:Debug>:${PAWELI_SANITIZE_FLAGS} -fno-omit-frame-pointer>
    $<$<CONFIG:RelWithAsserts>:-g -Ofast>
    $<$<CONFIG:RelWithDebInfo>:-g -DNDEBUG -Ofast>
    $<$<CONFIG:Release>:-DNDEBUG -Ofast -Werror>
)

set(PAWELI_LINK_OPTIONS
    $<$<CONFIG:Debug>:${PAWELI_SANITIZE_FLAGS}>
    $<$<CONFIG:RelWithDebInfo>:-s>
    $<$<CONFIG:Release>:-s>
)

set(PAWELI_DEFINITIONS "")

if(WIN32)
    # -static: the .exe needs no MinGW runtime DLLs next to it.
    # --stack: Windows gives the main thread 1 MB by default. Linux
    # gives 8 MB. A JIT frame is larger on Win64 (shadow space, two
    # more saved registers), so 16 MB gives deep paweli recursion at
    # least the room it has on Linux.
    list(APPEND PAWELI_LINK_OPTIONS -static -Wl,--stack,16777216)
    # Makes printf understand %zu and %lld on the older msvcrt runtime.
    list(APPEND PAWELI_DEFINITIONS __USE_MINGW_ANSI_STDIO=1)
endif()

# src/cli/ is the paweli executable's CLI: argv parsing, dispatch, and
# output, plus exec.c, which compiles and runs a program. It is kept
# out of PAWELI_LIB_SOURCES so jit_selftest does not link it.
set(PAWELI_CLI_SOURCES
    src/cli/commands.c
    src/cli/exec.c
    src/cli/output.c
)

add_executable(paweli src/main.c ${PAWELI_CLI_SOURCES} ${PAWELI_LIB_SOURCES})
target_include_directories(paweli PRIVATE src)
target_compile_options(paweli PRIVATE ${PAWELI_WARNING_OPTIONS})
target_link_options(paweli PRIVATE ${PAWELI_LINK_OPTIONS})
target_compile_definitions(paweli PRIVATE "PAWELI_BUILD_TYPE=\"$<CONFIG>\"" ${PAWELI_DEFINITIONS})

if(UNIX)
    target_link_libraries(paweli PRIVATE m)
endif()

# A standalone C test of the JIT. It calls jit_compile_function() on a
# hand-built chunk_t and runs the result, with no .pw script.
# scripts/run_tests.sh does not run it. Run it by hand.
add_executable(jit_selftest tests/jit/jit_selftest.c ${PAWELI_LIB_SOURCES})
target_include_directories(jit_selftest PRIVATE src)
target_compile_options(jit_selftest PRIVATE ${PAWELI_WARNING_OPTIONS})
target_link_options(jit_selftest PRIVATE ${PAWELI_LINK_OPTIONS})
target_compile_definitions(jit_selftest PRIVATE ${PAWELI_DEFINITIONS})

if(UNIX)
    target_link_libraries(jit_selftest PRIVATE m)
endif()

# `cmake --install build/<config>` installs the paweli binary and its
# manpage. jit_selftest is a test harness, not installed.
include(GNUInstallDirs)
install(TARGETS paweli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES docs/man/paweli.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES docs/man/paweli-syntax.7 DESTINATION ${CMAKE_INSTALL_MANDIR}/man7)
