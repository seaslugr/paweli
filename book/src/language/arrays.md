# Arrays

```paweli
int[] nums = [1, 2, 3];
string[] names = ["sea", "slug"];
```

An array literal is a comma-separated list of expressions inside `[ ]`. Elements are read and written by index:

```paweli
print(nums[0]); // 1
nums[1] = 20;
```

Reading or writing past the end, or with a negative index, is a runtime error. Indexing anything other than an array or a string, or indexing with anything other than an `int`, is also a runtime error.

Strings can also be indexed, read-only: `"hello"[0]` gives `"h"`, a one-character string (paweli has no separate character type). Assigning into a string index (`s[0] = "x"`) is a runtime error.

Arrays live on the garbage-collected heap, the same as strings. `stdlib.length` reports an array's element count the same way it reports a string's byte length:

```paweli
print(stdlib.length(nums)); // 3
```

`stdlib.push` and `stdlib.pop` grow and shrink an array in place:

```paweli
int[] stack = [];
stdlib.push(stack, 10);
stdlib.push(stack, 20);
int top = stdlib.pop(stack); // 20, stack now has one element
```

Obviously, `stdlib.pop` on an empty array is a runtime error.

Array literals are type-checked: every element must have the same type. An empty literal (`[]`) has no elements to infer a type from, so it needs a known array type from context (a `var_decl`'s declared type, an assignment's target, a `return`, or a call argument where only one overload could possibly match); `int[] nums = [];` works, but `[]` on its own with no such context is a compile error.

## Nested arrays

Repeating `[]` nests arrays: `int[][]` is an array whose elements are themselves `int[]`. Each level is type-checked independently, so `[[1, 2], [3, 4]]` is an `int[][]`, `grid[1][2]` reads one cell, and `grid[1]` yields the whole inner `int[]` row:

```paweli
int[][] grid = [[1, 2, 3], [4, 5, 6]];
print(grid[1][2]); // 6
int[] row = grid[0]; // [1, 2, 3]
grid[1][1] = 99;
```

<span style="color: var(--icons)">Ignore the array soup</span>

Rows are ordinary arrays, so they need not have the same length (a "jagged" grid):

```paweli
int[][] jagged = [[1], [2, 3], [4, 5, 6]];
```

`for (T x : grid)` iterates a nested array's *elements*, so the loop variable's type is one level shallower. `for (int[] row : grid)` over an `int[][]` binds each inner row:

```paweli
for (int[] row : grid) {
    int total = 0;
    for (int v : row) {
        total = total + v;
    }
    print(total);
}
```

Element types are checked one level at a time: `int[][] grid = [1, 2, 3];` is a compile error (`int[][]` holds `int[]`, not `int`), as is `int[][] grid = [[1, 2], ["a", "b"]];` (its second element is a `string[]`). An empty `[]` still borrows its type from context, so `int[][] grid = [];` works, and an inner `[]` inherits the outer element type: `int[][] x = [[], [1, 2]];`.

There is still no way to spell "array of function type" (`fn(int) -> int[]` is a function returning an `int[]`, not an array of functions); `[]` always binds to the nearest preceding type, so nesting only ever adds depth to the *innermost* base type.
