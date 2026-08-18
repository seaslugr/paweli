# Todo

## JIT v2

The v1 JIT (all opcodes compile, hotness-dispatched by default, 76-check
matrix green, loop_sum 4.2x / fib 2.3x over the interpreter) is done.
Everything below is the second generation: closing the gap to "within 2x
of C" that memory-operand codegen leaves open.

### 1. Register allocation

The single biggest remaining win. Current generated code is
memory-operand style: every value round-trips through vm->stack (r14),
so the interpreter's dispatch overhead is gone but the compiled code
still pays a load/store per value. A proper allocator keeps the hot
operands in CPU registers across instructions.

**Phase R1 (done, see design.md's 2026-08-15 "JIT register allocation,
phase R1" entry):** cache one numeric (`int`/`float`/`bool`), non-captured,
non-slot-reused local in `r15` for a function's whole lifetime. Needed
zero spill/reload machinery: a register-eligible slot's `value_t.type`
tag never becomes `VAL_OBJ`, so `mark_roots()` never observes a stale
payload as a problem, sidestepping the GC-interaction question below
entirely rather than solving it. loop_sum: 4.2x -> ~6.0x over the
interpreter (0.297s -> 0.211s), from exactly one register.

**Phase R2a (done, see design.md's 2026-08-15 "JIT register allocation,
phase R2a" entry):** extend to `r10`/`r11` (caller-saved), up to 3
register-locals total per function. Same eligibility rule as R1.
Spilling around a call turned out to be an ABI-correctness concern
(caller-saved registers survive no call unless the caller saves them
itself), not a GC-safety one; R1's tag argument already covers any
register, regardless of which one. `r9` was passed over despite having
no current scratch conflicts, since it's the SysV ABI's 6th integer
argument register and no call site here uses 6 args yet. A landmine,
not a clean pick. `r8` was disqualified outright (5th argument
register, already live in `emit_op_closure`). New `emit_call()` wraps
every call site to push/pop whichever bound registers are caller-saved.
A three-hot-local, no-call loop benchmark ran ~1.43x faster with all 3
registers versus capped-at-1 (490ms -> 343ms); `loop_sum.pw` (2 hot
locals, no spilling needed either way) unchanged at ~188ms.

**Phase R2b (done, see design.md's 2026-08-16 "JIT register allocation,
phase R2b: object-typed locals" entry):** extend eligibility to
`string`/`struct`/array/closure locals (`VAL_OBJ`), competing in the
same 3-register pool. Needed genuine spill-before-safepoint after all
(the hard part flagged before R1 started): `emit_gc_check()` now
flushes a bound `VAL_OBJ` register's payload into its own memory cell
before every allocating opcode/call, gated to the exact bytecode-offset
window (`[decl_end, dead_at)`) that memory legitimately belongs to the
local, found the hard way via three real, GC_STRESS-confirmed bugs
(pre-declaration scratch reuse, post-scope-exit stale writes, a
decl_end-sync-vs-jump-target conflict between loop back-edges and
`OP_TRY_CALL`'s catch handler) plus a fourth, tag-corruption bug from
the same scratch-reuse coincidence hitting the tag byte instead of the
payload. Also closed a related, pre-existing bug the same mechanism
surfaced: a register-cached scalar assigned `null` (legal; `null`
unifies with any declared type) was silently corrupting to a 0-payload
scalar, since a scalar's tag was assumed permanently fixed the same way
R1 assumed. `PAW_VERSION` bumped 2 -> 3 for the new `local_reg_dead_at`
per-slot array. Benchmarked honestly: a real ~9% *regression* for a
call-heavy shape (a struct local passed as a call argument every loop
iteration, paying the new flush plus R2a's own caller-saved push/pop on
every call), a real but modest ~4% win for a read-heavy/call-light
shape. Correctly framed as the correctness/GC-safety unlock this was
always going to be, not a guaranteed win independent of call shape.

**Quick follow-up (done, see design.md's 2026-08-17 "call-shape-aware
ranking for object locals" entry):** `select_register_locals()` now
penalizes a `VAL_OBJ` candidate's own ranking score for every
allocating-opcode occurrence within its live window, letting it lose
its slot entirely (not just its priority) when caching it would not pay
off. Flipped `struct_locals.pw`'s own regression into a win (~118.7ms
-> ~106.4ms, beating even the fully-disabled baseline) with zero effect
on the call-light case. Still a flat, loop-nesting-blind heuristic, not
the real fix; a future tiering pass (item 4 below) is the natural place
for that.

Everything below is now the real remaining scope:

- [ ] Add a `--dump-jit` style debug flag for inspecting emitted
      machine code, mirroring `--dump-bytecode`
- [ ] Benchmark gate: how much closer to the C equivalent R1+R2a's
      3 registers get, on a real workload beyond the synthetic
      three-hot-local benchmark used to measure R2a's own delta

### 2. Profiling-driven fast paths (done)

Before starting NaN-boxing, profiled two representative workloads
(string/struct-heavy and float/struct-heavy) with `perf`. See
design.md's 2026-08-15 "profiling infrastructure" entry for the
`PAWELI_PERF_MAP`/`RelWithAsserts` setup this needed, since neither
existed yet. Found two concrete bottlenecks bigger than anything
NaN-boxing would address on their own, both now fixed:

- **Float arithmetic fast path** (design.md, 2026-08-15): the JIT's
  arithmetic fast path only ever handled both-`VAL_INT`; every float
  op fell through to a full `vm_binary_op` C-helper call. Added SSE2
  scalar-double primitives to `jit_asm.c` (none existed before) and a
  both-`VAL_FLOAT` branch to `emit_op_arith_or_cmp`/`emit_op_div_mod`,
  with NaN-correct comparisons via the reversed-operand `UCOMISD`
  pattern GCC/Clang themselves use. `vm_binary_op` dropped out of the
  float-heavy workload's hot-function list entirely; ~18% fewer
  instructions, ~22% fewer cycles on that workload (`perf stat`).
- **Inline caching for OP_GET_GLOBAL/OP_GET_PROPERTY** (design.md,
  2026-08-15): every global/module-member access re-walked the hash
  table from scratch, every time. Added a generation-validated
  per-call-site cache (`jit_table_cache_t`), grounded in `table.c`'s
  actual reallocation sites rather than an assumption that globals are
  fixed. `table_get` dropped out of both profiled workloads' hot-function
  lists entirely (was ~6-16% depending on workload).

Neither workload showed NaN-boxing as the clear next lever: `perf stat`
showed low L1 miss rates (1.5-2.3%) and high IPC (2.8-3.5) in both.
That is not a picture of a stalled, memory-bound VM waiting to be unlocked.
Worth reassessing #3 (NaN-boxing) against a *new* profile once these two
fixes have had a chance to shift where the remaining time goes, rather
than assuming the original profile still applies.

A separate, unrelated bug turned up while chasing an anomaly in
`speedtest.sh`'s own numbers (`.paw`/`--bin` slower than running from
source, backwards from expectation): the `.paw` serializer never wrote
R1/R2a's register-allocation metadata at all, silently disabling the
JIT's whole register allocator for anything not run straight from
`.pw` source (design.md, 2026-08-16). Fixed; `PAW_VERSION` bumped 1->2.

**Call-site inline caching for OP_CALL** (design.md, 2026-08-16):
`speedtest.sh` also showed `fib` as the worst benchmark after
`string_build`, and unlike that one (allocation/interning, outside the
JIT's domain), this was squarely a dispatch problem. Every JIT'd call,
even to an already-compiled, always-identical callee, paid four real
function calls (`vm_jit_call` -> `call_value` -> `jit_maybe_compile` ->
`jit_entry`) plus redundant arity/frame-count checks and frame setup,
every time. Cached per call site the same way (`jit_call_cache_t`), with
one simplification over the table caches: `jit_state_t` is monotonic, so
once a cell is populated with an already-`JIT_COMPILED` callee, it needs
no staleness check ever again. ~17% faster on `fib` from dispatch
caching alone (isolated via the same forced-always-miss technique used
to measure R2a's own delta), on top of everything else. Worth knowing
for future reasoning: caching only warms up for *repeated* or
*branching* call patterns (loops, `fib`-shaped recursion). A single
linear recursion chain (each level calling exactly once) can never warm
its own cache within that one chain, by construction of the reentrant
call model, though a second separate call through the same site does
see it warm. This is a real property of caching under a synchronous,
non-OSR JIT.

**Array-indexing fast path for OP_GET_INDEX/OP_SET_INDEX** (design.md,
2026-08-16): the last two opcodes whose JIT translation was entirely a
C-helper call. Both emitters now inline the array + in-bounds-int
common case as a direct memory read/write through `array->values[index]`
and fall back to the existing helpers (string indexing, out-of-bounds,
wrong types) unchanged. ~12% faster on a new pure-indexing benchmark
(`bench/pw/array_index.pw`, added because no existing benchmark's
paweli-side hot code did any indexing. `array_sort`'s doesn't, since
`stdlib.sort` is native C; that benchmark is unchanged, as expected).

### 3. NaN-boxing (value representation)

Deferred since the 2026-08-13 tagged-union decision ("NaN-boxing is a
later micro-optimization"). Relevant
to JIT v2 because value round-tripping costs dominate in both the
interpreter and compiled code; a single-u64 NaN-boxed value_t removes a
tag load/store and a compare from the hot path of every arithmetic
opcode. Pairs with #1: register allocation is simpler and more effective
on 8-byte values.

- [ ] Reassess against #1 before starting: whichever lands first
      changes value_t's ABI used by every opcode emitter, the GC,
      the `.paw` serializer, and the interpreter
- [ ] Requires the same full verification matrix the language features
      have always used

### 4. Type specialization / tiering

v1 deliberately emits runtime type checks (int-vs-float) in every
compiled opcode with no speculation and no deopt. A tiered approach
statically-specialize a function's body from its (statically known)
signature and locals, and only fall back to checked code where a value
is genuinely polymorphic, is the natural second tier.

- [ ] Only after #1 the win is mostly measured in instruction count
      saved, and allocation-free code makes measurement meaningless
- [ ] Keep the no-speculation/no-deopt property unless a concrete
      workload proves speculation is worth its complexity

### 5. On-stack replacement

Deferred, per the JIT's decision-log entry: threshold 1 compiles
functions on their first call, so a called-once function's loop already
runs native.
OSR's remaining niche is a function too large to want eagerly compiled
but with one hot loop.

- [ ] Only reconsider if a real program shows that shape to be a
      bottleneck; not worth building preemptively

### 6. Non-x86-64

Walled off by the hand-written x86-64 assembler (jit_asm.c). ARM64
would be the natural second backend once #1 settles the code-gen model,
so the ISA-specific emitter doesn't get rewritten twice.

## Notes

- Verification discipline does not change: clean Debug and Release
  (`-Werror`) rebuilds, the full test suite, and the whole matrix
  rerun under `PAWELI_GC_STRESS=1` and `PAWELI_JIT_THRESHOLD=0` with
  ASan/UBSan, then the bench suite after every JIT-affecting change.
- #1 and #3 share a hard dependency (value_t's size/ABI), and #6
  depends on #1's code-gen model. Budget work in that order.
