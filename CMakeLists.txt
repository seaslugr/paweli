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

set(PAWELI_SOURCES
    src/main.c

    src/diagnostics/diagnostic.c

    src/lexer/lexer.c
    src/lexer/token.c

    src/parser/ast.c
    src/parser/parser.c

    src/compiler/chunk.c
    src/compiler/compiler.c

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
)

add_executable(paweli ${PAWELI_SOURCES})
target_include_directories(paweli PRIVATE src)

target_compile_options(paweli PRIVATE
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

target_link_options(paweli PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    $<$<CONFIG:RelWithDebInfo>:-s>
    $<$<CONFIG:Release>:-s>
)

if(UNIX)
    target_link_libraries(paweli PRIVATE m)
endif()
