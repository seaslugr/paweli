# C Style Guide

## Indentation
Use tabs, never spaces.

## Braces
LLVM style: opening brace on the same line as the statement.

## Naming
- Functions and variables: `snake_case`
- Macros and constants: `ALL_CAPS`
- Typedef names: `snake_case_t` (e.g. `player_t`, `state_t`)

## Comments
Only comment what genuinely requires explanation. If the code is self-explanatory, no comment.

- `/* */` above functions only, when the function truly needs explaining
- `//` inside function bodies & file scope only, when a specific line truly needs explaining
- Never add comments to examples or sample code
- Never use comments as labels or descriptions of obvious behaviour
- Never add section dividers (but never remove existing ones)
- Multi-line `/* */` comments: continuation lines align to the text after `/* `, not `*`-prefixed:

```c
/* Reads up to n bytes from fd into buf.
   Returns bytes read, or -1 on error. */
```

## Includes
System headers first, blank line, then own headers:

```c
#include <stdio.h>
#include <string.h>

#include "mymodule.h"
```

## Structs and Enums
Always use `typedef`. Order struct members smallest to largest byte size to minimize padding:

```c
typedef struct {
	char flag;
	short count;
	int value;
	double total;
} record_t;

typedef enum {
	STATE_IDLE,
	STATE_RUNNING,
	STATE_DONE,
} state_t;
```

## Header Guards
Always use `#pragma once`, never `#ifndef` guards.

## Variable Declarations
- Variables used more than once, or across multiple sections, are declared at the top of the function, without initialization; assign them where they are first used
- Variables used only once are declared immediately above their use, with initialization
- `for` loop counters are declared inline

```c
void process(char *buf, int n) {
	int total;

	total = 0;
	for (int i = 0; i < n; i++) {
		total += buf[i];
	}

	int clamped = total > 255 ? 255 : total;
	apply(clamped);
}
```

## Operators and Spacing
- Spaces around all binary operators: `x = x + 1`, `a == b`
- Space between keywords and parentheses: `if (x)`, `while (x)`
- No space between function name and call: `foo(x)`, not `foo (x)`

## Pointers and Const
- `*` attaches to the variable name: `char *buf`, not `char* buf`
- West const: `const char *buf`, not `char const *buf`
- Use `NULL` for null pointers, never `0`

## Function Parameters
Leave parameter lists empty: `void foo()`, not `void foo(void)`. In C, `void foo()` technically means unspecified parameters (K&R style), not zero parameters — but we prefer it for brevity and consistency with how C++ reads.

## Static Functions
All file-local functions must be marked `static`.

## File Structure
Within a `.c` file, sections appear in this order:
1. Includes
2. `#define` constants and macros
3. `typedef` / struct / enum definitions
4. Forward declarations for `static` functions
5. Static function definitions
6. Public function definitions

Non-static functions are declared in the corresponding `.h` file, not forward-declared in the `.c` file.

## Forward Declarations
Declare all `static` functions in a forward declaration block at the top of the `.c` file:

```c
static void do_thing();
static int compute(int x);

static void do_thing() {
	compute(1);
}

static int compute(int x) {
	return x * 2;
}
```

## Static Inline
Use `static inline` for small, hot utility functions. If the body is more than a few lines, it should not be inline:

```c
static inline char to_lower(char c) {
	return (unsigned char)c >= 'A' && (unsigned char)c <= 'Z' ? c + 32 : c;
}
```

## Ctype Casts
Only cast `char` to `(unsigned char)` when passing to ctype functions or doing unsigned comparisons if it is both necessary and 100% safe to do so. Avoids UB on platforms where `char` is signed:

```c
isdigit((unsigned char)*s);
tolower((unsigned char)c);
```

## Designated Initializers
Use designated initializers when constructing struct literals:

```c
pattern_list_t plist = {
	.patterns = patterns,
	.pattern_count = n,
};
```

## Magic Numbers
Numbers or strings that are frequently used must be defined with `#define`. One-off values obvious from context can stay inline.

## No Columnar Alignment
Never align variables, assignments, struct members, or anything else into columns:

```c
/* Don't */
int x        = 1;
int total    = 0;
char *name   = NULL;

/* Do */
int x = 1;
int total = 0;
char *name = NULL;
```

## Else After Return
Drop `else` when the `if` block already returns, unless keeping it genuinely improves readability:

```c
/* Drop it */
if (err) {
	return -1;
}
do_thing();

/* Keep it when the branches are a true pair */
if (x > 0) {
	return 1;
} else {
	return -1;
}
```

## Always Use Braces
Always use braces for control flow bodies, even single-line ones.

## Switch Statements
Prefer `switch` over chains of `if/else if` when branching on an enum or small set of known values. Only include `default` when there is genuinely something to do for unhandled cases. For a fully-covered enum where a `default` would be dead code, omit it entirely — do not add `assert(0)` or `__builtin_unreachable()`.

## Performance
- Code performance is a priority — prefer efficient algorithms and data structures
- Avoid unnecessary allocations, copies, and indirection
- Prefer stack allocation over heap where appropriate
- Be mindful of cache locality
- Struct member ordering (smallest to largest) is part of this

## No Line Length Limit
No enforced line length.

## Portability
Paweli will be ported to Windows eventually, so code must stay portable from the start:

- Never use GNU libc extensions (`asprintf`, `getline`, `strcasestr`, `memmem`, statement expressions `({ })`, nested functions, `__attribute__` outside of what MSVC/clang-cl also accept, etc.)
- Never use POSIX-only APIs directly (`<unistd.h>`, `<pthread.h>`, `<sys/mman.h>`, `fork`, `mmap`, etc.). If platform-specific functionality is unavoidable, isolate it behind a small abstraction with a Windows implementation, not scattered `#ifdef __linux__` blocks
- Stick to standard C11 and the C standard library; anything beyond that must work on MSVC/MinGW too
- Avoid GCC-only compiler extensions in source (`typeof`, `__builtin_*`, case ranges, etc.). Build-tool flags like sanitizers and `-fanalyzer` are fine since those are compiler config, not source code, but they must be guarded off on non-GNU toolchains
