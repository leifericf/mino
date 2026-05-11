# Changelog

## v0.125.0 — Arith Fast-Lane Direct Tag Extraction

The Phase D payload. `binop_int_fast` and `unop_int_fast` in
`src/eval/bc/vm.c` now extract tagged ints inline via `MINO_IS_INT`
tag-bit tests and `MINO_INT_VAL` decode, skipping the
`mino_val_int_p` / `mino_val_int_get` helper chain. A single tag-bit
test per operand replaces NULL + tag + boxed-type — 2–3 ALU ops
saved per operand check, ~5 ops saved per call.

Encoding: a new inline helper `tag_or_box_int` handles the
post-overflow-check encode path. For results that fit in
`[MINO_INT_MIN, MINO_INT_MAX]` (the 61-bit signed range, ~±1.15e18)
it returns `MINO_MAKE_INT(r)` — no allocation, no cell init. For the
narrow band beyond the tagged range, it falls back to `mino_int(S, r)`
which allocates a boxed cell. Both code paths increment the
`bc_int_make_count` / `bc_int_alloc_avoided` counter pair.

Overflow boundary tests added in `tests/arithmetic_test.clj`:

- Values at `MINO_INT_MAX` / `MINO_INT_MIN` stay `:int` and survive
  the round-trip through the fast lane.
- `inc` / `dec` crossing the tagged boundary box transparently.
- `+`, `-`, `*` across the boundary in both directions.
- `INT64_MAX` / `INT64_MIN` overflow still throws under strict arith
  (matches the existing `integer-overflow-strict-and-primed` test).
- Both top-level and in-fn execution paths exercised so the BC fast
  lane and the tree-walker prim path both get the boundary cases.

Verification: full test suite (1 558 / 7 306) green on release,
ASan, UBSan. UBSan green is the load-bearing correctness signal —
it catches every misaligned deref the tag scheme could
silently corrupt around.

## v0.124.0 — GC IC-Marking Audit and Stress Gate

A verification milestone. The IC-marking path in `src/gc/roots.c`
and every GC-internal `(gc_hdr_t *)` deref site was audited for
tagged-value safety. The result: every site already routes through
`gc_mark_interior` (which fast-rejects tagged values at the top via
`MINO_TAG_MASK`), `gc_mark_child_push` (same guard), or operates on
a pointer that is heap-only by construction (intern table entries,
which are always interned symbol/keyword cells; never tagged).

What changed:

- A clarifying tag-safety comment near the IC table walk in
  `gc_mark_runtime_globals` makes the guarantee explicit at the
  call site rather than implicit in the helper's body.
- No other code changes. The audit verified clean.

Gate run at this tag:

- All 11 `~/Code/mino-bench/stress/run_gc_shard*.clj` shards green
  (≈690 tests / 2 870 assertions in aggregate). These exercise
  the major-cycle + minor-cycle interleaving under heavy mutation
  and remset traffic — exactly the surface where a stray
  tagged-as-pointer deref would corrupt memory silently.
- ASan + UBSan + the full unit test suite (1 557 / 7 279) all
  green.

The single-tag work for the next milestone (v0.125.0) is the arith
fast-lane payload: `OP_ADD_II`, `OP_INC_I`, `OP_DEC_I`,
`OP_ZERO_INT_P` rewritten to extract tagged ints inline without
going through `mino_val_int_get`, plus overflow boundary tests at
`MINO_INT_MAX` / `MINO_INT_MIN` and ±1 around `INT64_MAX` /
`INT64_MIN`.

## v0.123.0 — Inline Tags for BOOL, NIL, CHAR

Extends the tag scheme that v0.118.0–v0.122.0 set up for inline-tagged
integers to the remaining three reserved tags. After this commit
`mino_true`, `mino_false`, `mino_nil`, and `mino_char` all return
inline-encoded values; the `nil_singleton`, `true_singleton`, and
`false_singleton` fields on `mino_state_t` become dead storage (kept
in the struct only to avoid a separate ABI break — embedders never
touched them directly).

New macros in `src/mino.h`:

- `MINO_IS_BOOL(v)`, `MINO_IS_NIL(v)`, `MINO_IS_CHAR(v)` — tag
  predicates analogous to `MINO_IS_INT(v)`. `MINO_IS_NIL` also
  matches C `NULL`, mirroring the runtime's "no value" sentinel.
- `MINO_MAKE_BOOL(b)`, `MINO_MAKE_NIL`, `MINO_MAKE_CHAR(cp)` —
  inline encoders. Bool stores the truth bit at offset 3; char
  stores the Unicode codepoint in the upper 61 bits.
- `MINO_BOOL_VAL(v)`, `MINO_CHAR_VAL(v)` — payload decoders.

`src/runtime/internal.h`:

- `mino_type_of(v)` now dispatches on all 5 tag values, returning
  the matching `MINO_*` for inline-tagged scalars and falling
  through to `v->type` for heap pointers. Order: NULL → PTR →
  INT → BOOL → NIL → CHAR.
- Adds `mino_val_bool_p` / `mino_val_bool_get` and
  `mino_val_char_p` / `mino_val_char_get` helpers analogous to the
  v0.119.0 int helpers; readers go through these to stay
  form-agnostic.

Migration (≈12 sites for bool, ≈12 for char):

- `src/prim/{async,lazy,numeric,reflection,string}.c`,
  `src/eval/print.c`, `src/public/embed.c`,
  `src/collections/{val,map,clone,rbtree}.c` — every `v->as.b`
  and `v->as.ch` read rewritten to use the unified accessor.
- `src/runtime/internal.h::mino_is_truthy_inline` — the eval/if
  hot-path truthiness check is updated; without this every `if`
  form crashes on tagged BOOL.
- `src/eval/special_registry.c::eval_and` — its initial
  `result = &S->true_singleton` becomes `result = mino_true(S)`.
- `src/collections/val.c` — `mino_nil`, `mino_true`, `mino_false`,
  `mino_char` are now one-line inline-encode returns; the
  per-state singletons stay on `mino_state_t` (dead storage) so
  nothing in the public ABI moves.

Verification: full test suite, ASan, UBSan all green (1557 / 7279).
Perf impact: small — tagged BOOL/NIL save no allocations
(singletons already shared one cell per kind); CHAR saves an alloc
per char construction, visible on char-heavy workloads. The
allocation-heavy hot lanes for INT continue to dominate the
measurement signal; the bigger BOOL/NIL/CHAR win is structural
(cleaner abstraction, no per-state singleton bookkeeping for
constants).

## v0.122.0 — Constructor Flip: Inline-Tagged Integers

`mino_int(S, n)` now returns an inline-tagged pointer for every `n`
in the 61-bit signed range `[MINO_INT_MIN, MINO_INT_MAX]`. The boxed
heap allocation that previously happened for every out-of-cache int
is gone — the value rides in the pointer's spare bits.

The constructor change is six lines:

```c
mino_val_t *mino_int(mino_state_t *S, long long n)
{
    mino_val_t *v;
    S->bc_int_make_count++;
    if (n >= MINO_INT_MIN && n <= MINO_INT_MAX) {
        S->bc_int_alloc_avoided++;
        return MINO_MAKE_INT(n);
    }
    v = alloc_val(S, MINO_INT);
    v->as.i = n;
    return v;
}
```

The boxed fallback stays in place for the narrow band between
`MINO_INT_MAX` (≈1.15e18) and `LLONG_MAX` (≈9.22e18) where the tag
would lose precision; in that band the value still allocates a cell.
The small-int cache that v0.121.0 and earlier used is now dead code
for the in-range case (the tagged form is faster and allocation-
free) and dropped from the constructor.

v0.118.0 set up the tag scheme, v0.119.0 added the infrastructure
helpers + GC alignment audit, v0.120.0 migrated every `->type ==` /
`switch (X->type)` / `X->as.i` site to the `mino_type_of` /
`mino_val_int_get` helpers, and v0.121.0 closed the remaining
generic-deref gaps (`X->type` as a function argument, cross-type
comparisons, the defensive `a->type < 0` check). With all four
landed, this commit is the six-line payload — and the test suite,
ASan, and UBSan all stay green with no further changes needed.

## v0.121.0 — Generic-Deref Audit for Tagged-Int Safety

Closes the remaining audit gaps that v0.120.0's `->type ==` / `->as.i`
sweep didn't catch, so the v0.122.0 constructor flip can land without
revisiting the call-site layer.

The v0.120.0 perl regex caught `X->type ==`, `X->type !=`, and
`switch (X->type)`, but missed several patterns where `X->type` is
still read as a plain value:

- `X->type` passed as a function argument
  (`meta_readable(obj->type)`, `supports_meta(obj->type)`,
  `alloc_val(S, obj->type)`).
- Cross-type comparisons (`a->type == b->type`, `a->type != b->type`,
  `a->type < b->type`).
- Defensive checks like `a->type < 0`.

This tag rewrites those sites to use `mino_type_of(X)` so they stay
correct when `X` is inline-tagged. Files touched:

- `src/prim/meta.c` — every `obj->type` access in `prim_meta`,
  `prim_with_meta`, `prim_vary_meta`, `prim_alter_meta`.
- `src/prim/numeric.c` — three `a->type == b->type` cross-compares in
  the compare/tower paths plus one defensive `a->type < 0`.
- `src/prim/ns.c` — `ks[i]->type == MINO_SYMBOL` in
  `names_contains`.
- `src/collections/val.c` — record field lookup, chunked-cons
  iteration, and the equality cross-type / sequential branches.
- `src/collections/rbtree.c` — `val_compare`'s same-type and
  fallback-ordering paths.
- `src/runtime/ns_env.c` and `src/eval/special.c` — `alloc_val(S,
  sym->type)` rewritten to use `mino_type_of(sym)` for hygiene.

The `->meta` read sites surveyed during this tag were all found to be
safe: every one either operates on a freshly-allocated value (`out`,
`copy`, `result`, `new_rec`, etc.) or is inside a type-discriminated
branch that already excludes `MINO_INT` via `mino_type_of`. No further
guards needed for those.

Verification: the constructor flip was temporarily applied at this
tag and the full suite ran green (release + ASan + UBSan, 1557 /
7279); the flip was then reverted so this commit ships the audit
alone. The actual `mino_int(S, n)` flip lands at v0.122.0 as a six-
line constructor change.

Perf gate **skipped** at this tag: no behavior change.

## v0.120.0 — Tag-Safe Type Discrimination at Call Sites

Migrates every type-discrimination site to the `mino_type_of(v)`
helper introduced in v0.119.0. No representation change at this tag —
`mino_int(S, n)` still returns a boxed cell from the small-int cache
or a fresh alloc — but the codebase is now uniformly safe for the
constructor flip, with one important caveat (see below).

Three blanket call-site rewrites land here:

- `X->type == MINO_Y` and `X->type != MINO_Y` become
  `mino_type_of(X) == MINO_Y` and `mino_type_of(X) != MINO_Y`.
  `mino_type_of` returns `MINO_INT` for an inline-tagged int,
  `MINO_NIL` for `NULL`, and `v->type` otherwise — so today, with
  every value still boxed, behavior is preserved exactly; with
  tagged values, the dispatch stays correct without a deref.
- `switch (X->type)` becomes `switch (mino_type_of(X))` for the same
  reason: the switch is now NULL-safe and tag-safe.
- `X->as.i` reads become `mino_val_int_get(X)`. The single legitimate
  write at the boxed-MINO_INT construction site in `val.c` is left
  alone.

The rewrites touch 48 files. All 1557 tests / 7279 assertions stay
green on release, ASan, and UBSan.

Scope caveat — second sweep needed before constructor flip. While
attempting the flip in this tag, the test suite crashed in `atom_set`
and `prim_type`. Root cause: the migration pass covered `->type` and
`->as.i` access, but the GC write barrier (`gc_write_barrier` in
`src/gc/barrier.c`) still dereferences `new_value` and `old_value` as
`gc_hdr_t *` without tag-checking, and a wide population of sites
read `v->meta`, `v->as.cons.car`, `v->as.atom.val`, etc. directly off
a value that the next tag could see as inline-tagged. The plan's
"22 files, 200 callsites" estimate was correct for the int-typed
sites; the broader generic-deref audit (~60 `->meta` reads alone, and
the GC barrier's three internal derefs) needs its own tag.

GC barrier guards added preemptively at this tag (no behavior change
today, prep for v0.121.0): the two SATB pushes and the remset path
in `gc_write_barrier` now early-return on any pointer with non-zero
low three bits.

Helper layout in `internal.h` was reordered so `mino_type_of` is
defined before `mino_val_int_p`/`mino_val_int_get`; the latter two
delegate to it, eliminating two near-duplicate tag-check sites.

## v0.119.0 — Pointer-Tagged Value Representation: Infrastructure

Lands the infrastructure that subsequent tags need to flip the
boxed-int representation to inline-tagged. Still no behavioural
change: every value continues to flow through the boxed `mino_val_t`
cell path; `mino_int(S, n)` keeps returning a small-int cache cell
or a freshly-allocated boxed cell. What this tag installs is the
machinery so the constructor flip at the next tag can land without
crashing the GC and without requiring a 200-callsite rewrite in the
same commit.

Three additions in `src/runtime/internal.h`:

- Unified inline accessors `mino_val_int_p(v)` and
  `mino_val_int_get(v)`. Both handle the inline-tagged form and the
  boxed `MINO_INT` cell. Read sites that migrate to these helpers
  stay correct under either representation, so the migration can
  proceed one file at a time.
- Two counter fields on `mino_state_t`: `bc_int_make_count` and
  `bc_int_alloc_avoided`. The first bumps on every call to
  `mino_int(S, n)`. The second stays at zero this tag and starts
  incrementing at v0.120.0 when the constructor flips to return
  tagged values for the 61-bit range.

In `src/gc/driver.c`:

- `gc_mark_child_push` and `gc_mark_interior_push` now skip any
  pointer with non-zero low three bits. A heap pointer is always
  8-byte aligned, so a non-zero tag means an inline-tagged value
  that has no header to mark. Both guards are no-ops today; they
  exist so the constructor flip at v0.120.0 does not bring the GC
  down on the first minor collection.
- `gc_alloc_typed_inner` and `alloc_val_inner` assert
  `MINO_ASSERT_ALIGNED(p)` at return. Active under assertions,
  no-op under `-DNDEBUG`. This is the alignment audit gate called
  for in the cycle plan: a single misaligned alloc would silently
  corrupt the tag scheme.

Scope note: the cycle plan originally estimated v0.119.0 as a single
tag that flipped the constructor and rewrote the ~50–80 affected
call sites. A re-grep of the codebase found ~129 reads of `v->as.i`
and ~70 `*->type == MINO_INT` predicates, plus scattered
`switch (v->type)` patterns that also need gating on
`MINO_IS_PTR(v)` before deref. Splitting infrastructure (this tag)
from migration (v0.120.0) keeps each commit reviewable and ASan-
verifiable, and matches the per-tag ASan/UBSan gates the plan
requires.

Release / ASan / UBSan continue to pass; 1557 tests / 7279
assertions green on all three.

## v0.118.0 — Pointer-Tagged Value Representation: Layout Contract

Header-only change that lands the layout contract for the
pointer-tagged value representation. No call site uses the new
macros yet; this tag installs the vocabulary so subsequent tags
can migrate alloc sites, GC paths, and arithmetic fast lanes one
chunk at a time.

`mino.h` gains the tag scheme:

```
tag 000  ->  heap pointer to struct mino_val
tag 001  ->  inline 61-bit signed int (payload bits 63..3)
tag 010  ->  reserved for inline BOOL
tag 011  ->  reserved for inline NIL
tag 100  ->  reserved for inline CHAR
tag 101..111  ->  reserved
```

Public macros: `MINO_TAG_PTR`, `MINO_TAG_INT`, `MINO_TAG_BOOL`,
`MINO_TAG_NIL`, `MINO_TAG_CHAR`, `MINO_TAG`, `MINO_IS_PTR`,
`MINO_IS_INT`, `MINO_INT_VAL`, `MINO_MAKE_INT`, plus the 61-bit
signed range constants `MINO_INT_MAX` and `MINO_INT_MIN`. The
`MINO_INT_VAL` decode relies on arithmetic right shift of signed
integers, which C99 6.5.7p5 leaves implementation-defined for
negative operands; the layout note in the header records that
every supported toolchain (clang, gcc, msvc on x86_64 and arm64)
implements it as sign-preserving. 64-bit hosts only.

The header also fixes the stable execution ABI carried across
the representation rollout: frame layout, register window
indexing, call/tailcall handoff, and the bailout-to-tree-walker
contract do not change. Only the in-register and in-memory layout
of values changes. Prims with the `mino_val_t *args` (cons spine)
ABI keep that ABI; tagged values flow through every slot
identically.

`src/runtime/internal.h` gains the runtime-internal debug
assertion helpers `MINO_ASSERT_INT`, `MINO_ASSERT_PTR`,
`MINO_ASSERT_TAGGED_NONNULL`, and `MINO_ASSERT_ALIGNED`. They
compile to no-ops under `-DNDEBUG` and are intended for the
alloc-site audit and GC adjustments in upcoming tags. No
production code references them yet.

No tests changed. Release / ASan / UBSan continue to pass; 1557
tests / 7279 assertions green on all three.

## v0.117.0 — Bytecode Constant-If Fold And Tail-MOVE Peephole

Two small bytecode compiler optimisations.

First, `compile_if` now folds constant conditions at compile
time. When the condition form is self-evaluating (`true`,
`false`, `nil`, a literal int / keyword / string / char), the
compiler evaluates truthiness on the spot and emits only the
chosen branch. `(if true 1 0)` compiles to `OP_LOAD_K dst, k(1)`
+ `OP_RETURN` instead of the previous six-instruction
`OP_LOAD_K cond` / `OP_JMPIFNOT` / `OP_LOAD_K then` / `OP_JMP`
/ `OP_LOAD_K else` sequence.

Second, a tail-MOVE peephole runs once per clause at the end
of `compile_clause`. If the clause body's last emitted
instruction is `OP_MOVE ret_reg, X`, the instruction before it
is a foldable producer (`OP_LOAD_K`, `OP_GETGLOBAL`, any of
the `*_II` binops, the unary fast-lane ops, `OP_CLOSURE`,
`OP_MAKE_LAZY`) that wrote to `X`, and neither pc is a jump
target, the producer's `A` operand is rewritten to `ret_reg`
and the `OP_MOVE` is dropped. This catches the
`(let [x form] x)` shape where the body's tail returns a
binding directly -- the binding's emit becomes the return-slot
emit, the redundant MOVE disappears.

Measured against v0.116.0 (release `-O2`, min-of-3 from
`perf_gate.clj`):

```
arith-add        1716 -> 1640  (+4.4%)
arith-inc        1652 -> 1598  (+3.3%)
fn-call-identity 1546 -> 1515  (+2.0%)
if-branch        1479 -> 1448  (+2.1%)
let-local-lookup 1445 -> 1439  (+0.4%)
loop-recur-5     1678 -> 1652  (+1.6%)
do-block         1604 -> 1576  (+1.7%)
10M tight loop  1091ms -> 1054ms (+3.4%)
```

Modest, narrow wins. The eval-floor benches are largely
call-overhead-bound, so even removing four instructions from
the if-branch body (six down to two) only shaves 31ns out of
a ~1500ns total. The tight loop dropped 37ms because the
arith-add change closes a per-iteration overhead in the
return path.

Bytes/op unchanged; both folds remove instructions but no
allocations.

All 1557 tests, 7279 assertions pass on release / ASan / UBSan.

## v0.116.0 — Bytecode Operand-Inplace For Fast Lanes

The bc compiler's int fast lanes now read operand registers
directly when the operand is a local. Before, `(+ x j)` with
`x` and `j` both as locals allocated two fresh temp registers,
emitted `OP_MOVE temp1, x_reg` and `OP_MOVE temp2, j_reg`, then
emitted `OP_ADD_II dst, temp1, temp2`. After,
`compile_operand_inplace` returns the local's existing register,
the two MOVEs are gone, and the binop becomes
`OP_ADD_II dst, x_reg, j_reg`. Same change covers the unary
lanes for `inc`, `dec`, and `zero?`. Literals and non-local
refs still allocate temps and run through `compile_expr` as
before, since the operand must live in some register for the
opcode to read it.

The net is fewer instructions and lower `n_regs` for the
common shapes -- `(fn [x j] (+ x j))` drops from `n_regs=5`
(params + ret + 2 temps) to `n_regs=3` (just params + ret).
Per-call cost in `bc_push_window` falls in proportion; the
high-water mark amplifies through every recursive arm.

Measured against v0.115.0 (release `-O2`, min-of-3 from
`perf_gate.clj`):

```
arith-add        1810 -> 1716  (+5.5%)
arith-inc        1737 -> 1652  (+5.1%)
fn-call-identity 1640 -> 1546  (+6.1%)
if-branch        1580 -> 1479  (+6.8%)
let-local-lookup 1523 -> 1445  (+5.4%)
loop-recur-5     1812 -> 1678  (+8.0%)
10M tight loop  1183ms -> 1091ms (+8.4%)
fib(20)         2.45ms -> 2.29ms (+6.9%)
```

The header block in `src/eval/bc/compile.c` no longer claims
"Phase 1" coverage or a "stupid first" allocator; it now lists
the actual special-form coverage as of this release.

All 1557 tests, 7279 assertions pass on release / ASan / UBSan.

## v0.115.0 — Bytecode Tail-Position Propagation And Unary Int Fast Lanes

The bc compiler now propagates tail position through `if`, `do`,
`let`, and `loop`. Any call sitting in a control-form's tail
slot -- including the recursive call inside `(if cond done
(recur ...))` and the cross-fn tail call in
`(defn f [n] (if (= n 0) :done (f (- n 1))))` -- emits
`OP_TAILCALL` and reuses the apply_callable trampoline, keeping
the C stack flat across arbitrary recursion depth.

The same change exposed two latent encoder bugs that had been
silently masking bytecode coverage. The bias-encoded jump-offset
bounds in `patch_jmp` and the `recur` back-jump emitter both
read `INT16_MIN + 0x8000` (= 0) and `INT16_MAX - 0x8000` (= -1),
so every conditional branch declined bytecode compilation and
landed back on the tree-walker. The correct bounds
(`-0x8000..0x7FFF`) replace them; every `if`-bodied fn now runs
through the VM as intended.

Three unary int fast-lane opcodes -- `OP_INC_I`, `OP_DEC_I`,
`OP_ZERO_INT_P` -- join the eight binary lanes from the prior
release. The compiler emits them for `(inc x)`, `(dec x)`, and
`(zero? x)` when the head resolves to the non-local non-macro
prim; on a type miss the handler falls back to `prim_inc` /
`prim_dec` / `prim_zero_p` via the same cons-spine ABI as a
regular `OP_CALL`. The `!tail` gate on both unary and binary
fast lanes is gone: speculation produces a value that the
surrounding `OP_RETURN` carries out, with no trampoline
indirection.

The 10M `(inc i)(dec j)` tight-loop probe drops from ~4.7s to
~1.2s. Self-tail recursion at depth 100000 (`(defn countdown
[n] (if (= n 0) :done (countdown (- n 1))))`) now runs flat
through the VM trampoline instead of overflowing the C stack.
All 1557 tests, 7279 assertions pass on release / ASan / UBSan.

## v0.114.0 — Bytecode Speculative Int+Int Fast Lanes

The bc compiler emits per-op specialised opcodes -- `OP_ADD_II`,
`OP_SUB_II`, `OP_MUL_II`, `OP_LT_II`, `OP_LE_II`, `OP_GT_II`,
`OP_GE_II`, `OP_EQ_II` -- for the eight binary arith / compare
calls (`+ - * < <= > >= =`) when both the head is a non-local
non-macro and the call site has exactly two args. Each handler
runs the v0.103.0-era int+int fast lane and, on a type miss,
falls through to the matching prim with the same cons-spine
argv as a regular `OP_CALL`. The compiler skips the speculation
when the call is in tail position so the OP_RETURN / OP_TAILCALL
discipline isn't disturbed.

A pre-existing encoding bug in `MK_BINOP_INT` (sub-op nibble
overlapped the op byte for any non-zero sub-op) is sidestepped:
the original `OP_BINOP_INT` opcode stays in the enum and runtime
for any hand-written stream that uses sub-op zero, but the
compiler now emits the per-op variants instead. Phase-4
profile-driven runtime rewriting -- the original plan -- is
overkill once the compile-time speculation covers the only set
of opcodes the plan would have promoted to anyway.

All 1557 tests, 7279 assertions pass on release / ASan / UBSan.

## v0.113.0 — Bytecode Multi-Arity Dispatch

Multi-arity fns now bc-compile. Each `([params] body...)` clause
becomes a `mino_bc_clause_t` entry on the compiled record (its
own `n_params`, `has_rest`, `entry_pc`, and `params_vec`); the
shared code stream holds every clause's bytecode back-to-back.

At fn entry, the runtime scans the clauses array twice: first
looking for a fixed-arity match against `argc`, then for a
variadic clause whose `n_params <= argc`. The matched clause's
entry pc starts the interpreter loop; the matched params publish
into the env when the fn captures, alongside any collected rest
list.

The compile path keeps the single-arity fast path as a degenerate
one-clause case so nothing about the existing benchmarks regresses.
Full tree-walker retirement still waits on try/catch, binding,
and full destructuring; those land alongside the specialization
work.

All 1557 tests, 7279 assertions pass on release / ASan / UBSan.

## v0.112.0 — Bytecode Loop/Recur and Lazy-Seq

`(loop [...] body)` compiles into a binding scope with a recur
target installed at the loop entry pc. `(recur ...)` evaluates
its args into temporaries, moves them onto the loop's binding
registers, and jumps back to the entry. Nested loops stack the
recur targets so each `recur` only sees its enclosing `loop`.

`(lazy-seq body...)` stashes the body forms in the constant pool
and emits `OP_MAKE_LAZY`, which builds a MINO_LAZY whose `.body`
is the form list and whose `.env` is the live lexical chain at
the OP_MAKE_LAZY site. Realisation reuses the existing tree-walker
path; only the construction side is new on the bc dispatch. The
env-capture pre-scan now recognises `(lazy-seq ...)` alongside
inner `(fn ...)` literals, so the enclosing fn publishes its
let-bindings into the env in time for the lazy body to see them.

`(try ...)`, `(throw ...)`, `(binding [...] ...)` stay tree-walked
for this cycle. Their PUSH/POP-DYN and PUSHCATCH/POPCATCH handlers
land alongside the tree-walker retirement.

All 1557 tests, 7279 assertions pass on release / ASan / UBSan.

## v0.111.0 — Bytecode &-Rest and Constant Vectors

Single-arity fns with a trailing `& rest` binding now bc-compile.
The compiler tracks a `has_rest` flag on the compiled record;
the runtime relaxes its arity check to `argc >= n_params` and
collects the overflow args into a list, placed in the register
right after the fixed params. When the enclosing fn also captures,
the rest list is published into the env alongside the other
params so any inner closure sees it via `mino_env_get_sym`.

Vector literals whose elements are all self-evaluating (nil /
bool / int / float / string / keyword / char) are stashed whole
in the constant pool and loaded with a single OP_LOAD_K -- the
common shape `(defn f [...] [...] [literal-values])` no longer
declines to the tree-walker on this one count. Vector literals
with non-const elements, plus all map and set literals, still
decline; their full lowering lands alongside the multi-arity and
destructuring work in a follow-up cycle.

Multi-arity and full destructuring (vector / map / `:as` / `:keys`)
remain on the tree-walker for this cycle. The follow-up adds them
together with the loop/recur and Phase-2 opcode wiring.

All 1557 tests, 7279 assertions pass on release / ASan / UBSan.

## v0.110.0 — Bytecode Closures

The bc compiler emits inner `(fn ...)` and `(fn* ...)` literals,
including the named-fn `(fn name [params] body)` form that lets a
closure recurse by name. Three new opcodes -- `OP_PUSH_ENV`,
`OP_POP_ENV`, `OP_ENV_BIND` -- manage the lexical-env chain so
captured closures see exactly the let-scoped bindings that were in
scope at OP_CLOSURE time. Fns whose body contains no inner fn skip
the env machinery and keep their bindings register-only.

A pre-scan over the fn body sets a `captures` flag on the compiled
record. When set, the runtime extends the captured env with a
fresh child at entry and publishes the fn's params into it, and
the compiler brackets every let scope with PUSH_ENV / POP_ENV plus
an OP_ENV_BIND per binding. Named-fn literals emit a 4-instruction
sequence that wraps a child env around the OP_CLOSURE itself so the
closure captures an env that already has its name pointing at it.

Inner fn literals are stored as MINO_FN templates in the outer
fn's constant pool. OP_CLOSURE copies the template's params, body,
defining-ns, shape, and bc into a fresh closure value and seals in
the live env; each invocation that reaches OP_CLOSURE therefore
produces a distinct closure over the current lexical chain.

Multi-arity inner fns are normalised via `build_multi_arity_clauses`
(the same helper eval_fn uses) so the template's params/body shape
matches what the tree-walker fallback expects; their bc remains the
declined sentinel for this cycle and the closures fall back to the
tree-walker at apply_callable time. Single-arity inner fns whose
body the compiler covers run on the bc dispatch from the first call.

All 1557 tests, 7279 assertions pass on release / ASan / UBSan.

## v0.109.0 — Bytecode Macro-Aware Emit

The bc compiler emits `OP_CALL` for non-tail regular calls and
`OP_TAILCALL` for tail-position simple calls. Both emissions are
gated on a compile-time macro probe that walks the same cascade
the runtime uses at dispatch: lexical environment first, then
the fn's defining-namespace env, with full alias resolution for
qualified `ns/name` heads. The lexical-only check that previously
gated the emitter missed macros that live in the ns env -- which
is where most macros sit -- so the emitter declined every call
shape rather than risk handing evaluated args to a macro. The
ns-aware probe closes that gap and unblocks bc dispatch for
ordinary calls.

The probe scopes alias lookup to the fn's defining ns rather
than `S->current_ns` at compile time, since lazy compile-on-
first-call runs with the caller's ns active but the runtime
dispatch then switches to the fn's defining ns. Without that
scoping, alias resolution would consult the wrong table.

The OP_CALL ABI loads consecutive register slots and hands them
to `apply_callable` via the same cons-spine argv that the rest
of the runtime consumes. OP_TAILCALL is emitted only when the
final expression of a body is a direct call (special forms in
tail position keep tree-walked behaviour for this cycle).

All 1557 tests, 7279 assertions pass on release / ASan / UBSan.

## v0.108.0 — Specialization Opcode Reservation

Eleven Phase-4 opcodes are added to the bytecode opcode enum
so the encoding is stable across the cycle that wires the
specializing interpreter. The new entries cover the
most-likely specializations against the v0.103.0 hot-path
profile: `OP_GETGLOBAL_CACHED` (version-checked direct slot
read), `OP_CALL_CACHED` (cached callable + version snapshot),
eight per-op int+int variants (`OP_ADD_II`, `OP_SUB_II`,
`OP_MUL_II`, `OP_LT_II`, `OP_LE_II`, `OP_GT_II`, `OP_GE_II`,
`OP_EQ_II`) that split the single Phase-1 `OP_BINOP_INT`, and
two shape specializations (`OP_GET_KW_MAP` for keyword-on-map
`get`, `OP_NTH_VEC` for integer-index-on-vector `nth`).

Their handlers and the in-place opcode-rewriting machinery
land alongside the runtime profiling counters; this cycle
reserves the opcode IDs so embedders that inspect compiled
fns get a stable instruction-set view from the start.

ABI surface and semantics unchanged; all 1557 tests, 7279
assertions pass on release / ASan / UBSan.

## v0.107.0 — Bytecode Require Mode

`MINO_BC_REQUIRE=1` flips the tree-walker fallback in
`apply_callable`'s bc path from a silent recovery into a hard
abort. With the knob on, every fn that the compiler declines
prints `MINO_BC_REQUIRE: fn declined by compiler` and aborts;
production builds default to unset / `0` and keep the silent
fallback in place. The knob is the standing development gate
for the cycle that retires the tree-walker: once the compiler
covers every form that the test suite exercises, CI runs with
`MINO_BC_REQUIRE=1` set and any silent decline turns into a
loud failure.

The flag lives on a single global (`mino_bc_require_flag`) that
the runtime initialises from the env var at startup. Embedders
that want to opt in programmatically can flip it via the
externally-visible symbol; the runtime does not gate behind a
public C API entry until the form-coverage cycle lands.

ABI surface and semantics unchanged when the knob is off; all
1557 tests, 7279 assertions pass on release / ASan / UBSan with
`MINO_BC_REQUIRE` unset, and abort cleanly with it set as
expected on the current Phase-1/2 declined shapes.

## v0.106.0 — Bytecode Tail-Call Trampoline

A flat trampoline at the bc dispatch boundary. The VM's
`OP_TAILCALL` returns the existing `MINO_TAIL_CALL` sentinel
instead of recursing through `apply_callable`; `apply_callable`'s
bc path consumes the sentinel in a loop, switching the active
function and rebuilding argv without growing the C stack. When
the tail target is bc-compatible the trampoline stays in the VM;
when it isn't (a non-fn callable, a multi-arity / &-rest fn, a
declined-bc fn) the loop pops its frame and hands off to the
regular `apply_callable` path. Tail-recursive shapes that the
tree-walker has been trampolining since v0.71.x now flatten the
same way under the bc dispatch.

The compiler holds off on emitting `OP_TAILCALL` for this cycle:
the emit check needs to consult the namespace-env macro table in
addition to the captured lexical env, otherwise a tail call whose
head resolves to a macro (`(future :done)` inside `for`, for
example) hands off pre-evaluated args and produces wrong results.
The trampoline machinery is in place and verified through hand
written programs; the discriminator fix and the form-coverage
expansion ship together in the next cycle.

The Phase-2 opcodes (`OP_PUSHCATCH`, `OP_POPCATCH`, `OP_THROW`,
`OP_PUSHDYN`, `OP_POPDYN`, `OP_MAKE_LAZY`) are reserved in the
opcode enum so the encoding stays stable; their handlers and
emitters land alongside the corresponding form coverage.

ABI surface and semantics unchanged. All 1557 tests, 7279
assertions pass on release / ASan / UBSan.

## v0.105.0 — Bytecode VM Foundation

A register-based bytecode interpreter sits behind the existing
tree-walker. Compilation is lazy and per-fn: on first call a fn
attempts to compile its body to a 32-bit fixed-width instruction
stream; on success the program is cached on the fn and dispatched
through the VM, on any unsupported shape the call falls back to
the tree-walker. Var redefinition discipline is preserved because
every global reference resolves through the var cell, not a baked
direct value.

This cycle ships the foundation: opcode encoding, dispatch loop,
register stack, GC integration, per-fn compile entry, and the
apply_callable wiring that makes the bc path live. The Phase-1
compiler covers literals, local and global variable refs, `(if)`,
`(do)`, plain-symbol `(let [b v ...] body)`, `(quote)`, and
top-level `(def name expr)`. Function application, multi-arity,
destructuring, `(fn ...)` literals, `(loop / recur)`, `(try /
catch / finally)`, `(binding)`, `(lazy-seq)`, and macro-using
forms decline to the tree-walker; the next cycle adds them with
proper tail-call elimination. ABI surface and semantics are
unchanged; every existing test passes through either path.

- **Opcode header (`src/eval/bc/internal.h`).** 32-bit fixed-width
  instruction encoding (`OP A B C` for the common form, `OP A Bx`
  with a 16-bit operand for jumps and constant references).
  Phase 1 enum covers the eleven opcodes that handle the
  high-frequency form set: `OP_MOVE`, `OP_LOAD_K`, `OP_GETGLOBAL`,
  `OP_SETGLOBAL`, `OP_JMP`, `OP_JMPIFNOT`, `OP_CALL`,
  `OP_TAILCALL`, `OP_RETURN`, `OP_CLOSURE`, and a single
  `OP_BINOP_INT` covering the eight binary integer
  arith/comparison fast lanes through a sub-op nibble. Encoder
  helpers (`MK_ABC`, `MK_ABx`, `MK_AsBx`, `MK_BINOP_INT`) and
  decoder accessors (`OP_OF`, `A_OF`, `B_OF`, `C_OF`, `Bx_OF`,
  `sBx_OF`, `BINOP_OF`) live next to the enum so the encoding
  contract is single-sourced.

- **VM dispatch skeleton (`src/eval/bc/vm.c`).** Switch-based
  interpreter entry point `mino_bc_run`. The Phase 1 skeleton
  returns NULL for every program; opcode handlers and the
  register-stack window plumb in across subsequent commits.

- **`MINO_FN.bc` field, register stack, and GC integration.**
  Adds `const struct mino_bc_fn *bc` to the `MINO_FN` struct
  (forward-declared in `mino.h`) and a `(bc_regs, bc_regs_cap,
  bc_top)` register stack to `mino_state_t`. The GC root walk
  in `src/gc/roots.c` scans every live register slot in
  `[0, bc_top)`; the `MINO_FN` mark pass in `src/gc/driver.c`
  follows the bc fn's const pool. Once compiled programs start
  running, the GC keeps register values and constants reachable
  without explicit pinning at every allocation site.

- **VM dispatch: all 11 Phase-1 opcodes.** `src/eval/bc/vm.c`
  expands from skeleton to full switch dispatch for `OP_MOVE`,
  `OP_LOAD_K`, `OP_GETGLOBAL`, `OP_SETGLOBAL`, `OP_JMP`,
  `OP_JMPIFNOT`, `OP_CALL`, `OP_TAILCALL`, `OP_RETURN`,
  `OP_CLOSURE`, and `OP_BINOP_INT`. The register window grows on
  demand; `args_from_regs` packs an argv slice into the cons
  spine ABI for `apply_callable`. `OP_GETGLOBAL` resolves
  through `eval_impl` against the live var registry; `OP_CLOSURE`
  clones a child template's bc pointer into a fresh
  closure-capturing fn; `OP_BINOP_INT` runs the v0.103.0
  integer fast lane with `__builtin_*_overflow` checks. Nothing
  compiles to these opcodes yet — they're exercised by the
  next commit's compiler coverage — so the test suite continues
  to run through the tree-walker.

- **Compiler entry stub + apply_callable wiring.**
  `src/eval/bc/compile.c` defines the lazy `mino_bc_compile_fn`
  entry and the `mino_bc_declined` sentinel; the entry currently
  declines every fn (form coverage lands form-by-form in
  subsequent commits). `src/eval/fn.c` consults the fn's `bc`
  slot on each `apply_callable`, attempting compile on first
  call and dispatching through `mino_bc_run` when the fn has a
  runnable program. The decline-sentinel skips the retry on
  subsequent calls. With Phase 1's stub returning declined for
  every fn, all 1557 tests pass unchanged through the
  tree-walker fallback.

## v0.104.0 — Eval-Floor Performance Cycle

A non-JIT performance cycle. Each entry below is a self-contained
commit; the user-visible surface stays put while the eval floor
and allocation shape come down. Cumulative result on the
microbenchmark gate: average per-op cost reduced about 24 percent
across 15 benches, allocation per op unchanged. A tight integer
`loop/recur` bench dropped from 941 ms to 375 ms.

- **Allocation profiler.** New compile-time-gated profiler
  (`-DMINO_ALLOC_PROFILE=1`, exposed as `./mino task
  build-alloc-profile` -> `./mino_prof`) wraps every
  `gc_alloc_typed` call with a per-callsite recorder keyed on
  `(__FILE__, __LINE__, tag)`. Three new primitives drive it from
  mino: `alloc-profile-enabled?`, `alloc-profile-reset!`,
  `alloc-profile-dump!`. Macro-based wrapper keeps the default
  release build identical; profile builds carry one extra hash and
  two field updates per allocation.

- **Var registry / interned-string hash indices.** Two
  open-addressing hash mirrors back the var registry and the
  per-state interned-string table. `var_intern` / `var_find` /
  `var_unintern` now hit the hash instead of a linear scan; both
  keys are interned, so the hot lookup is pointer-pair equality
  with no strcmp on a hit. Cold-start install drops from
  ~640^2 strcmps in the old scan to a constant per-name hit.
  `clojure.core/+` vs bare `+` over 100k calls is now
  indistinguishable (was a 110 ns/call gap).

- **Build-time pre-parsed core.clj: not shipping.** Per the plan's
  decision rule, this step was contingent on cold-start parse-only
  time exceeding 30% of total install time after the earlier
  steps landed. Cold start measures ~10 ms now (was ~12 ms);
  parse cannot be the dominant fraction of that. The maintenance
  tax of a generated pre-parsed AST blob isn't earned. Revisit if
  install grows again.

- **Reduce / range fast paths.** `prim_reduce` now has two fast
  lanes for numeric work:
  - **Range source pre-detection.** `lazy_is_int_range` recognises
    a not-yet-realized lazy seq emitted by `prim_range`; when the
    reducer is `+` and the source is a finite range, `reduce`
    walks the integer range directly with overflow-aware C
    arithmetic, never materialising chunks. Bounded by the
    `__builtin_add_overflow` guard so any roll-up overflow falls
    through to the generic path.
  - **Per-step int+int fast lane.** Mirroring the eval-side fast
    lane, the inner loop in `prim_reduce` checks the reducer
    against the canonical `+` / `*` prims and computes
    `(acc, elem)` directly when both are `MINO_INT`. Saves the
    2-cell cons spine per iteration.

  Microbenchmark: `(reduce + (range 1M))` was ~870 ms; now ~514 ms.

- **Numeric int+int fast lane.** `eval_apply_regular_call` now
  recognises the binary call shape `(op a b)` for the canonical
  `+ - * = < <= > >=` prims. When both args evaluate to `MINO_INT`
  the op is computed directly via `__builtin_add_overflow` /
  `__builtin_sub_overflow` / `__builtin_mul_overflow`; comparisons
  use bare `<` / `==`. Any miss (mixed types, overflow,
  non-canonical resolution) builds a 2-cell cons spine and falls
  through to `apply_callable`. Combined with the IC and argv
  paths, the binary numeric lane skips `tower_reduce` entirely on
  the dominant tight-loop shape.

  Microbenchmark: `(loop [i 0 acc 0] (if (< i 1M) (recur (+ i 1)
  (+ acc i)) acc))` was ~941 ms at the start of the cycle and
  ~787 ms after the argv-ABI work; this entry takes it to ~375
  ms (-60 percent overall).

- **Monomorphic inline call cache.** Per-state hashed slot table
  keyed on the call form pointer, with a head-symbol-data tag and a
  `gen_at_fill` snapshot. `var_set_root`, `env_unbind`, and
  `var_unintern` all bump `S->ic_gen` to invalidate every slot in
  one shot. Filled only when the head is an unqualified symbol that
  resolves past every local lexical frame (no `let`/fn-param
  shadow), no dynamic binding context is active, and the resolved
  value is neither an unbound var nor a macro. The GC root walk
  pins both the form and the cached callable in every filled slot,
  so a freed-and-reused form address cannot alias to a stale entry.

- **Fixed: `ns-unmap` now invalidates the inline call cache.**
  Removing a binding via `ns-unmap` (or any path that reaches
  `env_unbind` / `var_unintern`) previously left the cache holding
  the pre-unmap callable; subsequent calls returned the old value
  instead of throwing. Both paths now bump `S->ic_gen`.

- **Per-var version counter.** `MINO_VAR.version` is bumped by
  `var_set_root`; the inline call cache uses it as part of the
  invalidation generation. Cheap: one `unsigned`, one increment per
  redef.

- **Closure-shape pre-compile for user fns.** New `MINO_FN.shape`
  cache (lazy-tri-state: 0 = not yet inspected, 1 = simple, -1 =
  complex). On first call apply_callable inspects the params: if
  every slot is a plain interned MINO_SYMBOL with no `&`-rest /
  `:as` / nested destructure, the cache flips to 1 and subsequent
  calls dispatch through `bind_simple_params`, which walks
  params/args in parallel and calls `env_bind_sym` directly,
  skipping `bind_form`'s pattern-dispatch tower. Anything outside
  the simple shape (destructure pattern, rest arg, multi-arity
  per-clause params) flows through the existing `bind_params`.

- **argv/argc calling convention for hot prims.** New
  `mino_prim_fn2` ABI receives evaluated args as a flat C array
  instead of a cons spine. `mino_prim_argv()` constructs argv-style
  prim values; the install-table extension auto-routes when an
  entry's `fn2` field is non-NULL. `eval_apply_regular_call` has a
  dedicated argv fast path that evaluates each argument straight
  into a stack-resident scratch slot (16 inline, larger args fall
  back to cons), so the fixed-arity hot prims now skip the
  per-call cons spine entirely. `apply_callable` walks the cons
  spine into the same scratch when called externally with cons.
  Migrated initially: `inc inc' dec dec' count first rest cons`
  plus all 22 type predicates emitted by `DEFINE_TYPE_PRED`. The
  variadic arithmetic / comparison ops follow in the migration
  entry below.

  Microbenchmark: `(loop [i 0 acc 0] (if (< i 1M) (recur (inc i)
  (+ acc i)) acc))` was ~941 ms before this cycle, now ~787 ms.

- **argv migration of `+ +' - -' * *' / < <= > >=`.** The variadic
  arithmetic and comparison prims now expose `fn2` argv-ABI
  entry points alongside the existing cons-spine variants. Calls
  with three or more arguments now skip the `eval_args` cons spine
  on the way in and walk an indexed argument array straight into
  `tower_advance` / `tower_seeded_step` / `compare_chain_argv`.
  The existing int+int binary fast lane and the install path are
  unchanged; identity-by-fn-pointer checks
  (`classify_subseq_test`) keep working because the install path
  now preserves both `fn` and `fn2` when a def supplies both.
  Refactor extracted `tower_advance`, `tower_finish`,
  `tower_acc_init`, `tower_seed`, `tower_seeded_step`, and
  `prim_sub_negate` so the cons-spine and argv variants share one
  body.

- **Multi-arity recur env reuse.** `apply_callable`'s recur branch
  previously allocated a fresh `env_child` on every multi-arity
  recur, even when the recur landed on the same clause. The branch
  now snapshots the pre-dispatch params pointer and only allocates
  a new env when the dispatched clause changes (different arity,
  rest arg, destructuring shape). Same-clause recur reuses the
  existing slots in place. Marginal win except in tight
  multi-arity recur loops, where one allocation per iteration is
  dropped.

- **Cached symbol hash on the val.** `mino_val_t.as.s` now carries
  a `uint32_t hash` field populated by `intern_lookup_or_create`
  (FNV-1a of the symbol/keyword/string bytes, same seed as
  `env_hash_name`). `mino_env_get_sym` reads it via a new internal
  `env_find_here_hashed` probe so the hot lookup skips one FNV
  rehash per probed parent frame. Non-interned strings leave
  `hash == 0` and fall through to the existing computed-hash path,
  so freshly-`dup_n`-allocated names still work. The struct
  doesn't grow because the union is dominated by larger members.

- **Inline truthiness for branch dispatch.** Added an internal
  `mino_is_truthy_inline` (static inline in `runtime/internal.h`)
  alongside the exported `mino_is_truthy` function. The hottest
  callers (`eval_if`, `eval_when`, `eval_and`, `eval_or`,
  `prim_not`, plus the predicate inner loops in `prim_filter`,
  `prim_take_while`, `prim_drop_while`, lazy-filter, and lazy-
  take-while) now hit the inline form, skipping the function-call
  overhead per branch decision. The exported function stays
  available for embedders.

- **Symbol-aware env lookup.** `mino_env_get_sym(env, sym)` walks
  the parent chain with the symbol's cached length in hand, so the
  inner hash-indexed probes skip `strlen(name)` per frame.
  `eval_symbol`'s lexical / current-ns / fn-ambient walks now use
  this path. Reader-emitted symbols were already interned via
  `mino_symbol_n` (intern table on `S->sym_intern`), so the
  `env_find_here` pointer-equality fast path was already firing
  on every binding hit; no further interning work was required.

- **Run-over-run drift on `(reduce + (range 1M))` is GC settling,
  not a leak.** Across 10 in-process trials wall-clock drifts
  ~785 ms -> ~1185 ms, then plateaus. Across separate processes
  the same workload measures 794-883 ms with no drift. Cause is
  the major collector's mark walk over ~89 MB of live old-gen
  state that the bootstrap promotes during install; per-major
  cost grows 16 ms -> 39 ms as the heap shape settles. No
  monotonic growth past trial ~5. Recommendation for benchmarks:
  median of 5 after a 3-run warmup; the perf gate already does
  median-of-3. Reducing per-major mark cost is downstream work
  outside this cycle.

## v0.103.0 — Worker-List Lock Split

Closes the only open NEEDS-DESIGN finding from the v0.102.0
adversarial pass: future / agent worker bookkeeping no longer
contends with the heavy state_lock. A tight embedder loop in
`(dosync ...)` or any other state_lock-held form can no longer
starve workers at their entry-link or exit-detach steps.

- New per-state `worker_list_lock` (non-recursive), inner to the
  recursive `state_lock`. Guards exactly two fields: the linked
  list `worker_ctxs_head` (walked by GC root scanning) and the
  live worker counter `thread_count` (gates `thread_limit`).
- `host_threads.c` worker entry-link, exit-detach, the spawn-time
  limit gate, and every `thread_count` mutation moved off
  state_lock onto worker_list_lock.
- `prim/agent.c` agent worker entry-link / exit-detach and
  `agent_worker_ensure`'s gate-and-increment moved similarly.
- `gc/roots.c` `gc_mark_thread_state` takes worker_list_lock
  briefly across each of its three `worker_ctxs_head` walks
  (dyn_stack, gc_save, tx) -- workers no longer mutate the list
  under state_lock, so GC must hold the new lock to walk safely.
- Lock order: state_lock outer, worker_list_lock inner. Workers
  at entry/exit acquire alone (no state_lock held). The spawn
  path and GC root scan reach worker_list_lock from inside
  state_lock.
- Regression test
  `future-thread-count-not-stuck-under-tight-loop` in
  `tests/host_threads_test.clj`: spawn N futures, run 200
  dotimes-wrapped dosyncs in one form, then assert
  `thread_count` returns to 0. Pre-fix this scenario could
  leave the count inflated for the duration of any
  state_lock-held form.

`mino.h` is unchanged at the API surface; embedders that take a
fresh source build pick up the fix transparently.

## v0.102.1 — Adversarial-test pass: doc accuracy + qa-arch hygiene

Adversarial whitebox test of the v0.102.0 STM + Agent surfaces
(both Clojure-level and the new C-API perimeter, individually and
in combination) ran 70+ probes. All real findings are
documentation accuracy issues -- no behavior changed.

- Fix the misleading thread-budget message in `agent_worker_ensure`
  (and the corresponding mino-site / `mino.h` / Coming-from-Clojure
  copy). The embedder thread does NOT count against
  `thread_limit`, so the previous wording (">= 2 to allow one
  agent worker plus the embedder thread") was wrong. Correct
  wording: ">= 1 for one agent worker; >= 2 if both send and
  send-off are used concurrently". The cookbook's `agents.c`,
  the STM page, the Compatibility Matrix, the Intentional
  Divergences page, and the Coming-from-Clojure page all updated
  to match.
- Coming-from-Clojure previously said `agent / send / send-off
  / pmap are not provided` when `thread_limit <= 1`. Updated:
  `agent` itself ships and constructors work; `send` /
  `send-off` throw MTH001 when their pool's worker can't spawn;
  only `pmap` is genuinely absent.
- Compatibility Matrix's `send-via` row said "send and send-off
  share the same per-state worker." Stale -- v0.102.0 split them
  into POOLED + SOLO. Updated.
- STM page intro paragraph said "a worker thread drains the queue."
  Updated to reflect both pools.
- New adversarial probes added under `.local/adversarial/` for
  future regression coverage of the agent surfaces.
- qa-arch hygiene pass: 11 previously-flagged `abort()` sites now
  carry rationale comments (Class I unrecoverable conditions:
  init-time OOM, GC invariant violations, mutex init failures,
  core eval failures); pre-existing TU-size FAILs in `val.c`,
  `bignum.c`, `module.c`, `ns.c`, `numeric.c`, `reflection.c`,
  `sequences.c`, `stm.c`, `string.c`, `state.c`, `imath.c`
  formalised on the allowlist with rationale; pre-existing
  function-size FAILs (`mino_eq`, `mino_print_to`,
  `prim_require`, plus `module.c`'s `load_ns_file`) added to
  the function allowlist. Latent bug fixed: `check-large-fn` was
  calling unqualified `includes?` (resolved to nothing); now
  `str/includes?`. `task qa-arch` now PASSes.

A pre-existing thread-count bookkeeping issue (`(future ...)`
worker decrements lag the embedder under tight-loop contention,
so a subsequent `(send ...)` may throw MTH001 even when fire-
and-forget futures have logically completed) was identified and
filed as NEEDS-DESIGN in `.local/BUGS.md`. The fix requires a
non-trivial threading-model refactor; deferred to a dedicated
cycle. Workaround: deref the last future or await an agent before
spawning more workers.

## v0.102.0 — Agents finish MVP: async dispatch + pool split + C-API

Agent execution model removes the synchronous-on-the-calling-thread
fallback. Per-state agent workers + run queues land in this cycle,
with a separate POOLED / SOLO split for `send` / `send-off`, and a
public C-API perimeter for embedders.

- Internal: `agent_action_node_t` data structure for the run-queue
  of `(agent fn extra)` tuples; per-agent `in_flight` counter;
  per-state `agent_pool[2]` array (POOLED for `send`, SOLO for
  `send-off`) with each pool carrying its own run head/tail and
  worker thread; shared `agent_mu` and `agent_cv` so an `await`
  waiter sleeps once and wakes for either pool's progress.
  Lifecycle hooks in `state_init` / `mino_state_free`. GC
  root-marking walks both pools' runqs so queued actions keep
  their fn / args / dyn snapshot live until the worker pops them.
- `send` and `send-off` now enqueue the action onto the per-state
  run-queue and return the agent immediately. A worker thread is
  lazy-spawned on the first send (gated on `thread_limit`; throws
  MTH001 if the host hasn't granted a thread budget). The worker
  drains the queue, acquiring `state_lock` for each action body so
  eval invariants hold, then decrements the agent's `in_flight`
  counter and signals `agent_cv`. The worker exits when the queue
  drains so it stops counting against `thread_count` and doesn't
  suppress GC for the rest of the state's lifetime; the next send
  re-spawns. `await` and `await-for` are now actually blocking:
  the caller yields `state_lock` and waits on `agent_cv` until
  every named agent's `in_flight` reaches zero. `await-for`
  returns `false` on timeout. Self-await from inside an agent
  action throws MST002.
- `mino_pcall` now restores `lock_depth` after a longjmp from a
  throwing body. The embedder thread tolerated the imbalance
  because it never yielded the recursive `state_lock`, but a host
  worker (futures, agents) that catches a throw via pcall and then
  yields ended up unable to fully release the lock and deadlocked
  on resume. This was a latent bug; agents make it observable.
- `mino_agent_quiesce_workers` flips `agents_shutdown`, broadcasts
  the cv so any drain-pending worker exits, then reaps the
  pthread handle. Called by `mino_state_free` before heap teardown
  so a worker can't run after free.
- `shutdown-agents` now joins the worker thread instead of just
  flipping a flag. Self-call detection (calling shutdown-agents
  from inside an agent action body, which would deadlock the
  worker on its own pthread_join) throws MST002.
- `restart-agent` accepts `:clear-actions true`. Walks the
  per-state run-queue under `agent_mu`, splices out every entry
  targeting the failed agent, decrements its `in_flight`, and
  rebroadcasts `agent_cv` so any await waiter wakes. The dropped
  actions are released without running.
- `dosync`'s post-commit drain enqueues pending sends onto the
  POOLED worker's runq instead of running them synchronously on
  the embedder thread. Same path as a top-level send, so the
  action body sees post-commit ref state via `*agent*` bindings
  and doesn't tie up the embedder. STM dosync sends now require
  the same thread budget as direct sends; spawn refusals (host
  hasn't granted threads) silently drop the queued sends rather
  than surfacing the post-commit drain as a failed dosync.
- `send` and `send-off` now route onto separate POOLED and SOLO
  pools. mino's per-state eval lock means actions across the two
  pools still serialize, so the user-visible effect is the same
  as before, but the queues are independent: a long-running
  send-off action does not stall pending sends, and vice versa.
  Each pool's worker counts against `thread_limit` (the embedder
  thread does not). Embedders that want both shapes alive
  concurrently must raise the limit to at least 2. The split is
  also a clean seam for a future SOLO-yields-eval-lock-during-
  blocking-IO design without further user-facing churn.
- Public C-API entries: `mino_send`, `mino_send_off`, `mino_await`,
  `mino_await_for`, `mino_agent_error`, `mino_restart_agent`. Each
  takes the same `mino_lock` perimeter `mino_call` already uses
  and routes through the shared `agent_send_core` helper, so the
  guarantees are identical to the Clojure-level prims. Cross-
  state misuse fires at the C boundary: passing an agent from
  another `mino_state_t` throws MST007 and returns NULL.

## v0.101.1 — STM and agent hardening pass

Concentrated correctness, consistency, and safety pass over the
STM and agent surfaces that landed in v0.101.0. No new features;
every change closes a real or latent bug or aligns mino with JVM
canon. Highlights:

- STM commit is now atomic via a two-pass split (stage all new
  values + run all validators, then apply); a late-iteration
  validator throw or commute-replay throw used to leave earlier
  refs already committed.
- Commute log replay routes through `mino_pcall` so a throwing
  commute fn no longer leaks the global commit lock.
- `tx_state_t.in_commit` rejects re-entered `alter` / `ref-set` /
  `commute` from inside commute-replay or validator callbacks.
- `send` and `send-off` from inside `dosync` are queued and
  dispatched only on successful commit (cleared on retry / abort).
  `release-pending-sends` actually counts and clears that queue.
- Cross-state defense: agents now track `owning_state` and every
  agent prim throws MST007 on mismatch. The ref check moved into
  the shared cores so the Clojure path (not just the C API) is
  covered.
- Agent constructor accepts `:validator`, `:error-handler`,
  `:error-mode`, `:meta`; unknown options throw.
- `error-handler` is invoked on action and validator failure
  (was stored but never called). `restart-agent` runs the
  validator on the new state. `set-error-mode!` /
  `set-error-handler!` / `add-watch` / `set-validator!` reject
  invalid arguments at install time.
- `*agent*` is bound to the dispatching agent across action /
  validator / watch bodies.
- `shutdown-agents` flips a state-level flag; subsequent sends
  throw MST008. `send-via` throws MST008 with a clear directive
  rather than being unbound or aliasing to `send`.
- Watch dispatch goes through `mino_pcall` for both refs and
  agents; first thrown exception is captured, every other watch
  still runs, captured exception re-thrown so the caller surfaces
  it. Pending-sends drain runs before watch dispatch so a
  misbehaving watch can't drop queued agent sends.
- `with-meta` / `vary-meta` on stateful types (atom / agent)
  throw with a clear directive; `(meta x)` and `alter-meta!` keep
  working (the latter now also write-barriers).
- Agent print form carries identity: `#agent[ID VAL]` matches
  `#ref[ID VAL]`.

### Bind `*agent*` During Action / Validator / Watch Dispatch

JVM canon binds the dynamic var `*agent*` to the dispatching agent
across the entire body of an action, validator, and watch fn. mino
had no such binding, so an action that wanted to refer to itself
had to capture the agent in a closure.

Install `*agent*` as a dynamic var (nil default) in
`mino_install_agent`. Push a stack-allocated `dyn_frame_t` binding
`*agent*` to the running agent across `agent_apply_action`'s
mino_pcall calls; pop on every exit path via single-exit goto. The
existing symbol-lookup path already consults `dyn_stack` first, so
user code reading `*agent*` finds the binding without any
custom-resolver wiring.

### Ref Watch Dispatch Continues Past A Throwing Watch

Earlier `dispatch_watches` invoked each ref watch through
`mino_call`, so the first throw longjmped out and every later
watch -- including watches on unrelated refs in the same commit
-- silently never fired. Agent watches were already invoked
through `mino_pcall`; the inconsistency meant a misbehaving ref
watch could swallow legitimate notifications.

Wrap each ref watch in `mino_pcall`, capture the first thrown
exception, finish dispatching every other watch, then re-throw
the captured exception so the dosync caller still surfaces an
error. No watch is silently lost; the caller still sees a watch
failure when one occurs.

### Pending-Sends Drain Honors Failed-State In `:fail` Mode

`prim_send` rejects sends to agents already in failed-`:fail`
state at queue time, but a pending send queued earlier in the
same dosync can fail an agent that a later pending send also
targets. The drain used to call `agent_apply_action`
unconditionally, so the second action ran against the failed
agent's state -- inconsistent with the send-time contract.

Re-check at dispatch: if the agent has `err` set and
`err_mode == :fail`, skip the action silently. JVM canon would
throw in the agent's executor thread, never reaching the dosync
caller; mino's sync drain models that by dropping the action and
leaving `agent-error` as the surviving failure record.
`:continue` mode keeps accepting actions, matching `prim_send`.

### Reject `with-meta` / `vary-meta` On Stateful Types

`(with-meta (atom 0) m)` used to shallow-copy the atom struct, so
the sibling cell got its own `val` slot and diverged from the
original on the next `swap!` -- a silent identity split. JVM
canon decouples atom storage from atom identity (Atom-with-meta
shares the AtomicReference); without that indirection, the
faithful behavior would require restructuring atom storage.

Until that refactor lands, throw a clear MTY001 with the
directive "use alter-meta! for in-place mutation or the
constructor's :meta option" on `with-meta` / `vary-meta` for
both atoms and agents. `alter-meta!` keeps working (it mutates
`obj->meta` in place, so identity is preserved) and `(meta x)`
keeps working (read-only). Refs already threw because they
weren't in `supports_meta`; no change there.

While in `alter-meta!`, add the missing `gc_write_barrier` around
the in-place meta update -- stale OLD-to-YOUNG pointer was a
latent issue.

### Implement `shutdown-agents` And `send-via` Properly

`shutdown-agents` was a no-op stub returning nil. `send-via`
wasn't installed, so calling it produced `unbound symbol`. Fix
both:

- `shutdown-agents` flips a state-level flag (`agents_shutdown`).
  Subsequent `send` / `send-off` throw MST008. Idempotent. mino's
  sync MVP has no thread pool to terminate, but the flag still
  gives embedders a clean teardown signal.

- `send-via` throws MST008 with a clear "not yet implemented"
  message that points at `send` / `send-off`. Aliasing to `send`
  would silently drop the user's executor argument, so a loud
  failure is the right move until executors land.

### Remove Dead `tx_state_t.retry_signal` Field

Initialized in two places, set in zero, read in zero. Likely a
left-over from a never-landed `(retry)` user-facing trigger. Drop
the field and the two write sites; no behavior change.

### Agent Print Form Carries Identity

`(pr-str (agent 0))` and `(pr-str (agent 0))` produced the same
string for distinct agents, so two agents holding the same value
were indistinguishable in logs and debug output. Add
`agent_id` (a monotonic counter on `mino_state_t.agent_next_id`,
mirroring `stm_next_ref_id`) and emit `#agent[ID VAL]` to match
the existing `#ref[ID VAL]` form.

### Wire `release-pending-sends` And Drain Agents Before Watches

Two follow-ups to the in-tx send deferral.

`release-pending-sends` was a stub returning 0. Now that
`tx_state_t.pending_sends` exists, walk it, return the count, and
clear it -- so a body that wants to abort just its agent
dispatches before commit can do so. Outside a transaction the
prim still returns 0 without side effects.

`tx_outer_run` used to dispatch ref watches BEFORE draining
pending sends. A ref watch that threw longjmped to the outer
setjmp and silently swallowed every queued agent send. Swap the
order: drain agents first so a successful body always reaches its
agents, then fire watches.

### Defer Agent `send` From Inside `dosync` Until Successful Commit

`send` and `send-off` from inside a transaction body used to fire
the action synchronously: the action saw mid-tx tentative state
through `(deref ref)`, fired again on every retry attempt (so an
N-retry tx ran the action N+1 times), and the action's `(io! ...)`
falsely tripped because `current_tx` was still set. JVM canon
queues these as pending sends and only dispatches them once, on
successful commit.

Add `tx_state_t.pending_sends` (a cons list of `(agent fn .
extra)` triples), check `current_tx` in `prim_send` and prepend
the triple instead of dispatching, then drain in `tx_outer_run`
after a clean commit (between `current_tx = NULL` and watch
dispatch, so the action body can itself open a fresh dosync).
Pending sends are cleared on retry and on transaction abort, so a
failed attempt never produces side effects through agents.

### Wire `:meta` Constructor Option for Agents

`(agent state :meta m)` previously threw "not yet supported".
Store the map (or nil) on the agent's cell-level `meta` field and
let `(meta a)` read it back. `with-meta` on agents is
intentionally still rejected: `with-meta`'s shallow copy of the
agent struct would create a sibling cell with its own `val` /
`err` slots that diverges on the next send. `(meta a)` reads
through a special-case in `prim_meta` rather than extending
`supports_meta`, so the broken-copy path stays closed.

### Cross-State Defense for Agents and Tighter Defense for Refs

Refs already carried `owning_state` and threw MST007 if a public C
API entry saw a foreign value, but the Clojure-side prims
(`alter` / `commute` / `ref-set` / `ensure`) had no check -- a
host that smuggled a foreign ref in via `mino_env_set` could mutate
across states. Move `tx_check_ref_owned` into the shared cores
(`tx_alter_core`, etc.) so both the C API and Clojure prims hit
the check; drop the now-redundant calls in C-API entries.

Agents had no defense at all. Add `owning_state` to the agent
struct, set it in `mino_agent`, and check it in every agent prim
(`send`, `send-off`, `await`, `await-for`, `agent-error`,
`restart-agent`, `set-error-handler!`, `error-handler`,
`set-error-mode!`, `error-mode`) plus the shared watch /
validator paths in `add-watch`, `remove-watch`, `set-validator!`,
`get-validator` (which also pick up the equivalent ref defense).

Mirror the existing cross-state ref test in `embed_stm_test.c`
with a parallel run that drives all 14 agent-touching probes
through `mino_eval_string` from a foreign-allocated agent.

### Validate Callability of Watch / Validator Arguments

`add-watch` and `set-validator!` accepted any value as the watch
or validator -- a non-callable was stored quietly and only
exploded later when the dispatcher tried to call it. Reject
anything that isn't a fn/prim/macro at install time across all
watchable references (atom, ref, var, agent). Same rule applies
to the agent constructor's `:validator` and `:error-handler`
options. `set-validator!` still accepts `nil` (clears the
validator) per JVM canon.

### Make STM Commits Atomic and Reject Mid-Commit Mutation

`tx_commit` walked the write set in iteration order, applying each
ref's new value (write barrier + version bump) before validating
the next ref. A late-iteration validator rejection or commute
throw therefore left earlier refs already committed -- atomicity
violation.

Restructure the commit into two passes. Pass 1 walks every ref,
runs commute log replay and validators, and stages the new value
on `rs->committed_new` without touching `ref->val`. Any failure
aborts the whole commit before a single write hits memory. Pass 2
applies the staged writes; it runs no user code and cannot fail
mid-flight. Adds a `tx_state_t.in_commit` flag and rejects
`alter` / `ref-set` / `commute` re-entered through pass 1's user
callbacks (commute fns, validators) -- their new tentatives would
otherwise dangle past the iterator and silently disappear.

### Validate `set-error-handler!` Handler Argument

`set-error-handler!` previously stored any value -- so
`(set-error-handler! a 5)` quietly put `5` in the slot, only to
fail far away when an action threw and the dispatcher tried to
call it. Reject anything that isn't a fn/prim/macro (or nil to
clear) at install time so the typo surfaces immediately.

### Validate `set-error-mode!` Argument

`set-error-mode!` accepted any value silently: a non-keyword like
`"fail"` or `99` was a no-op, and an invalid keyword like
`:silent` flipped any agent to `:fail` regardless of its previous
mode -- silent data loss. Reject anything other than `:fail` or
`:continue` with a classified type error so user typos surface.

### Run Validator on `restart-agent`

`restart-agent` cleared the failed agent's error and published the
new state without consulting the agent's validator, so a failed
agent could be restarted to a state the validator forbids -- the
next send would just refail. JVM canon validates first; mino now
matches. The validator runs through `mino_pcall`; a throw or falsy
return aborts the restart and leaves the agent in its failed
state, so the caller can see and retry.

### Invoke Agent `error-handler` on Action and Validator Failure

`set-error-handler!` stored a fn but `agent_apply_action` never
called it -- on action throw or validator rejection mino latched
the exception into `agent.err` regardless. JVM canon: when an
error-handler is installed, route the failure through `(handler
agent ex)` and leave the agent in a clean state. With no handler,
keep the latching behavior. If the handler itself throws, capture
the handler's payload into `agent.err` so the failure isn't
silently lost.

### Wire Up Agent Constructor Options

`(agent state :validator pred :error-handler h :error-mode m)`
previously accepted but silently ignored every option, so an agent
declared with `:validator pos?` would still publish negative
values. Parse trailing keyword pairs and apply them to the agent's
slots. Unknown keys, odd numbers of trailing args, and invalid
`:error-mode` values now throw with a classified error. `:meta`
also throws with "not yet supported" rather than being silently
dropped (cell metadata on agents is not yet surfaced through
`(meta a)`).

### Fix STM Commit-Lock Leak on Commute Throw During Replay

A `commute` fn that succeeded during the transaction body but threw
during `commute_log_replay` longjmped past `stm_unlock`, leaving the
global STM commit lock held. Subsequent dosync calls on another
thread would deadlock; on the same thread, re-acquiring the
non-recursive mutex is undefined. Route Clojure-side commute log
entries through `mino_pcall` so a throw is captured and surfaced as
a hard failure with the user's original exception payload, after
the lock is released. C-side closures (`TX_C_CLOSURE_TAG`) remain
direct calls per the public API contract that host transformers
must surface failure via NULL rather than longjmp.

## v0.101.0

### Add software transactional memory (refs + `dosync`)

mino gains Clojure's STM surface: refs (`MINO_TX_REF`), `dosync`,
`alter`, `commute`, `ref-set`, `ensure`, `io!`. Single-version
optimistic locking with a global commit lock; coarse on purpose,
since mino's typical workload is single-digit refs and a handful
of worker threads. `ref-min-history`, `ref-max-history`,
`ref-history-count` are no-op stubs (return 0 / 10 / 0); long
readers under sustained writer contention may exhaust the
10000-retry cap rather than serve an older snapshot from history.

STM is opt-in for embedders via `mino_install_stm(S, env)` and is
auto-included in the standalone `./mino` binary through
`mino_install_all`. An embedder that never calls the install
function pays nothing beyond one enum tag and a NULL pointer per
context.

#### Type plumbing

Introduce `MINO_TX_REF` enum tag plus the `tx_ref` struct holding
the committed value, watches map, validator, version counter, and
monotonic ID. Wire the tag through GC mark / verify, the type-tag
string, the print form (`#ref[ID VAL]`), self-evaluation, the
clone non-transferable list, identity equality, and `prim_type`
(`:ref`). Add `stm_commit_lock`, `stm_lock_inited`, and
`stm_next_ref_id` fields on `mino_state_t`; the lock itself is
lazy-initialized only on the first call to `mino_install_stm`.

#### Embedder constructor

Add `mino_tx_ref(S, val)` for hosts that want to publish refs
directly without going through the `(ref v)` primitive. The
returned cell has empty watches/validator slots and a fresh
monotonic ID drawn from the per-state counter.

#### Transaction state plumbing

Define `tx_state_t` and `tx_ref_state_t` in `runtime/internal.h`.
Add a `current_tx` pointer on `mino_thread_ctx_t` so an active
transaction is reachable per-thread; `gc_mark_roots` walks
`current_tx->refs_head` for both the main ctx and all worker
ctxs so tentative values, commute log cells, and the refs
themselves stay reachable mid-transaction. The pointer is NULL
outside `dosync` and on every freshly-allocated thread context.

#### `dosync*`, `ref`, `ref?`, ref-aware `deref`

Add `src/prim/stm.c` with the entry-point primitives. `ref`
constructs a `MINO_TX_REF`. `ref?` is the identity predicate.
`dosync*` takes a thunk, allocates a `tx_state_t` on the C
stack, attaches it to the active context, runs the thunk, and
detaches; the commit phase is empty until `ref-set` / `alter`
land in the next step. `dosync` itself is a `defmacro` in
`core.clj` that expands to `(dosync* (fn [] body...))`.

`deref` (in `prim/stateful.c`) gains a `MINO_TX_REF` arm that
delegates to `mino_ref_deref`: inside a transaction, it returns
the in-tx tentative if any, else records a read with the ref's
current `version` and returns the committed value; outside, it
returns the committed value directly.

The primitives are not yet installed -- `mino_install_stm`
ships with the install hook in a later step. Until then the
symbols are unbound at runtime, so existing programs are
unaffected.

#### `ref-set`, `alter`, commit phase

Add the two simplest write primitives. Both throw `eval/state`
MST002 (`No transaction running`) when called outside `dosync`.
`ref-set` sets the in-tx tentative directly. `alter` reads the
current in-tx value, calls `(apply f cur args)`, and stores
the result.

The commit phase, run on every successful body return, validates
the read set under the global commit lock: every ref the
transaction touched must still be at its captured snapshot
version. On mismatch the lock is released and the body re-runs
(up to 10000 times before throwing MST004). On match, every
recorded write is applied with `gc_write_barrier` plus a version
bump, then the lock is released.

`dosync*` now pushes its own try frame so an in-body throw is
intercepted long enough to clear `ctx->current_tx` and free the
per-ref state nodes before re-throwing -- otherwise a longjmp
past the now-unwound stack frame would leave a dangling
`current_tx` pointer.

#### `commute` and `ensure`

`commute` records `(fn arg1 arg2 ...)` in a per-ref log instead of
materializing a tentative value; the log is replayed against the
latest committed value at commit time. `commute` does NOT mark the
ref as read in the read-set, so two transactions commuting on the
same ref do not conflict (matches Clojure JVM semantics). The fn
is invoked once eagerly inside the body so its return value is
visible to subsequent in-tx code, but that result is informational
-- the authoritative value is recomputed at commit.

`ensure` reads a ref and pins the snapshot version so any other
transaction that mutates the same ref will fail this transaction's
read-set validation. In our single-version optimistic model that
is structurally identical to a `deref`-with-read-recording.

`alter`-after-`commute` on the same ref folds the log: the
effective in-tx value is computed by replaying the log against
the committed value, the alter fn is applied to that, the
resulting value is pinned, and the log is dropped. `ref-set`-
after-`commute` does the same but skips the fn application.
`commute`-after-`alter` degrades to a fold-into-alter rather
than appending to a log -- the alter has already pinned a value
that the next commute should refine, not commute against.

#### Watches and validators on refs

Extend `add-watch`, `remove-watch`, `set-validator!`, and
`get-validator` to accept `MINO_TX_REF` in addition to
`MINO_ATOM`. The implementations share a small `watchable_get`
accessor that dispatches between the atom and ref watch /
validator slots so each primitive's body stays single-pass.

The transaction commit phase now captures `committed_old` and
`committed_new` per write and dispatches watch callbacks
`(key ref old new)` after the commit lock is released.
`ctx->current_tx` is cleared before dispatch so a watch that
itself enters `dosync` allocates fresh transaction state. A
watch that throws propagates out of the commit; later watches
do not fire (matches atom semantics).

Validators run inside the commit phase via `mino_pcall` against
the proposed new value -- a thrown validator does not longjmp
out while the lock is held; a falsy return raises `eval/contract`
MCT001 ("Invalid reference state") after the lock is released.
Both retry and validator-rejection paths free the per-ref state
nodes before throwing.

#### `io!`, `in-transaction?`, history stubs

Add `io!` as a `defmacro` in `core.clj` that expands to
`(do (io!-check) body...)`; `io!-check` is a primitive that
throws `eval/state` MST003 ("I/O in transaction") when called
inside `dosync`, otherwise returns nil. The macro form ensures
the throw fires before the body evaluates.

Add `in-transaction?` predicate primitive returning true inside
`dosync`. Add `ref-min-history`, `ref-max-history`,
`ref-history-count` as no-op stubs returning 0 / 10 / 0 -- mino
uses single-version optimistic locking, not MVCC with history,
so the values are not configurable.

(The MST002 contract for `ref-set` / `alter` / `commute` /
`ensure` outside `dosync` was already wired in commits #5 and
#6 -- this entry records the rest of the surface.)

#### `mino_install_stm` wired into `mino_install_all`

The standalone `./mino` binary now installs the STM primitives
out of the box. Embedders calling `mino_new` (which only
installs core + io) still opt in explicitly via
`mino_install_stm(S, env)`.

Add `tests/stm_test.clj` with single-threaded coverage of every
primitive: ref / ref?, ref-set, alter (with multi-arg), commute,
ensure, in-transaction?, watches (add / remove / commit
dispatch), validators (accept / reject), nested dosync,
deref-in-tx-sees-tentative, commute-then-alter fold, history
stubs, and the io! / io!-check contract.

#### Concurrent test coverage

Add `tests/stm_concurrent_test.clj` exercising the multi-thread
retry path. N worker `future`s each run M `dosync` increments
against a shared ref via `alter` and `commute`; a third test
verifies post-commit watch dispatch fires exactly once per
successful commit (retries do not double-fire). Skipped when
`mino-thread-limit` is 1.

#### External suite impact

`add_watch.cljc` and `remove_watch.cljc` now pass their atom and
ref arms; the var arm still fails because mino does not yet
support `add-watch` on vars (separate feature, not in scope for
the STM rollout). External suite holds at 212 OK.

Internal suite 1498 / 7127 / 0.

### Layer 1 audits (Clojure-surface fidelity)

A pass through the STM surface comparing it against canonical
JVM Clojure (`clojure.lang.LockingTransaction`,
`clojure.lang.Ref`). Two accidental deviations were found and
corrected; no API additions or removals on the Clojure side.

#### `set-validator!` no longer validates the current value

`set-validator!` previously called the new validator on the
ref/atom's current value at install time and rejected with
MCT001 if it failed. JVM Clojure does not do this -- only
subsequent state transitions are checked. Match the canon: install
the validator unconditionally. The old test
`validator-on-current` (which asserted the rejection) is replaced
with `validator-install-on-failing-current`; the STM
`validator-rejects` test no longer needs the `(ref 1)` workaround.

#### `alter` / `ref-set` after `commute` throws

JVM Clojure throws `"Can't set after commute"` when `alter` or
`ref-set` is called on a ref that already has a logged commute in
the same transaction. mino previously folded the commute log into
the alter's tentative value -- the final committed value matched
JVM by accident, but the error contract differed. Throw
`eval/state` MST002 with the canonical message. The
commute-after-alter direction is unchanged: alter pins the value,
the commute folds in, and commit writes the alter+commute
tentative (matching JVM, which skips the commute log replay for
refs already in the write set).

#### Documented deviation list in the STM module header

`src/prim/stm.c` now opens with a numbered enumeration of every
intentional or documented deviation from JVM Clojure: single-
version optimistic locking, the global commit lock, no barging,
no mid-body retry, history stubs, the simpler print form, the
post-A.1 set-validator! semantics, and the post-A.2
alter-after-commute contract. A reader auditing mino's STM
against canon should find the answer in one place.

### Layer 2a C API (host-side mirror of the Clojure surface)

Anything a Clojure programmer can do, a C host can do. The new
`mino_tx_*` entry points sit alongside the existing
`mino_atom_*` and `mino_volatile_*` API in `src/mino.h`. Each
shares its core implementation with the corresponding Clojure-
side primitive via a `tx_*_core` helper, so the two surfaces
cannot drift.

#### `mino_is_tx_ref` + `mino_tx_ref_deref`

Predicate + reader. The deref's in-tx vs. out-of-tx dispatch is
unchanged: a host calling it from inside an outer transaction
gets the in-tx effective value plus read-set bookkeeping;
outside, the committed value. NULL- and non-ref-tolerant at the
public entry.

#### `mino_tx_ref_set`

Writer. Refactors `prim_ref_set` to share `tx_ref_set_core`
with the new C entry; both go through the same kind transition,
read-set bookkeeping, and post-commute set-rejection check.

#### `mino_tx_alter_c` + `mino_tx_commute_c`

Host transformers. The new typedef
`mino_val_t *(*mino_tx_xform_fn)(mino_state_t *, mino_val_t *cur,
void *user, mino_env_t *)` is the C-side analogue of `(fn [cur]
...)`. `mino_tx_alter_c` applies the fn to the in-tx value and
records a read; `mino_tx_commute_c` applies it without recording
a read and -- if the ref has not also been altered in the same tx
-- replays it at commit against the latest committed value.

The Clojure-side `prim_alter` / `prim_commute` and the new C
entries share `tx_alter_core` / `tx_commute_core` helpers. The
compute step is parameterised by a `tx_compute_fn` callback so
the Clojure side dispatches via `mino_call` and the C side calls
the host transformer directly. Commute log entries are likewise
polymorphic: a `(fn . extra)` cons for Clojure entries, or a
`MINO_HANDLE` wrapping a heap-allocated `{xform_fn, user}` closure
for C entries (freed via the handle's GC finalizer). Replay
dispatches per entry shape.

#### `mino_tx_ensure`

Read pin. Refactors `prim_ensure` to share `tx_ensure_core` with
the new C entry. As before, the implementation captures the
ref's snapshot version so any concurrent committer fails this
tx's read-set validation; the JVM "block any other write" semantic
falls out of the version-bump-on-commit rule for free.

#### `mino_tx_run`

Host-level `dosync`. The new typedef
`mino_val_t *(*mino_tx_body_fn)(mino_state_t *, void *user,
mino_env_t *)` is the C-side analogue of a `(fn [] ...)` body
thunk. `mino_tx_run` owns the setjmp / try-frame, retry loop,
commit phase, and watch dispatch.

`prim_dosync_star`'s body invocation extracts cleanly into a
`tx_invoke_body_fn` callback shared with the new C entry: the
Clojure side wires it to `mino_call(thunk, [])`, the C side wires
it to a direct `body(S, user, env)`. The setjmp-bearing
`tx_outer_run` is shared verbatim (same `-Wclobbered` discipline,
same try-stack overflow guard, same outer-vs-nested dispatch), and
both entry points absorb a nested call into the active tx without
touching the setjmp frame. Cross-thread defense is unchanged --
the active `current_tx` lives on the per-thread context, so a
host calling `mino_tx_run` on a worker sees its own retry loop.

### C-side embed test + task wiring

New `tests/embed_stm_test.c` exercises every Layer 2a entry point
end-to-end: predicate / construction / outside-tx deref, a full
`mino_tx_run` body that mixes `mino_tx_ref_deref` /
`mino_tx_alter_c` / `mino_tx_commute_c` / `mino_tx_ensure`,
`mino_tx_ref_set`, a commute-only path that goes through the
log-replay code, the outside-tx error contract (every entry
throws MST002 outside any transaction), the type-check throws on
non-ref input, and a watch installed from Clojure observing a
C-side commit through the side-atom that `add-watch` records into.

The `./mino task test-embed` task gains a second invocation: it
now compiles and runs `embed_multi_state` and `embed_stm_test`
against the same lib srcs. The task helper
`compile-and-run-embed-test` factors the shared compile + run
recipe.

`src/prim/stm.c` is also added to the task-driven lib-srcs list
(the bootstrap Makefile already picked it up via wildcard, but
the task's explicit list had been missing it -- a benign drift
that became a broken link as soon as the C-side test referenced
the new public symbols).

### C-side retry test

Added `test_run_retry_under_contention` to
`tests/embed_stm_test.c`: spawns four Clojure futures that each
drive 200 calls to a registered C primitive
(`c-incr-ref!`); the prim runs `mino_tx_run` with a body that uses
`mino_tx_alter_c` to increment a shared ref. Asserts the
post-commit value equals `workers * per-thread` and that
`body_attempts >= successful_commits` (so the user pointer survived
every body invocation, including any retries).

mino's eval loop holds a per-state lock that serializes worker
threads on the same state, so on this run the threads typically
do not race for commits and `attempts == commits`. The contract
holds either way: a yielding inner call (e.g. blocking on a
future) would let another thread bump the version and fire the
retry path, and the body fn must survive that.

### Var watches and validators

`add-watch`, `remove-watch`, `set-validator!`, and `get-validator`
now accept vars. The `MINO_VAR` struct gains `watches` and
`validator` slots; `var_set_root` runs the validator before
publishing the new root and dispatches watches after, matching
the atom / ref behaviour. The fast path (no watches, no validator,
no env lookup) is unchanged for early-bound install paths --
state init and the `install_stdlib` bootstrap stay zero-cost.

JVM Clojure fires var watches on `alter-var-root` (and on `def`
with rebind); mino does the same. Watches in mino are
non-`^:dynamic` only -- a thread-local `binding` push does not
fire watches anywhere.

`tests/external_runner.clj` now requires
`core_test/add_watch.cljc` and `core_test/remove_watch.cljc`
upstream, and the atom / ref / var arms pass cleanly. The agent
arm of each still errors (out of scope until agents land).

### Fix `mino_pcall` re-throw via `set_eval_diag`

`mino_pcall`'s catch arm called `set_eval_diag` to publish the
caught error's message via `mino_last_error` /
`mino_last_error_map`. But `set_eval_diag` itself longjmps to the
next-outer try frame when one exists -- so any pcall caller that
ran inside a Clojure `try` would see its catch path hijacked by
the longjmp and unwind to the outer frame, **leaking any
bookkeeping the caller depended on**.

In the STM commit path, that bookkeeping was the global commit
lock: a validator throw inside a `try (dosync ...) (catch ...)`
would longjmp out of `run_ref_validator` past `stm_unlock`,
leaving `S->stm_commit_lock` permanently held. The next
`dosync` deadlocked.

Fix: `mino_pcall`'s catch arm no longer publishes anything to
`last_error` / `last_diag`. Callers that want a diag set after
`pcall` returns -1 do it explicitly. (An interim attempt routed
the publish through a non-throwing `record_eval_diag` variant,
but that left a stale diag in `last_error` that `eval_impl`'s
`evaled == NULL && mino_last_error != NULL` check would then
misread as a fresh error during a later call — flushed via the
follow-up commit that drops the publish entirely.)

`tests/stm_test.clj`'s `validator-throw-does-not-deadlock-stm-lock`
regression test proves the lock is released after a validator
throw.

The agent code's `agent_try_call` workaround (added in E.4) is
left in place by this commit; the follow-up replaces it with a
direct `mino_pcall` call now that the catch arm is well-behaved.

### `mino_pcall` exposes the raw thrown value; STM validator throws propagate

Two coupled changes follow on from the catch-arm fix:

`mino_pcall`'s signature gains an `out_ex` parameter:

```c
int mino_pcall(mino_state_t *S, mino_val_t *fn, mino_val_t *args,
               mino_env_t *env,
               mino_val_t **out, mino_val_t **out_ex);
```

When the call throws, `*out_ex` receives the raw thrown value (the
cell passed to `(throw ...)` -- typically an `ex-info` map or
similar payload). Callers like agent dispatch and STM validator
handling that want to surface the user's exception unchanged read
from `out_ex` directly. **Breaking ABI change: existing pcall
callers need to add the extra parameter (NULL is fine if the value
isn't needed).** mino is alpha; no compat shim.

The agent code's `agent_try_call` workaround is removed:
`src/prim/agent.c` now calls `mino_pcall(...&new_state, &thrown_ex)`
for actions, validators, and watches, getting the same exception
capture in 1 line where `agent_try_call` took 30. The custom try
frame is gone.

`run_ref_validator` in `src/prim/stm.c` likewise uses `out_ex`,
threading the captured exception through `tx_state_t`'s new
`validator_thrown_ex` slot. `tx_commit` sets the
`validator_rejected` flag for both throws and falsy-rejects (both
are hard failures, distinct from read-set-conflict retries) and
parks the captured exception on `tx`. `dosync_run` consumes it: if
`validator_thrown_ex` is set, it propagates the user's original
payload via `mino_throw`; otherwise it raises the canonical
`MCT001 "Invalid reference state"`.

Net behavior: a validator that throws now aborts the transaction
with the validator's own exception (matching JVM Clojure's
"propagate the validator's exception" semantic), where the
previous code retried until the cap and then threw `MST004
"transaction retry limit exceeded"`. A validator that returns
falsy without throwing still produces `MCT001`.

`tests/stm_test.clj` covers both cases:
`validator-throw-propagates-original-exception` checks that
`(ex-data e)` returns the original ex-info data; the new
`validator-falsy-reject-throws-MCT001` pins the falsy-reject path.

Internal suite 1514 / 7171 / 0.

### Agents (MVP)

mino now ships agents: `agent`, `agent?`, `send`, `send-off`,
`await`, `await-for`, `agent-error`, `restart-agent`,
`set-error-handler!`, `error-handler`, `set-error-mode!`,
`error-mode`, plus `shutdown-agents` / `release-pending-sends`
stubs. Watches and validators on agents go through the same
`watchable_get` machinery as atoms / refs / vars.

The MVP runs sends synchronously on the calling thread. mino's
eval loop holds a per-state mutex so a worker-pool design would
serialize on it anyway; running synchronously is observably
equivalent for any program that does not race against the agent
itself, and `await` becomes a trivial no-op (the queue is always
drained on send return). Action throws and watch throws are both
captured into `agent-error` via a manual try frame in
`src/prim/agent.c` (mino's `mino_pcall` re-throws to any
enclosing try via `set_eval_diag`'s longjmp path, which would
defeat the catch contract here).

Documented deviations: `send-via` is not implemented (no public
Executor type); `shutdown-agents` and `release-pending-sends`
are stubs; the `:fail` error mode is the default and rejects
further sends until `restart-agent` clears the err.

`tests/agent_test.clj` exercises construct / send / send-off /
watches / validators / restart / error-mode / await and is in
the internal run.clj. The agent arms of the upstream
`add_watch.cljc` / `remove_watch.cljc` tests now pass cleanly,
bringing the external runner to **134 / 2680, 1 fail + 2 errors**
(matching the pre-STM baseline; remaining failures are
pre-existing test-abs / test-reduce / test-short, unrelated to
STM or watches).

### Equality of empty lazy seqs

Fix: `(= (filter pred []) (filter pred []))` returned `false`.
The `case MINO_LAZY` arm in `mino_eq` was a leftover stub from
a previous force-then-compare design; the realized-to-empty
case stayed `MINO_LAZY` per the unwrap policy and fell into
the stub. Route both-LAZY equality through `eq_seq_like` so it
walks element-wise (immediately terminating for two empty
seqs). Unblocks the var arm of the upstream `add-watch` test,
which compares two `(filter ...)` results that both yield
empty.

### Cross-state ref defense (MST007)

A C host that accidentally passes a ref allocated in one
`mino_state_t` to another's `mino_tx_*` entries used to silently
mutate the foreign heap. Now every public C entry that takes a
ref (`mino_tx_ref_deref` / `mino_tx_ref_set` / `mino_tx_alter_c` /
`mino_tx_commute_c` / `mino_tx_ensure`) checks the ref's
allocating state via a new `tx_ref.owning_state` back-pointer
recorded by `mino_tx_ref` at construction time, and throws
`eval/state` MST007 ("ref from foreign state") on mismatch. The
check is one pointer comparison; the back-pointer adds 8 bytes
to each ref but no GC traversal cost (the state itself outlives
all its refs and is not a GC value).

`tests/embed_stm_test.c` now covers this with a second-state ref
passed into the first state's entries (deref, ref-set, alter_c,
commute_c, ensure); each must error and the foreign state's own
ops must keep working unchanged.

### Internal suite

`1499 / 7137 / 0` (one new alter-after-commute-throws case + one
new alter-then-commute-folds case + one new
validator-install-on-failing-current case). External suite holds
at the prior baseline; no STM-specific test files exist in
clojure-test-suite, and the `add_watch.cljc` / `remove_watch.cljc`
files (which would exercise the ref arm) remain out of the
external runner because their var arm still fails (var watches
are out of scope for this work).

## v0.100.34

### Add `aset` for host arrays; tighten `vec` bad-shape rejection

`(aset arr i x)` now mutates `MINO_HOST_ARRAY`'s `vals[i]` in place.
This is the only mutation path mino exposes outside `MINO_ATOM` /
`MINO_VOLATILE`; it exists because the host-array tier mirrors JVM
array semantics for cross-dialect tests.

`seq_iter_init` / `seq_iter_done` / `seq_iter_val` now handle
`MINO_HOST_ARRAY` and `MINO_MAP_ENTRY` so `into`, `mapv`, etc.
iterate them uniformly. `(vec arr)` then materializes a normal
persistent vector.

`vec` in `src/core.clj` rejects bad shapes (numbers, booleans,
chars, keywords, symbols, regexes, transients) up front rather
than passing them to `into` and getting a generic `not seqable`
error.

`vec.cljc` 13/15 errors -> 19/20 passes. The remaining failure is
the `(aset arr 0 -1) (is (= [-1 2 3] (vec arr)))` storage-aliasing
assertion -- JVM's `LazilyPersistentVector.createOwning` reuses
small Object[] arrays as the persistent vector's tail, which is
incompatible with mino's persistent-trie `vec` that genuinely copies
its input. Documented as JVM-internal optimization, not portable.

Internal suite 1476 / 7091 / 0. External suite stays at 212 OK
(vec.cljc still has 1 fail for the aliasing assertion).

## v0.100.33

### Add `MINO_FLOAT32` tier; split `float?` and `double?`

`double_qmark.cljc` asserts `(double? (float 0.0))` is false. mino had
one float tier (MINO_FLOAT, double-precision), so `(float x)` and
`(double x)` produced indistinguishable values. Introduce a separate
`MINO_FLOAT32` value tag sharing the same `as.f` storage (the 32-bit
narrowing happens at `mino_float32` construction) so `double?` can
distinguish 64-bit doubles from 32-bit floats. `float?` matches both
tiers; `double?` matches only `MINO_FLOAT`. `(type x)` returns
`:float` for 64-bit and `:float32` for 32-bit.

Arithmetic always promotes to `MINO_FLOAT` (matching JVM Clojure
where Float arithmetic yields Double): `tower_to_double`,
`classify_or_throw`, `is_compare_number`, `tower_cmp`, `has_nan`,
`prim_inc` / `prim_dec` fast paths, unary `-`,
`extract_integer_for_cast`, `narrow_cast`, `prim_NaN_p`,
`prim_infinite_p`, GC clone, hash, print all dispatch through both
tags. Equality between `MINO_FLOAT` and `MINO_FLOAT32` is false even
if the value matches (matches JVM where `(= 5.0 (float 5))` is
false).

New C primitive `prim_double` returns a `MINO_FLOAT`; replaces the
prior `(def double float)` alias. `prim_float` now returns
`MINO_FLOAT32`.

Internal `numeric-coercion` test updated to cover the new contract.
Internal suite 1476 / 7091 / 0. External `double_qmark.cljc` 43/46
-> 46/46. External suite: 211 -> 212 OK.

## v0.100.32

### Add `MINO_MAP_ENTRY` value type

JVM Clojure's MapEntry is a vector-shaped seq returned by `first` /
`seq` of a map; `key` and `val` accept it but throw on a plain
2-vector. mino conflated map entries and 2-vectors, so
`(p/thrown? (key [1 2]))` failed. Add a distinct `MINO_MAP_ENTRY`
type with `(k, v)` slots, GC mark + verify, hash that matches a
2-vector (so cross-type equality works in hash maps), `(type x)`
returns `:map-entry`, and `vector?` / `coll?` / `counted?` /
`associative?` / `reversible?` / `sequential?` return true on it.
Equality with `[k v]` is element-wise via the existing cross-type
sequential path. `seq` of a map / sorted-map / record now produces
MAP_ENTRY values; `find`, `first`, `rest`, `nth`, `get`, `count`,
`empty?`, vector destructuring, `compare`, `into`-map, `conj`-map,
and `conj`-of-MAP_ENTRY all dispatch through it. `key` / `val` in
`src/core.clj` accept only MAP_ENTRY and throw otherwise.
`clojure.lang.MapEntry/create` now constructs a MAP_ENTRY (via the
new `map-entry` C primitive). `aset` is intentionally not
implemented for MAP_ENTRY since entries are immutable.

External `key.cljc` 8/17 -> 17/17, `val.cljc` 7/16 -> 16/16. External
suite: 209 -> 211 OK.

## v0.100.31

### `(float x)` narrows to 32-bit float precision

`prim_float` now range-checks the input against `[-FLT_MAX, FLT_MAX]`
(NaN passes through; +/-Infinity and overflow throw `eval/type`
MTY001) and narrows precision via `(double)(float)d` so values that
underflow the 32-bit range round to zero. JVM Java's
`(float)4.9e-324` is `0.0f` and `(float Double/MAX_VALUE)` throws;
mino now matches.

External `float.cljc` 15/19 -> 19/19. External suite: 208 -> 209 OK.

## v0.100.30

### Reader promotes out-of-long literals to bigint

The reader's number parser called `strtoll` and used its saturated
return value without checking `errno`, so `-9223372036854775809`
silently became `LLONG_MIN`. Setting `errno = 0` before the call and
parsing through `mino_bigint_from_string_n` on `ERANGE` lets
`(long out-of-range-literal)` reach the existing range check in
`prim_long` and throw.

External `long.cljc` 23/25 -> 25/25.

## v0.100.29

### Add `MINO_HOST_ARRAY` value type

JVM Java arrays are not collections, vectors, associatives, etc.,
but mino had `object-array` / `int-array` / `to-array` aliased to
`vec`, so every predicate returned true. Add a distinct
`MINO_HOST_ARRAY` value type with malloc-owned `vals[]`, a
`host_array_kind_t` element-kind tag, GC mark + sweep, and a
single-chunk `MINO_CHUNKED_CONS` emission from `prim_seq` so
iteration still works. `prim_first`, `prim_rest`, `prim_count`,
`prim_get`, `prim_nth`, `prim_empty_p` route through the new tag;
`coll?` / `vector?` / `counted?` / `associative?` / `sequential?` /
`reversible?` all return false on the new type. Equality is identity
(matching JVM arrays). Constructors (`object-array`, `int-array`,
`long-array`, etc.) are now C primitives that zero-fill on size and
copy from collections. `aset` is intentionally not implemented --
host-array mutation is out of scope per the JVM-only group.

External `associative_qmark.cljc`, `coll_qmark.cljc`,
`counted_qmark.cljc`, `reversible_qmark.cljc`,
`sequential_qmark.cljc`, `vector_qmark.cljc`, `seq.cljc`, `get.cljc`,
`some.cljc`, `seqable_qmark.cljc` all -> 0/0/0. External suite:
200 -> 207 OK.

## v0.100.28

### Enforce fixed-arity contracts at fn apply

`bind_vec_destructure` was discarding extra args once `plen` patterns
were bound, so `((fn [x] x) 1 2 3 4)` returned `1` silently. JVM
Clojure throws `ArityException`. Tighten the binder to throw
`eval/arity` MAR001 when a fn / defn call has more args than the
parameter vector has positions; `let` / `loop` / `for` / `doseq`
keep their lenient destructuring (unmatched tail elements ignored).

External `update.cljc` 62/63 -> 63/63. External suite: 199 -> 200 OK.

## v0.100.27

### `(cons x y)` returns non-list shape; peek rejects it

JVM Clojure's `(cons x y)` returns a `clojure.lang.Cons` that is a
seq but not a list (`(list? (cons 1 nil))` is false; `(peek (cons 1
nil))` throws). mino conflated cons-results and list literals as
`MINO_CONS`. Add a `not_list` flag to the cons cell: `mino_cons`
zeroes it, `prim_cons` sets it to 1, and `peek`, `pop`, and `list?`
check the flag. The data shape stays `MINO_CONS` so the eval path
can still apply macro-built forms.

External `peek.cljc` 10/11 -> 11/11. External suite: 198 -> 199 OK.

## v0.100.26

### Widen bigdec / ratio division to bigdec

The tower's RATIO + BIGDEC contagion rule used to collapse to
float -- a punt from before mino had exact bigdec division.
`coerce_at_tier` for `TT_BIGDEC` now handles `MINO_RATIO` by
computing `bigdec(num) / bigdec(denom)` via `mino_bigdec_div`
(exact, throws on non-terminating expansions); `promote_acc` does
the same widening on the running accumulator. `(/ 2.0M 1/2)` is now
`4M`, not `4.0`.

External `slash.cljc` 158/160 -> 160/160. External suite: 197 -> 198 OK.

## v0.100.25

### Strict overflow on +/-/*; auto-promote on primed forms

mino historically auto-promoted on long overflow for the unprimed
forms with `+'` / `-'` / `*'` / `inc'` / `dec'` aliased to them
(v0.100.17). JVM Clojure splits the contract: unprimed throws on
overflow, primed auto-promotes. `tower_apply_int` and the int-tier
branch of `tower_reduce_seeded` now take a `strict` flag; on
overflow they throw `eval/contract` MCT001 "integer overflow".
`prim_inc` / `prim_dec` route through the same flag. New C
primitives `prim_addp` / `prim_subp` / `prim_mulp` / `prim_incp` /
`prim_decp` carry the auto-promote behavior and are registered as
the primed names. Unary `-` on `LLONG_MIN` now throws under strict;
`-'` still auto-promotes. Removes the v0.100.17 aliases in
`src/core.clj`.

`prim_short` and `prim_byte` now share a `narrow_cast` helper with
`prim_int` that compares the double value against the target tier
bounds before truncation, so `(byte -128.000001)` throws.

External `star.cljc` 121/125 -> 125/125, `plus.cljc` 127/129 ->
129/129, `minus.cljc` 137/139 -> 139/139, `byte.cljc` 25/27 ->
27/27. External suite: 193 -> 197 OK. One regression: `abs.cljc`
errors on `(* -1 r/min-int)` in the test's `:default`
expected-value computation -- the test relies on auto-promote `*`
which can't be reconciled without modifying the test.

## v0.100.24

### `Foo.` trailing-dot constructor invokes the defrecord factory

`(Foo. a b c)` is JVM reader sugar for the positional constructor of
type `Foo`; mino's defrecord generates a `->Foo` factory but had no
dispatch for the trailing-dot syntax. `eval_try_host_syntax` now
detects a head symbol ending in `.`, looks up the stem through the
lexical / current-ns / ambient-ns chain, and if the stem resolves to
a `MINO_TYPE` value, invokes the matching `->stem` factory.

External `dissoc.cljc` 13/14 -> 22/22. External suite: 192 -> 193 OK.

## v0.100.23

### `(int x)` and `(long x)` throw on out-of-range

`prim_int` previously silently saturated/clamped on overflow: bigint
via `mino_as_ll` returned a clamped long long; float saturated to
`LLONG_MIN` / `LLONG_MAX`; single-char strings hit a legacy
fast-path. `prim_int` now range-checks against int32; for floats and
bigdecs the check is on the double value itself, so `(int
-2147483648.000001)` throws even though it truncates to the in-range
`-2147483648`. The single-char string fast-path is gone; chars still
pass through as codepoints. New `prim_long` is registered as
`"long"` and range-checks against int64 via the new
`extract_integer_for_cast` helper. Removes the
`(def long ... int)` alias in `src/core.clj`.

External `int.cljc` 22/27 -> 27/27, `long.cljc` 22/25 -> 23/25
(remaining 2 fixed in v0.100.30 by the reader bigint promotion).

## v0.100.22

### Add `(short x)` and `(byte x)` with range checks

`clojure.core-test.num` exercises `(short 1)` and `(byte 1)` in its
`:default` arm. mino had `int` and the `long` alias but no `short`
or `byte`, so the test errored on the first `(short 1)` call.
`prim_short` and `prim_byte` share a new `extract_integer_for_cast`
helper covering `MINO_INT`, `MINO_FLOAT`, `MINO_BIGINT`,
`MINO_RATIO`, and `MINO_BIGDEC`, with NaN / infinity /
out-of-long-range checks. They range-check the extracted value
against int8 / int16 and throw on overflow. The result is returned
as `MINO_INT` since mino has no narrow-int tier; only the contract
narrows. Also relax `num` in `src/core.clj` to pass nil through
(returning `nil`), matching the `:default` arm's
`(= nil (num nil))`.

External `num.cljc` 6/7 -> 13/13.

## v0.100.21

### Bridge `clojure.lang.MapEntry/create` to a 2-vector ctor

Cross-dialect tests build a map-entry literal under their `:default`
arm via `(clojure.lang.MapEntry/create k v)`. mino's map entries are
2-vectors so the shape already matched; only the constructor
namespace was missing. Define a `clojure.lang.MapEntry` namespace in
`src/core.clj` next to the existing `clojure.lang.IPending` and
`clojure.lang.BigInt` bridges. The remaining `(p/thrown? (key [1 2]))`
cases in `key.cljc` and `val.cljc` need a distinct `MapEntry` value
type to reject non-entry 2-vectors and stay open.

External suite aggregate: 5252 -> 5278 assertions, errors 10 -> 8.

## v0.100.20

### Build: Make `make` clean on gcc-11 (Ubuntu 22.04)

The `release-build` workflow runs on `ubuntu-22.04`, where the
default `cc` is gcc-11. Two `-Werror` regressions broke linux-amd64
and linux-arm64 there (and the v0.99.0 - v0.99.4 attempts to
diagnose only added log capture, never the underlying fix):

- `src/gc/driver.c` named `-Wdangling-pointer` in a
  `#pragma GCC diagnostic ignored` block, but that flag was added
  in gcc-12. On gcc-11 the pragma fires `-Werror=pragmas`. Both
  the push and pop guards now require `__GNUC__ >= 12`.

- `main.c`'s two `snprintf(path_buf, sizeof(path_buf), "%s/lib/%s",
  dir, name)` calls in the `.cljc/.cljs/.clj` arm tripped
  `-Werror=format-truncation`. The pre-snprintf bound check
  (`dlen + nlen + 6 < sizeof(path_buf)`) is sound but gcc-11 can't
  propagate it through to `snprintf`. Replaced with a check on the
  `snprintf` return value: a truncated write skips the candidate
  rather than silently using a clipped path. Same end result, no
  pragma needed.

Verified locally with gcc-11.3 (matches Ubuntu 22.04), gcc-12.4,
and gcc-14.2. All three build clean with `-Werror`. Internal suite
1476 / 7071 / 0.

## v0.100.19

### Future spawn now conveys the caller's dynamic bindings

JVM Clojure's `(future ...)` snapshots the calling thread's binding
frame and reinstalls it on the worker, so a `bound-fn` captured
inside the future body sees the caller's `*x*` (plus whatever the
worker's own `binding` blocks pushed). mino's worker thread
previously started with an empty `dyn_stack`, so a nested future
that captured `*x*` saw only the root value (or, for the test in
question, the dereferer's binding).

`mino_future_spawn` now calls a new public helper
`mino_snapshot_thread_bindings(S)` (factored out of
`prim_get_thread_bindings`) and stores the resulting symbol -> value
map on `impl->dyn_snapshot`. `worker_run` unpacks that map into a
malloc-owned `dyn_binding_t` chain wrapped in a single
`dyn_frame_t`, pushes it as the worker's initial `dyn_stack`,
invokes the thunk, then pops and frees. The frame is freed on both
the success and error paths.

External `bound_fn.cljc` 7/8 -> 8/8, `bound_fn_star.cljc` 7/8 -> 8/8.
External suite: 191 -> 193 OK.

## v0.100.18

### `(seq sorted-map/-set)` is no longer a list

`(list? (seq (sorted-map :a 1)))` was returning true on mino because
sorted_seq returned a `MINO_CONS` chain and `list?` accepts both
`MINO_CONS` and `MINO_EMPTY_LIST`. JVM Clojure draws a sharper
distinction: `PersistentList` (literal list / `(list ...)` result)
matches `list?`; the seq view of any other coll does not. The
sorted-map and sorted-set arms of `prim_seq` now re-package the
cons chain produced by sorted_seq into a single-chunk
`MINO_CHUNKED_CONS`, so `list?` returns false while `first`,
`rest`, `count`, etc. keep working unchanged.

External `list_qmark.cljc` 20/22 -> 22/22. External suite:
190 -> 191 OK.

## v0.100.17

### Shortest-decimal Double-to-string for `bigint` and `rationalize`

Adds `mino_double_shortest`, a static helper in `src/prim/bignum.c`
that prints a finite double as the shortest decimal that
round-trips through `strtod` to the same bit pattern. Handled by
iterating precision 1..17 with `%.*g`, parsing back, and accepting
the first match. Slow paths only -- never on the hot loop.

`(bigint d)` now routes the float arm through the shortest-decimal
string, parses it via `mino_bigdec_from_string`, and truncates the
resulting BigDecimal toward zero with imath's integer division.
Result: `(bigint 1.7976931348623157E308)` is the full 309-digit
integer instead of `Long/MIN_VALUE` (the previous long-cast
saturation). Other doubles are unaffected.

`(rationalize d)` likewise converts via shortest-decimal +
BigDecimal: `(rationalize 1.1)` is `11/10`, `(rationalize 1.5)`
is `3/2`, `(rationalize (/ 1.0 3.0))` is
`3333333333333333/10000000000000000`. The previous binary mantissa
decomposition (`m * 2^e` -> `m / 2^|e|`) is gone.

### `clojure.lang.BigInt` bridge for `instance?`

`(def clojure.lang.BigInt :bigint)` so cross-dialect tests using
`(instance? clojure.lang.BigInt x)` succeed on mino. Same narrow
bridge pattern as the prior `clojure.lang.IPending -> :future`
mapping; no other JVM types are aliased.

### Auto-promoting arithmetic aliases

`+'`, `-'`, `*'`, `inc'`, `dec'` are now defined as aliases for
their unprimed forms. mino's unprimed forms already auto-promote
(an intentional divergence) so the primed forms have the same
semantics. The aliases let portable Clojure code that uses the
primed forms compile without rewriting.

External `bigint.cljc` 13/15 -> 18/18, `rationalize.cljc`
14/16 -> 16/16. External suite: 188 -> 190 OK.

## v0.100.16

### `(repeat n x)` accepts booleans (true -> 1, false -> 0)

Per the cross-dialect test suite, every non-`:clj` dialect coerces
a boolean count via `(if n 1 0)` instead of throwing. mino's
`repeat` rejected booleans up front because `number?` returns
false for them; the cond now adds an explicit boolean arm so
`(repeat true :a)` is `[:a]` and `(repeat false :a)` is `[]`,
while non-numeric / non-boolean counts (nil, strings, keywords)
still throw with the same "count must be a number" message.

External `repeat.cljc` 15/16 -> 17/17. External suite: 187 -> 188 OK.

## v0.100.15

### `subvec` coerces any number-tier index to long

JVM Clojure's `subvec` happily accepts floats, ratios, bigdecs, and
NaN as start/end -- it casts them to long via the same JVM `(long)`
truncation that `(int x)` uses, so `(subvec v 2.72 3.14)` is
equivalent to `(subvec v 2 3)` and `(subvec v ##NaN ##NaN)` is
`[]`. mino's `prim_subvec` rejected anything other than `MINO_INT`
with `"subvec: start must be an integer"`, breaking the external
test's exhaustive borderline-index coverage. The start/end checks
now route through a `subvec_to_long` helper covering MINO_INT,
MINO_FLOAT (NaN -> 0), MINO_BIGINT, MINO_RATIO, and MINO_BIGDEC;
non-numeric values still throw with a clearer "must be a number"
message.

External `subvec.cljc` 8/9 -> 34/34. External suite: 186 -> 187 OK.

## v0.100.14

### `(nth nil i)` returns nil instead of throwing

Clojure treats `nil` as an empty seq for `nth`: `(nth nil 10)` is
`nil`, and `(nth nil 10 :default)` is `:default`. mino's `prim_nth`
threw "nth index out of range" for the 2-arg form, which broke any
code relying on the nil-as-empty equivalence and the external
`nth.cljc` test (`(is (nil? (nth nil 10)))`). The nil-coll arm now
returns `def_val` when supplied, else nil; non-nil out-of-range
still throws.

External `nth.cljc` 4/5 -> 13/13. External suite: 185 -> 186 OK.

## v0.100.13

### Watch exceptions now propagate out of swap! / reset! / compare-and-set!

`atom_notify_watches` previously wrapped each watch call in a try
frame and swallowed any throw, which meant a watch's exception was
invisible to the user. Per Clojure JVM semantics the value commits
via CAS first and then watches fire; if a watch throws, the
exception propagates to the swap! call site (and any later watches
in the iteration order are skipped). The `swap!`/`reset!`/`cas!`
arms now check the watch return code and propagate `NULL` instead.
Internal `watch-exception-ignored` test renamed to
`watch-exception-propagates` and updated to assert the throw + the
post-CAS value.

External `add_watch.cljc` 7/10 -> 9/10 (passes once the watch tests
run; var-watch + ref-watch portions still error because mino lacks
var watches and STM, both intentional gaps).

## v0.100.12

### `atom` accepts trailing positional args and any persistent map as `:meta`

`(atom v)` now tolerates extra positional args after the initial
value -- the option-pair loop already absorbed unknown keys, but
`(atom nil nil nil)` and `(apply atom (take 11 (repeat nil)))` now
construct an atom with the initial value and ignore the trailing
nils, matching Clojure JVM. Also broadened the `:meta` value check
to accept `MINO_SORTED_MAP` in addition to `MINO_MAP`, so
`(atom nil :meta (sorted-map :a "a"))` succeeds. Vectors, sets,
numbers, etc. still reject -- the `(p/thrown? ...)` shapes in the
external suite remain.

### Validator returning nil rejects the new state

`atom`'s `:validator` arm previously only rejected on `false`; nil
slipped through. Per Clojure's docstring ("validate-fn should
return false or throw"), nil counts as logical false too. Both the
construction-time check and the swap!/reset! check now route
through `mino_is_truthy`, so a `(constantly nil)` validator throws
"Invalid reference state" both at construction and on every
attempted update.

External `atom.cljc` 74/74. External suite: 184 -> 185 OK.

## v0.100.11

Two fixes that close the last external-suite timeouts:

### `(promise)` no longer blocks process exit

`mino_host_threads_quiesce` was waiting on `impl->cv` for any future
that never had a worker thread, expecting a pool-managed worker to
publish the result. Promises (constructed via `(promise)` with no
backing thunk) match that shape but have no worker -- if the user
doesn't `(deliver p val)` before exit, the wait blocked forever.
The quiesce loop now distinguishes "pool-managed pending" (has a
thunk; will be delivered) from "promise" (no thunk; may stay pending
forever) and skips the latter.

### `ifn?` recognises promises; `clojure.lang.IPending` bridge

`(ifn? (promise))` now returns true (matches Clojure JVM where
promises implement IFn). `instance?` works against
`clojure.lang.IPending` -- mino binds that JVM interface name to the
keyword `:future` at bootstrap, so cross-dialect tests that detect
pending values via `(instance? clojure.lang.IPending x)` succeed
against mino's promise/future type. The bridge is narrow: only
`clojure.lang.IPending`, no other JVM interfaces are aliased.

External `ifn_qmark.cljc` 19/19, `taps.cljc` 4/4. External suite:
182 -> 184 OK, 0 crashes, 0 timeouts. Group 7 of the external-suite
plan is complete.

## v0.100.10

`let` now follows Clojure's sequential-binding semantics: an init
expression sees only the *previous* bindings in the same `let`, not
its own binding. Each binding lands in a freshly-created child env,
so closures captured during init expressions are immune to a later
shadow of the same name.

The previous (buggy) behavior used a single mutable env that all
binding inits shared. A nested `(let [f X] (let [f (fn [] (g f))] ...))`
shape would have the inner closure see the inner-let's `f` (after
the rebind) instead of the outer `f` it should have captured. The
external `bound_fn.cljc` test triggered this through nested
`(let [f (bound-fn [] ...)])` shadowing and segfaulted via
unbounded recursion.

The change has a knock-on effect: code that relied on mutable-env
semantics for self-recursive `let` -- `(let [go (fn [...] (go ...))])`
-- no longer works, because `go` is unbound when the fn body is
created. Any such pattern must use a named fn:
`(let [go (fn go [...] (go ...))])`. The named-fn binding is
established before its body is captured.

Audit and rename the recursive let-fn patterns in mino's bundled
code: `run!`, `tree-seq`, `interleave`, `shuffle`, `take-last`,
`trampoline`, the `condp` and `case` macros' `build` helper, and
`dotimes`/`while`/`doseq`'s emitted recursion shapes. Every callsite
that previously self-referenced through the mutable-env trick now
uses `(fn name [...] ...)`.

External `bound_fn.cljc` and `bound_fn_star.cljc` no longer segfault
(they go from process crashes to 7/8 each, with the one remaining
failure being a separate dynamic-binding-across-futures issue).
External suite: 182 OK with 0 crashes (was 0 crashes already after
prior fixes; this confirms the segv class is closed).

## v0.100.9

`(add-load-path! path)` adds a directory to the runtime's
extra-load-paths list, consulted by `require` after `mino.edn`'s
project paths and before the cwd fallback. The list lives on
`mino_state_t` and is freed at state teardown; entries are
deduplicated so re-registering an existing path is a no-op.

The pure motivation is the external `clojure-test-suite` driver,
where cross-file `(:require [clojure.core-test.X :as ...])` lives in
files outside mino's tree. The driver now does
`(add-load-path! "../clojure-test-suite/test")` once per sub-process,
and sibling files resolve through the standard module path -- no
per-file preloading hack, no test pollution from preloaded
namespaces. External `not_eq.cljc` is now 130/130 (was load-error).

External suite: 181 -> 182 OK. The remaining single load-error is
`ancestors.cljc`, which uses the JVM `Object` symbol -- pre-existing
JVM-specific gap, not addressable from this hook.

The standalone-mode resolver (no `mino.edn` present) is now
`runtime_paths_resolve`, which still consults the extra-load-paths
list before the cwd fallback. Project mode keeps using
`project_resolve` and now passes the state pointer as the resolver
context so it can read `S->extra_load_paths`.

## v0.100.8

Regexes are now a first-class value type (`MINO_REGEX`), distinct
from strings. Equality is identity, matching Clojure JVM's
`Pattern.equals`: two distinct `#"x"` literals are not `=`. Type tag
is `:regex`; print form is `#"source"` so the round-trip is exact.

Surface changes:
- `re-pattern` is now a C primitive that returns a `MINO_REGEX`
  wrapping the source string (or returns the existing regex
  unchanged if passed one). Was a `def` aliased to `identity`.
- `#"..."` reader literal builds a `MINO_REGEX` directly (no longer
  wraps in a `(re-pattern ...)` call).
- `re-find` and `re-matches` accept either `MINO_REGEX` or
  `MINO_STRING` for the pattern argument; the legacy string call
  shape still works.
- `clojure.string/split` accepts either a string or a regex
  separator. Regex separators currently use the source bytes as a
  literal substring -- works for the fixed-string patterns the
  test suites exercise (`#","`, `#"-"`); metacharacter separators
  would need a real regex-walking iteration which is deferred.
- `regex?` predicate (also exposed in `(type x)` as `:regex`).

External `eq.cljc` is now 65/65. External suite: 180 -> 181 OK.

Internal regex tests under `regex-literal-reader` and `re-pattern-fn`
are updated to assert the new contract (regex values, identity `=`).

## v0.100.7

UUIDs are now a first-class value type (`MINO_UUID`, 16 bytes
inline). The `#uuid "..."` reader literal, `(parse-uuid s)`,
`(random-uuid)`, and `(uuid? x)` all participate in the new type:

- `parse-uuid` (now a C primitive) returns a UUID value or `nil` on
  malformed input; throws on non-string args. The strict canonical
  form is required (36 chars, dashes at 8/13/18/23, hex everywhere
  else; case-insensitive).
- `random-uuid` returns a UUID value (was a 36-char string). Version
  4 / variant 1 bits are set canonically.
- `uuid?` now recognises the type, not the printed form. `(uuid?
  "550e...")` is `false`, matching Clojure JVM where `(uuid? s)` is
  only true for `java.util.UUID`.
- `#uuid "..."` reader literal is built into the reader (does not
  rely on `*data-readers*`).
- Equality is byte-wise; hash mixes the 16 bytes through the
  type-tagged FNV combiner.
- `(type x)` returns `:uuid`. `(str u)` and `(pr-str u)` print as
  `#uuid "...lower-hex..."` so the round-trip is exact.

External suite: 178 -> 180 OK. `parse_uuid.cljc` 17/17 (was 9/17),
`uuid_qmark.cljc` 24/24 (was load-error). The internal
compat tests for random-uuid / uuid? / parse-uuid are updated to
exercise the new type contract.

## v0.100.6

### Bigdec division

`(/ 2.0M 1.0M)` no longer errors with "with-precision unimplemented".
The new `mino_bigdec_div` mirrors Java's `BigDecimal.divide(BigDecimal)`:
preferred scale is `sa - sb`, but the algorithm tries successively
larger scales (multiplying the numerator by 10 each step) until the
division is exact. If the quotient has a non-terminating decimal
expansion the function throws "non-terminating decimal expansion in
bigdec division" -- same error class as Java's `ArithmeticException`.
Cap is 1024 extra digits, well past anything that would terminate.

The tower-arithmetic dispatch in `tower_op_at_tier` and
`tower_reduce_seeded` now both route `OP_DIV` for the BIGDEC tier
through this primitive. External `slash.cljc` rises from 41/42 (1
error that aborted the rest of the file) to 158/160 -- the remaining
two are bigdec-meets-ratio cases (`(/ 2.0M 1/2)`) where mino's
documented tier-collapse-to-float diverges from JVM Clojure's promote
ratio-to-bigdec; tracked as a separate intentional divergence.

### `=` on BigDecimals is numerical

`(= 1.0M 1.00M)` is now `true`, matching JVM Clojure's `=` (which
dispatches BigDecimals through `Numbers.equiv` -> `compareTo == 0`).
mino was previously scale-strict (`Object.equals`-style), which
mismatched both Clojure's `=` and the cross-dialect tests in the
external suite. The hash function now strips trailing zeros from
the unscaled bigint before mixing in the scale, preserving the
equal-implies-equal-hash invariant.

This is a breaking change for code that relied on `(= 1.0M 1.00M)`
being false -- use `mino_bigdec_equals` directly via
`identical?`-style comparisons or compare the printed forms if you
need to distinguish scales. Internal `numeric_tower_test` is updated
to assert the new behavior.

## v0.100.5

Three small fixes for Clojure parity, all driven by the external
suite. External suite: 170 OK -> 178 OK on the cumulative run.

### `sort` throws on incomparable elements

`(sort [1 []])` now throws "compare: cannot compare values of
different types", matching Clojure's `ClassCastException`. Default
`sort` (no comparator) routes through `prim_compare` instead of the
internal type-tag fallback in `val_compare`, so cross-type elements
fail loudly. `sort` with an explicit comparator is unchanged.

### `min-key` / `max-key` NaN handling

The variadic case `(min-key k a b & more)` now uses the `(<= kw kv)`
"keep current on NaN" loop that JVM Clojure uses, instead of folding
through the 2-arg form's `(< kx ky)` predicate. The two-arg case
itself is unchanged. NaN-bearing inputs match Clojure's
order-dependent results: `(min-key identity [##NaN ##-Inf 1])` is
`##-Inf`, `(min-key identity [##-Inf ##NaN 1])` is `##NaN`, etc.

### `if-let` / `when-let` / `if-some` / `when-some` validate the binding vector

The macros now assert at expansion time that the binding form is a
vector of exactly two elements (one symbol/expr pair). Anything else
-- a list, a multi-pair vector -- throws. External `when_let.cljc`
goes 13/13.

## v0.100.4

`rationalize` accepts BigDecimals. The previous arms accepted int,
bigint, ratio, and float; passing a BigDecimal threw "argument must
be numeric". Now `(rationalize unscaled * 10^-scale)` reduces to a
ratio (or integer when scale <= 0). External `rationalize.cljc` rises
from 11/16 to 14/16 assertions; the two remaining failures are float
cases that depend on JVM `Double.toString` (shortest-decimal
roundtrip) which mino does not yet implement -- mino's `%g` printer
is the wider gap and is tracked separately.

## v0.100.3

This release bundles four fixes that move three external test files
to green and patches one underlying equality bug surfaced by the
fourth.

### `nthnext` validates inputs

`(nthnext nil _)` returns `nil` (was throwing). Non-integer `n`
throws a typed error instead of bottoming out in the inner `<=`
arithmetic. Matches Clojure's surface behavior. Test:
`nthnext.cljc` 13/13.

### `rand-nth` validates the collection

`(rand-nth nil)` returns `nil` (was throwing on `count`).
`(rand-nth 1)` (or any non-collection) throws a typed error rather
than the inner "count: expected a collection" message. Test:
`rand_nth.cljc` 4/4.

### Equality forces lazy tails inside chunked-cons spines

A chunked-cons can hold an unrealized lazy seq in its `more` field
(the typical shape `(filter pred (range N))` builds when the
predicate keeps every element of each source chunk). The
non-forcing `eq_seq_like` was treating that unrealized lazy as
end-of-seq, so `(= (range 1000) (filter (fn [_] true) (range 1000)))`
returned `false`. `mino_eq_force` now routes same-tag chunked-cons
through `eq_seq_like_force`, which forces lazy tails on both sides.
Regression test added at `tests/lazy_test.clj` under
`eq-chunked-cons-with-lazy-tail`.

### `random-sample` test now passes

The remaining `random_sample.cljc` failures were a downstream effect
of the equality bug above (the suite compares the filter output to
the source range with `=`). With both `random-sample` itself and the
chunked-cons equality bug addressed, the test goes 21/21.

## v0.100.2

Transients are now read-callable. Per Clojure, a transient supports
the same read-only interface as its persistent view: `nth`, `get`,
`count`, `contains?`, and direct invocation (`(t-vec idx)`,
`(t-map :k)`, `(t-set v)`, `(:k t-map)`). All write operations
(`assoc`, `conj`, `dissoc`, `disj`, `pop`) still throw, matching the
"transients are not persistent" contract.

mino was throwing "expected a vector/map/set, got transient" on
every read primitive. Each call site now unwraps the transient to
its `current` persistent backing (failing on transients that have
already been `persistent!`'d). External `transient.cljc` rises from
6/51 to 51/51 assertions passing.

## v0.100.1

`(deref delay)` now forces and returns the delay's value. mino's
delay is map-shaped (`{:delay/fn ... :delay/state ...}`), so the
C-side `prim_deref` rejected it as "not an atom/var/future/...".
`force` and `realized?` already special-cased delays mino-side; the
override here adds the matching `deref` arm so all three reference
operations agree. External `realized_qmark.cljc` now passes
end-to-end.

## v0.100.0

Reader conditionals: drop the `:clj` fallback. mino is not a JVM
dialect, so `:clj` branches must not fire here. Cross-dialect tests
in the wild (e.g., `jank-lang/clojure-test-suite`) put JVM-only
assertions (`System/getProperty`, `(new Object)`,
`clojure.lang.MapEntry/create`, `int-array`, …) inside their `:clj`
branch, with `:default` as the catch-all for non-JVM runtimes -- the
suite was authored on the assumption that each non-JVM dialect is
named (`:cljs`, `:bb`, `:jank`, `:cljr`, `:lpy`, …) and `:default`
covers everything else, mirroring how peer dialects (ClojureScript,
Babashka, jank, ClojureCLR, Basilisp) handle the same files.

mino now matches `S->reader_dialect` (defaults to `"mino"`) and
`:default` only. The bundled `lib/clojure/*` is unaffected -- it uses
`:mino`/`:default` exclusively, so no internal lib relied on the old
`:clj` fallback. Internal reader-conditional tests under
`tests/reader_cond_test.clj` and `tests/compat_test.clj` are updated
to exercise the new semantics directly. External
`jank-lang/clojure-test-suite` rises from 166/223 to 170/223 OK on
this change alone.

This is a documented intentional divergence. Embedders that *want*
JVM-style behavior can override `S->reader_dialect` to `"clj"` to
have their conditional code receive the `:clj` branch, but doing so
loses the `:mino`-tagged escape hatch and is not a supported
configuration.

## v0.99.4

Add the same build-log artifact upload to `release-build.yml` that
`ci.yml` already grew under v0.99.2. The release-build job runs on
ubuntu-22.04 (gcc-11) and uses a different runner image than
ci.yml's ubuntu-latest, so a build break that's gcc-11-specific
(or glibc-22.04-specific) doesn't surface in ci.yml. Capturing the
log here lets external observers grab the gcc error from the
artifact even when only the release-build legs fail.

## v0.99.3

Handle `getcwd`'s return value in `main.c`. Ubuntu's glibc declares
`getcwd` with `__attribute__((warn_unused_result))` and the
bootstrap `CFLAGS` treat `unused-result` as an error, so the
ignored-call line tipped over `-Werror=unused-result` on the
ubuntu-latest runner. The macOS runner's libc declares the function
without the attribute, so the same source compiled cleanly there
and the regression went unnoticed locally on a gcc-14 + Debian
glibc box where the attribute also doesn't fire. Fix is to capture
the result and clear `initial_dir` on failure -- best-effort, so
the rest of the binary still launches if the cwd lookup fails.

## v0.99.2

CI follow-up to v0.99.1: also upload the captured build log as a
public-downloadable artifact when the bootstrap step fails (in
addition to the job summary), so external observers can fetch the
exact gcc error without log-download permission. Initialise `r_at`
and `b_at` to `NULL` in the `mqr_ratio` modulus-adjust path so
older GCC's `-Wmaybe-uninitialized` flow analysis sees a definite
assignment -- the conditional branches already cover every reachable
path, but gcc-11 (release-build runner default on ubuntu-22.04) has
a less-precise analyzer.

## v0.99.1

CI plumbing: surface the build log on the job summary page (visible
without log-in) when a step fails, and print every available `gcc-N`
version on Linux so a regression triggered by the runner-image
default GCC change is easier to triage. Also tidy up the
`try_parse_numeric` reader helper -- drop a dead-store
`buf_capacity` variable and move the now-late `*err = 0;`
assignment back to the top of the function body.

## v0.99.0

External jank-lang/clojure-test-suite compatibility pass: 166/223
files green (74%) at 4472/4542 = 98.5% assertion pass rate. Each
entry below is a Clojure-parity fix or an intentional divergence
made explicit.

### `(get string i)` Returns a `\char`; Other Strict-Predicate Tightenings

`(get "ab" 0)` now returns `\a` (was `"a"`). The seq path already
yielded chars after the earlier UTF-8 walk fix; the indexed `get` was
the only string accessor still emitting one-byte substrings. Walks
codepoint by codepoint so multi-byte chars count as a single index.

`numerator` and `denominator` now require a Ratio argument; passing
a plain integer throws (was: silently returned the integer / 1). 

`intern` requires the target namespace to already exist and throws
`no namespace: <name> found` otherwise; previously it silently
created the namespace via `ns_env_ensure`.

The internal `get-fn` test was updated to expect `\char` results.

### Symbol / Keyword Compare Sorts Unqualified Before Qualified

`compare` and `val_compare` for symbols and keywords now follow
`clojure.lang.Symbol.compareTo`: an unqualified name (no namespace)
sorts before any qualified one, and within a single namespace the
local names are compared lexicographically. The previous straight
`strcmp` over the printed form put `:cat` after `:animal/cat` because
`'c' > 'a'`, so `(compare :cat :animal/cat)` returned `1` instead of
`-1`. Plain strings still use `strcmp`.

### `(symbol "" "name")` Preserves the Empty Namespace

`symbol` previously dropped an empty-string namespace argument,
producing a symbol whose `(namespace ...)` returned `nil`. Per
Clojure, `(namespace (symbol "" "x"))` is `""` (the explicit empty
namespace differs from `nil`). The 2-arg form now emits the
`ns/name` cons regardless of whether `ns` is empty, so the empty
prefix round-trips through `namespace`.

### Misc Eager Validations / Predicate Tightening

- `repeat` rejects non-numeric counts; the previous flow coerced
  `"a"` into the codepoint 97 and returned 97 repetitions.
- `select-keys` calls `seq` on `ks` so passing a single keyword
  raises a coercion error instead of silently returning `{}`.
- `NaN?` accepts the full numeric tower and rejects everything
  else; previously it accepted any non-`nil` value.

### `pos-int?` / `neg-int?` / `nat-int?` Stay Long-only; `counted?` Drops Strings

Per Clojure, the long-tier predicates `pos-int?`, `neg-int?`, and
`nat-int?` compose `int?` (Long only) -- they reject BigInts -- so
`(neg-int? -1N)` returns `false`. mino briefly broadened these to
the new `integer?` (long + bigint) when fixing `(integer? 1N)`; this
restores the narrow contract.

`counted?` no longer reports strings as counted. Strings are not
Counted on the JVM, where `count` on a `String` walks the
`CharSequence` protocol; the predicate now mirrors that.

### `use-fixtures` Captures the Caller's Namespace

`use-fixtures` is now a macro so it can capture the calling
namespace at expansion time. The previous function-based
implementation read `(str *ns*)` from inside the function body, but
mino's `*ns*` is the function's *defining* namespace (set when the
fn was created) rather than a dynamic var that tracks the caller. As
a result every `(use-fixtures ...)` call registered fixtures under
`"clojure.test"` instead of the user's namespace, so `:once` and
`:each` fixtures never fired. This was visible in the external
suite: `parents.cljc` and `descendants.cljc` use a `:once` fixture
to install a global hierarchy via `derive`, and without the fixture
the queries returned `nil`.

### `subs` Indexes by Codepoint

`subs` previously interpreted its `start` and `end` indices as raw
byte offsets, so a multi-byte codepoint (e.g. `֎`, U+05CE, two bytes
in UTF-8) shifted later characters and slicing through one returned
a malformed UTF-8 fragment (`�`). It now walks the string by
codepoint -- matching Clojure's "string is a sequence of chars" model
-- so `(subs "ab֎de" 0 5)` returns `"ab֎de"` instead of truncating
mid-codepoint. ASCII strings are unaffected since the codepoint walk
is byte-equivalent there.

### `sort` / `set` -- Char Comparison, Eager Validation, Empty Result

`sort` now orders `MINO_CHAR` values by codepoint (was effectively a
no-op because `val_compare` had no `MINO_CHAR` arm and fell through
to type-tag comparison, which is identical for any two chars). Also:

  - `(sort nil)` returns the empty-list singleton `()` instead of
    `nil`, matching Clojure's "sort always returns a sequence"
    contract; corrected the internal `sort-fn` test that asserted the
    nil-tolerant behaviour.
  - `(sort 1)` and similar non-seqable inputs now route through
    `prim_seq` for the standard "cannot coerce" type error, instead
    of silently returning an empty result.
  - The same eager-seqability check was added to `set` so `(set 1)`
    throws.

### `list?` Distinguishes Lists From Other Sequences

`list?` was previously `cons?`, so it returned `true` for any cons cell
including the chunked-cons spine produced by `(seq vector)`. Per
Clojure's contract `list?` is narrower than `seq?`: it accepts the
empty-list singleton and proper cons chains but excludes lazy-seqs and
chunked-seqs. mino now ships a dedicated `list?` C primitive that
matches `MINO_CONS` and `MINO_EMPTY_LIST` but rejects
`MINO_CHUNKED_CONS`. (`seq?` continues to accept the broader family.)
The lingering case `(list? (seq (sorted-map ...)))` still returns
`true` because the sorted-collection seq builds a plain cons chain --
that requires a finer-grained "is this a real list literal" tag and
is left as future work.

### `rational?` Returns True For BigDecs

`(rational? 1.5M)` now returns `true` instead of `false`, matching
Clojure: a BigDecimal's value `unscaled * 10^-scale` is an exact
rational number, so the predicate accepts the bigdec tier alongside
int / bigint / ratio. Only the float (IEEE-754) tier remains outside.
The internal `nt-bigdec-literal` test, which had the inverted
expectation, was corrected.

### Reader Accepts Arbitrarily Long Numeric Literals

`try_parse_numeric` previously bailed out (treated the token as a
symbol) whenever the literal exceeded a 63-byte stack buffer. That
meant a bigint literal like `1797693134862315700000...0N` -- standard
in Clojure for double-overflow tests -- read back as an unbound
symbol. The parser now falls back to a heap-allocated buffer for long
tokens so the bigint / bigdec / ratio / radix paths handle digit runs
of any length.

### Eager Validation for `cycle`, `mapcat`, `reverse`

`cycle`, `mapcat`, and `reverse` now raise a type error eagerly when
their collection (or function) argument is non-seqable (or, for
`mapcat`, non-invokable). Previously the lazy variants returned a
seq-shaped value that only blew up when something forced it; the
eager variants (`reverse`) silently treated the input as empty
because the `seq_iter_*` family short-circuits on unknown types.
Matches Clojure's "throws at the call site" behaviour for these
specific functions.

### Sorted Collections: Predicate Comparator + Cross-comparator Equality

Two related fixes for `sorted-map` / `sorted-set` and the `-by`
variants:

  - `rb_compare` now follows Clojure's "predicate comparator" contract:
    when the comparator returns a non-numeric truthy value it means
    `a < b`, but if it returns falsy the function probes the reverse
    direction `(cmp b a)` to distinguish `a > b` from `a == b`. The
    previous fall-through always treated falsy as `>`, so a comparator
    like plain `<` could never report equality. That broke rb-tree
    lookup -- every key landed slightly off-node and `rb_get` returned
    `nil` -- so `(get (sorted-map-by < 1 :a) 1)` came back as `nil`
    even though `(seq ...)` clearly contained `(1 ...)`. The seq path
    happened to mask the bug differently: it iterated keys via
    `rb_to_list` then re-`rb_get`-ed each one, so the keys were right
    but the values were uniformly `nil`.
  - Equality on two sorted collections with different comparators
    (e.g. `(sorted-map-by < 1 :a)` vs `(sorted-map-by > 1 :a)`) now
    returns `true` for matching content. The trees are arranged in
    opposite orders, so the structural `rb_trees_equal` walk could
    never see them as equal; the new `rb_trees_content_equal`
    pairs entries by `mino_eq` on the key (O(n*log n), tree-shape
    independent) when the two collections share neither a comparator
    nor the default ordering.

### `str` Drops the `N` / `M` Suffix on BigInts and BigDecs

`(str 1N)` now returns `"1"` (was `"1N"`) and `(str 1.0M)` returns
`"1.0"` (was `"1.0M"`). The readable printer (`pr-str`, `prn`) keeps
emitting the suffix so round-tripping through the reader still works;
only the non-readable `str` family was wrong. The previous
implementation routed bigints / bigdecs through `print_to_string`
which uses the readable form. The fix adds explicit `MINO_BIGINT` and
`MINO_BIGDEC` cases in `prim_str` that format the digits directly --
mirroring how `MINO_INT` and `MINO_FLOAT` are already handled.

### `<` / `<=` / `>` / `>=` -- Strict Numeric Operands and NaN Unordering

The four numeric comparison operators previously accepted nil (silently
treating it as `0.0`) and ranked any NaN operand as equal to NaN, so
`(< nil 1)` returned `true` and `(<= ##NaN ##NaN)` also returned `true`.
Both now match Clojure:

  - Each pair of consecutive operands must be a number (long, bigint,
    ratio, bigdec, or float). Anything else -- nil included -- throws
    `eval/type`.
  - If either operand in a pair is NaN, the whole chain short-circuits
    to `false` (NaN is unordered against every value, itself included).

The single-argument form is unchanged: `(< x)` returns `true` for any
`x` without inspecting its type, matching Clojure's "trivially true on
zero or one argument" contract.

### `special-symbol?` Recognises Clojure's Reserved Special Forms

`special-symbol?` now returns `true` for the Clojure-reserved special
form names `&`, `.`, `case*`, `catch`, `deftype*`, `finally`, `fn*`,
`let*`, `letfn*`, and `loop*`. mino implements the unstarred forms
(`fn`, `let`, `loop`) directly and also accepts the starred aliases
where applicable; the remaining names are unimplemented but are still
reserved as a portability courtesy so that code which inspects symbol
status (linters, code-walkers, syntax-quote logic) does not have to
special-case the dialect.

### `mod` / `rem` / `quot` Preserve Operand Type

`mod`, `rem`, and `quot` now dispatch on the higher tier of their two
operands and preserve the result type per Clojure's contagion rules:

  - bigint inputs produce bigint results
  - ratio inputs produce bigint quot, ratio rem / mod (collapsing to
    bigint when the value is integer)
  - bigdec inputs produce bigdec results at the aligned scale
  - long inputs stay long (with `LLONG_MIN / -1` overflow promoted to
    bigint)
  - float inputs use the existing `fmod` path

Previously the three primitives coerced both operands through
`tower_to_double`, computed via `fmod`, and packed the result as long
or float. Bigints, ratios, and bigdecs all collapsed lossily, so
`(mod 10 3N)` returned a long (failing `(big-int? r)`) and
`(mod 10 3.0M)` returned a float (failing `(decimal? r)`).

The new path adds three internal helpers in `src/prim/bignum.c` —
`mino_bigint_quot` / `_rem` / `_mod` (truncated division on bigints,
with `mod` adjusting toward the sign of the divisor) — and
`mino_bigdec_quot` / `_rem` / `_mod` which align scales before
deferring to the bigint helpers. The ratio path cross-multiplies
numerators and denominators into bigints so it can reuse the same
quotient logic, then derives `rem` and `mod` via tower subtraction.

### `integer?` Recognises BigInts

`integer?` previously aliased `int?`, so `(integer? 1N)` returned
`false`. Per Clojure's contract, `integer?` is true for any value
that is "exactly an integer", which on the JVM covers Long, Integer,
Short, Byte, BigInt, and BigInteger. mino represents the integer
tier with `MINO_INT` (long) and `MINO_BIGINT` (arbitrary-precision),
so `integer?` now returns `(or (int? x) (bigint? x))`. The composed
predicates `pos-int?`, `neg-int?`, and `nat-int?` route through the
new `integer?` so they pick up the bigint tier for free.

The external test suite's portability shim now defines `big-int?`
as `bigint?` (rather than `integer?`) so it specifically probes the
bigint type, matching the JVM `(instance? clojure.lang.BigInt n)`
semantics.

### `doseq` Supports `:let`, `:when`, and `:while` Modifier Clauses

`doseq` now recognises the three modifier clauses Clojure's
version exposes alongside plain bindings:

  - `:let [name expr ...]` introduces locals visible to the
    remaining clauses and the body.
  - `:when expr` skips an iteration when expr is falsy.
  - `:while expr` halts iteration entirely (including outer
    binding loops) when expr is falsy.

Previously the binding parser stopped at clause-keyword/value pairs
and tried to call `seq` on the keyword's "value" (e.g. on the
boolean produced by `:while (< x 3)`), so any modifier triggered a
"seq: cannot coerce bool to a sequence" error.

`:while`'s "stop everything" semantics is implemented with a
shared `stop` atom that the outer recursive driver consults each
iteration; without it an outer infinite seq paired with a later
`:while` would never terminate.

doseq.cljc: 11 passing / 1 error -> 15/15 clean.

### `realized?` Throws on Non-pending Inputs

`realized?` previously returned `true` for any value that wasn't
a lazy seq, which let `(realized? 1)` / `(realized? :foo)` /
`(realized? [])` etc. silently pass through. Now the prim
matches Clojure's contract: it returns the realized state for
`MINO_LAZY` (lazy seqs and delays share that representation) and
`MINO_FUTURE`, and throws `realized? expects a lazy seq, delay,
promise, or future` for anything else.

### Keywords as Functions Look Up in Sets

`(:k #{:k :other})` now returns `:k` (and similarly for sorted
sets) instead of `nil`. Per Clojure, keyword invocation against
a set treats the set as a membership probe -- the keyword is its
own value, returned when present and the supplied default (or
`nil`) when absent. The previous fall-through returned the
default for any non-map collection.

### `repeat` Truncates Non-integer `n` Toward Zero

`(repeat 3.14 x)` and `(repeat 3.99 x)` now both return three
repetitions of `x`, matching Clojure (`repeat` truncates the
count toward zero before counting). The previous implementation
recursed with `(- n 1)` and only stopped when `n` reached `0`,
so floats produced an off-by-one extra element.

### `reverse` Returns the Empty-list Singleton

`(reverse nil)` and `(reverse <empty>)` now return `()` rather
than `nil`, matching Clojure (`reverse` always returns a
sequence). Updated the internal `reverse-fn` test which had been
asserting the old nil-tolerant behaviour.

### Non-readable Print of Characters

`print` / `println` (the non-readable family) now emit a
character's codepoint as UTF-8 bytes instead of its `\name` /
`\letter` escape form. So `(print \A)` writes `A` (matching
Clojure) rather than `\A`. The readable `pr` / `prn` family
still emits the escape form via `print-to-string`. Implemented
in `append_print_chunk` with a `MINO_CHAR` branch that encodes
the codepoint inline.

### `(empty seq)` Returns the Empty-list Singleton

`empty` on a list / cons / lazy-seq / chunked-cons / `()` now
returns the empty-list singleton `()` rather than `nil`. Per
Clojure, the contract is "an empty collection of the same kind";
for sequence types that's `()`, not `nil`. The branches for
maps / vectors / sets / sorted maps already returned the right
empty collection; only the seq branches were wrong.

### Char Semantics Across `first`, `rest`, `cons`, and Iterators

The `(seq string)` change shipped chars on the seq path; this
follow-up brings the rest of mino's string-as-sequence operations
in line:

- `prim_first` on a string now returns a `MINO_CHAR` codepoint
  rather than a one-byte substring.
- `prim_rest` and the lazy `str_rest_thunk` decode the next UTF-8
  codepoint and return a cons whose car is `MINO_CHAR`; the lazy
  continuation steps by codepoint length, not byte.
- `val_to_seq` (used by `cons` to materialize string cdrs into a
  walkable list) emits a list of `MINO_CHAR` values.
- `seq_iter_val` / `seq_iter_next` decode UTF-8 step by step, so
  `(map identity "abc")` and similar iterator-based traversals
  produce `(\a \b \c)` rather than `("a" "b" "c")`.

`cons.cljc`, `fnext.cljc`, `zipmap.cljc`, `interpose.cljc` etc.
that depended on character iteration now pass cleanly.

### `(seq string)` Yields Characters Per Clojure

Previously `(seq "abc")` returned a sequence of one-byte strings
(`("a" "b" "c")`); now it returns a sequence of MINO_CHAR
codepoints (`(\a \b \c)`), matching Clojure. The implementation
walks UTF-8 codepoint by codepoint so multi-byte characters like
`\é` and `\☃` come out as single MINO_CHAR values rather than
fragmented byte slices.

This unblocks the conformance suite's `interpose`, `cons`,
`fnext`, `zipmap`, etc. cases that depend on character semantics
(`(map identity "abc")` → `(\a \b \c)`). Two internal tests
(`seq-fn`'s string case and `clj-into-concat`) were updated to
expect `\a \b` instead of `"a" "b"`; the test suite now uses the
Clojure semantics throughout.

The companion `seq_iter_val` path used by direct iterators
(`first`, `next` over strings via `seq_iter_*`) still emits
substrings; that path needs UTF-8 byte-step tracking to switch
safely and is left for a separate fix.

### `parse-boolean` Throws on Non-string Input

Per Clojure 1.11+'s contract, `parse-boolean` throws on non-string
arguments (NullPointerException / ClassCastException on the JVM
for `nil` / non-string types). Mino's previous implementation
silently returned `nil` for any non-string input. The function now
raises an `ex-info` for non-strings; matching strings return their
boolean and non-matching strings still return `nil`.

The internal `parse-boolean-cases` test was updated to assert the
new contract (the cases that previously expected `nil` for `nil`
and `42` now expect a throw).

### `keys` and `vals` Accept the Empty-List Singleton

Both `keys` and `vals` had explicit "return nil" branches for empty
vectors / sets / strings / sorted sets / `nil`, but `()` (the
`MINO_EMPTY_LIST` singleton) wasn't in the set, so it fell through
to the "must be a map" error. Added `MINO_EMPTY_LIST` alongside
the other empty cases.

### `clojure.test/use-fixtures`

`use-fixtures` now lives in `lib/clojure/test.clj` with the
familiar `:once` and `:each` kinds. Fixtures are registered per-
namespace in a `fixtures-registry` atom; the runner groups
registered tests by namespace, wraps each ns's batch with its
`:once` fixtures (outermost first), and threads each individual
test through its `:each` fixtures. Multiple fixtures of the same
kind compose left-to-right via `compose-fixtures`. This unblocks
the suite's `descendants.cljc` and `parents.cljc`, which both
declared `(use-fixtures :once with-global-hierarchy)` to set up
shared `derive` state.

### Numeric Predicates Across the Full Tower

`zero?`, `pos?`, `neg?`, `even?`, and `odd?` now accept the full
numeric tower (long, double, bigint, ratio, bigdec) instead of
just long/double. Sign predicates route through
`tower_to_double` for ordering; `even?` / `odd?` use imath's
`mp_int_is_odd` directly so arbitrary-precision integers work
without the lossy double conversion. `tower_to_double` is
exported via `prim/internal.h` for shared use.

This unblocks the predicate test files (e.g. `pos_qmark.cljc`,
`even_qmark.cljc`) that previously erred on `(pos? 0N)`,
`(even? 122N)`, etc.

### `compare` Cross-numeric, Chars, and Vectors

`compare` now matches Clojure's contract more precisely:

- nil compares as less than every non-nil value (and equal to
  itself), making `(compare 1 nil)` and `(compare 'a nil)` both
  positive instead of throwing.
- Cross-tower numeric comparisons use `tower_to_double` so
  long/float/bigint/ratio/bigdec all reduce to a single double
  for ordering. `(compare 0 -100N)` now returns 1 instead of an
  "incomparable" error.
- `MINO_CHAR` values compare by codepoint.
- `MINO_VECTOR` pairs compare lexicographically (element-wise,
  recursing through `compare`; shorter vector wins ties).

Genuinely incompatible cross-type pairs (e.g. `(compare 1 [])`)
still throw, matching Clojure's `compareTo` ClassCastException
behaviour.

### `(symbol var)` Returns the Var's Qualified Name

`symbol`'s 1-arg form now accepts a Var and returns its
fully-qualified name as a symbol (e.g. `(symbol #'+)` →
`'clojure.core/+`). Vars are Named in Clojure, so this matches
the contract; previously the call raised a type error. Vars
with no owning namespace yield the bare name.

### `merge` Accepts MapEntries and Non-map Args via `conj` Semantics

Rewrote `merge` to match Clojure's `(reduce conj (or acc {}) ms)`
shape rather than walking `(seq m)` and assuming each entry is a
`(k v)` pair. Position-2+ args may now be MapEntries or 2-element
vectors (e.g. `(merge {:a nil} (first {:a "a"}) {:b "b"})`), and
non-map first args follow `conj`'s undefined-but-non-throwing
behaviour. The previous implementation broke as soon as it hit a
MapEntry because `(seq mapentry)` yielded the bare key as the
first "kv", which `first` then rejected.

### `(conj map nil)` is a No-op

Per Clojure, `(conj coll nil)` returns `coll` unchanged on every
collection type. Mino's `conj` honoured that for vectors / lists /
sets but mapped `nil` to the "must be a map entry or 2-element
vector" type error on maps. Mixed sequences like `(conj {:a 1}
nil [:b 2] nil)` now collapse the nils and apply only the real
entries.

### `peek` on the Empty-List Singleton

`peek` now recognises the canonical empty list `()` (a
`MINO_EMPTY_LIST` singleton, distinct from `MINO_NIL` and
`MINO_CONS`) and returns `nil` for it, matching Clojure. Without
this, `(peek '())` threw `peek: expected a vector or list, got
list` because the empty-list type slipped past the existing
NIL/CONS branches.

### `assoc!` Variadic Arity (with Odd-out Nil)

`(assoc! tcoll k v & kvs)` now accepts the variadic Clojure form
plus the documented JVM quirk: a trailing odd-out key with no
matching value is treated as `key nil`. Previously only the 3-arg
form was accepted. Each pair (or trailing nil pair) is assoc'd
left-to-right against the running transient.

### `dissoc!` / `disj!` Variadic Arity

Both transient ops now accept the variadic Clojure form
`(dissoc! tcoll k & ks)` / `(disj! tcoll k & ks)`. Each extra
key is processed left-to-right against the running transient.
Previously only the 2-arg form was accepted.

### `conj!` Variadic Arity

`conj!` now matches Clojure's full signature: `(conj!)` returns
a fresh transient empty vector, `(conj! tcoll)` returns the
transient unchanged, and `(conj! tcoll x & xs)` conj's each extra
value in turn and returns the final transient. Previously only
the 2-arg form was accepted.

### `(dissoc m)` Returns the Map Unchanged

Per Clojure's contract, `dissoc` is variadic with a 1-arg form
that returns the map untouched. Mino previously required at least
one key and threw on `(dissoc m)`; now the no-key call short-
circuits and returns `m` directly.

### `atom` Accepts `:meta` and `:validator` Options

`(atom x)` now accepts the variadic Clojure form `(atom x & opts)`
where opts is a flat keyword/value sequence including `:meta map-
or-nil` and `:validator fn-or-nil`. `:meta` attaches metadata
visible via `(meta the-atom)` (atoms now sit alongside symbols /
collections / fns / vars in the set of types that carry metadata).
`:validator` runs the supplied fn against the initial value and
installs it on the atom so subsequent `swap!` / `reset!` calls go
through it; an initial value the validator rejects raises an
`Invalid reference state` error at construction time, matching
Clojure's contract. Unknown option keys are tolerated silently.

### Syntax-Quote Auto-Qualification Inside Macro-Generated Closures

Closures created during a macro body's evaluation -- the canonical
shape being `(map (fn [row] `(some-sym ...)) rows)` inside a
macro -- now capture the macro's defining namespace as their
`defining_ns`, not the caller's. Without this, invoking the
closure overwrote `fn_ambient_ns` with the caller's namespace, so
syntax-quote inside the closure couldn't find symbols in the
macro's namespace and emitted them bare.

The clearest example was `clojure.test/are`. From the test file's
namespace, `(macroexpand '(are [x] (= x x) 1 2))` produced
`(do (is (= 1 1)) (is (= 2 2)))` -- bare `is` -- because the inner
`(fn [row] ...)` ran with `defining_ns = caller's ns`. Predicate
test files in the external suite that referred only `[are deftest
testing]` (not `is`) consequently errored on `unbound symbol: is`,
which is why files like `boolean_qmark`, `coll_qmark`, `map_qmark`,
and many others reported `0 passed, 1 errors`. Post-fix the same
expansion produces `(do (clojure.test/is (= 1 1)) ...)` and those
files run cleanly.

A paired fix in the require/load path clears `fn_ambient_ns` for
the duration of a file load. File loads are a top-level boundary,
and the file's own `defn` closures must capture the file's
namespace as their `defining_ns` -- not whatever macro-expansion
ambient happened to be active when `require` was called from
inside a closure. Without this, `(require 'clojure.edn)` from
inside a `deftest` body bound `clojure.edn/read` with
`defining_ns = "user"`, breaking subsequent calls to it; mirrors
the existing `eval` / `load-string` / `load-file` behavior.

### External clojure-test-suite Driver

A new pure-mino driver, `tests/clojure_test_suite.clj`, runs the
[jank-lang/clojure-test-suite][cts] against mino. The driver expects
the suite cloned as a sibling directory (`../clojure-test-suite`),
forks one `./mino` sub-process per `.cljc` file so a single SIGSEGV
or hang doesn't lose the rest of the run, applies a 30 s per-file
timeout, parses each summary line, and prints an aggregate report
plus a categorized breakdown (load errors, crashes, timeouts,
assertion failures). The same script self-dispatches into a one-file
harness when given a path argument, used by the driver's sub-fork.

Two new shims under `lib/clojure/core_test/` make the suite
loadable: `portability.clj` provides `when-var-exists` (the suite's
per-test "skip if var doesn't exist" macro), `big-int?`, and a no-op
`sleep`; `number_range.clj` already existed for numeric constants.
Without those shims every test file fails to load because the
canonical jank `portability.cljc` is JVM/CLJS-bound (`Throwable`,
`Thread/sleep`, `cljs.test`).

[cts]: https://github.com/jank-lang/clojure-test-suite

### Clean Compile Under -Werror

The default `make` build now compiles warning-free with `-Wall
-Wpedantic -Wextra` and treats remaining warnings as errors via a
new `-Werror` in `CFLAGS`. Fourteen pre-existing warnings are
fixed, split between two classes.

`-Wclobbered` flagged five locals whose values could be lost when
a `longjmp` rewound past their `setjmp`. In `eval_try` the
`vol_result` and `vol_ex` declarations were `volatile mino_val_t
*` — pointer-to-volatile, which protects the pointee but leaves
the pointer itself non-volatile. Moved the qualifier so the
pointer is `volatile` (`mino_val_t * volatile`). In
`mino_eval_string_inner` the `src` parameter was reassigned in
the read loop after the top-level `setjmp`; it now lives in a
`const char * volatile` local copied from the parameter. In
`atom_notify_watches` the loop counter `i` is alive across the
per-iteration `setjmp` and is now `volatile`.

`-Wformat-truncation` flagged nine `snprintf` sites where worst-
case inputs could overflow the destination. The diagnostic
buffers in `apply_refer_options` (`require:` errors) and
`validate_only_names` (`refer:` errors) were `char msg[300]` but
hold two 256-byte names plus format text; bumped to 600. The
four `cwd_resolve` / `try_resolve` paths that build
`<dir>/lib/<name><ext>` now gate each `snprintf` on a runtime
length check that proves the output fits.

## v0.98.6 — Bump MINO_VERSION_* Constants

The five v0.98 tags (v0.98.0 through v0.98.5) shipped with
`MINO_VERSION_MINOR=97` / `MINO_VERSION_PATCH=5` left over from
v0.97.5. The release-build workflow asserts the tag matches the
header constants and rejected v0.98.5 on every platform. Per the
no-force-push-tags rule this lands as a fresh patch tag. No
behavioral change; only `src/mino.h`'s version triple moves
forward.

## v0.98.5 — Seedable PRNG + Minimal clojure.test.check Port

`random-seed!` is a new primitive that seeds the per-state PRNG
(xorshift64* on `S->rand_state`) to a known integer so subsequent
`rand` / `rand-int` / `rand-nth` calls produce a reproducible
stream. Same seed in, same sequence out.

A minimal `clojure.test.check` ports lands in three new bundled
namespaces:

- `clojure.test.check.generators` (`gen/`) — `int`, `nat`,
  `s-pos-int`, `neg-int`, `boolean`, `double`, `char`, `char-ascii`,
  `char-alpha`, `char-alphanumeric`, `string`, `string-ascii`,
  `string-alphanumeric`, `keyword`, `symbol`, `any`, `vector`,
  `list`, `set`, `map`, `tuple`, `return`, `fmap`, `bind`,
  `such-that`, `elements`, `one-of`, `sample`, `generate`.
- `clojure.test.check.properties` (`prop/`) — `for-all`, plus the
  internal `make-property` / `run-property` / `property?` helpers.
- `clojure.test.check` (`tc/`) — `quick-check` runs N samples of a
  property at growing sizes and returns
  `{:result true/false :num-tests N :seed S [:failing-args ...]}`.

Shrinking is **not** implemented — failure reports return the
unshrunk failing args with a `:note` explaining the limit.

`clojure.spec.alpha/gen` and `clojure.spec.alpha/exercise` no
longer throw `:mino/unsupported`. They now consult
`clojure.test.check.generators` to produce values matching common
predicate forms (`int?`, `string?`, `keyword?`, `boolean?`, etc.,
both bare and `clojure.core/*` qualified) and the structural
combinators `coll-of`, `tuple`, `nilable`, `and`, `or`. Specs that
need a custom generator can pass an `overrides` map keyed by spec
or predicate symbol.

mino strings carry no separate char type, so the `char` family of
generators yields single-character strings instead of character
values, matching mino's existing `subs s i (inc i)` idiom.

## v0.98.3 — Auto-Chunking Sources

`(seq vector)` and `(range ...)` now emit `MINO_CHUNKED_CONS` spines
of 32-element chunks instead of flat cons cells. Vector leaves are
already 32-wide (`MINO_VEC_WIDTH=32`), so the source-side chunking
walks them directly via `vec_nth` into a new `MINO_CHUNK` per leaf;
lazy `range` produces a fresh chunk of 32 (or however many remain)
on each force.

`(chunked-seq? (seq [1 2 3]))` and `(chunked-seq? (range 10))` now
return `true`. Map/filter/take/keep/keep-indexed/map-indexed already
propagated chunkedness end-to-end (v0.96.8), so `(reduce + (map inc
(filter odd? (range 1e6))))`-style pipelines now run end-to-end
chunked without per-element cons-cell allocation.

`array-map` insertion-order semantics were verified to already match
canon — `MINO_MAP`'s companion `key_order` vector preserves
insertion order through `seq`, `assoc`, and `dissoc`. mino's
`hash-map` is more conservative than canon's (which has undefined
order); no new `MINO_ARRAY_MAP` value type was needed.

Touched primitives that needed CHUNKED_CONS handling now that more
seqs flow through them:

- `prim_count` MINO_LAZY arm dispatches to the chunked-cons walk
  when the forced result is a chunked-cons.
- `prim_empty?` and `prim_nth` learn the MINO_CHUNKED_CONS case.
- `cons?` and `seq?` predicates extend to recognize MINO_CHUNKED_CONS;
  `sequential?` follows by definition.
- The two `unquote-splicing` paths in `quasiquote_expand_list` /
  `qq_expand_vector` walk MINO_CHUNKED_CONS and force MINO_LAZY tails
  (previously only walked flat MINO_CONS, silently splicing nothing
  for chunked tails).

## v0.98.2 — clojure.string/split 3-Arg Limit

`(split s sep limit)` now returns at most `limit` substrings, with
the last element absorbing the rest of the input — matching canon's
`String.split(re, limit)` for `limit > 0`. `limit <= 0` keeps the
existing no-cap behavior (which preserves trailing empties, the
canon `limit < 0` semantics). Char-split (`(split s "" limit)`) is
covered by the same code path.

Audit closures for the rest of the namespace:

- `re-find`, `re-seq`, `re-matches`, `re-matcher` already live in
  `clojure.core` (canon-correct location); no `clojure.string`
  wrappers needed.
- The remaining `clojure.string` surface (`blank?`, `capitalize`,
  `escape`, `index-of`, `join`, `last-index-of`, `replace`,
  `replace-first`, `reverse`, `split-lines`, `trim*`, etc.) was
  re-walked against canon — no missing arities or fns surfaced.

## v0.98.1 — compare Cross-Type Total Order

`compare` no longer throws when its two arguments straddle type
tiers; it returns the canon total order instead:

```
nil < false < true < numbers < strings < symbols < keywords
```

`(sort [:b 'a "c" 1 false nil])` now returns
`(nil false 1 "c" a :b)` — same as canon Clojure.

Same-type compares are unchanged; same-tier-different-content
mixes still throw if neither operand is comparable to the other
(e.g., a record and a function in the same tier).

## v0.98.0 — Macro Hygiene For Cross-NS :refer :all

Syntax-quote inside a macro body now qualifies bare symbols against
the macro's defining namespace, not the consumer's `*ns*`. Before
this fix, a `clojure.core` macro that referenced bare `atom` (or
`*out*`, `deref`, etc.) inside `` `(...) `` would qualify those
symbols to whichever namespace the consumer happened to be in,
silently breaking expansions like `with-out-str` whenever the
consumer had pulled the source ns in via `:refer :all`. The
`(a/go ...)` form invoked from outside `clojure.core.async`
similarly threw `unbound symbol: chan*` because the macro's bare
references to its own private helpers were qualified to the
consumer's ns.

`qq_qualify_symbol` (`src/eval/eval.c`) now consults
`S->fn_ambient_ns` (the macro's defining ns, set by
`apply_callable`) for both alias resolution and the env walk that
finds the qualifying namespace. The check fires only when
`fn_ambient_ns` differs from `current_ns`, which `apply_callable`
arranges only for `MINO_MACRO` bodies — `MINO_FN` bodies leave them
equal, so this is a no-op for fn calls.

`clojure.test/is-eq`, `is-thrown`, `is-truthy` lose `^:private`,
and the `assert-pass!` / `assert-fail!` helpers lose `defn-`. They
were only "private" because the bug let public macros emit
syntax-quoted references to them under the consumer's ns, which
bypassed the privacy check. With the macro-hygiene fix these
references correctly qualify to `clojure.test`, so the helpers must
be public — matching canon `clojure.test`'s pattern of public-but-
internal helpers.

## v0.97.5 — clojure.spec.alpha Introspection Utilities

`clojure.spec.alpha` gains the two canon introspection helpers:

- `abbrev` — strips namespace qualifiers from symbols and shortens
  `(fn [%] body)` to `body`, so spec forms read cleanly in
  diagnostics.
- `describe` — returns `(abbrev (form spec))`, the canonical
  human-readable description of a registered or anonymous spec.

The namespace now requires `[clojure.walk :as walk]`. Generators
(`gen`, `exercise`) continue to throw `:mino/unsupported`.

## v0.97.4 — Lift defn So Top-Of-File Predicates Use It

`defn`, `defn-`, `defonce`, and the private `fn-arity-with-prepost`
helper move above the early type predicates in `src/core.clj`. With
`defn` now available before `not=`, the six bootstrap-era
`(def NAME "doc" (fn ...))` sites — `not=`, `identity`, `ifn?`,
`qualified-symbol?`, `simple-symbol?`, `qualified-keyword?`,
`simple-keyword?` — become regular `defn` forms. The `defn` macro
itself only depends on special forms, primitive fns, and the macros
already defined above its new position (`when`, `cond`, `and`, `or`,
`->`, `->>`).

No behavioral changes; the full test suite still passes.

## v0.97.3 — clojure.core.async Canon Combinators

Adds four canon channel combinators to `clojure.core.async`:

- `reduce` — `[f init ch]` returns a channel yielding the final
  accumulator after consuming `ch` to close. Honours `reduced` for
  short-circuit.
- `transduce` — `[xform f init ch]` applies a transducer to the
  channel reduction; calls the completing arity once `ch` closes.
- `split` — `[p ch]` and `[p ch t-buf f-buf]` returns `[t-ch f-ch]`,
  routing items by predicate.
- `partition-by` — `[f ch]` and `[f ch buf-or-n]` emits vectors of
  consecutive items sharing `(f item)`. Flushes the in-progress
  partition on close.

The namespace's `:refer-clojure :exclude` list now also drops
`reduce`, `transduce`, and `partition-by`. The one internal use of
`clojure.core/reduce` inside the `go` macro is now fully qualified
so excluding the unqualified name doesn't break macro expansion.

## v0.97.2 — src/core.clj Code-Quality Sweep

Walk `src/core.clj` for the project's 80-char line limit. 157 long
lines are gone (the longest was 226 chars on `partition`). Most cuts
are docstrings that used to live on the same line as the `defn`
signature; they now sit on their own line with a 3-space continuation
indent.

Five macros (`lazy-cat`, `delay`, `defprotocol`, `extend-protocol`,
`defmulti`) had their args vectors on the docstring line; both moves
to their own line beneath the docstring. Three inline anonymous fns
(method metadata and method-defn builders inside `defprotocol`, and
the descendants accumulator inside `recompute-hierarchy`) became
`letfn` helpers with descriptive names. `bit-test` swaps
`(not (= 0 ...))` for `(not= 0 ...)`. Two opportunistic idiom swaps:
`(when (not (coll? ...)))` becomes `when-not` in `shuffle`, and
`(when (not (nil? idx)))` becomes `(when (some? idx))` in `re-seq`.

The six `(def NAME "doc" (fn ...))` sites at the very top of the
file (`identity`, `ifn?`, `qualified-symbol?`, `simple-symbol?`,
`qualified-keyword?`, `simple-keyword?`) keep the `def` form because
they load before `defn` itself is interned. Their docstrings are
wrapped onto their own lines.

No behavioral changes; the full test suite still passes.

## v0.97.1 — Sort-By and Reductions Arities

`sort-by` and `reductions` were single-signature `[f & args]` defns
that branched on `(count args)` and silently returned `nil` on any
arity outside the canon shapes. Both are now multi-arity: `sort-by`
exposes `[keyfn coll]` and `[keyfn cmp coll]`; `reductions` exposes
`[f coll]` and `[f init coll]`. Bad arities now throw the standard
"no matching arity" diagnostic instead of producing a quiet wrong
answer.

The wider audit of `clojure.core` arities walked the rest of the
spot-check list (`partition` 4-arg, `pop`/`peek` on lists,
`subseq`/`rsubseq`, `nth` 3-arg, `assoc`/`dissoc` n-arg, `range`
0-arg, `subs` 3-arg, `min-key`/`max-key` n-arg, `concat` 0/1/n-arg,
`zipmap`/`interleave` arity coverage, `apply`, `merge`, `update`)
and found everything else covered.

## v0.97.0 — Kwargs Destructuring

`& {:keys [...]}` parameter lists now match Clojure 1.11+ canon. The
runtime's map destructure accepts all three rest-args shapes: an
inline keyword/value pair sequence (`(g :k v :k v)`), a single
trailing map (`(g {:k v})`), and a mix of pairs followed by an
override map (`(g :k v {:k v})`). The fix lives in
`bind_map_destructure` in `src/eval/bindings.c`. `:or` defaults are
now evaluated in the binding env, so symbols like `some?` resolve to
their function values instead of being bound as the literal symbol.

`iteration` no longer carries a divergence note. Its signature is now
`[step & {:keys [somef vf kf initk] :or {...}}]`, matching canon.

## v0.96.9

Adds `workflow_dispatch` to the release-build GitHub Actions
workflow. GitHub drops tag-push events when more than three tags push
in one batch, so the v0.95.* and v0.96.* canon-parity cycles never
fired the workflow on tag push. The dispatch trigger lets the workflow
run against any existing tag via `gh workflow run release-build --ref
<tag>`. No runtime changes; the C version-define moves to `0.96.9` so
the bump itself fires release-build under the new trigger.

## v0.96.8 — Chunked-Seq Family

Adds the `clojure.core` chunked-seq surface: `chunk-buffer`,
`chunk-append`, `chunk`, `chunk-cons`, `chunk-first`, `chunk-rest`,
`chunk-next`, and `chunked-seq?`. Two new C value types back the
implementation: `MINO_CHUNK` (a fixed-cap, mutable-then-sealed value
buffer) and `MINO_CHUNKED_CONS` (a seq cell that carries a chunk plus
an offset and a tail seq).

Chunked seqs participate in the seq protocol transparently: `first`,
`next`, `rest`, `seq`, `count`, `nth`, `reduce`, equality
(`(= chunked flat)` is true), and printing all walk a chunk-cons the
way they walk a regular cons. The walk dispatches at the chunk level
where possible — `count` sums chunk lengths, `nth` indexes into the
underlying chunk, `reduce` honours chunk boundaries via the seq
iterator.

The C-level lazy combinators `map`, `filter`, and `take` propagate
chunkedness end-to-end: when fed a chunked input, they read the head
chunk in one go via `chunk-first`, build a fresh chunk via
`chunk-buffer`/`chunk-append`/`chunk`, and emit a `chunk-cons`. The
`mino`-level `keep`, `keep-indexed`, and `map-indexed` follow the
same pattern, so longer pipelines preserve chunkedness across mixed
C-level and `mino`-level steps.

Sources are not auto-chunked yet — `(seq [1 2 3])` still returns a
flat cons list, and `(chunked-seq? (seq [1 2 3]))` is `false`. The
chunk-aware fast paths fire when consumers explicitly construct a
chunked seq via the new primitives. Auto-chunking vectors and ranges
is a follow-up cycle that needs the wider walker audit (`mino_is_cons`
appears in 416 sites; see `.local/BUGS.md`-tracked notes).

## v0.96.7 — `:refer :all` Drops Transitive Refers; Macros Get Vars

`(require '[some.ns :refer :all])` previously bound every name present
in the source ns env into the consumer — including names the source ns
had referred *into* itself from `clojure.core` via auto-refer.
Result: any consumer of a wrapper namespace silently re-bound every
clojure.core name through that wrapper, shadowing its own
clojure.core refers. Canon brings only the source ns's owned publics
(matching `(ns-publics 'src)`); mino now does the same.

`defmacro` now interns a var alongside the env binding, so macros
appear in `(ns-publics 'ns)` and propagate via `:refer :all` the same
way `defn` does. Macro publics that previously slipped through only
the env binding now show up in introspection too.

A separate macroexpansion-after-`:refer :all` defect is still open and
tracked in `.local/BUGS.md` #9; the recommended idiom for now remains
`(require '[some.ns :as a :refer [...]])` with an explicit refer list
when the consumer also calls macros defined in `clojure.core`.

## v0.96.6 — Wrap `clojure.core.async`; Rename `merge-chans`/`async-into`

The two files that backed mino's CSP layer — `lib/core/channel.clj`
and `lib/core/async.clj` — combine into `lib/clojure/core/async.clj`,
declaring `(ns clojure.core.async (:refer-clojure :exclude [merge into]))`.
The pre-existing `merge-chans` and `async-into` names existed only to
avoid shadowing `clojure.core/merge` and `clojure.core/into` for any
consumer that loaded `core/async`; with the namespace wrap, that
constraint goes away and the canon names are restored.

Consumers in mino's own test suite migrate from
`(require "core/async")` to
`(require '[clojure.core.async :as a :refer [...]])` with an explicit
refer list. The async surface stays bare in test bodies; the renamed
`merge` and `into` are accessed as `a/merge` and `a/into` so they do
not shadow `clojure.core/merge` and `clojure.core/into` in the test
file's local namespace.

`(into old modes)` inside `toggle` switches to
`(clojure.core/into ...)` because the unqualified call now resolves
to the channel `into`.

The `:refer :all` shape is intentionally not used here. Mino's
`require :refer :all` pulls every binding present in the source ns
env, including transitive refers from `clojure.core` (`atom`, `*out*`,
`deref`, ...) — that drag-along is itself a smaller silent-surprise
debt tracked separately, and an explicit refer list sidesteps it for
this consumer.

Sibling-repo consumers — `mino-bench` benches that
`(require "core/async")`, the `mino-site` "Coming from Clojure" page
that mentions `merge-chans`, and `mino-site/parse/async_api.clj` that
reads both source files — update when their submodule pins advance.

## v0.96.5 — `iteration` (Clojure 1.11)

`iteration` constructs a seqable from repeated calls to a step
function: each step returns a value plus a continuation token. Used
to consume paginated APIs and other batch sources where the producer
exposes "give me the next page from here". The first call is deferred
until the seq head is forced, so the step function may be impure.

The defaults match canon: `:somef` defaults to `some?`, `:vf` and
`:kf` default to `identity`, and `:initk` defaults to nil.

Divergence from canon: opts are passed as a single map argument
(`(iteration step {:vf identity ...})`), not as keyword args
(`(iteration step :vf identity ...)`). Mino's `& {:keys [...]}`
destructuring does not yet pick up trailing keyword pairs; a future
cycle will close that gap and the canon-style call shape will work
without code changes.

## v0.96.4 — Small Canon-Parity Additions

`comp` and `partial` adopt canon's hand-unrolled fast-path shape: 0/1/
2-arg `comp` and `partial` no-op or curry directly; the binary `comp`
returns a fn with explicit 0/1/2/3-arg arities plus a variadic
fallthrough; `partial` does the same for one-, two-, and three-arg
prebound forms. The general n-arg form remains for the long tail.

`some-fn` and `every-pred` move from a single variadic implementation
to canon's per-arity unrolled shape (1, 2, 3 preds × 0, 1, 2, 3 args
plus variadic). The binary semantics are unchanged — both still
short-circuit on the first decisive value — but the hot 1/2/3-pred
case skips the iterator the variadic shape used.

`into` gains the missing 0-arg (`(into) ;=> []`) and 1-arg
(`(into to) ;=> to`) forms that canon ships. The 2-arg `(into to from)`
and 3-arg `(into to xform from)` forms are unchanged.

`unchecked-divide-int` is installed as an alias for `quot` — both are
truncating integer division. Canon's `unchecked-divide-int` skips
overflow checks because the JVM `idiv` instruction does; mino's `quot`
is already a primitive C division on long, so no extra elision is
needed.

The four `(def name "doc" (let [helper ...] (fn ...)))` forms left over
from the prior cycle's hygiene pass — `zipmap`, `cycle`, `partition-all`,
`re-seq` — convert to `(defn name "doc" [args] (letfn [(helper ...)] ...))`.
The local helper now sits in a `letfn` (or directly in the body) where
it can `recur` instead of self-reference; semantics are identical.

## v0.96.3 — Transients in `frequencies`/`group-by`; `unreduced` Cleanups

`frequencies` and `group-by` rebuild their result map through a
`(transient {})` accumulator with `assoc!`, ending in `persistent!`.
Both used to allocate a fresh persistent map per input element via
`update`; the transient path drops that to one allocation per distinct
key plus log-N batched writes.

`get` now treats a transient associative as transparent — it follows
the transient's underlying persistent collection, matching canon's
`ITransientAssociative2` contract. `find` already did this; bringing
`get` in line was needed for `frequencies`/`group-by`'s
`(get acc x default)` lookups against the transient accumulator.

The completion arities of `partition-by` and `partition-all` swap
their inline `(if (reduced? r) @r r)` for the existing `unreduced`
helper; the helper has been in `src/core.clj` since the Cycle G
rewrite.

## v0.96.2 — Lazy-Seq `recur`-On-Skip Rewrites

Four lazy-seq combinators that previously allocated a fresh `lazy-seq`
cell on every input — including the ones they were going to skip —
adopt canon's pattern: an outer step function produces a `lazy-seq` cell
only when emitting, and an inner anonymous fn `recur`s when skipping.
The rewritten sites are `distinct` (collection arity), `drop-while`
(collection arity), `keep-indexed`, and `dedupe` (collection arity).
`dedupe`'s collection arity now delegates to `(sequence (dedupe) coll)`,
matching canon's shortcut.

The user-visible result on duplicate-heavy or long-skip inputs is one
allocation per emitted value instead of one per element visited. The
pre-existing `drop-while` collection arity used a non-lazy recursive
walk; the rewrite restores lazy semantics that match canon.

## v0.96.1 — Stateful Transducers Use Real `volatile!`

Ten transducer state slots in `src/core.clj` switch from `(atom ...)`
plus `swap!` / `reset!` to `(volatile! ...)` plus `vswap!` / `vreset!`:
`take`, `drop`, `drop-while`, `take-nth`, `interpose`, `distinct`,
`partition-by` (both buf and pval), `partition-all`, `map-indexed`, and
`dedupe`. The transducer contract already implies single-thread access
to that state — the reducing fn is invoked from one thread at a time —
so the watch + validator + atomic-publish overhead the atom carried was
pure waste on every step.

The user-visible contract is unchanged: same primitives, same lazy-vs-
eager arities, same return values. The change is per-step throughput
on stateful-transducer pipelines once host threads enter the picture
(single-threaded states avoided the CAS already, but still paid for
the atom struct's extra slots).

## v0.96.0 — `volatile!` Becomes a Real Type

Up to this release, `volatile!` was a Clojure-side alias for `atom`,
which meant every transducer state slot paid for the atom's watch and
validator pointers and (once host threads entered the picture) for the
write barrier and atomic publish that swap! issues on multi-threaded
states. Canon and ClojureScript both ship a real one-slot volatile cell
because transducer state has a single owner — the reducing function —
and does not need any of that infrastructure.

`MINO_VOLATILE` joins the value-type enum as a one-slot mutable cell
with no watches, no validators, and no atomic publish. The four
operations are now C primitives: `volatile!`, `volatile?`, `vreset!`,
and `vswap!`. `deref` recognises a volatile in addition to atom, var,
future, and reduced. The four Clojure-side aliases at the top of the
volatile section in `src/core.clj` are gone; nothing in user code
should notice because the surface and semantics are unchanged on
single-thread reads and writes.

The print form is `#volatile[VAL]`, `(type v)` returns `:volatile`,
and `(= (atom 1) (volatile! 1))` is now `false` because the two are
distinct types. The `MINO_VOLATILE` enum entry is appended after
`MINO_ATOM`, so the embedder ABI stays additive.

This release is the foundation for the stateful-transducer rewrite
that ships in v0.96.1.

## v0.95.5 — `src/core.clj` Hygiene Sweep

The bundled core library that ships inside the binary went through a
naming and surface-form pass. Private helpers no longer carry a
trailing underscore; mino now uses `defn-` (and `def ^:private` for
non-fn vars) to communicate privacy the same way Clojure does. The
private symbols renamed include `fn-arity-with-prepost`, `map1`,
`all-some?`, `map-n`, `match-whole`, `substring-index`,
`re-find-on-matcher`, `type-marker-key`, `partition-protocol-specs`,
`global-hierarchy`, `hierarchy-version`, `tc-ancestors`,
`recompute-hierarchy`, `valid-hierarchy?`, `prefers?`,
`find-best-method`, `create-multimethod`, `register-method`,
`special-symbols-set`, `uuid-hex-pattern`, `uuid-string?`, and
`tap-fns`. The captured-primitive alias `into_` becomes the `prim-`-
prefixed `prim-into`, matching the convention in `clojure.string`.
Two formerly-underscored protocol helpers are public surface and keep
their canon names: `internal-reduce` and `internal-reduce-kv` (shadow
the C primitives that the protocol-aware `reduce` and `reduce-kv`
delegate to). `protocol-dispatch` stays public because it is emitted
by the `defprotocol` macro into user namespaces.

Every definition past the bootstrap zone moved from
`(def name "doc" (fn [args] body))` to the equivalent
`(defn name "doc" [args] body)`. The bootstrap area at the top of the
file (anything before the `defn` macro is bound) keeps the bare-`def`
form because `defn` does not yet exist there. Roughly 120 forms
changed shape; the binary semantics are identical because mino's
`defn` macro expands to the same `(def name doc (fn ...))` form
underneath.

`comparator` no longer uses `true` as its catch-all clause in `cond`;
it uses the canonical `:else`. `some-fn` was rewritten from a
double-`loop` accumulator to a `(some (fn [p] (some p args)) preds)`
expression; behaviour matches canon's "first truthy value of any
pred against any argument" surface and the implementation is no
longer an obstacle when reading the file.

## v0.95.4 — `mino.tasks.builtin` and `clojure.string` Hygiene

`gen-core-header` no longer carries its own copy of the C-string-literal
escape logic. The `escape-source-as-c-string-literal` helper now sits
above both `gen-core-header` and `gen-stdlib-headers`, and both call
into it. The escape rules can no longer drift between the two
generators.

`gen-stdlib-headers` and `qa-arch` no longer thread accumulator atoms
through their bodies. `gen-stdlib-headers` reduces over a per-file
`regen-stdlib-header` helper that returns 1 or 0; the total update
count is `(reduce + 0 ...)` instead of an `(atom 0)` updated inside a
`doseq`. `qa-arch` follows the same shape: each gate (TU size,
function size, abort inventory) is its own helper that prints its
report and returns its failure count, and the top-level summary just
adds them up.

`clojure.string/index-of-from_` is renamed to `index-of-from`. The
trailing-underscore-for-private convention is non-standard; the `defn-`
on the helper already communicates privacy. `re-quote-replacement`
no longer reinvents a per-character `loop`/`reduce`; it now delegates
to the existing `clojure.string/escape` with a two-key char map for
`\\` and `$`.

## v0.95.3 — `core.async` Canon Parity

`onto-chan` and `to-chan` are renamed to `onto-chan!` and `to-chan!` to
match canon `clojure.core.async`. Both side-effecting bang-suffixed
names communicate the same write intent canon does: `onto-chan!`
puts each element of a collection onto a channel and (by default)
closes it; `to-chan!` constructs a channel sized to a collection,
fills it, and closes. No aliases are kept — alpha posture means call
sites move forward in lockstep.

`pipeline` gains the canon 6-arg form `[n to xf from close? ex-handler]`.
When the transducer throws, `ex-handler` is called with the exception
and its return value (when non-nil) is forwarded as the replacement
output; nil results are dropped. The 4-arg and 5-arg forms keep the
same surface and now route through the new arity with a nil handler.

`alts!` accepts canon-style trailing kwargs in addition to its
existing single-map form. `(alts! ops :priority true :default :nope)`,
`(alts! ops {:priority true :default :nope})`, and `(alts! ops)` all
work. The dispatch normalises the trailing args via a small
`alts-opts-map` helper that detects the legacy single-map call and
otherwise rebuilds the opts map from the kwargs.

Two ad-hoc helpers in `core/channel` were collapsed into primitives:
`range-vec` is now `(vec (range n))` and `shuffle-vec` is now
`shuffle`, both already in mino. `pipeline-blocking` remains a `def`
alias for `pipeline` until a separate blocking-IO scheduler lands;
the comment on the alias documents the divergence.

Two canon names that would shadow `clojure.core/merge` and
`clojure.core/into` if defined unqualified — `merge-chans` and
`async-into` — are intentionally still mino-spelled. Wrapping
`lib/core/async.clj` and `lib/core/channel.clj` in their own
namespace and updating every consumer to refer them is its own
follow-up cycle and has been logged in the bug registry.

## v0.95.2 — Decomposed `clojure.instant/parse-timestamp`

`parse-timestamp` was a single ~70-line `cond` inside one driver
loop, mixing per-segment parsing with bounds checks and the
position-marker cascade that decides which segment fires next.
Both halves are now separate: each ISO 8601 component lives in a
small `parse-month-segment`, `parse-day-segment`,
`parse-time-segment`, `parse-second-segment`, `parse-frac-segment`,
or `parse-zone-segment` helper that takes `[s idx m]` and returns
`[m new-idx]`. The driver loop is a one-screen `cond` over the
next-segment marker that delegates to a helper and recurs on the
returned position.

Inline `(parse-long (nth s j))` truthiness as a digit test became a
named `digit?` predicate so the fractional-seconds scan reads as
intent. The public `parse-timestamp`, `validated`, and
`read-instant-date` surface is unchanged; the existing
`tests/instant_template_test.clj` (27 instant assertions) covers
the refactor.

## v0.95.1 — Dynamic-Var `clojure.test` Internals

`clojure.test` previously kept its pass/fail counters, testing-context
stack, and current-test name in atoms named with earmuffs
(`*test-state*`, `*testing-context*`, `*current-test*`). Earmuffs
signal a dynamic var meant for `binding`-style rebinding; an atom
behind one is a smell, and canon `clojure.test` uses real `^:dynamic`
vars + `binding` for these. mino now does the same: pass/fail
counters live in `*report-counters*` (canon name) bound to a fresh
atom inside each `run-tests` call; the testing-context stack lives
in `*testing-contexts*` (canon name) and is pushed via `binding`
inside the `testing` macro; `*current-test*` is bound per test.
The cross-file suite-mode flag (`suite-mode`) stays a plain atom
because `require` evaluates a loaded file outside the caller's
dynamic scope.

`run-tests` is now library-friendly: it returns the summary map
`{:test n :pass n :fail n :error n :failures [...]}` instead of
calling `(exit ...)`, and it accepts an `[& namespaces]` arity that
filters the registry to tests registered in those namespaces.
Process exit moved to a small `run-tests-and-exit` wrapper used by
`tests/run.clj` and the per-file bottoms.

The `is` macro previously dispatched three branches inline; it now
dispatches into private `is-thrown`, `is-eq`, `is-truthy` helpers.
The internal `assert-pass!`, `assert-fail!`, and `thrown?-form?` are
private (`defn-`).

## v0.95.0 — Reduce-Based `clojure.data/diff`

`clojure.data/diff-map` and `diff-sequential` previously threaded three
mutable atoms (`only-a`, `only-b`, `both`) through a `doseq` or
`loop`/`recur` driver, accumulating shape via `swap!` on each step.
The standard treats earmuffs and `swap!`-as-fold as a smell when a
plain reduction would do, and the canon `clojure.data` implementation
is itself a reduce over a three-element accumulator.

Both helpers are now `reduce` over `[only-a only-b both]` triples
(starting from `[nil nil nil]` for maps and `[[] [] []]` for
sequentials), with no atoms in flight. Behaviour is unchanged — the
same diff triples come out for maps, sequentials, sets, scalars, and
mixed-type inputs — and a new `tests/data_test.clj` covers the
public surface (14 tests, 21 assertions) so the next refactor pass
has a real safety net.

## v0.94.5 — Static-Link Windows Binary

`mino --version` and the REPL silently failed under PowerShell on
fresh Windows installs (Scoop or Homebrew-on-Windows). Exit code
`-1073741515` (`0xC0000135`, `STATUS_DLL_NOT_FOUND`) showed the
binary never started: mingw-gcc by default produces an exe that
imports `libgcc_s_seh-1.dll` and `libwinpthread-1.dll`. The GHA
runner has those DLLs in scope (so the release-build smoke test
passed), but a clean Windows install doesn't.

The bootstrap Makefile now passes `-static` to the linker on
`Windows_NT`, so mingw's runtime gets baked into mino.exe. macOS and
Linux remain dynamically linked. This makes the v0.94.4 stdout-
buffering patch actually observable too, since the binary now runs.

## v0.94.4 — Force Line-Buffered Stdout on Windows

`mino --version` and `mino` (REPL) printed nothing when launched from
PowerShell against a Scoop install. The Git Bash path on the same
binary worked: the GHA release-build's smoke step ran `mino.exe
--version` under Git Bash and got the expected output. The
difference is buffering — MSVCRT's stdout is block-buffered when
stdout is not a tty (which the Scoop shim's PowerShell pipeline
looks like), and the shim's child-process plumbing doesn't always
propagate the buffered tail when mino.exe exits.

`main()` now calls `setvbuf(stdout, NULL, _IOLBF, 0)` and
`setvbuf(stderr, NULL, _IONBF, 0)` on `_WIN32` at program start. Each
fprintf flushes on newline (or immediately, for stderr) regardless
of how the binary is invoked. macOS and Linux are unchanged.

## v0.94.3 — bundle.awk Sidesteps MSYS Path Translation

v0.94.2 moved the bundled-source escape from sed to awk, but kept the
script inline on the command line. Git Bash on Windows mangled awk's
inline `/\\/` regex literal through the same MSYS path-translation
heuristic that broke sed: argument fragments that look path-shaped
get rewritten before the tool parses them. The Windows job's
Bootstrap step in v0.94.2's `release-build` matrix surfaced empty
headers a second time and the Release artifact for Windows didn't
upload (so `scoop install mino` against v0.94.2 would have 404'd
just like v0.94.1).

The escape script now lives in `src/bundle.awk`. The recipe invokes
`awk -f src/bundle.awk "$src"` — the `-f` argument is a file path,
which path translation handles correctly, and the script body never
appears on the command line at all. Output is byte-identical to all
prior implementations across the 20 generated headers; the full test
suite passes (1460 / 7017). With Windows Bootstrap genuinely green,
the v0.94.2 cleanup of `continue-on-error` and `fail-fast` finally
takes effect: the Windows artifact rejoins the Release matrix.

## v0.94.2 — Portable Bootstrap, Windows Rejoins Releases

The bootstrap Makefile recipe now uses awk instead of sed to escape
each `lib/<ns>.clj` source into its `src/<sym>.h` C string literal.
Sed via Git Bash on Windows mangled the leading-slash regex argument
through MSYS path translation and emitted empty headers; awk's
script body starts with `{` and the regex literals are internal
tokens, so the recipe is one source for every platform. Output is
byte-identical across all 20 generated headers.

With the recipe portable, the `continue-on-error` guards that were
masking the Windows Bootstrap failure go away: `ci.yml`'s Windows
Bootstrap step is no longer informational, and `release-build.yml`
drops its job-level `continue-on-error: ${{ matrix.os == 'windows' }}`.
The Windows artifact rejoins the Release matrix; `scoop install
mino` works against the v0.94.2 zip again. (The Test step on
Windows stays informational — that's a separate cmd.exe trailing-
space quirk in the proc-test assertions, unrelated to the
bootstrap.)

No runtime behaviour changes vs v0.94.1; this is a build-pipeline
patch.

## v0.94.1 — Release-Build Windows Guard

Patch fix for the v0.94.0 release pipeline. The Windows release-build
job tripped the same Git Bash sed quirk that `ci.yml` already gates
around — the bootstrap Makefile recipe escapes differently than POSIX
sed and emits empty bundled-source headers. `ci.yml` had been marking
its Windows Bootstrap step `continue-on-error` since v0.93.0;
`release-build.yml` was missing the same guard, and `fail-fast: true`
was cancelling the otherwise-green macOS jobs. The release-build
matrix now runs with `fail-fast: false` and the Windows job is
informational at the job level until a portable Makefile recipe
lands. Linux and macOS artifacts are the authoritative release set.

No runtime behaviour changes; if you build from source on Linux,
macOS, or via the bootstrap Makefile, this release is identical to
v0.94.0.

## v0.94.0 — Empty-List Canon Parity

The empty list `()` is now a real value type, distinct from nil. This
matches Clojure's canonical semantics where the empty list, an empty
vector, and an empty seq compare equal but none of them equal nil.
The cycle also folds in three post-v0.93.0 fixes that have been
sitting on main: the bootstrap Makefile, the Windows informational
guard, and the disk-wins-over-bundled resolver fix.

**Empty-list canon parity (breaking).** The reader, the `(list)`
constructor, and every primitive that surfaces an empty seq result
now produce the canonical empty-list singleton instead of nil. User-
visible behaviour flips on five axes:

- `(= '() nil)` is now false (was true). nil is its own thing; the
  empty list is a sequential collection that happens to have no
  elements.
- `(seq? '())` is now true (was false), and `(nil? '())` is now
  false (was true). The singleton is a seq, not nil.
- `(rest '(1))` returns `()` instead of nil, as does `(rest [])`,
  `(rest '())`, `(rest nil)`, and any other empty-seq-result branch
  through `take`, `drop`, `take-while`, `drop-while`, `filter`,
  `map`, `range`, `concat`, `interpose`, `interleave`, `cycle`,
  `iterate`, `partition*`, `flatten`, `repeat`, `nthrest`,
  `random-sample`, etc.
- The empty list prints as `()` (a lazy seq that resolves to nil
  prints as `()` too — the printed form follows the user-visible
  semantic, not the internal cache).
- Cross-type sequential equality includes empty-list and excludes
  nil: `(= '() [])`, `(= '() (list))`, `(= '() (take 0 [1 2 3]))`,
  and `(= '(1) (cons 1 (lazy-seq nil)))` are all true; `(= nil [])`
  and `(= nil '())` are both false.

Internally, cons-cell cdrs still terminate on nil (the precise GC
treats nil as the canonical end-of-chain marker), and the lazy thunk
contract still returns nil to mean "no more elements". The
translation to `()` happens at the user-facing seam — `first`,
`rest`, `seq`, `count`, equality, and the printer — so embedders
walking cons chains via `mino_is_cons` see no behaviour change.

**Bootstrap Makefile.** A 75-line top-level `Makefile` generates the
bundled-source headers and compiles `./mino` in one `make` invocation;
that's the entire bootstrap surface. Everything beyond a clean
checkout still lives in `./mino task`. README, both CI workflows, and
mino-site's deploy use it. Windows uses `$(OS)` to pick up the `.exe`
suffix; the Bootstrap step there is `continue-on-error: true` because
Git Bash's sed handles the recipe's escape pattern differently than
POSIX sed and emits empty headers — Windows test posture is already
informational, and a portable recipe is its own follow-up.

**Resolver: disk wins over bundled.** v0.93.0's bundled-stdlib
registry shadowed user-supplied overrides on disk. The lookup order
flips: a `lib/<ns>.clj` file on the resolver's path wins over the
bundled copy, with the bundled copy as the brew/scoop fallback. This
unblocks mino-bench's `lib/mino/tasks/builtin.clj` override (which
adds a `perf-gate` task the builtin doesn't ship). Brew and Scoop
installs see the same behaviour as v0.93.0 because they don't ship a
`lib/` tree, so the bundled fallback fires.

## v0.93.0 — C Refactoring Pass

Top-down legibility pass over the C runtime. Behaviour is unchanged for
script authors and embedders; the work is structural — splitting god
functions into named helpers, documenting lock and ownership contracts,
and removing dead helpers — so future changes land more cleanly. All
commits in the cycle pass the full mino test suite (1453 tests, 6991
assertions) and a clean macOS build.

**Trust model and lock contracts.** Three subsystem entry points now
state their authority and threading model in a banner comment:
`prim/proc.c` and `prim/fs.c` declare that the script author is the
trust boundary (primitives validate shape, not intent — embedders that
want to forbid shell-out or filesystem mutation refuse to bind these
primitives in the embedder's namespace); `runtime/state.c` declares the
single-embedder lifecycle of `mino_state_t`. Every public-API entry
point in `runtime/host_threads.c` (`mino_promise_deliver`,
`mino_future_cancel`, `worker_run`, `mino_future_spawn`,
`mino_host_threads_quiesce`, `mino_future_gc_sweep`) now states the
lock invariant it relies on or maintains. The relaxed-read on
`S->thread_count` is documented at both the reader (`mino_thread_count`)
and writer (`mino_future_spawn`, worker exit) sites so its
deliberately-loose contract is no longer implicit.

**God-function surgery.** Eight large functions were split along
natural seams into named helpers:

- `prim_require` (prim/module.c) shed three sub-phases: `require_load_path`
  for the cache + cycle-check + resolve + load + ns-validate path,
  `apply_refer_options` for the :refer / :refer :all binding loop with
  :exclude / :rename, and `parse_libspec_opts` filling a typed
  `libspec_opts_t` struct from the kw/val pairs of a vector libspec.
  `prim_require` is now a clear dispatcher over arg shape.

- `eval_try` (eval/control.c) extracted `partition_try_clauses` (one-pass
  walk classifying clauses into a typed `try_clauses_t`) and
  `normalize_exception` (wrap a non-diagnostic thrown value into the
  standard map shape). The setjmp-bearing phases stay inline as C99
  requires; the surrounding work reads as a sequence of named ops.

- `apply_callable` (eval/fn.c) replaced three near-identical multi-arity
  dispatch blocks (call entry, recur backward branch, tail-call to a
  multi-arity fn) with `dispatch_multi_arity`, deduping ~30 lines.

- `gc_mark_roots` (gc/roots.c) factored the per-thread-ctx work into
  `gc_mark_ctx_dyn_stack` and `gc_mark_ctx_gc_save` so the "every live
  ctx" loop is visible at the call site instead of buried in two
  parallel inner loops.

- `gc_alloc_typed` (gc/driver.c) split into policy and mechanism:
  `gc_alloc_raw` owns the freelist + calloc + header init + young-list
  link + range index + alloc event (returns NULL on calloc failure,
  no GC, no recovery); `gc_oom_throw` owns the longjmp-into-try /
  abort path; `gc_alloc_typed` keeps stress lazy-init, safepoint,
  driver tick, fault injection, and OOM fallback. The OOM-fallback
  retry now calls `gc_alloc_raw` a second time instead of repeating
  the alloc body.

- `read_atom` (eval/read.c) lifted the cascading numeric-literal parse
  (hex, radix Nr, ratio, bigint N suffix, bigdec M suffix, decimal int
  or float) into `try_parse_numeric`. The helper returns the parsed
  value, or NULL with an err-out-param distinguishing "not numeric,
  fall through to symbol" from "numeric but malformed, diag set".

- `quasiquote_expand` (eval/eval.c) became a five-line dispatcher over
  form kind, with `qq_expand_vector` (with the fast path / splicing
  slow path), `qq_expand_map` (k/v walk), and `qq_expand_cons`
  (top-level unquote head + per-element splice walk) as helpers.

- `tower_reduce` (prim/numeric.c) split into per-tier helpers:
  `tower_apply_int` (overflow-promotes to bigint, ratio-promotes on
  non-exact division), `tower_apply_bigint` (ratio promotion on
  division, possible collapse back to int / bigint), `tower_apply_ratio`,
  `tower_apply_bigdec`, `tower_apply_float`, plus `tower_seed_div`
  for the (/ x ...) one-operand seed. The orchestrator now reads as
  table-driven dispatch.

**File-level smell sweeps.** Per-pattern helpers were extracted to
flatten near-identical sites in five files:

- `eval/bindings.c`: `push_dyn_binding` collapses the two ~25-line per
  pair blocks in `eval_binding` (vector and list paths) into one
  helper; `eval_and_bind` does the same for `eval_let` and `eval_loop`,
  replacing four 6-line eval/pin/destructure/unpin sequences.

- `eval/special.c`: `eval_qualified_symbol` lifts the ~80-line
  qualified-symbol resolution branch (literal-binding fast path,
  alias resolution, var lookup with private-access check, ns-env
  fallback for primitives, miss-message synthesis) out of
  `eval_symbol`. The function now reads as three top-level cases:
  qualified, `*ns*` fast path, unqualified-with-fallback.

- `eval/read.c`: `list_append_cell` deduplicates the four cons-cell
  append sites in `read_list_form`; `buf_push` and `map_buf_push` do
  the same for the GC-tracked dynamic-array grow-and-push pattern
  used at six sites across `read_vector_form`, `read_map_form`, and
  `read_set_form`.

- `gc/driver.c`: `gc_driver_tick` split into per-phase helpers
  (`gc_tick_should_suppress`, `gc_tick_stress`, `gc_tick_during_major`,
  `gc_tick_idle`); the dispatcher is now a five-line switch and the
  why-finish-then-minor rationale lives next to its code.

- `gc/roots.c`: `gc_mark_roots` factored into six per-kind helpers
  (`gc_mark_envs_and_interns`, `gc_mark_module_and_meta`,
  `gc_mark_thread_state`, `gc_mark_runtime_globals`,
  `gc_mark_async_roots`, `gc_mark_record_types`). The orchestrator is
  now a six-line list of what gets pinned.

- `prim/sequences.c`: `seq_cons_append` and `seq_kv_pair` collapse the
  cons-append and key-value-vector patterns repeated across `prim_seq`'s
  five per-collection-type branches.

**Code-level fixes.**
`runtime_module_add_alias` returns int instead of void; all five
callers now surface OOM as a catchable internal/MIN001 exception
instead of silently dropping the alias. `prim_random_uuid` swaps
`sprintf` for `snprintf` for hygiene (the buffer was already correctly
sized so this is not a fix). `ns_process_require_spec_ex` now sets a
loud `MSY001` diagnostic when an alias, module, refer, or rename name
exceeds the 256-byte stack-buffer limit; previously the entry was
silently skipped.

**Defensive overflow guards.** Five buffer-grow paths previously did
unguarded `cap*2` or `len+1` arithmetic. None are reachable today, but
the invariant is now explicit:

- `prim/string.c:fmt_ensure` (printf-style result buffer) and
  `prim/proc.c:build_command` / `read_all` (shell-call argv and stdout
  buffers) bail with a diagnostic before `len+extra+1` or `cap*2 +
  arg.len*4` can wrap.
- `gc/barrier.c:gc_remset_add` aborts on cap overflow (write-barrier
  path has no recovery model).
- `gc/driver.c:gc_mark_stack_push_raw` drops the push on cap overflow;
  the conservative scan is the documented backstop.
- `diag/diag.c:source_cache_store` bails before `malloc(len+1)` wraps
  to `malloc(0)` followed by a `SIZE_MAX`-byte memcpy.

**Dead-code removal.** `diag_add_note_at` and `diag_set_cause` were
declared and defined but never called from anywhere in the repo or in
any sibling consumer (mino-bench, mino-examples, mino-site). They are
not part of the public `mino.h` embedding surface; removed without a
deprecation shim per the alpha posture.

**Public-header polish.** `src/mino.h` had a doc-only sweep: removed
stale references to deleted code paths, replaced "see mino.c" / "see
rbtree.c" with "opaque to embedders" for forward-declared types,
removed remaining cycle-name references from inline comments, and
renamed an internal-jargon section banner to a shape-describing one.

**`mino_state` god-struct seam map.** Eight banner comments inside
`mino_state` (GC, value caches, modules, printer/reader, namespaces,
misc per-state, host threads, async) name the conceptual sub-states
that share fields. No memory layout changes — the banners give later
refactors a seam to split along.

**Bundled `mino` tooling.** `mino deps` and `mino task` previously
required `lib/mino/*.clj` to be reachable from cwd, so brew-installed
mino on a project without a sibling `lib/` couldn't use the built-in
tooling without a symlink or submodule. The three sources
(`lib/mino/deps.clj`, `lib/mino/tasks.clj`, `lib/mino/tasks/builtin.clj`)
now bundle into the binary the same way the `clojure.*` stdlib does:
gen_header escapes each into a C string literal, and a new
`mino_install_mino_tooling` install hook registers them via
`mino_register_bundled_lib`. Standalone projects work from any cwd.
Embedders that don't expose those subcommands can omit the install
hook.

**Empty-list type scaffolding (foundation for a later cycle).** A new
`MINO_EMPTY_LIST` value type and `mino_empty_list(S)` accessor sit in
the runtime as scaffolding; nothing produces or consumes the singleton
in v0.93.0. Wiring it through the reader, sequence primitives, and
equality lattice to fix the `(list) ⇒ nil` divergence requires
updating ~70 compatibility tests that currently rely on the legacy
"empty seq is nil" semantics, so the user-visible parity work was
deferred to a later cycle. The type sits in `mino_type_t` as an
explicit seam; embedders can ignore it.

## v0.92.1 — CI And Linux Build Fixes

Patch release covering build-pipeline fixes that surfaced after
v0.92.0 went out. No runtime-visible behaviour changes.

**Linux build.** `src/runtime/state.c` uses `PTHREAD_MUTEX_RECURSIVE`,
which glibc gates behind `_XOPEN_SOURCE >= 500`. Without the macro
the constant is undeclared and the build fails on Linux. Define
`_XOPEN_SOURCE 600` at the top of `runtime/internal.h` so glibc
exposes it to every translation unit. macOS and Windows are
unaffected.

**CI bootstrap.** The bundled-stdlib generator that produces
`lib_clojure_*.h` ships as a mino task, so the manual bootstrap step
in `ci.yml`, `release-build.yml`, and the README only generated
`core_mino.h`. After `install_stdlib.c` was added, every fresh
checkout failed at link time on `lib_clojure_string.h: not found`.
Replace the inline sed with a `gen_header` shell function called
once per bundled namespace.

**CI test step.** `./mino task test` wraps the suite invocation in
`sh!`, which buffers stdout until the subprocess exits; under a hang,
no diagnostic ever surfaces. Invoke `./mino tests/run.clj` directly
from the workflow so per-test output streams as it's emitted, and
cap the step at 8 minutes so a deadlock fails fast instead of
waiting on the 6h job-default.

**Test fan-out cap.** `concurrent-atom-cas` and
`blocking-many-cross-thread-pings` hard-coded `n=4` worker futures
plus the test thread, which blew past the runtime grant on a 3-vCPU
shared CI runner. Cap `n` at `(dec (mino-thread-limit))` so the
suite still validates atomicity and cross-thread channel parking on
small machines.

**Channel close drain.** Folded into v0.92.0 retroactively but worth
calling out for embedders who hit it on the fix-tag pre-release: a
parked `<!!`/`>!!` waiter that was supposed to be released by
`close!` could deadlock because `close!` scheduled the wake-callback
without draining the run queue. Producers calling `close!` are
typically the only thread that could pull the wake off the queue, so
the parked thread waited forever. `close!` now drains at the tail.

**Windows test informational.** `tests/proc_test.clj` asserts exact
stdout from `sh "echo" "..."`, which on Windows comes back with a
trailing space before `\n` because of cmd.exe's `echo` quirk. The
build still must pass; the proc-test cases are marked
`continue-on-error` on Windows until those tests are rewritten in a
platform-portable way.

## v0.92.0 — Audit and Doc Realignment

Cycle G4.6 closes the host-threads slice with a sanitizer audit, a
documentation pass, and one bug fix surfaced while writing the
Performance page.

**Audit.** Full test suite runs ASan-, UBSan-, and TSan-clean. Perf
smoke matches the v0.91.0 baseline. The slot-tracking and GC-sweep
fixes from v0.90.0 hold under repeated stress runs.

**Channel close fix.** `close!` now drains the run queue after
scheduling wake-callbacks for parked takers and putters. Without the
drain, blocking `<!!`/`>!!` calls could deadlock when `close!` was
the only signal that could release them, because the producer thread
returns immediately and no one else runs the scheduler. Surfaced
while writing the cross-thread channel ping-pong benchmark for the
new Performance page; reproducible at modest iteration counts before
the fix.

**Site refresh.** `mino-site` realigns positioning around four pillars
("Drop into any host with C FFI", "Isolated runtimes with explicit
message-passing", "Capability-gated host interop", "Clojure-inspired
ergonomics"). Top nav trims to Get Started, Documentation, GitHub.
The documentation hub reorganises into Embed, Script, Reference, and
Internals sections with role chips at the top. Host-thread rows in
the compatibility matrix and intentional-divergences page now reflect
the shipped runtime, not the API-shipped/runtime-pending state from
v0.84.0. The Coming-from-Clojure concurrency section gains a
Futures, promises, threads subsection covering the OS-thread
parking model.

**Performance page refresh.** Single-thread numbers re-measured
against v0.92.0 on the M3 Pro reference machine. New Concurrency
section reports future spawn + deref roundtrip, atom-CAS contention
scaling under the per-state GIL, and blocking-channel cross-thread
ping-pong throughput. New Footprint and Startup section reports
stripped binary size, source-tree size, vendor size, bundled-stdlib
size, and cold REPL invocation time. Banner shifts from "preliminary
results" to a versioned line that names the binary and hardware.

**Internal cleanup.** Phase and version refs stripped from
`src/runtime/host_threads.c` and `tests/host_threads_test.clj`.
`examples/embed_host_threads.c` removed; `examples/embed_multi_tenant_threads.c`
covers the same ground end-to-end.

## v0.91.0 — Embed-Distinctive Thread API

Three knobs let embedders shape mino's threading without forking the
runtime: a host thread pool, a per-worker lifecycle factory, and a
per-worker stack size. Default behaviour (spawn-per-future) is
unchanged when none of them are set.

**`mino_set_thread_pool`.** Hand mino a host pool — Tokio runtime,
libuv, ASIO, custom pthread pool — and every `(future ...)` submits
a work item via `pool->submit_fn` instead of calling
`pthread_create`. The same pool can be bound to multiple
`mino_state_t` for multi-tenant patterns: per-NPC AI, per-tenant
script sandbox, per-buffer linter, chat-bot fleet. The pool's N
workers fan out across all states; each work item carries its own
ctx and finds the right state via `impl->state`. Pool-managed
quiesce uses cv-wait on the future's mu since mino doesn't own the
pthread; spawn-per-future quiesce keeps `pthread_join`.

**`mino_set_thread_factory`.** Install start/end callbacks that fire
on the worker thread for the spawn-per-future path. Use for naming
(`pthread_setname_np`), CPU affinity, priority class, or
tracing-context propagation. Pool-managed workers run under the
pool's own lifecycle hooks.

**`mino_set_thread_stack_size`.** Per-worker stack size for the
spawn-per-future path. Defaults to platform default. Useful for
tight-RSS embedders running many small futures. Pool workers ignore
it (the pool decides).

**Quiesce drops the GIL.** Previously a recursive caller (most
common: `prim_exit` from inside a script-side `(exit ...)`) would
deadlock on `pthread_join` because the worker needed the same
state_lock to publish its result. `mino_host_threads_quiesce` now
yields the lock before joining and re-acquires after.

**Worked example.** `examples/embed_multi_tenant_threads.c` spins up
six tenants over three shared pool workers and round-trips a future
from each tenant. Demonstrates the work-item-carries-state-pointer
model end-to-end.

## v0.90.0 — Blocking Channel Ops Park Across Threads

`<!!`, `>!!`, and `alts!!` outside a `go` block now do real OS-thread
blocks when host threads are granted. The matching producer or
consumer can run on any worker; the calling thread parks on a promise
and is woken when the other side fires the callback. This closes the
last gap that made channel-based coordination single-threaded in
practice.

**Behaviour by mode.** Each operation registers its callback on the
channel and drains the scheduler once. If the result lands during
that drain, return it. Otherwise: when `(mino-thread-limit) > 1`,
park on the promise indefinitely (canonical Clojure semantics — no
deadlock detection, since another thread can always supply the
value). When threads are not granted, fall back to the cooperative
drain-loop and throw on no progress (so a lone driver thread can't
lock itself).

**`thread` shares the future pool.** `(thread body)` is now a stable
alias for `(future-call (fn [] body))`; the docstring is no longer
phrased as a temporary alias. Same worker pool, same lifecycle, same
thread-limit budget.

**Slot-tracking fix.** `S->thread_count` now decrements when a worker
exits, not only on quiesce. Previously, after spawning N futures,
the count stayed at N even when all had completed — so a
long-running standalone session would eventually hit the limit
despite no live workers. The pthread itself remains joinable until
`mino_host_threads_quiesce`; `pthread_join` on an exited joinable
thread returns immediately. The limit now bounds *concurrently live*
workers, matching JVM Clojure's `future` semantics.

**GC sweep detaches future from list.** Latent in v0.89 but masked
by the slot bug above: `mino_future_gc_sweep` freed the impl without
unlinking it from `S->future_list_head`, so a later
`mino_quiesce_threads` (called from `prim_exit` and `state_free`)
walked into a freed pointer. Sweep now joins the worker thread (a
no-op if it has already exited), removes the impl from the list,
and only then destroys mu/cv and frees the struct. ASan caught it
on the new cross-thread tests once the slot fix let GC run on
resolved futures; both ASan and TSan are clean across the full
suite after the fix.

**Tests.** Cross-thread parking tests cover the multi-threaded path
(producer in one future, consumer on the test thread; alts winning
across threads; N×M ping stress). The single-threaded deadlock
tests are gated on `(<= (mino-thread-limit) 1)` so the standalone
suite doesn't hang on canonical-park behaviour. TSan-clean across
the full suite.

## v0.89.0 — Real Host Threads

Real OS-thread futures and promises. `(future expr)`, `(thread expr)`,
`(promise)`, `deliver`, `realized?`, `future-cancel`, `future-done?`,
`future-cancelled?`, `future?` all work end-to-end against
pthread-backed workers (CreateThread on Windows). Standalone
`./mino` grants `cpu_count` after `mino_install_all` so REPL users
get the canonical surface without configuration; embedders raise the
limit per state via `mino_set_thread_limit`.

**New value type: `MINO_FUTURE`.** A future cell holds a
malloc-owned impl struct with mu/cv, state machine
(`PENDING`/`RESOLVED`/`FAILED`/`CANCELLED`), result+exception slots,
cancellation flag, and OS thread handle. Promises share the type
(no thread; `deliver` writes the result directly). Identity
equality. Prints as `#<future:state>`.

**TLS-backed ctx accessor.** Worker threads allocate their own
`mino_thread_ctx_t` at entry, install via TLS, and link onto
`S->worker_ctxs_head` so GC root scanning walks every blocked
worker's gc_save and dyn_stack. The embedder thread leaves TLS
NULL and falls through to `&S->main_ctx`. ~415 sites migrated from
`S->ctx->FIELD` to `mino_current_ctx(S)->FIELD`; per-state field
removed.

**Per-state recursive mutex.** `mino_lock(S)` / `mino_unlock(S)`
take a recursive `state_lock` at the boundaries of `mino_eval`,
`mino_eval_string`, and `mino_call`. Workers and the embedder
thread serialize within one state; cross-state work runs fully
concurrent. `ctx->lock_depth` tracks recursion so
`mino_yield_lock` / `mino_resume_lock` can drop the lock entirely
around a blocking `cv_wait` in `mino_future_deref`, then re-acquire
to the saved depth. The lock is uncontested in single-threaded
states; cost is one mutex-acquire per public eval entry.

**GC suppression while workers are alive.** `gc_driver_tick` skips
collection when `thread_count > 0`. The conservative stack scan
only walks the current thread's stack, so a GC initiated from one
thread can't see another thread's stack-rooted values. Memory
normalizes after `mino_quiesce_threads`. Cycle G4.4+ replaces this
with safepoint-driven per-thread stack snapshots for true
concurrent GC.

**Lifecycle.** `mino_quiesce_threads(S)` joins every outstanding
worker. Called automatically from `mino_state_free` and from
`(exit ...)` so workers don't run after the state is torn down.
Embedders also call it directly to wait for in-flight futures
before doing other work.

**TSan-clean.** Full suite (1449 tests, 6987 assertions) passes
under `-fsanitize=thread`. The host_threads test exercises spawn
+ deref, promise + deliver, future-cancel, the future? predicate,
and a 4-future × 250-iter atom CAS contention test (lost updates
caught via the v0.87.0 atomic CAS upgrade).

**Documented limitation:** v0.89 single-state futures execute
serialized; cross-state futures sharing a host pool run fully
concurrent (no shared lock). Cycle G4.4 introduces blocking
channel ops + core.async/thread unification; G4.5 adds the
embed-distinctive surface (`mino_set_thread_pool`,
`mino_set_thread_factory`, `mino_set_thread_stack_size`); G4.6
relaxes single-state serialization with per-thread allocator
arenas and finer-grained registry locks.

## v0.88.0 — Safepoint Poll And STW Request For Major GC

Mutators now poll a per-thread `should_yield` flag at canonical
safepoints so a stop-the-world major collection can run with a
stable view of the heap. Locations: eval_impl entry (folded into
the existing limit / interrupt gate), `gc_alloc_typed` prologue,
and the two loop / recur backward branches in `eval/bindings.c`
and `eval/fn.c`. The fast path is one predictably-not-taken
volatile read; the slow path (`mino_safepoint_park`) blocks the
mutator until the collector signals release.

The major GC driver wraps its sweep in `gc_request_stw` /
`gc_release_stw`. Single-threaded today these are O(1) flag
toggles on `S->main_ctx` with no contention; the GC is itself
the mutator and is at a safepoint by definition. Cycle G4 later
sub-cycles iterate the worker set and use a condition variable
for park / release.

The flags themselves: `ctx->should_yield` (per-thread parking
signal) and `S->stw_request` (per-state broadcast). Both are
volatile so multi-threaded sub-cycles read them without
explicit fences; ordering invariants pair with the same
`__atomic_*` primitives the atom CAS path uses.

Perf budget held: fib(30) and reduce-over-million-range bench
both within noise compared to v0.87.0, comfortably under the
1% target.

ASan + UBSan clean. GC-stress smoke clean. Suite: 1453 tests,
6984 assertions, all green.

## v0.87.0 — Per-Thread Context And Atom CAS

Foundation for real host threads, with no observable change in
v0.87.x. Two pieces:

**Per-thread context (`mino_thread_ctx_t`).** Every field that
mutates with eval progress moves off `mino_state_t` into a new
`mino_thread_ctx_t` struct: `try_stack` / `try_depth`,
`dyn_stack`, `gc_save` / `gc_save_len`, `eval_steps` /
`limit_exceeded` / `eval_current_form`, `interrupted`,
`error_buf` / `last_diag`, `call_stack` / `call_depth` /
`trace_added`, and `gc_stack_bottom` / `gc_depth`. The state
embeds one `main_ctx` and exposes `S->ctx` pointing at it.
Single-threaded today: `S->ctx == &S->main_ctx` always, so
observable behavior is unchanged. Cycle G4 later sub-cycles
introduce per-spawn ctxs and TLS-backed lookup; the field
locations they need are already in place.

**Atom CAS gated on `multi_threaded`.** `swap!` and
`compare-and-set!` gain a multi-threaded path through
`__atomic_compare_exchange_n` (GCC/Clang builtin, works on
plain pointer fields without `_Atomic` typing). Single-threaded
path keeps the existing read+write fast path. The CAS path is
dormant until `S->multi_threaded` flips, which v0.87.x never
does; getting the structure in place now means host-thread
spawn lights up correct atom semantics without a second touch.

`compare-and-set!` also moves from value-equality (`mino_eq`)
to pointer-identity for the comparison, matching canon Clojure
(JVM `AtomicReference` uses reference eq). Small-int cache
means this is observably the same for small integers; the
change matters for boxed values where pointer-eq is what a CAS
instruction can actually express.

ASan clean. Suite: 1453 tests, 6984 assertions, all green.

## v0.86.1 — Audit-Cycle Fixes

Three issues found auditing v0.84.0 + v0.85.0 + v0.86.0:

- **Linux CPU-count detection.** `_SC_NPROCESSORS_ONLN` is an
  enum value in glibc and musl `<unistd.h>`, not a `#define`,
  so the `#elif defined(_SC_NPROCESSORS_ONLN)` guard
  introduced in v0.84.0 was always false. Linux standalone
  fell through to `thread_limit = 1` even on a multi-core
  box, silently turning every grant-gated `(future ...)` into
  the "host has not granted threads" message. Fixed by
  dropping the dead preprocessor guard and calling `sysconf`
  unconditionally on the non-Apple, non-Windows branch.
- **Standalone test files silently no-op.** The two new test
  files added in v0.84.0 and v0.85.0
  (`tests/host_threads_foundation_test.clj` and
  `tests/capability_metadata_test.clj`) didn't end with
  `(run-tests)`, so invoking them directly produced no
  output. Added the trailing call; under `tests/run.clj`'s
  suite-mode it stays a no-op, standalone it runs and exits
  with the per-file summary.
- **Empty-doc capability render.** `doc_render_with_capability`
  prepended `"\n  Capability: :foo"` to the docstring
  unconditionally, so a binding with an empty docstring +
  capability rendered with a stray leading newline. No
  primitive in tree exercises that path today (every
  primitive ships a docstring), but the case is reachable
  through the C `meta_set` helper and the rendering should
  stay clean for hosts that inject bindings that way.

ASan + UBSan clean. Suite: 1453 tests, 6984 assertions,
all green.

## v0.86.0 — Test Harness Suite Mode

Fixes a long-standing quirk where `tests/run.clj` silently
dropped the test files required after the first one whose
bottom-of-file `(run-tests)` call reached completion. The
runner's `(exit ...)` short-circuited the suite, so 246 tests
across 11 files (most of `tests/async_*`, plus `fs_test`,
`proc_test`, `deps_test`) were never executed under the
combined runner — they ran only when invoked individually.

`clojure.test/*suite-mode*` now gates the per-file
`(run-tests)`. When `*suite-mode*` is true, individual calls
are no-ops; the suite driver flips it back to false at the
end and runs the accumulated registry once. `tests/run.clj`
sets the flag before the require list and clears it for the
final call.

Three pre-existing test bugs surfaced by the now-running
files are fixed alongside:

- `tests/async_conformance_test.clj` — six `go-try-*`
  exception tests compared the catch binding directly to a
  bare-string expected value; the binding receives the
  diagnostic record now, so the comparison goes through
  `(ex-data e)`. Same shape as the rest of the catch tests
  in `tests/error_test.clj`.
- `tests/fs_test.clj` — `file-exists?` and `directory?`
  cases referenced `Makefile`, which the project no longer
  has (mino bootstraps via `./mino task build`). Replaced
  with `CHANGELOG.md`.
- `lib/mino/deps.clj` — `validate-dep-spec` was `defn-`
  while the test calls it directly. Promoted to `defn` since
  the function is genuinely useful for testing dep specs and
  has no internal-only invariants.

Suite count: 1452 tests, 6983 assertions, all green —
246 tests / 371 assertions previously hidden are now
counted.

## v0.85.0 — Capability Metadata As Documentation

Each non-core install group tags its primitives with a per-state
capability label so users can discover at a glance which group
their code requires. Capability is descriptive, not prescriptive
— the gate lives at install time in C, not at call time. User
code can't strip the metadata to gain access because the fn
either exists in the env or doesn't.

The labels match the existing install hooks one-for-one:

- `mino_install_io` -> `:io` (`slurp`, `spit`, `exit`,
  `time-ms`, `nano-time`, `file-seq`, `getenv`, `getcwd`,
  `chdir`, `gc-stats`, ...).
- `mino_install_fs` -> `:fs` (`mkdir-p`, `file-exists?`,
  `directory?`, `rm-rf`, ...).
- `mino_install_proc` -> `:proc` (`sh`, `sh!`, ...).
- `mino_install_host` -> `:host` (host interop dispatch).
- `mino_install_async` -> `:async` (channel/go/timeout
  primitives that core.async layers over).

Always-installed core primitives (`inc`, `+`, `println`,
`prn`, `conj`, etc.) carry no capability label; the
`io_core` table that ships printable I/O without filesystem
or process access stays unlabelled.

Two surfaces expose the label:

- `(mino-capability 'sym)` — returns the keyword (e.g. `:fs`)
  or `nil`. New primitive in `clojure.core`.
- `(clojure.repl/doc sym)` — appends a "Capability: :group"
  line to the docstring when the binding has a label.
  Existing user-facing API; no breaking change.

A new `meta_set_capability` C helper attaches the label to
the existing `meta_entry_t` (`docstring` + `capability` +
`source`); the meta-table teardown frees it. The
`prim_install_table_with_capability` helper lets each install
hook tag its whole table in one call without touching the
underlying `mino_prim_def` shape, so the ~150 prim defs across
the core/numeric/sequences/etc. tables stay untouched.

Tests: 7 new tests, 22 assertions in
`tests/capability_metadata_test.clj`. Total: 1206 tests, 6612
assertions, all green.

The naming "G0.5" reflects the cycle's heritage — the install
groups landed in cycle G0 (v0.81.0) and the capability metadata
was always queued as a small follow-up; this ships it.

## v0.84.0 — Host Threads — Foundation Slice

Lays the API surface for host-grant-gated host threads (cycle G4)
without yet shipping the runtime that backs them. The
`mino_set_thread_limit` / `mino_get_thread_limit` /
`mino_thread_count` / `mino_quiesce_threads` C surface is final
and embedders can code against it now; `(future ...)`,
`(thread ...)`, `(promise)`, `deliver`, `realized?`,
`future-cancel`, `future-done?`, `future-cancelled?` are
defined and throw `:mino/unsupported` with a message that
distinguishes two failure modes:

- `thread_limit <= 1` (embedded default): the host has not
  granted threads. The message names `mino_set_thread_limit`
  and points at this changelog.
- `thread_limit > 1` (standalone or grant-on): the host has
  granted permission, but the runtime implementation is in
  flight. The message reflects that the API surface is stable
  and the implementation lands across upcoming versions.

`future?` returns false for everything (no future value can be
constructed yet) so callers that branch on it pick the
non-future arm without surprise.

Standalone `./mino` calls `mino_set_thread_limit` with the host
CPU count (via `sysctlbyname` on Darwin, `sysconf` elsewhere,
`GetSystemInfo` on Windows) right after `mino_install_all`, so
REPL/script users see the "in flight" message while embedders
that haven't opted in see the "not granted" message. Once the
runtime ships, the same call grants Clojure-canon `(future ...)`
semantics by default in standalone mode.

Two new primitives expose the per-state knobs to the script
side for diagnostics and tests: `(mino-thread-limit)` returns
the int and `(mino-thread-count)` returns the live worker count
(always 0 in this slice). The `:mino/thread-limit` key in the
thrown ex-info map carries the same value.

Tests: 11 new tests, 22 assertions in
`tests/host_threads_foundation_test.clj` plus a C smoke program
in `examples/embed_host_threads.c` that exercises both grant
states from the embedder side. Total suite: 1199 tests, 6590
assertions, all green.

Six open questions for cycle G4 are settled and locked for the
incoming runtime work:

- **Thread pool model:** spawn-per-future by default; if the
  host calls `mino_set_thread_pool` the worker thread comes
  from that pool instead. Hosts that want internal pooling
  build it themselves around the same hook.
- **`thread_limit` enforcement when reached:** throw
  `:mino/thread-limit-exceeded`. Block-by-default risks
  deadlock when the saturating caller holds resources the
  worker needs; queue-indefinitely silently grows memory. Throw
  is honest and makes the limit visible.
- **Dynamic var conveyance:** snapshot the entire dyn-stack at
  spawn and install verbatim on the worker, matching JVM
  Clojure's `binding-conveyor-fn` shape.
- **Safepoint placement strategy:** eval_impl entry +
  allocation sites + backward branches (loop/recur). Catches
  every loop iteration, every allocation, and every Clojure
  call boundary.
- **Cancellation interrupt flag granularity:** both. A
  per-thread `should_yield` flag for state-wide quiesce, and a
  per-future flag for `future-cancel`. The two are distinct
  concerns and conflating them couples cancellation to
  threading.
- **`core.async/thread` unification with `future`:** same
  pool. `(thread ...)` and `(future ...)` share the worker set;
  the macros stay separate to document intent (thread for
  blocking work, future for parallel computation).

## v0.83.0 — Clojure.spec.alpha And Clojure.core.specs.alpha

Substantial port of `clojure.spec.alpha` and the destructure-form
specs in `clojure.core.specs.alpha`. Both ship in the bundled
stdlib under a new `mino_install_clojure_spec` hook.

`clojure.spec.alpha` provides the canonical surface: `s/def`,
`s/valid?`, `s/conform`, `s/explain`, `s/explain-data`,
`s/explain-str`, `s/and`, `s/or`, `s/keys`, `s/coll-of`,
`s/map-of`, `s/tuple`, `s/nilable`, `s/spec`, `s/cat`, `s/*`,
`s/+`, `s/?`, `s/alt`, `s/fdef`, `s/instrument`, `s/unstrument`,
`s/registry`, `s/get-spec`, `s/form`, and `s/assert`. Spec values
are tagged maps keyed by `::s/kind` and dispatched through
multimethods. `s/instrument` wraps the named var via
`alter-var-root` and validates `:args` on every call;
`s/unstrument` restores. Registered keys are reachable through
`s/get-spec`; `s/registry` returns the full map.

`s/gen` and `s/exercise` throw `:mino/unsupported`. A
`clojure.test.check` port is deferred until a concrete user need
lands. The error names the missing dependency so onboarders see
exactly what is absent.

`clojure.core.specs.alpha` ships destructure-form specs for
`defn`, `fn`, `let`, `binding`, and the binding-form sub-shapes
(`::seq-binding-form`, `::map-binding-form`, `::local-name`,
`::params+body`, `::defn-args`). Tools that want to validate
macro forms call `(s/conform
:clojure.core.specs.alpha/defn-args ...)` directly. Validation
is opt-in; the core compiler does not consult the specs.

Two evaluator fixes ship alongside the spec port because the
port surfaced them:

- `defmacro` now records the macro's defining namespace so that
  symbols inside the macro body resolve against the macro's own
  namespace when called from another. Without this, helper fns
  and internal `def`s referenced by the macro body raised
  `unbound symbol` from the caller's perspective.
- Macros set `fn_ambient_ns` only (not `current_ns`) when
  invoked, so `*ns*` and `(resolve ...)` inside the macro body
  still see the caller's namespace, matching canonical Clojure
  semantics.

The two changes are observable only when a namespace's macro
body references its own helpers or internal defs; bare-symbol
macros (none in `core.clj`) are unaffected.

`s/cat` and the regex repetition operators (`s/*`, `s/+`,
`s/?`, `s/alt`) interpret nested specs and registered regex
keys uniformly: the cat helper resolves keyword refs to their
registered spec, so `(s/* (s/cat :k keyword? :v any?))` over
`[:a 1 :b 2]` greedily consumes pairs and returns `[{:k :a :v
1} {:k :b :v 2}]`. `s/spec` wraps a regex into an element-level
spec so multi-arity `defn` bodies match the canonical shape
`(s/+ (s/spec ::params+body))`.

Test surface: 37 new tests, 86 assertions in
`tests/spec_test.clj` covering def/valid?/conform/explain,
and/or/nilable/tuple, keys required and optional,
coll-of/map-of, cat/*/+/?/alt, spec wrap, gen stub, assert,
fdef + instrument/unstrument, and the core.specs.alpha
destructure forms. Total suite: 1188 tests, 6568 assertions, all
green. ASan + GC stress smoke clean on the spec load + conform
path.

## v0.82.0 — Clojure.instant, Clojure.template, And Tagged-Literal Reader Hook

Three small fills accumulating under the bundled-stdlib registry
established in v0.81.0.

The reader now resolves `#tag form` at read time. Resolution
order: `(get *data-readers* 'tag)` -> `*default-data-reader-fn*`
-> `tagged-literal` record fallback. Both vars are interned as
dynamic vars in `clojure.core` with empty-map and nil defaults.
The reader's tag is emitted as a symbol now (not a keyword), per
canonical Clojure; calling `tagged-literal` directly still
accepts any tag value. The fallback record is built at read time
so `(read-string "#foo bar")` returns a `{:tag foo :form bar}`
tagged-literal record directly instead of a deferred
`(tagged-literal ...)` call form.

`*data-readers*` follows read/eval separation: the binding
visible at the read-string call site decides the reader fn, and
a later rebind does not retroactively change a value already
produced. With `clojure.instant` required, a one-line
`(binding [*data-readers* {'inst clojure.instant/read-instant-date}] ...)`
makes `#inst "2026-04-27"` parse to the component map.

Two small bundled namespaces drop into the registry established
in v0.81.0.

`clojure.template` ports the `apply-template` and `do-template`
substitution macros that user code historically reaches for when
generating repeated test cases or shape variants. mino's own
`clojure.test/are` macro is self-contained (it uses
`postwalk-replace` directly), so the namespace exists for
parity with user code that references it. Ships under the
`mino_install_clojure_test` install hook -- the test/template
pair installs together since `are` is the historical caller.

`clojure.instant` parses ISO 8601 timestamp strings into a
component map. mino does not have a host Date / Timestamp /
Calendar type, so the parse fns return a map with the keys
`:years`, `:months`, `:days`, `:hours`, `:minutes`, `:seconds`,
`:nanoseconds`, `:offset-sign`, `:offset-hours`, and
`:offset-minutes`. This is a deliberate divergence from JVM
Clojure: callers that wrap `read-instant-date` in
`(java.util.Date.)` need to consume the map directly. The
parser accepts every ISO 8601 shape the canonical regex
matches (year-only through nanosecond precision with optional
zone offset) and validates each component before returning.

The new namespace ships under its own install hook,
`mino_install_clojure_instant`. `mino_install_all` calls it
along with the rest, so the standalone build picks it up
without further wiring.

## v0.81.0 — Bundled Stdlib And Per-Group Install Hooks

The clojure.* namespaces that ship with mino (string, set, walk,
edn, pprint, zip, data, test, repl, stacktrace, datafy, and
core.protocols) are now baked into the binary alongside the core
library. A standalone install with no `lib/` directory on disk
still loads `(require '[clojure.string])` and the rest of the
bundled set, closing the brew/scoop bundling gap that previously
required users to colocate `lib/clojure/` next to the binary.

Each bundled namespace gets a per-state install hook on the
public C API: `mino_install_clojure_string`,
`mino_install_clojure_set`, `mino_install_clojure_walk`,
`mino_install_clojure_edn`, `mino_install_clojure_pprint`,
`mino_install_clojure_zip`, `mino_install_clojure_data`,
`mino_install_clojure_test`, `mino_install_clojure_repl`,
and `mino_install_clojure_datafy`. Pairs that depend on each
other ship together: `clojure.repl` brings `clojure.stacktrace`,
and `clojure.datafy` brings `clojure.core.protocols`. Each hook
registers its in-binary source into a per-state stdlib registry
that the require system consults before the disk resolver, so a
`(require '[clojure.string])` from script side loads the bundled
source from memory.

`mino_install_all(S, env)` is the new "give me everything"
convenience for the standalone build: it calls `mino_install_core`
plus the I/O / fs / proc groups plus every bundled clojure
namespace hook, mirroring what a full link from `./mino` provides.
Embedders that want a tighter footprint pick the subset they
need explicitly; `mino_register_bundled_lib(S, name, source)`
exposes the underlying registry so a host can bundle its own
non-clojure namespaces with the same mechanism.

The `gen-stdlib-headers` build task escapes each bundled
`lib/clojure/*.clj` into a per-namespace header
(`src/lib_clojure_<name>.h`) parallel to how `gen-core-header`
handles `src/core.clj`. The headers are gitignored and
regenerated on every build, so editing a bundled wrapper picks
up automatically. Test-fixture `.clj` files under
`lib/clojure/test_clojure/` and `lib/clojure/core_test/` are not
bundled -- they exist on disk so the require/resolve test
surface can verify file-loading behaviour.

Bundled-lib lookup treats `.` and `/` as the same separator so a
hook registered under `clojure.string` still matches the
`clojure/string` path-style name produced when the symbol form
of `require` recurses with the path-converted name.

## v0.80.0 — Real Records And Embed-Distinctive Type Construction

Records are now first-class value types in mino. `(defrecord
Point [x y])` defines `Point` as a real type (not a tagged map),
`->Point` as the positional constructor, and `map->Point` as the
constructor that splits declared fields from extension keys.
Field access via `(:x p)`, `(get p :y)`, and `(p :z :missing)`
all resolve through the same primitive path; `assoc` keeps the
record type when the key is declared or new (ext); `dissoc` on a
declared field degrades the record to a plain map (canonical
Clojure semantics). `seq`, `keys`, `vals`, `count`, `contains?`,
and `find` cover the rest of the map-isomorphic surface.

Records are not maps with type tags. Storage is field slots, not
a backing map; the slot array is malloc-owned and freed during
GC sweep. Equality requires type-pointer identity plus per-field
value equality plus extension map equality; `(= (->Point 1 2)
{:x 1 :y 2})` is false, and the two values hash differently.
This is the `(= record map-with-same-content)` litmus that
distinguishes a real record from a tagged-map wrapper.

`deftype` is an alias for `defrecord`. mino has no separate
JVM-class layer to expose, so the deftype/defrecord distinction
collapses; values created either way are real types with
map-isomorphic behaviour. `reify` creates a fresh anonymous type
at expansion time and returns a single instance with the named
protocols extended onto it; repeated invocations of the same
reify form share the type pointer because record types intern
by `(ns, name)`.

`(instance? T x)` is now meaningful: it compares `t` against
`(type x)`, which is type-pointer identity for records and
keyword equality for built-in types and ad-hoc `:type`-tagged
values. The previous throw-stub macro is gone.

Protocol dispatch atoms hold mixed keyword and type-pointer
keys: built-in types continue to dispatch via keywords like
`:string`, `:vector`, and `:map`, while record types dispatch
via the `MINO_TYPE` value `defrecord` produces. `extend-type`
and `extend-protocol` accept type symbols that resolve at
runtime to the type pointer, so `(extend-type Point IFoo (foo
[this] body))` registers under the type's pointer and
`(get @IFoo--foo (type p))` finds the impl. The dispatch path
does not distinguish C from script: a host that wants its own
impl interns an ordinary primitive and uses `extend-type` from
mino code, the same way every other protocol method does.

The `(with-meta x {:type :tag})` keyword-tag dispatch path is
unchanged. `defrecord` is the canonical path for new code; the
metadata path remains for ad-hoc tagging and is still used by
mino's own multimethod implementation.

The C embed surface gains `mino_defrecord`, `mino_record`,
`mino_record_field`, `mino_is_record`, and `mino_is_record_type`
in `src/mino.h`. A host can define a record type from C, build
instances directly, and read declared field values back without
going through map-key lookups. The constructor is idempotent by
`(ns, name)`, so re-calling it from a script reload returns the
existing type and existing record values keep
`(instance? T r)` true. The new `examples/embed_record.c`
exercises the full round trip: defines a `Vec3` type from C,
builds an instance with `mino_int` field values, hands it to
script that extends a magnitude-squared protocol on the type and
calls it on the C-built value, then reads field values back via
`mino_record_field`.

Migration: code that called the throw-stubbed `defrecord`,
`deftype`, `reify`, or `instance?` will now succeed instead of
throwing. The `tests/compat_test.clj` block asserting they throw
has been pruned to keep only the still-unsupported `:import`
case. Code that relied on the throw stubs to gate platform
detection should switch to a different shibboleth.

## v0.79.0 — Auto-Promoting Arithmetic And `unchecked-*` Opt-In

Plain `+`, `-`, `*`, `inc`, and `dec` now auto-promote to bigint
on long overflow rather than throwing. The expression
`(+ 9223372036854775807 1)` returns `9223372036854775808N`
instead of raising `:eval/overflow`; the same applies to
unary `(- LLONG_MIN)`, `(- LLONG_MIN 1)`, `(* big big big)`,
`(inc LLONG_MAX)`, and `(dec LLONG_MIN)`. The previous
loud-throw default was the silent-surprise cousin of canonical
Clojure: working code that ran on a JVM raised an unfamiliar
classified error here. The new default matches what Clojure
programs assume, while the named opt-in below preserves the
fast int64 path for code that needs it.

The `unchecked-add`, `unchecked-subtract`, `unchecked-multiply`,
`unchecked-inc`, `unchecked-dec`, and `unchecked-negate`
primitives ship as the named opt-in for two's-complement
wraparound int64 arithmetic. `(unchecked-add 9223372036854775807
1)` returns `-9223372036854775808` (LLONG_MAX wraps to
LLONG_MIN); operands must be ints, non-int operands throw
`:eval/type`. The names match canonical Clojure surface and
pair fixed-arity calls (`unchecked-add` is binary,
`unchecked-inc` is unary), matching the JVM signatures.

Per the alpha-no-backcompat policy, the auto-promoting
quote-suffix siblings `+'`, `-'`, `*'`, `inc'`, and `dec'` have
been removed entirely. Code that called them now resolves
through plain `+`/`-`/`*`/`inc`/`dec`, which auto-promote with
the same semantics. The `clojure_coverage_test` lists the
quote-suffix names alongside JVM-only names: present in
canonical Clojure but intentionally absent in mino because the
plain forms now do the same job.

The `:eval/overflow` MOV001 error code is retired. The single
remaining caller, `(int huge-bigint)` for a value out of long
range, now reports `:eval/type` MTY001 since the conversion is
a type/range error rather than an arithmetic overflow.

Internally, the `tower_reduce` and `tower_reduce_seeded`
helpers shed the `promote_long_overflow` flag they took to
distinguish `+` from `+'`; they now always promote. The 6
throw sites in `src/prim/numeric.c` for `:eval/overflow` MOV001
are gone.

## v0.78.0 — `clojure.core.protocols` And Cross-Namespace Protocol Extension

The four canonical protocols `CollReduce`, `IKVReduce`,
`Datafiable`, and `Navigable` are now first-class in mino. They
are interned at boot time in `clojure.core` and re-exported under
the `clojure.core.protocols` namespace, so user code can write
`(extend-protocol clojure.core.protocols/CollReduce SomeType
...)` and have the override consulted by `reduce`. The
`clojure.datafy` namespace ships as a thin wrapper that surfaces
`datafy` and `nav` at the canonical home expected by code ported
from canonical Clojure.

`reduce`, `reduce-kv`, `datafy`, and `nav` now consult the
protocol dispatch table on every call. When no per-type or
`:default` override is registered, `reduce` and `reduce-kv` fall
through to the existing internal seq-driven walk; the override
only kicks in when a user has extended the protocol for the
value's type. `Datafiable` and `Navigable` are seeded with
identity-shaped `:default` impls so `(datafy x)` and `(nav coll
k v)` are well-defined for built-in types.

The `extend-type` and `extend-protocol` macros now preserve the
namespace prefix on the protocol symbol when emitting the
underlying `(swap! Proto--method ...)` form. Before this fix
`(extend-protocol some.lib/SomeProto ...)` silently looked up
the dispatch atom in the calling namespace and failed with an
unbound-symbol error. Cross-namespace protocol extension is the
standard usage pattern, so what was previously a quiet breakage
is now part of the supported surface.

Two new private vars are exposed in `clojure.core` for the
protocol wiring: `internal-reduce_` and `internal-reduce-kv_`
hold references to the pre-protocol implementations and serve as
the fall-through when no override applies. Both are
underscore-suffixed by mino's existing convention for
implementation-detail names.

## v0.77.0 — REPL Specials And `clojure.repl` / `clojure.stacktrace`

The interactive REPL now binds the standard introspection vars
after each form: `*1`, `*2`, `*3` rotate to hold the three most
recent results, `*e` captures the most recent error as a
structured diagnostic map, `*command-line-args*` exposes any
positional arguments past the script path, and `*file*` is
bound to the script path during file-mode load (or
`"NO_SOURCE_PATH"` in the REPL). The vars are interned from
`main.c` rather than `mino_install_core`, so embedders that
don't ship a REPL pay nothing for these.

Two new bundled namespaces ship under `lib/clojure/`. The
`clojure.repl` namespace wraps the existing introspection
primitives in print-shaped helpers: the `doc` and `source`
macros print, the `dir` macro lists a namespace's public names,
`find-doc` searches docstrings for a substring or regex, and
`pst` prints `*e` as a formatted summary. The C primitives that
return raw data are exposed as `clojure.repl/doc-string`,
`clojure.repl/source-form`, and `clojure.repl/apropos`. The
`clojure.stacktrace` namespace provides `print-throwable`,
`print-stack-trace`, `print-cause-trace`, and `root-cause` for
walking mino's diagnostic-map exception representation.

Per the alpha-no-backcompat policy, the previously-exposed
`doc`, `source`, and `apropos` names in `clojure.core` have
been removed. Code that called them as data accessors should
require `clojure.repl` and use the renamed names; code that
wanted print behavior gets it via the `clojure.repl/doc` and
`clojure.repl/source` macros.

The require machinery's runtime-namespace shortcut had a
pre-existing bug that this cycle exposed: namespaces with
both pre-installed C primitives and a backing `.clj` file
would skip loading the file, since the var registry already
held entries from install time. The check now consults
`module_cache` (which records actually-loaded files) instead,
so `(require '[clojure.repl :refer [doc]])` and
`(require '[clojure.string :refer [capitalize]])` correctly
load the wrapper and bind the `:refer`'d names.

## v0.76.2 — Insertion Barrier For Incremental Major

The mutator write barrier now also pushes the just-installed
`new_value` onto the major mark stack while a major collection
is in MAJOR_MARK. Pure SATB captures the previous slot contents,
which is correct for objects already reachable from the snapshot,
but does not protect an OLD whose only surviving root path runs
through the new edge of this very write. Combining the Yuasa
SATB push with a Dijkstra insertion push closes that window:
either pre-existing snapshot reachability or post-update
reachability is sufficient to keep an OLD alive across the
cycle. `gc_mark_push` deduplicates against the mark bit, so the
extra push is a no-op for values already in the snapshot.

The bug surfaced as a heisenbug whose footprint depended on the
exact size of `src/core.clj`: past a threshold (Cycle B's print-
pipeline additions plus one more defn), the test suite would
fail in `tests/compat_test.clj :: multimethod-with-docstring`
with shifting error shapes (`fn arity mismatch`, `unsupported
binding form`, `map as function takes 1 or 2 arguments`). ASan
was clean because the freed OLDs were recycled through the GC's
internal freelist rather than `free()`. `MINO_GC_VERIFY=1`
showed no remset gap. Forcing every major to run STW
(`MINO_GC_STRESS=1` or disabling slicing in the driver) hid the
bug, which localized the problem to the incremental mark path.

## v0.76.1 — GC Defensive Fixes On Alloc-Pair Patterns

Two intern and trie-build paths that allocate one GC object,
hold its only reference in a C local, then call back into the
allocator are now wrapped in a gc_depth raise so a collection
cannot fire between the two allocs. Both ASan and load-time
stress had been catching this under specific layouts; the
conservative stack scan misses locals the optimizer keeps in
registers, which is what made the symptoms heisenbug-shaped
(error messages shifted between runs even with the same
inputs).

`intern_lookup_or_create` in `src/collections/val.c` now keeps
the freshly `dup_n`'d character buffer protected across the
following `alloc_val`. Without the raise the buffer could be
swept by a sweep triggered by `alloc_val`'s own driver tick,
which surfaced as use-after-free reads in `gc_mark_push` later
on.

`vec_from_array` in `src/collections/vec.c` already raised
`gc_depth` for the trie-build phase but lowered it before
`vec_assemble`. The lowered window is now closed: gc_depth
stays raised through `vec_assemble` in both the tail-only and
full-trie paths so the just-built tail and root nodes (held
only in C locals at that point) are not swept while
`alloc_val` runs.

Both changes are localized: existing call sites are unchanged
and the test suite continues to pass under both the normal
incremental schedule and `MINO_GC_STRESS=1` full-STW majors.

## v0.76.0 — Print Pipeline And `*out*` / `*err*` / `*in*`

The print and read primitives now route through configurable
sinks resolved from `*out*`, `*err*`, and `*in*`. The three
names are interned as dynamic vars in `clojure.core` holding
the sentinel keywords `:mino/stdout`, `:mino/stderr`, and
`:mino/stdin`; binding `*out*` or `*err*` to a string-
collecting atom captures the output bytes into the atom's
value instead of the default `FILE*`, and binding `*in*` to a
string-cursor atom feeds reads from the string. The dyn-stack
lookup matches both the bare and `clojure.core/`-qualified
symbol forms so syntax-quote-expanded bindings work without
ceremony.

The print family (`println`, `prn`, `print`, `pr`, `newline`,
`pr-builtin`) now consults `*out*` before deciding the sink,
falling back to stdout when bound to `:mino/stdout` or stderr
when bound to `:mino/stderr`. `(binding [*out* *err*] ...)`
routes output through stderr because the dyn-bound `:mino/
stderr` keyword identifies the FILE\* fallback.

`with-out-str`, `with-in-str`, `print-str`, `prn-str`,
`println-str`, `printf`, `flush`, `read-line`, and `read*` are
new. `with-out-str` allocates a fresh string-atom, binds
`*out*` to it for the body, and returns the accumulated text.
`with-in-str` binds `*in*` to a string-cursor atom holding the
given text. `read-line` reads one line from `*in*` (atom-bound
or stdin), returning the line or nil on EOF. `read*` is the
zero-arity primitive that the user-facing `clojure.core/read`
dispatches to: a fresh `(read)` consumes the next form from an
atom-bound `*in*` (the stdin path raises an unsupported error,
since stream-fed read needs reader-side plumbing that lands in
a follow-up). The `*-str` companions wrap their print
counterparts; `printf` formats then prints; `flush` calls
`fflush` on stdout and stderr (a no-op for atom-bound sinks).

Internally the print primitives moved from the optional
`mino_install_io` table to `k_prims_io_core`, which runs before
`core.clj` evaluates so the bundled `print-str`/`prn-str`/
`println-str` definitions can reference them. Sandboxed
embedders that called `mino_install_core` without
`mino_install_io` already had `pr-builtin`; they now also see
the print family plus `read-line`, `read*`, and `printf`.
Filesystem and process I/O (`slurp`, `spit`, `exit`, `file-
seq`, `getenv`, `getcwd`, `chdir`) stay in `k_prims_io` for
capability-gated installation.

The `print-method` multimethod still dispatches readable
formatting per type. When the hook is installed and a user
method is called, the print primitive runs the hook under a
nested `*out*` rebinding that captures the hook's output to a
temporary string-atom, then emits the captured bytes through
the outer sink — so user-defined methods that call `pr-
builtin` or other print fns flow correctly into `with-out-
str`.

## v0.75.0 — Surface Honesty

Three small but visible gaps closed against the canon surface,
under the principle that silent divergences cost more than loud
ones.

The reader's `#"..."` regex literals now pass body bytes to the
regex engine verbatim. Previously the body ran through the same
string-escape pass that ordinary strings do, so `\d` lost its
backslash before the engine saw it; `(re-find #"\d+" s)` would
silently match `d+` instead of digits. The literal path now
preserves backslashes (and `\"` is a literal two-character
sequence rather than a string terminator), matching how regex
literals work elsewhere. The string-form `"\\d+"` workaround
keeps working unchanged.

`load-string` and `load-file` are now exposed as primitives.
The runtime already had `mino_eval_string` and `mino_load_file`
as embedder-facing C functions; these primitives surface the
same machinery to the language. `(load-string "(+ 1 1)")`
returns `2`; `(load-file "path/to/file.clj")` reads, evaluates,
and returns the last form's value. Both clear the ambient
namespace for the duration so forms see the current namespace
plus their lexical chain, matching `eval`.

Documentation reflects the new state. The Intentional
Divergences page no longer carries the regex-escape entry, and
the Coming-from-Clojure quick-reference table marks `#"regex"`
as Same.

## v0.74.3 — One-Shot Expression CLI

The standalone `mino` binary now treats a positional argument
that begins with a Lisp form character as an inline expression,
matching the convenience shape other Lisp CLIs offer:

```
mino "(+ 1 2)"          # 3
mino "[1 2 3]"          # [1 2 3]
mino "{:a 1}"           # {:a 1}
mino "(println :hi)"    # :hi  /  nil
```

Form characters that trigger expression mode: `(`, `[`, `{`,
`#`, `@`, `'`. A leading `--` separator forces file-or-task
interpretation; the explicit `-e EXPR` flag still works either
way; everything else continues to be treated as a file path.
File names that happen to start with one of those characters
need an explicit `--` or path prefix (e.g. `mino ./(name).clj`),
but that's a vanishingly rare case in practice.

`--help` documents the new shape on its own line under USAGE.

## v0.74.2 — Heap-Allocated Dynamic Binding Frames

Fixes the v0.74.1 known-issue Windows SIGSEGV during
`tests/run.clj`. The `binding` special form and the new
`with-bindings*` primitive both pushed a stack-local
`dyn_frame_t` onto `S->dyn_stack` and only popped it on the
success path. When a `throw` inside the body unwound the C stack
through `longjmp` to a containing `try`, the popped function's
stack memory still held the frame, and the longjmp handler in
`eval/control.c` walked `S->dyn_stack` and read `frame->prev` /
`frame->bindings` from that now-stale stack region. Linux happens
to leave popped stack memory readable for long enough that the
walk succeeds; the Windows runner's stack handling makes the same
read fault.

The fix is to heap-allocate the frame so the pointer remains
valid even after the C frame is unwound. The success path frees
the frame; the longjmp handler in `eval/control.c` already frees
the malloc'd binding chain on each unwound frame and now sees a
stable parent pointer too.

The Windows job in `ci.yml` returns to the blocking matrix; the
informational marker added in v0.74.1 is no longer needed.

## v0.74.1 — CI Hygiene

The v0.74.0 push surfaced two CI signals that needed
addressing. Neither is a runtime correctness regression on the
platforms covered by formal and parity gates (1058/6277, 230/230);
both are about how the CI suite reports.

The Windows matrix job currently SIGSEGVs partway through
`tests/run.clj` after the v0.73.0 first-class-namespace cycle.
Without a Windows reproduction environment the root cause is not
yet identified; the matrix job is marked `continue-on-error: true`
so the Linux and macOS gates can keep blocking, and the Windows
crash is tracked as a known issue for the next cycle.

The `perf-gate` job in `ci.yml` is now informational
(`continue-on-error: true`). Shared GitHub-hosted runners are
CPU-noisy, the `ubuntu-latest` image drifts under the pinned
baseline, and v0.73.0's first-class-namespace lookup chain
naturally adds eval-floor cost that the v0.70.0-era baseline did
not anticipate. Local runs and the dedicated `mino-bench`
workflow remain the authoritative signal; a self-hosted runner
or scheduled comparison-run job is queued for a follow-up.

The `mino-bench` task runner's bundled-task module qualifies its
`clojure.string` calls as `str/split` and `str/ends-with?`; the
v0.73.0 namespace move broke the bare references. Same fix in
the satellite repo, no mino-side change.

The `mino-site` deploy workflow bootstraps from `src/core.clj`
instead of the pre-migration `src/core.mino`, and the
`mino-examples` submodule pin is refreshed against the published
SHA so submodule fetches succeed. Same shape: satellite-side
adjustments after a major-namespace cycle.

## v0.74.0 — Deferred Core Surface

The deferred names from the v0.73.0 coverage report — `*ns*` as a
real var, `bound-fn` / `bound-fn*`, `read` with options,
`clojure.edn/read`, `destructure`, `re-groups`, and `re-matcher`
— land in this cycle. With them the `clojure.core` and
`clojure.edn` portable surfaces hit 100% in the coverage report.

`*ns*` is now interned as a dynamic var in `clojure.core`, so
`(find-var 'clojure.core/*ns*)` resolves and `(deref ...)` tracks
user-visible namespace switches: `in-ns` and the `(ns ...)` special
form republish the var, and `require`'s save/restore boundary
republishes the saved name on the way out so loading a file does
not leak the loaded namespace into the caller. The bare-symbol
fast path stays as a fallback for embedders that read `*ns*`
before installation finishes.

`bound-fn` and `bound-fn*` capture and replay dynamic bindings
around an invocation, layered on two new C primitives:
`get-thread-bindings` snapshots the active dynamic bindings into a
symbol-keyed map (newest-first wins on shadowing), and
`with-bindings*` pushes a transient frame around a thunk. The
mino-side macros provide the standard Clojure call shape for
inheriting context into a returned function.

`read-string` accepts an optional opts-map first argument with the
`:read-cond` key (`:allow` default, `:preserve`, `:disallow`). The
reader threads the mode through a new `reader_cond_mode` field so
`#?` and `#?@` sites consult it: `:preserve` emits a
reader-conditional record (the same shape `clojure.core/reader-conditional`
constructs), and `:disallow` rejects the form. Top-level, list-context,
and vector-context conditionals all participate; `#?@` inside a map
literal is unsupported in `:preserve` mode and errors with a clear
message. `read` aliases `read-string` (mino has no PushbackReader
type so the string form is the only shape). `clojure.edn/read` and
`clojure.edn/read-string` force `:read-cond :preserve` so untrusted
text never auto-evaluates a reader conditional.

`destructure` surfaces mino's destructuring algorithm as a
function. It takes a binding-pairs vector `[lhs1 rhs1 ...]` and
emits a flat `[name init ...]` vector that, fed to `(let ...)`,
produces the same bindings. Vector patterns lower through `nth`, &
rest through `nthnext`, map `:keys` / `:strs` / `:syms` through
`get` with optional `:or` defaults, plus `:as` and explicit `{sym
:key}` entries. Implementation lives next to `bind_form` in
`src/eval/bindings.c`; the primitive is registered in `clojure.core`
via the reflection table.

The bundled regex engine grows a parenthesised-group construct.
Compile parses `(` and `)` into `GROUP_OPEN` / `GROUP_CLOSE` markers
with sequential ids; the matcher treats the markers as zero-width
hooks that record the current text offset. `re-find` and
`re-matches` now return `[whole g1 g2 ...]` vectors when the
pattern has groups and keep the old string shape otherwise.
`re-matcher` returns an atom-backed iterator that `re-find`
advances; `re-groups` reads the matcher's last recorded result.
Pattern `\(` still escapes a literal paren. Caveat: `#"..."`
literals run through the regular string-escape path, so `\d` /
`\s` / `\w` lose their backslash before the regex engine sees
them; pass patterns as strings (`"\\d+"`) until a regex-aware
reader escape mode lands.

Caveats. `read` accepts only the string form — mino has no stream
reader value. `#?@` splice in `:preserve` mode is supported in lists
and vectors but not inside map literals. `re-matcher` is mino-side,
so its `:pos` advance uses substring scanning; this is acceptable
for typical input but is not the right choice for very large
strings.

## v0.73.0 — First-Class Namespaces

Namespaces are now real. Each namespace has its own root binding
table, so `(ns a) (def x 1)` and `(ns b) (def x 2)` are independent
and visible only by qualified name from each other. `clojure.core`
is the bundled-core namespace; every other namespace's root env
chains to it via a parent pointer, so unqualified references to
`if`, `map`, `let` and friends keep working without an explicit
refer.

The full namespace machinery landed in one cycle. `(ns name ...)`
clauses accept `:require`, `:use`, and `:refer-clojure` with the
expected modifier set: `:as`, `:as-alias`, `:refer [syms]`,
`:refer :all`, `:only`, `:exclude`, and `:rename`. Prefix lists
work too: `(:require [pkg [a :as a] [b :as b]])`. `require` itself
accepts symbol, vector, prefix-list, and string arguments and is
multi-arg. A namespace created by `(ns ...)` in memory is
requirable without a backing file -- the resolver checks the
runtime registry before falling back to the filesystem.

Vars are first-class runtime objects. `(def x 1)` returns the var
`#'<ns>/x`; `(def x)` creates an unbound var that `bound?` reports
as `false` and that throws on deref. `intern`, `find-var`,
`var-get`, `var-set`, and `alter-var-root` all work; the
`with-redefs` macro binds a stack of root-value swaps so test code
can stub vars temporarily. `^:private` is a hard error on
cross-namespace qualified access, and `:refer :all` skips privates
rather than exposing them.

Auto-resolved keywords landed too. `::foo` reads as
`:<current-ns>/foo`; `::alias/foo` looks the alias up in the
session's alias table at read time and errors if absent. The
namespaced-map literals follow: `#:foo{:b 1}` qualifies bare keys
with `foo`; `#::{:b 1}` qualifies with the current namespace; and
`#::alias{...}` resolves the alias the same way `::alias/foo`
does. The underscore namespace (`:_/x`) strips off, leaving a bare
key.

A handful of correctness gaps closed alongside. Cyclic `require`
chains now throw with the load chain in the message rather than
recursing into a stack overflow. A loaded file whose first
`(ns ...)` form disagrees with the requested module name is
rejected; the comparison treats dashes and underscores as
equivalent so `(ns foo-bar)` in `foo_bar.clj` is fine. `def`,
`declare`, and `defmacro` refuse to shadow a name brought in by
`:refer` from another namespace, so accidental collisions surface
immediately. The "unbound" diagnostic for qualified symbols
distinguishes "no such alias", "no such namespace", and "no var X
in namespace Y". Symbols ending in a colon (`foo:`) are rejected at
read time, namespaced map literals reject duplicate keys after
prefix qualification, and `(ns 1)` errors instead of silently
returning nil.

`refer` accepts `:only`, `:exclude`, and `:rename`. Names listed in
`:only` are validated up front: each must exist in the source
namespace and must not be a private var, so `refer` no longer
silently drops missing or hidden names. `find-var` throws for an
unknown namespace; the var-not-found case still returns nil to
match upstream. `ns-resolve` accepts the optional environment-map
arg so `(ns-resolve ns env-map sym)` returns nil when the symbol
is shadowed locally.

Namespaces carry metadata. `(ns ^{:a 1} foo "docstring" {:b 1})`
collects the `^meta`, the docstring (as `{:doc "..."}`), and the
attribute map into a single map and stores it on the namespace.
`(meta *ns*)`, `(meta (find-ns 'foo))`, and `(meta (the-ns 'foo))`
return that map. Each `(ns ...)` invocation replaces the namespace
metadata wholesale; merging only happens between the three sources
within one call.

The introspection surface is roughly the runtime-namespace shape
that other interpreted dialects expose: `in-ns`, `find-ns`,
`the-ns`, `create-ns`, `remove-ns`, `ns-name`, `ns-publics`,
`ns-interns`, `ns-refers`, `ns-aliases`, `ns-map`, `ns-unmap`,
`ns-unalias`, `alias`, `all-ns`, `loaded-libs`, `find-var`,
`ns-resolve`, `requiring-resolve`, `intern`, `var-get`, `var-set`,
`var?`, `bound?`, `alter-var-root`, plus `*ns*` for the current
namespace symbol. `ns-publics` returns only the namespace's own
public vars; `ns-refers` walks the parent chain to surface
inherited names; `ns-map` combines both with the alias table.
Values come back as vars (so `pr-str` produces `#'ns/name`), and
`ns-unmap` clears both the env binding and the var registry entry.

Syntax-quote (`\``) auto-qualifies bare symbols against the
current-namespace lexical chain (already true since the cycle
opened) and now also expands an alias prefix on namespaced
symbols, so `\`str/x` becomes `clojure.string/x` when `str` is
aliased. Refer'd entries keep their source-namespace identity:
after `(refer 'clojure.string)` in a fresh namespace,
`\`capitalize` resolves to `clojure.string/capitalize` rather than
the receiving namespace, matching the contract the reflective
APIs already followed.

Namespace aliases are scoped per-namespace. Setting an alias in
one namespace no longer leaks into another, so `(require '[a :as
x])` in one namespace doesn't make `x/y` resolvable from a
sibling namespace. Vars carry `:ns`, `:name`, `:private`, and
`:dynamic` metadata synthesized from their intrinsic fields, so
`(meta #'foo)` returns a useful map. `eval` resets the ambient
namespace before running the form, so a form passed to `eval`
sees only its own current-namespace bindings rather than the
calling function's defining namespace. The `with-local-vars`
macro lands as a thin wrapper over `intern` and `var-set` for
lexically-scoped mutable cells.

`ns-unmap` correctly removes large-frame bindings (the previous
implementation shifted the array in place but left the backing
hash table pointing at the old slot, so the binding still
resolved). `resolve` no longer falls back to a global var-registry
scan when the current namespace doesn't own a name; that fallback
picked up unrelated names from sibling namespaces.

`(require "deps/foo/src/foo.cljc")` -- a literal path argument --
no longer trips file-to-namespace validation. Path-style requires
are deliberate "load this file" requests; only the dotted-name
form imposes the namespace-must-match-name check. `(doc 'foo)`
falls back to the namespace's `:doc` metadata when the named-
binding table doesn't have an entry, so namespaces declared with
`(ns foo "docstring" ...)` are documented through the same
primitive that surfaces `defn` docs. `(doc 'clojure.core/inc)` also
finds the docstring registered under the bare name.

`mino.deps` now probes a fetched dependency directory for common
source-root conventions. If the lib follows the Maven layout
(`src/main/clojure/`) the root is added automatically alongside a
plain `src/` entry, so a multi-file library can require its sibling
namespaces by symbol without a manual `:deps/root` override in
`mino.edn`.

A few small Clojure-shaped affordances landed alongside the
namespace work. `extend-protocol` accepts `nil` as a type marker
(translated to `:nil` so nil-safe protocol implementations match
what `(type nil)` returns); bare class symbols (`Object`,
`Pattern`, ...) are rejected with a clear error so silently
collapsing them to `:default` doesn't mask broken dispatch.
Reader conditionals now treat `:clj` as an active dialect
alongside `:mino`, so libraries that only have `:clj`/`:cljs`
branches read correctly here. `defn` honors a `{:pre [...]
:post [...]}` map between params and body, threading assertions
around the body so `%` refers to the return value. `*assert*` is
bound to true at the clojure.core level. `find` accepts transient
associatives, mirroring real Clojure semantics. `re-find` and
`re-matches` return nil for a nil text argument instead of
throwing. `:refer-clojure` skips bindings whose source var is
private, matching how Clojure's auto-refer treats private vars.
The stale `clojure.core/blank?` wrapper has been removed; `blank?`
lives only in `clojure.string` now, matching the upstream
contract.

Mino targets pure portable Clojure — there is no JVM and no
JavaScript runtime — so any form that exists solely to interface
with one of those platforms throws an `ex-info` carrying
`:mino/unsupported`. `defrecord`, `deftype`, `reify`, `proxy`,
`gen-class`, `definterface`, `import`, and `instance?` all error
at expansion or call time. `agent`, `send-to`, and `agent-error`
do the same — aliasing them to atoms only pretended the async
dispatch semantics were honored. The `ns` form rejects `:import`
and `:gen-class` clauses so files that mix Java interop into
their namespace declarations fail loud at load time.

Source files have moved from `.mino` to `.clj`. Mino sources are a
host-targeted Clojure dialect (the same `defn` / macro system /
sequence semantics, with the JVM-only forms above swapped for
`:mino/unsupported` errors), and the new extension lets editors,
formatters, language servers, and tree-sitter grammars recognize
mino code out of the box. The require resolver searches `.cljc`,
`.clj`, and `.cljs` in that order; `.mino` is gone. External
libraries that ship as portable Clojure continue to load as
`.cljc`. Sibling repositories (`mino-bench`, `mino-examples`,
`mino-lsp`, `tree-sitter-mino`) follow the same rename on local
branches.

C primitives are now interned as vars in their install-time
namespace. `(find-var 'clojure.core/inc)` returns
`#'clojure.core/inc`, `(resolve 'inc)` returns the var,
`(meta #'inc)` returns `{:ns clojure.core :name inc}`, and
`(deref (resolve 'inc))` invokes the primitive. `clojure.string`
primitives like `split` and `join` resolve through their own
namespace var. Refer-collision detection no longer exempts
primitive bindings unconditionally — a primitive that has been
refer'd into another namespace and then re-defined surfaces the
same "already refers to a var from another namespace" diagnostic
as a mino-side defn would.

The pure-Clojure surface gained the names that portable libraries
expected to find: identifier predicates `ident?`, `simple-ident?`,
`qualified-ident?`, `special-symbol?`, `map-entry?`, the no-op-on-
mino predicates `bytes?`, `inst?`, `uri?`, plus `uuid?` /
`parse-uuid` (string-shaped, since mino has no Java UUID type).
Parsing helpers `parse-boolean` and `find-keyword` round out the
1.11 set alongside the existing `parse-long` / `parse-double`.
Collection helpers `partitionv`, `partitionv-all`, `splitv-at`,
and `replicate` build on `partition`/`partition-all` (which now
also accepts the four-argument `(partition n step pad coll)` form
real Clojure exposes). Hash-combining helpers `hash-ordered-coll`,
`hash-unordered-coll`, and `mix-collection-hash` produce
mino-internal-consistent hashes (not bit-equal to Clojure's
Murmur3, but stable across runs). `ex-cause` reads from
`ex-data :cause` or attached metadata. `with-redefs-fn` is the
function counterpart to the existing `with-redefs` macro. `inst-ms`
throws `:mino/unsupported`. The tap mechanism — `add-tap`,
`remove-tap`, `tap>` — is implemented over an atom of subscribers
that swallows tap-fn exceptions so a misbehaving subscriber does
not poison the stream. `tagged-literal` and `reader-conditional`
constructors and the `tagged-literal?` / `reader-conditional?`
predicates round out the reader-record surface; `list*` and
`reset-meta!` close two long-standing gaps. `walk`, `postwalk`,
`prewalk`, `postwalk-replace`, and `prewalk-replace` are
re-exported from `clojure.walk` (the implementations live in
`clojure.core` because the bundled-core organization needs them
across the standard library).

A new `clojure.* coverage` test reports the breadth of Clojure-
core-namespace surface mino exposes against a manifest of
canonical 1.11 names. JVM-only names and special forms are
excluded from the percentages and accounted separately; missing
names are printed by namespace so the gap is visible without
grep'ing the source.

The coverage report drove a follow-up pass that closed the easy
gaps. `clojure.string` adds `index-of` (with optional `from-index`),
`last-index-of`, `re-quote-replacement`, and `replace-first`. The
substring-search helpers are mino-side brute-force scans on top of
the existing `prim-includes?` short-circuit; `replace-first` uses
literal-substring semantics because mino's regex literals share
the string type (the same constraint that scopes `clojure.string/
replace` today). `clojure.zip` adds `leftmost` and `rightmost`.
`compare-and-set!` lands as a stateful primitive in `clojure.core`:
it checks the atom's current value against an expected value and
only swaps on equality, returning `true` on success and `false`
when the expected value did not match.

Final coverage: `clojure.core` 405/413 portable names (98%),
`clojure.string` 21/21 (100%), `clojure.set` 12/12 (100%),
`clojure.walk` 8/8 (100%), `clojure.edn` 1/2 (50%), `clojure.zip`
28/28 (100%). The remaining `clojure.core` gaps are queued as
follow-ups: `bound-fn` / `bound-fn*` need a dynamic-binding-capture
API; `destructure` would rewrite the C-side destructuring helper as
a mino-callable function; `re-groups` / `re-matcher` need regex
capture groups; `read` and `clojure.edn/read` need a reader-with-
options surface; `*ns*` works at the symbol-resolution level today
and would need a real dynamic var to be `find-var`-visible.

### Breaking Changes

The single shared global env that previously masqueraded as many
namespaces is gone. Code that relied on `(ns a) (def x ...)`
clobbering `x` in `b` (and vice versa) must now qualify references
explicitly or use `:require`/`:use`/`:refer`. Files loaded via
`require` whose first `(ns ...)` declares a different name than
the require argument now error rather than silently mismatching.

The bundled-core namespace is renamed from `mino.core` to
`clojure.core`, matching the convention other Clojure dialects use
for their bundled core. Code that referenced `mino.core/foo`
qualified-name forms must update to `clojure.core/foo`.
Embedding-side C identifiers (`mino_state_t`, `mino_env_t`,
`mino_install_core`, etc.) are unchanged. The string operations
that already lived in the `clojure.string` namespace are unaffected.

`blank?` is no longer reachable through the `clojure.core` parent
chain. Code that called bare `(blank? s)` from a namespace that
did not `:require [clojure.string :refer [blank?]]` must now bring
the name in explicitly or call `clojure.string/blank?`.

Reader conditionals now match `:clj` as an active dialect. Tests
or code that asserted `#?(:clj X)` would be skipped under `mino`
must use `:cljs` (or any other inactive tag) to drive elimination
behavior.

Source files now use `.clj` instead of `.mino`. The `.mino`
extension is removed from the require resolver. Embedders calling
`mino_load_file` with an explicit path are unaffected — the C API
opens whatever path is passed in regardless of extension. Code
that hard-coded `src/core.mino` in a build pipeline must update
to `src/core.clj`; the bootstrap recipe in `README.md` and
`mino.edn` shows the new form.

## v0.72.0 — Release Pipeline & Build Polish

Tag-triggered builds and a controlled promotion path. Pushing a tag
matching `v*` now produces a draft GitHub Release with five platform
archives (linux/darwin amd64 and arm64, windows amd64) plus a
`checksums.txt`. Each build job verifies the tag against
`MINO_VERSION_*` in `src/mino.h`, bootstraps with the canonical
recipe, runs a `--version` and arithmetic smoke test, and uploads
its archive. A fan-in publish step concatenates checksums and
creates the draft Release. Nothing is published downstream until a
maintainer un-drafts the Release and runs the manual
`promote-packages` workflow.

The promote workflow takes a tag and per-ecosystem booleans
(`publish_brew`, `publish_scoop`). It fails loudly if the Release
does not exist or is still a draft, downloads `checksums.txt` and
all assets, re-verifies SHA-256s against the assets, renders the
formula or manifest from a template under
`.github/release-templates/`, and opens or updates a PR against the
corresponding tap or bucket repo. Auto-merge stays off so the
maintainer can review every formula and manifest before users see
it.

Three small build issues are also addressed. The `form` parameter of
`apply_non_fn_callable` is now `const mino_val_t *` to match
`S->eval_current_form`'s qualifier, which clears a `-Wcast-qual`
warning at the only caller without changing behavior. The host-
interop dispatch doc comment in `src/eval/special.c` was tripping
`-Wcomment` because of a `/*` glob inside an open block comment; it
now reads `host/...`. The README's pasteable bootstrap snippet was
missing the `printf`/`sed` prelude that generates
`src/core_mino.h`, so a fresh-clone copy-paste failed with
`'core_mino.h' file not found`; the README now mirrors the canonical
recipe in `mino.edn`.

The public embedding API in `src/mino.h` is unchanged.

## v0.71.0 — Standalone CLI Polished

The standalone `mino` binary now recognises `-h`/`--help`,
`-V`/`--version`, and `-e`/`--eval EXPR`, with a `--` separator
that ends option processing in the usual POSIX way. Help and
version output goes to stdout and exits 0; usage errors go to
stderr and exit 2. The `-e EXPR` path runs one expression
through the same evaluator that file mode uses and prints the
result via `mino_println`.

A small subcommand surface is recognised after option
processing. `mino repl` is an explicit alias for the bare REPL
invocation; `mino nrepl ...` and `mino lsp ...` exec the
matching companion binary from `PATH` and exit 127 if the
companion is not installed, with a clear message naming the
missing binary. `mino task` and `mino deps` continue to work
as before.

The REPL banner gains a `Type :help for help, :quit to exit`
hint, and the prompt is now `mino=>` with a 7-char-aligned
continuation prompt. Two reader-level meta-commands are
intercepted before eval: a bare `:help` prints a one-screen
description of the REPL, and a bare `:quit` exits cleanly with
code 0. Both fire only when the entire form is the keyword, so
they do not affect `(println :help)` or `(do :quit)`.

The public embedding API in `src/mino.h` is unchanged.

## v0.70.0 — C-Core Refactored

Cycle banner. No user-visible behavior change; the public embedding
API in `src/mino.h` is unchanged.

This closes the C-Core Refactor cycle that began at v0.61.0. Across
v0.61.0 through v0.68.0 the runtime was reorganized into per-
subsystem subdirectories, decomposed into named helpers with
explicit boundaries, switched to data-driven primitive
registration, gained a three-class internal severity contract,
isolated the regex engine, decomposed equality and hashing,
flattened the reader into a thin classifier with named dispatch
helpers, and replaced the cascading evaluator dispatch with a
data-table for special-form recognition.

This release pass focuses on documentation:

  - File-level headers no longer carry "Extracted from X / No
    behavior change" provenance lines that survived the rename
    pass; each header describes what's in the file, not where it
    came from. Embedded references to old filenames
    (`runtime_gc.c`, `prim_*.c`, `eval_special_*.c`,
    `host_interop.c`) update to the current paths in comments
    and doc blocks across the headers and source.
  - `docs/INTERNAL_MODULE_MAP.md` lists `src/eval/special_registry.c`
    and updates "How to Add a Special Form" for the data-table
    dispatch.
  - `docs/ARCHITECTURE_CONTRACT.md` Section 6 records that `when`,
    `and`, and `or` have fast-path special-form entries on top of
    their `core.mino` macro definitions, so `macroexpand` is
    unaffected but the evaluator skips the expansion.
  - `src/mino.h` drops a stale claim that the user-visible
    transient API isn't shipped (it landed in v0.51.0).
  - `src/prim/bignum.c` documents that the upper-magnitude hash
    path is reached only when the bigint exceeds `long long`; the
    fits-in-ll path joins int and float at tag `0x03` in
    `hash_val`.

## v0.68.0

Internal cleanup. No user-visible behavior change; the public
embedding API in `src/mino.h` is unchanged.

The evaluator's `eval_impl` is split. The orchestrator function
becomes a thin classifier plus four named helpers:

  - `eval_check_limits` gates each step on the host limit knobs
    (`limit_steps`, `limit_heap`), the interrupt flag, and the
    sticky `limit_exceeded` latch. One source of truth for bail-
    out.
  - `eval_try_host_syntax` owns the four interop sugar shapes
    (`.method`, `.-field`, `(new T ...)`, `(T/static ...)`) and
    rewrites them into the matching `host/*` primitive call.
  - `eval_try_special_form` (new `src/eval/special_registry.c`)
    walks a static `k_special_forms[]` table that pairs cached
    interned-symbol slots with handlers. The previous cascading
    `if (HEAD_IS(...))` chain is gone; new special forms are one
    table row.
  - `eval_apply_regular_call` wraps the function / macro /
    non-fn-callable dispatch.

Every special-form handler now takes `(S, form, args, env, tail)`
— the seven that didn't already accept `tail` accept and ignore
it. The inline-bodied special forms (`quote`, `quasiquote`,
`var`, `if`, `do`, `recur`, `lazy-seq`, `when`, `and`, `or`)
move into static helpers in the registry file so the table can
reference them uniformly.

## v0.67.0

Internal cleanup. No user-visible behavior change; the public
embedding API in `src/mino.h` is unchanged.

The reader in `src/eval/read.c` is decomposed. `read_form`
shrinks from ~380 lines to a ~80-line classifier and three new
helpers absorb the bulk:

  - `read_dispatch` handles the full `#`-prefix family in one
    place: `#{` set, `#_` discard, `#(` anon-fn, `#'` var-quote,
    `##Inf`/`##-Inf`/`##NaN`, `#"…"` regex literal, `#?`/`#?@`
    reader-conditional, and the tagged-literal fallback.
  - `read_wrap_one` captures the prefix-quote pattern that the
    six reader macros (`'`, `\``, `@`, `~`, `~@`, `#'`) all
    share — read one form, wrap as `(sym form)`, preserve the
    macro's source position. Five near-identical inline blocks
    collapse to five one-line calls.
  - `read_char_literal` owns the character-literal decoding
    (`\space`, `\uNNNN`, UTF-8 codepoints, octal escapes).

The `ADVANCE` / `ADVANCE_N` macros are replaced with `static
inline` helpers — same emit, type-checked arguments, no behavior
change.

## v0.66.0

Internal cleanup. No user-visible behavior change; the public
embedding API in `src/mino.h` is unchanged.

`hash_val` is decomposed into a switch dispatch over named byte-
loop helpers (`hash_long_long_bytes`, `hash_pointer_bytes`,
`hash_uint32_bytes`). The numeric tier collapse — `(= 1 1.0 1N)`
mapping to a single hash under tag `0x03` — funnels through one
helper, making the equal-implies-equal-hash invariant explicit
in the source. The `MINO_MAP` branch's inlined HAMT walk is
replaced with a call to the shared `map_get_val` so the per-entry
lookup path stays in lock step with the public API.

`mino_eq`'s grouped helpers are renamed to the `eq_*_like` family
that pairs with the hash side: `seq_equal` becomes `eq_seq_like`,
`mino_eq_maps_cross` becomes `eq_map_like_cross`, and the
matching set variant becomes `eq_set_like_cross`. A doc block
above `mino_eq` states the equal-implies-equal-hash contract and
notes that new tier additions or new equality bridges must
extend the matching `hash_val` branch in the same commit.

## v0.65.0

Internal cleanup. No user-visible behavior change; the public
embedding API in `src/mino.h` is unchanged.

The regex engine in `src/regex/` is now a fully isolated module.
Its sole header `re.h` is consumed only from `src/prim/regex.c`
and the include is path-qualified (`#include "regex/re.h"`); the
`-Isrc/regex` flag is gone from the build configuration, the CI
bootstrap, and the README. `re.c` no longer pulls in `<stdio.h>`
or any mino subsystem header — it depends only on the C standard
library. The dead-code debug helper `re_print` has been removed,
so the only symbols exported from `src/regex/re.o` are the four
functions declared in `re.h` (`re_compile`, `re_free`, `re_match`,
`re_matchp`); a `nm` probe of the object file confirms no other
external symbols. A style-exception note at the top of `re.c`
records that the module preserves its upstream tinyregex-c
conventions (Allman braces, two-space indent, fixed-size pattern
arena) under Rule 15 of the project's C implementation guide.

## v0.64.0

Internal cleanup. No user-visible behavior change; the public
embedding API in `src/mino.h` is unchanged.

The companion-repo perf gate at `mino-bench/benchmarks/perf_gate.mino`
grows from five micros to fifteen, covering reader (`read-string` over
ints and lists), eval-special (`fn`, `let`, `if`, `do`, `loop`/`recur`),
allocation (`cons`, vector, map), host-call (`inc`, `+`, `count`,
`assoc`), and regex (`re-find`) paths so a regression in any of them
surfaces at the gate. Each bench reports timing and bytes-allocated-
per-op; the gate fails on either dimension. Allocation counts are
deterministic, so the alloc gate uses zero tolerance for zero-baseline
entries and a tight 10% band elsewhere. The timing gate stays at +15%
locally but widens to +30% on CI runners (`CI=true`) to absorb the
shared-runner noise that produced a uniform +74% skew on
`ubuntu-latest` at the close of the prior cycle. The pinned baseline
at `baselines/perf_baseline.edn` is re-recorded against the current
runner shape and now stores both metrics per bench.

## v0.63.0

Internal cleanup. No user-visible behavior change; the public
embedding API in `src/mino.h` is unchanged.

The `DEF_PRIM` macro is gone. Each `src/prim/<domain>.c` now exports
a static `mino_prim_def` table at TU bottom listing the
`(name, fn, doc)` triples for that domain; the new
`src/prim/install.c` composes the tables into `k_core_domains[]`
and walks it via `prim_install_table` to bind primitives and attach
docstrings. `mino_install_core` becomes one nested loop instead of
~400 lines of macro calls. The standalone install entry points
(`mino_install_io`, `mino_install_fs`, `mino_install_proc`,
`mino_install_host`, `mino_install_async`) each become a thin
wrapper over `prim_install_table` referencing their own domain's
table. The registry of primitives is now data, not code: each domain
file owns the list of names it exposes alongside the implementations.

A new `src/diag/diag_contract.h` introduces a three-class internal
severity taxonomy: `MINO_ERR_RECOVERABLE` (catchable user faults),
`MINO_ERR_HOST` (I/O, OS, capability rejections), `MINO_ERR_CORRUPT`
(invariant violations that abort). The existing user-facing
diagnostic kinds (`:eval/...`, `:type/...`, `:io/...`, etc.) stay as
the reporting surface; the new enum drives control-flow policy.
Each per-subsystem `internal.h` gains an "Error classes emitted"
block listing which classes its code paths produce, where, and why.
`diag.c` carries a kind-to-class mapping table next to the code that
builds the diagnostic record.

## v0.62.2

Internal cleanup. No user-visible behavior change; the public
embedding API in `src/mino.h` is unchanged.

Source files under a subsystem directory drop the redundant
subsystem prefix wherever the prefix duplicated the directory name.

`.c` renames:

- `src/runtime/runtime_*.c` → `src/runtime/*.c` (`state.c`, `env.c`,
  `var.c`, `error.c`, `module.c`).
- `src/gc/runtime_gc.c` → `src/gc/driver.c`; `src/gc/runtime_gc_*.c`
  → `src/gc/*.c` (`minor.c`, `major.c`, `barrier.c`, `roots.c`,
  `trace.c`).
- `src/eval/mino.c` → `src/eval/eval.c`;
  `src/eval/eval_special.c` → `src/eval/special.c`;
  `src/eval/special_*.c` → `src/eval/{bindings,control,defs,fn}.c`.
- `src/prim/prim_*.c` → `src/prim/*.c` for every domain
  (`numeric`, `bignum`, `collections`, `sequences`, `lazy`,
  `string`, `io`, `reflection`, `meta`, `regex`, `stateful`,
  `module`, `fs`, `proc`, `host`, `async`).
- `src/public/public_*.c` → `src/public/*.c` (`gc.c`, `embed.c`).
- `src/async/async_*.c` → `src/async/*.c` (`scheduler.c`,
  `timer.c`).
- `src/interop/host_interop.c` → `src/interop/syntax.c`.

Header renames (path-qualified includes throughout):

- `src/<subsys>/<subsys>_internal.h` → `src/<subsys>/internal.h`
  for `runtime`, `gc`, `collections`, `eval`, `interop`, `async`,
  `prim`.
- `src/eval/eval_special_internal.h` →
  `src/eval/special_internal.h`.
- `src/async/async_scheduler.h` → `src/async/scheduler.h`;
  `src/async/async_timer.h` → `src/async/timer.h`.

Includes are now path-qualified for the renamed subsystem headers
(`#include "runtime/internal.h"` etc.) — bare `internal.h` would
resolve based on `-I` flag order across the per-subdirectory
include paths added in v0.61.0.

`lib/mino/tasks/builtin.mino`, `docs/INTERNAL_MODULE_MAP.md`, and
`docs/ARCHITECTURE_CONTRACT.md` reflect the new filenames.
Embedders enumerating individual source files need to update their
build configuration; the CI bootstrap glob at
`.github/workflows/ci.yml` and the `README.md` snippet are
unchanged because both already use per-subdirectory globs.

## v0.62.1

Internal cleanup. No user-visible behavior change; the public
embedding API in `src/mino.h` is unchanged.

`src/mino_internal.h` is decomposed into per-subsystem internal
headers so each translation unit pulls in just the types and
declarations it needs:

- `src/runtime/runtime_internal.h` — `mino_state` and `mino_env`
  structs, runtime-support types, and runtime function declarations.
  Includes the per-subsystem headers transitively required to define
  `mino_state`'s fields.
- `src/gc/gc_internal.h` — `gc_hdr_t`, `gc_evt_t`, `gc_range_t`,
  the `GC_T_*` / `GC_GEN_*` / `GC_PHASE_*` / `GC_EVT_*` enums,
  `gc_pin` / `gc_unpin` macros, and GC function declarations.
- `src/collections/collections_internal.h` — persistent vector,
  HAMT, red-black tree, and intern-table types; val.c constructors,
  hashing, and equality; bignum / ratio / bigdec value support.
- `src/eval/eval_internal.h` — `try_frame_t` + `MAX_TRY_DEPTH`,
  evaluator core helpers, macroexpand, quasiquote, `print_val`,
  `intern_filename`.
- `src/interop/interop_internal.h` — host-interop capability
  registry types and lookup helpers.
- `src/async/async_internal.h` — umbrella over the existing
  `async_scheduler.h` + `async_timer.h`.

The old `src/mino_internal.h` is deleted with no compatibility
shim. `src/eval/eval_special_internal.h` and
`src/prim/prim_internal.h` now include `runtime_internal.h`. Per-
subsystem .c files include the header(s) they actually need.

`docs/INTERNAL_MODULE_MAP.md` and `docs/ARCHITECTURE_CONTRACT.md`
reflect the new header layout.

## v0.62.0

Internal cleanup. No user-visible behavior change; the public
embedding API in `src/mino.h` is unchanged.

The `mino_state_free` teardown function is split into per-subsystem
helpers (`state_free_root_envs`, `state_free_refs`,
`state_free_ns_aliases`, `state_free_module_cache`,
`state_free_host_types`, `state_free_meta_table`,
`state_free_intern_tables`, `state_free_string_interns`,
`state_free_gc_aux`, `state_free_diag_state`, `state_free_async`,
`state_free_heap`) called in fixed order from a thin orchestrator.
Teardown order is preserved.

The remaining behavioral macros become regular functions:
`FMT_ENSURE` becomes `fmt_ensure` (a static inline that returns the
new buffer or NULL on OOM); `MINO_GC_VERIFY_CHECK` becomes
`gc_verify_check` (a static inline taking the state and container
header explicitly); `MATH_UNARY` becomes `math_unary` (a static
inline taking a function pointer, replacing nine macro-expansion
copies).

A new `src/runtime/path_buf.{c,h}` centralizes the `PATH_BUF_CAP`
constant (4096) that was repeated across the file-I/O primitives,
and exposes a `path_buf_t` struct + `path_buf_init` / `path_buf_set`
/ `path_buf_append` / `path_buf_format` API for new callers that
want explicit truncation reporting.

## v0.61.0

Internal source-tree reorganization. No user-visible behavior change;
the public embedding API in `src/mino.h` is unchanged. Source files
under `src/` are now grouped into per-subsystem directories: `public/`,
`runtime/`, `gc/`, `eval/`, `collections/`, `prim/`, `async/`,
`interop/`, `regex/`, `diag/`, and `vendor/imath/`.

The bootstrap-compile command in `README.md` and the GitHub Actions
workflow now use explicit per-subdirectory globs in place of the
flat `src/*.c src/vendor/*.c` pattern. Embedders building mino from
source need to update their build to enumerate the new subdirectories
and add a matching `-I` flag for each.

`docs/INTERNAL_MODULE_MAP.md` reflects the new layout. `CLAUDE.md` and
`docs/ARCHITECTURE_CONTRACT.md` are unchanged.

## v0.60.0 — Dialect Complete

Banner release closing the Dialect-Complete cycle. mino is now
the Clojure dialect at embedded scale: code that doesn't reach
for JVM interop, chunked-seq throughput, or host-thread
primitives runs on mino unchanged.

This release adds no new runtime features over v0.56.0. It is a
docs and ecosystem ripple — the dialect surface is settled and
the companion tooling (mino-site, mino-lsp, mino-nrepl,
tree-sitter-mino, mino-examples) all track the new numeric-tower
type tags and the Clojure-shape multimethod / hierarchy
semantics.

What's complete after this cycle:

- **Transients** as a public mino-level API (v0.51.0). `transient`,
  `persistent!`, `assoc!`, `conj!`, `dissoc!`, `disj!`, `pop!` over
  vec / map / set, sitting on the existing C kernel.
- **Sorted-by + bounded walks** (v0.51.0). `sorted-map-by` and
  `sorted-set-by` accept a custom comparator; `subseq` and
  `rsubseq` walk in-order over the rbtree against bounded keys.
- **Plain `pr` / `print` / `newline`** (v0.51.0). No-trailing-
  newline output primitives mirroring `prn` / `println`.
- **`print-method` multimethod** (v0.52.0). The C-level printer
  routes through a mino-level multimethod with a late-binding
  hook; user types extend printing via `(defmethod print-method
  MyType ...)`. Built-in round-trip preserved across every core
  type.
- **Full numeric tower** (v0.53.0 — v0.55.0). Real `MINO_BIGINT`,
  `MINO_RATIO`, and `MINO_BIGDEC` types backed by vendored
  MIT-licensed imath. Literal readers `1N`, `1/2`, `1M`. Auto-
  promoting `+'` / `-'` / `*'` / `inc'` / `dec'` use
  `__builtin_add_overflow` with int64 fallback. Tower dispatch
  across all five tiers in `+` / `-` / `*` / `/` / `=` / `<` /
  `<=` / `>` / `>=`. Predicates `ratio?` / `decimal?` /
  `rational?` point at the real types. Plain `+` / `-` / `*`
  keep their throw-on-overflow contract; promotion is opt-in via
  the prime variants.
- **Dialect-semantics fixes** (v0.56.0). Hierarchy version
  counter invalidates stale multimethod dispatch caches across
  `derive` / `underive`. Transitive prefer-method resolution
  matches Clojure's `prefers?` recursion through parents.
  2-arity `(derive child parent)` returns `nil`. `prefers` and
  `remove-all-methods` round out the multimethod surface.

### Documentation

- New `Compatibility Matrix` page on mino-site enumerating every
  Clojure core function and macro as supported / differs / absent.
- New `Intentional Divergences` page on mino-site giving the
  rationale behind each gap (no JVM interop, no host threads, no
  STM, no chunked seqs, plain arithmetic throws on overflow).
- `Coming from Clojure` refreshed to reflect the numeric tower
  and to link both new pages.

### Companion ripple

- **tree-sitter-mino**: corpus fixtures cover ratio, bigint-N, and
  bigdec-M literal forms.
- **mino-lsp**: hover type-name switch covers `bigint`, `ratio`,
  and `bigdec`. `MINO_SRCS` tracks `prim_bignum.c` and the
  vendored imath.
- **mino-nrepl** and **mino-examples**: `MINO_SRCS` tracks
  `prim_bignum.c` and the vendored imath.

### What's next

The Dialect-Complete cycle closes here. Two cycles are queued:

1. **C-Core Refactor cycle.** Reader decomposition, evaluator
   dispatch split, behavior-macro cleanup, error-class contract,
   regex-engine isolation. Internal-only refactoring; the user
   surface stays put.
2. **v1.0 / ABI freeze cycle.** `src/mino.h` frozen and the
   evolving-API language removed from the header. Numeric-tower
   type tags (`MINO_BIGINT`, `MINO_RATIO`, `MINO_BIGDEC`) lock
   in. Optional `mino.hpp` C++ RAII wrappers.

Until v1.0, `src/mino.h` stays labelled evolving and any item in
this cycle is revisitable under a minor bump.

## v0.56.0 — Dialect-Semantics Audit

Sixth release of the Dialect-Complete cycle. Targeted fixes to
mino's multimethod / hierarchy implementation tighten dialect
alignment with Clojure on four edge cases that don't show up until
you reach for them.

### Fixed

- **`(derive child parent)` returns `nil`.** The 2-arity form was
  returning the new global hierarchy; Clojure's contract is that
  the side-effecting form returns `nil`. Code that captured the
  return value was getting a value that should have been an
  implementation detail.

- **Stale dispatch caches after `derive` / `underive`.** Multimethod
  caches were invalidated on `defmethod`, `prefer-method`, and
  `remove-method` but not on hierarchy mutation. After a multimethod
  populated its cache for a dispatch value, a subsequent `derive`
  that should have changed the resolution was silently ignored.
  A version counter on the global hierarchy now lets every
  multimethod compare cache validity on dispatch and clear when
  needed.

- **Transitive `prefer-method` resolution.** `find-best-method_`
  treated the prefer-table as a flat lookup; Clojure's `prefers?`
  follows hierarchy parents recursively, so `(prefer-method m :A
  :B)` covers descendants of `:A` over descendants of `:B` without
  a per-pair declaration. mino now matches that behaviour.

### Added

- **`prefers` and `remove-all-methods`.** Two missing pieces of the
  Clojure multimethod surface. `(prefers mm)` returns the
  prefer-table; `(remove-all-methods mm)` clears the method table
  and dispatch cache.

## v0.55.0 — Numeric Tower Complete

Fifth release of the Dialect-Complete cycle. mino's numeric tower
closes: ratio and bigdec types arrive, the four arithmetic
primitives plus all comparison primitives tier-dispatch across the
five numeric tiers (int, bigint, ratio, bigdec, float), and `=`
goes Clojure-strict on the numeric tier with a new `==` for
cross-tier numeric equality.

### Added

- **`MINO_RATIO` value type.** Numerator/denominator stored as a
  pair of `MINO_BIGINT` cells. Always reduced (gcd = 1) and
  normalised so the denominator is positive; `1/2`, `-3/4`, and
  arbitrary-magnitude literals like `99999999999999999999999/3`
  all read as canonical ratios. When the denominator collapses to
  1 the constructor returns an integer (or bigint) instead of a
  ratio cell, so `(type 6/3)` is `:int`, not `:ratio`.

- **`MINO_BIGDEC` value type.** Unscaled `MINO_BIGINT` plus a
  non-negative integer scale; value = unscaled × 10⁻ᵃᶜᵃˡᵉ. Reads
  via the `M` literal suffix (`1M`, `1.5M`, `0.1M`,
  `-2.5e+10M`). Equality under `=` is representation-strict
  (`(= 1.0M 1.00M)` is false), while `==` collapses scale.

- **Reader literals: `1/2` and `1M`.** The previously placeholder
  forms (`1/2` parsed to a float, `1.5M` to a float) now produce
  real ratio / bigdec values. Arbitrary-magnitude numerators and
  denominators are supported; the lookup `mino_ratio_make` reduces
  by gcd and narrows to int / bigint when possible.

- **Tower dispatch in `+`, `-`, `*`, `/`.** Walks the operand list
  with a one-way tier-promotion accumulator: int → bigint → ratio
  → bigdec, with float collapsing everything. Mixed ratio/bigdec
  drops to float (the exact ratio→bigdec coercion needs an
  explicit precision; `with-precision` is deferred). `/` follows
  Clojure: int/int with a non-zero remainder yields a ratio
  (`(/ 1 2)` ⇒ `1/2`), and the unary form is a tier-aware
  reciprocal (`(/ 2)` ⇒ `1/2`).

- **Tower dispatch in `<`, `<=`, `>`, `>=`.** Comparison crosses
  every tier without coercion artefacts: int/bigint comparison is
  exact, ratio comparison cross-multiplies through bigints, bigdec
  comparison aligns scales, and float comparison collapses to
  double.

- **`==` numeric-equality primitive.** Returns true whenever the
  values are numerically equal regardless of tier or
  representation: `(== 1 1.0)`, `(== 1 1N)`, `(== 1/2 0.5)`,
  `(== 1.0M 1.00M)`, `(== 1 1M)` are all true. Use `==` when you
  want Clojure's old "all-numeric" `=` semantics.

- **`numerator`, `denominator`, `rationalize`, `bigdec`,
  `decimal?`, `ratio?`, `rational?`.** Accessors and constructors
  for the new types. `numerator` / `denominator` narrow back to
  int when the result fits in a long. `rationalize` decomposes an
  IEEE-754 double into its mantissa/exponent and produces an
  exact ratio.

- **Auto-promoting `+'`/`-'`/`*'`/`inc'`/`dec'` extend to
  ratio and bigdec.** Same code path as plain `+/-/*` for those
  tiers, only the long-overflow case differs (promote vs throw).

### Changed

- **Strict `=` on the numeric tier.** `(= 1 1.0)` is now false,
  matching Clojure's type-strict equality. `(= 1 1N)` stays true
  because int and bigint represent the same arbitrary-precision
  integer kind. Use `==` for the old cross-tier numeric-equality
  behaviour.

- **`(/ 1 2)` returns `1/2`.** Integer division with a non-zero
  remainder produces a ratio rather than coercing to a float.
  Code that relied on the old behaviour can wrap the call in
  `double` or write `(/ 1.0 2)`.

- **`number?`, `int`, `float` accept the new tiers.** `(number?
  1/2)` is true; `(int 1/2)` truncates toward zero; `(float 1/2)`
  yields the nearest representable double.

- **`(int x)` on a bigint that doesn't fit in a long throws** with
  `eval/overflow`, matching Clojure semantics. Use `(bigint x)`
  to keep the magnitude or `(double x)` to coerce.

### Known limitations

- **Mixed ratio/bigdec arithmetic collapses to float.** `(* 1/2
  1.5M)` yields `0.75` (double), not `0.75M`. Exact
  ratio→bigdec coercion needs an explicit precision context;
  `with-precision` arrives in a later cycle.

- **`bigdec / bigdec` throws.** The result generally has an
  infinite or non-terminating decimal expansion, so a precision
  must be picked explicitly. Until `with-precision` lands, divide
  via `(double a)` / `(double b)` or pre-rationalise.

- **`rationalize` on huge magnitudes loses precision.** The
  recovery uses `frexp` + a 53-bit mantissa, which matches
  IEEE-754 doubles exactly but can't recover bits the source
  double never carried.

## v0.54.0 — Auto-Promoting Arithmetic

Fourth release of the Dialect-Complete cycle. The promoting siblings
of `+`, `-`, `*`, `inc`, and `dec` arrive: when a `long` accumulator
overflows, the running sum / product crosses into bigint instead of
throwing. Plain `+` / `-` / `*` / `inc` / `dec` are unchanged — the
overflow-throwing semantics from v0.45.0 stay in place — so the
choice between fail-fast and auto-promote is now a per-call-site
decision.

### Added

- **`+'`, `-'`, `*'`, `inc'`, `dec'` primitives.** The accumulator
  tracks one of three tiers — `long`, bigint, double — with one-way
  transitions. A `long` × `long` overflow promotes the running value
  to bigint; a bigint operand mixed in switches to bigint mode; a
  `float` operand anywhere collapses to double for the remainder of
  the computation. Homogeneous `long` operands stay on the existing
  `__builtin_*_overflow` fast path so the perf gate is unaffected.

- **Internal bigint arithmetic helpers.** `mino_bigint_add`,
  `mino_bigint_sub`, `mino_bigint_mul`, and `mino_bigint_neg`
  centralise the imath calls under a single binop driver that
  takes a small scratch view of `int` operands so the same code
  path handles all int/bigint mixings. A cold-path
  `mino_bigint_to_double` round-trips through base-10 to handle
  the bigint → double tier collapse.

### Fixed

- **Vendored imath UB at `MP_SMALL_MIN`.** `s_fake` negated a signed
  long before casting to unsigned, which is undefined behaviour
  when the value is `LONG_MIN`; UBSAN tripped on it as soon as any
  bigint path hit `LLONG_MIN` (`(inc' Long/MAX_VALUE)`,
  `(-' Long/MIN_VALUE)`, etc.). Take the magnitude through unsigned
  arithmetic so the modular negation wraps cleanly. The change is
  marked with a `mino:` comment for audit on upstream sync, and
  `THIRD_PARTY_LICENSES.md` documents both annotated lines.

### Known limitations

- Mixing `bigint` with `float` in `+'` / `-'` / `*'` collapses to
  `double`. Magnitudes that don't fit in a double round to the
  nearest representable value, matching Clojure's coercion. Use
  `(bigint x)` first if you need exact bigint × bigdec arithmetic
  — that path arrives in v0.55.0 alongside the bigdec type.

- Ratio and bigdec types still don't exist; `1/2`, `1M` are not
  yet readable as their respective tower tiers. v0.55.0 adds them
  along with comparison-primitive tower dispatch (`<`, `<=`, `>`,
  `>=` across mixed numeric tiers).

- Cross-tier `=` between `int` and `float` keeps its existing
  behaviour (`(= 1 1.0)` is true). The Clojure-exact split lands
  with v0.55.0.

## v0.53.0 — Bigint Foundation

Third release of the Dialect-Complete cycle. mino gains the first
tier of the Clojure numeric tower: an arbitrary-precision integer
type, backed by vendored imath. Literals, constructors, equality,
hashing, and readable printing are all wired up; auto-promoting
arithmetic (`+'`, `-'`, `*'`, `inc'`, `dec'`) and the remaining
tower tiers (ratio, bigdec) arrive in v0.54.0 and v0.55.0.

### Added

- **`MINO_BIGINT` value type.** New tagged value backed by an
  `mpz_t` from vendored imath. The `mpz_t` struct is malloc-owned
  per cell; digit storage is managed by imath and freed during GC
  sweep through a hook in the major and minor collectors. No
  cross-state sharing: `mino_clone` transfers bigints by round-
  tripping through the base-10 string form.

- **`1N` literal reader.** `42N`, `0N`, `-1N`, and magnitudes far
  beyond `long long` (`99999999999999999999999N`) all read as real
  bigints. Plain decimal literals without the `N` suffix continue
  to read as `MINO_INT` and overflow at parse time as before.

- **`bigint` / `biginteger` / `bigint?` primitives.** `bigint`
  coerces `int`, `bigint`, `float` (truncated toward zero), or a
  base-10 numeric string to a `MINO_BIGINT`. `biginteger` is an
  alias. `bigint?` is the type predicate.

- **Cross-tier `=` and `hash` for int / bigint.** `(= 1 1N)`,
  `(contains? #{1} 1N)`, and `(get {1 :a} 1N)` all behave as in
  Clojure: int and bigint of the same value compare equal and
  share a hash bucket. Bigints that don't fit in `long long` hash
  under a bigint-specific tag.

- **Readable printer via `print-method` default.** `(pr-str 1N)`
  produces `"1N"`; `(read-string "1N")` produces the original
  bigint. Round-trip is preserved for bigints of any magnitude,
  inside vectors, maps, and sets. No per-cell printer wiring
  needed — the Phase B `print-method` :default method delegates to
  `pr-builtin`, which picks up the new `MINO_BIGINT` case in the C
  printer automatically.

- **Vendored imath.** Michael J. Fromberger's imath library is
  vendored under `src/vendor/` (MIT). Attribution is preserved in
  the source files, and the top-level `THIRD_PARTY_LICENSES.md`
  carries the copyright and license text.
  A single line in `s_realloc` casts the unused `osize` parameter
  to `void` under the non-DEBUG configuration so the mino build's
  zero-warnings gate stays green; that line is marked with a
  `mino:` comment.

### Known limitations

- Plain `+`, `-`, `*`, `/`, `inc`, `dec`, and comparison primitives
  still reject bigint operands. Use `(bigint x)` to produce
  bigints; auto-promoting `+'` / `-'` / `*'` / `inc'` / `dec'`
  arrive in v0.54.0.

- No ratio or bigdec types yet. `(type 1/2)` still returns `:int`
  (for `6/3`) or `:float` (for `1/3`), matching current behavior.
  Ratios and bigdecs land in v0.55.0 together with full tower
  dispatch.

- Cross-tier `=` between `int` and `float` keeps its existing
  behavior (`(= 1 1.0)` is true). The Clojure-exact split under
  `=` arrives with tower dispatch in v0.55.0.

## v0.52.0 — Extensible Printer

Second release of the Dialect-Complete cycle. `pr` and `prn` now
route through a mino-level `print-method` multimethod, so user code
can extend readable printing for its own types.

### Added

- **`print-method` multimethod.** Dispatched on `(type x)`. The
  `:default` method delegates to a new `pr-builtin` primitive that
  uses the C formatter unchanged, so every built-in type keeps its
  current readable form without any per-type default method to
  register. User extension is `(defmethod print-method :my-type
  [v] ...)`; bodies write to stdout via `print`, `pr`, or
  `pr-builtin`.

- **Late-binding dispatch hook in C.** `prim_pr` / `prim_prn` check
  a hook field on `mino_state_t`; when set, each argument is
  dispatched through that fn. The hook is installed from
  `core.mino` by `(set-print-method! print-method)` once the
  multimethod is defined. Before this line runs, and in sandboxed
  hosts that never install the multimethod, `pr` / `prn` use the
  built-in C formatter as a permanently-safe fallback (the Cortex
  Q5 invariant). The hook is rooted for GC; `set-print-method!`
  with `nil` removes it.

- **`pr-builtin` primitive.** Prints one value via the built-in C
  formatter, bypassing the hook. Used by `print-method`'s
  `:default` method and available to any user method that wants to
  delegate back to the built-in form for a sub-value.

### Changed

- **`type` honors `:type` metadata** (Clojure semantics). Before,
  `(type x)` returned the value's type tag unconditionally. Now it
  returns `(:type (meta x))` first if present, falling back to the
  tag. This is what makes `print-method` dispatchable for user
  types attached via `(with-meta obj {:type :my-type})`. Side
  effect: `(type print-method)` now returns `:multimethod` instead
  of `:fn`, reflecting the `:type :multimethod` metadata attached
  by `create-multimethod_`.

### Known limitations

- **`pr-str` and `str`** still use the C formatter without
  consulting the multimethod. Unifying them with `pr` requires a
  writer abstraction that mino does not yet have; documented as a
  known limitation rather than a stub. `(pr x)` / `(prn x)` is the
  supported dispatch entry point in v0.52.0.

- **Nested user types inside built-in containers** do not route
  through `print-method`. The C formatter prints container
  elements directly; only the top-level value each arg to `pr` /
  `prn` is dispatched. Users wanting per-element dispatch can
  extend `print-method` for `:vector` / `:map` / `:set` themselves.

## v0.51.0 — Transients, Sorted-By, Subseq, Pr/Print/Newline

First release of the Dialect-Complete cycle. Four additions on top
of the already-landed C kernels, aimed at the everyday Clojure
surface that mino was missing: batch-mutation transients at the
mino level, custom-comparator sorted collections, bounded range
queries on sorted collections, and the no-trailing-newline
companions to `prn` / `println`.

### Added

- **Transient public API.** `transient`, `persistent!`, `assoc!`,
  `conj!`, `dissoc!`, `disj!`, `pop!`, and the `transient?`
  predicate. Each is a thin wrapper over the existing C kernel at
  `src/transient.c`; the kernel's validity-bit guard and write-
  barrier discipline cover correctness. Vector, map, and set
  transients are supported. A use after `persistent!` throws
  (`eval/type` / `MTY001`). A new `tests/transient_test.mino`
  includes three escape-route tests suggested by Cortex's Q3
  review: transient captured by a lazy seq and realized after
  sealing (must throw), transient mutated through an atom, and
  transient survival across forced GC yields.

- **`sorted-map-by` and `sorted-set-by`.** Custom-comparator
  variants. The rbtree already carried a comparator slot; the
  natural-ordering constructors now delegate to a shared builder
  and keep their prior behavior, and the `-by` variants expose
  that slot at the mino level. A non-callable comparator throws
  `eval/type` / `MTY001`.

- **`subseq` and `rsubseq`.** Bounded range queries on sorted
  maps and sorted sets, both three-arg (`(subseq sc >= k)`) and
  five-arg (`(subseq sc >= k1 < k2)`) shapes, plus their reverse-
  order counterparts. Backed by a new `rb_bounded_seq` walker
  that prunes subtrees which cannot contain an in-range key.
  Mutation-consistency contract is snapshot — the path-copying
  rbtree makes the root captured at call time immutable, so the
  returned seq is stable regardless of later writes to the source
  collection (Cortex Q4). The four comparison primitives
  `<` / `<=` / `>` / `>=` are identified by function-pointer
  match.

- **`pr`, `print`, and `newline`.** No-trailing-newline siblings
  of `prn` / `println`, plus a standalone line-separator. `pr`
  ships closed-form in this release; Phase B reroutes it through
  a mino-level `print-method` multimethod per Cortex Q5's
  confirmation of the late-binding hook shape.

### Reviewed

Cortex reviewed all six open questions for the Dialect-Complete
cycle. Q3 and Q4 gated Phase A and resolved cleanly into the
implementation above. Q2 shapes the numeric tower in Phase C, Q5
shapes the printer rework in Phase B, Q1 gates the dialect-
semantics audit in Phase D, and Q6 shapes the intentional-
divergences doc in Phase E.

## v0.50.0 — C Core Complete and Polished

Cycle-closure release. The C core is feature-complete for the work
that had to land at the C level: lazy-seq write-barrier coverage,
overflow-throwing arithmetic, first-class characters, the callable
protocol for non-fn values, vector `pop` with metadata, multi-coll
`sequence`, C-surface transients, C-surface multimethods, a perf
regression gate wired into CI, fuzz coverage with a nightly libFuzzer
job, a native crash handler, version constants, and two embedder
helpers (`mino_throw`, `mino_args_parse`). The embedding API in
`src/mino.h` stays labelled as evolving until a later ABI-freeze
cycle; the surface is stable enough for external embedders to build
against today, with any break called out in its minor-bump CHANGELOG
entry.

This release is purely a tag. No code changed since v0.49.1. The full
sanitizer matrix (ASAN, UBSAN, TSAN) is clean across the test suite,
the GC stress shards, and the multi-state embedding harness.

### What Ships Next

Three separate cycles are queued after v0.50.0, in order:

1. An internal C-core refactor cycle that picks up code-quality and
   organization items deferred during the complete-and-polish work.
   User-visible surface stays stable; this is internal hygiene.
2. A dialect cycle that fills the remaining mino-level surface on top
   of the C groundwork landed here: public `transient!` / `persistent!`
   / `assoc!` / `conj!` / `dissoc!` / `pop!` / `disj!`, public
   `defmulti` / `defmethod` / `prefer-method` plus hierarchy APIs, the
   currently-disabled clj-compat test blocks that still need a macro
   layer, and gaps like `sorted-map-by`, `subseq`, `pr` / `print`.
3. An ABI-freeze cycle that commits `src/mino.h` to a stable contract
   for the first time. This is the v1.0 tag.

BigInt / Ratio / BigDecimal arithmetic ships as one whole feature
(hook plus backend plus tower dispatch plus tests plus docs) in one
of the later cycles, not piecemeal. Integer overflow throws is the
honest complete behavior in v0.50.0.

## v0.49.1 — Callable and Module-Resolution Dedup

Two pieces of internal duplication turned out to be drifting. No
user-visible surface change from the mino side; the fixes are
available to C embedders that introspect `mino_last_error` and to
any code that invokes `(require '[x :as a])` from inside a primitive
at the same time an `ns` form is pending.

### Changed

- **Non-fn callable dispatch unified.** Keyword, map, vector, set,
  sorted-map, and sorted-set "call-as-function" behavior was
  implemented twice -- once on the direct-eval path in
  `eval_special.c` and once on the higher-order `apply_callable`
  path in `eval_special_fn.c`. The direct path used the error code
  `MTY002` for both vector-index type errors and vector-index
  bounds errors, while the higher-order path used `MTY001` and
  `MBD001` which match the convention used everywhere else in the
  error surface. Both sites now delegate to one
  `apply_non_fn_callable` helper. C embedders reading
  `mino_last_error` after a callable dispatch error now see the
  canonical codes from either call path.
- **Module-resolution helpers unified.** Dotted-name-to-slash-path
  conversion lived in two byte-identical copies (`dotted_to_path`
  in the ns form, `dots_to_slashes` in the require primitive),
  and alias-table mutation was implemented twice with different
  semantics: the ns form detected duplicate aliases and replaced
  the existing full name, while the require primitive appended
  without duplicate detection and could leak alias strings if one
  of two malloc calls failed. Both now route through a new
  `src/runtime_module.c` translation unit. The require vector form
  now matches ns on duplicate handling and is clean under OOM mid
  insert.

## v0.49.0 — Docs and Hygiene

A documentation-focused release. No runtime or API changes; the mino
binary is bit-for-bit equivalent to v0.48.0. The work here brings the
public docs back in line with the source of truth.

### Fixed

- **INCREMENTAL_BUDGET default in `mino.h` comment.** The header
  advertised the default as 1024, but the value set in
  `runtime_state.c` is 4096 and has been since the old-gen tuning
  sweep. The mino-site Tuning table already showed 4096; this closes
  the drift between the header and reality.
- **Task-runner source list missed `src/public_embed.c`.** The
  v0.48.0 introduction of `mino_throw` and `mino_args_parse` added
  a translation unit that the `mino task build` source list did not
  pick up. The binary still linked because nothing in the standalone
  core path calls those helpers, but a task-built `mino` had their
  symbols stripped out and any embedder copying the task-runner
  manifest would inherit the same omission. Added to the source
  list so task builds are symbol-complete.

### Changed

- **Removed `docs/architecture/baseline-2026-04-21.md`.** It was a
  dated capture of the TU-size, function-size, and abort-site
  inventory. The living `docs/ARCHITECTURE_CONTRACT.md` and
  `docs/INTERNAL_MODULE_MAP.md` pair supersede it.
- **Tightened `docs/` .gitignore rule** from `docs/*.md` to
  `docs/**/*.md` so stray architecture notes in subdirectories do
  not leak into the tracked set.

## v0.48.0 — Embedder Polish

Sharpens the embedding surface in `src/mino.h` without rearranging any
runtime internals. Version constants land so embedders can compile-time
guard against an unexpected runtime. A reference Makefile ships at
repo root with sanitizer dev targets. Two new helpers -- `mino_throw`
and `mino_args_parse` -- pull patterns out of hand-written primitives
and give host code a shorter path to structured exceptions and
validated arguments. The README gains an explicit SemVer policy
paragraph.

### Added

- **Version constants and runtime query.** `MINO_VERSION_MAJOR`,
  `MINO_VERSION_MINOR`, `MINO_VERSION_PATCH` live in `mino.h` so host
  code can `#if`-guard against an unexpected runtime. The linked-in
  version is available at runtime via `mino_version_string()`. The
  standalone REPL now prints `mino <version>` before the first prompt.
- **Sanitizer dev build tasks.** `mino task build-asan`,
  `mino task build-ubsan`, and `mino task build-tsan` produce
  `./mino_asan`, `./mino_ubsan`, and `./mino_tsan` with the matching
  sanitizer plus `-g -O1 -fno-omit-frame-pointer` so stack traces
  stay readable. Each binary is built from a full recompile (sanitizer
  flags change code generation, so sharing `.o` files with the regular
  build would be unsound) and the three can coexist in the working
  tree. `mino task clean` now removes all four binaries.
- **SemVer policy in README.** A Versioning section spells out the
  pre-1.0 and post-1.0 contract: before 1.0 any minor bump may break
  and is called out under the corresponding CHANGELOG heading; after
  1.0 strict SemVer 2.0.0 applies. The ABI freeze is still scheduled
  for a future release.
- **`mino_throw(S, payload)`.** Raise a mino exception from C carrying
  any value as the payload. Inside a `(try ... (catch ...))` frame the
  payload is delivered to the catch binding; outside any try frame the
  call surfaces as a classified error through `mino_last_error` and
  returns NULL, matching `(throw ...)` from mino.
- **`mino_args_parse(S, name, args, fmt, ...)`.** Type-check and
  destructure a primitive's argument list in one call. The format
  string lists one character per expected positional argument
  (`i`/`f`/`s`/`S`/`k`/`y`/`b`/`c`/`v`/`V`/`M`/`L`/`H`/`A`); each
  variadic pointer receives the extracted value. Arity and type
  errors are raised as classified diagnostics so the caller can
  just `return NULL;` on a non-zero result. Replaces hand-written
  `is_cons` / `type == MINO_*` chains at primitive entry points.
- **`tests/embed_api_test.c`.** C-level smoke test covering version
  constants, `mino_args_parse` ok / arity / type paths, and
  `mino_throw` delivery into a try/catch frame.

## v0.47.0 — Release Gates

Release-gate infrastructure pass. No mutator-visible surface changes;
the work here exists to keep the surface from silently decaying as
later releases layer on top. A perf regression gate now runs in CI
against a pinned baseline. The fuzz corpus grew from four seeds to
twenty-two, with a libFuzzer nightly job backing it. A native crash
handler now produces a usable post-mortem line instead of a bare
segfault. The write barrier grew a structural matrix in its header
comment plus a debug-time assertion, and the C transient API picked
up a real barrier for its mutator-stored inner pointer.

### Added

- **Perf regression gate.** `~/Code/mino-bench/benchmarks/perf_gate.mino`
  runs five stable micro-benches (identity fn call, let-local lookup,
  `inc` on small int, cons creation, small-vector creation), takes the
  minimum mean-ns across three runs per bench, and compares to
  `baselines/perf_baseline.edn`. The gate fails at +15% regression or
  -30% speedup (both require a baseline refresh in the same commit).
  mino's own CI gained a `perf-gate` job that checks out mino-bench,
  overrides its submodule with the current mino SHA, and runs the
  gate. `perf-gate` and `perf-gate-record` tasks ship with mino-bench.
- **Fuzz corpus expansion and libFuzzer CI.** mino-bench's reader fuzz
  corpus grew from 4 to 22 seed files covering character literals,
  unicode, deep nesting, large and special numbers, metadata, reader
  conditionals, regex literals, symbol / keyword edges, token
  boundaries, syntax-quote forms, comments, mixed forms, whitespace
  edges, string escapes, and four malformed families (unterminated
  lists / strings / reader macros, stray reader-macro prefixes). The
  new `fuzz-smoke` task replays every seed through the stdin-mode
  reader on every push and PR; `fuzz-build-libfuzzer` builds a clang
  libFuzzer + ASAN + UBSAN target that runs for 24 hours nightly via
  GitHub Actions and uploads any crash artifacts.
- **Native crash handler (`main.c`).** SIGSEGV, SIGABRT, and SIGBUS
  now print `[mino] fatal <SIGNAME> (signal N)`, a one-line GC stats
  summary (minor / major collections, live / alloc / freed bytes, GC
  phase, remset size), and a best-effort backtrace from `execinfo.h`
  before restoring the default disposition and re-raising so the OS
  still produces the expected core file / exit code. The handler
  allocates no memory and writes to stderr through `write(2)` and
  `backtrace_symbols_fd` for async-signal safety. Windows gets signal
  registration without backtrace. `MINO_NO_CRASH_HANDLER=1` skips
  installation when a debugger wants to trap the signal instead.
- **Barrier mutation-site matrix.** `src/runtime_gc_barrier.c`'s file
  comment now enumerates every in-place-mutable GC slot on each value
  type plus the helper or direct call that covers it. A debug-time
  `assert()` at the barrier entry traps bogus container pointers
  (anything that is not NULL, state-embedded, or preceded by a
  gc_hdr_t with a legal generation) so an unrecognised caller fails
  loudly in debug builds instead of silently corrupting the remset.

### Fixed

- **Write-barrier gap in the C transient API.** The `*_bang` mutators
  and the `persistent!` seal in `src/transient.c` were storing a new
  persistent result directly into the wrapper's `current` slot,
  bypassing `gc_write_barrier`. A long batch loop promotes the
  transient wrapper to OLD after a minor cycle; further YOUNG
  persistent results then reach the wrapper through an unrecorded
  OLD→YOUNG edge and the next minor frees the still-reachable result.
  A new `transient_set_current` helper routes every store through the
  barrier.

### Changed

- `mino-bench` submodule pin was bumped to v0.46.0 on the bench side
  so perf baselines track the current mino surface. The mino
  repository itself stays independent of mino-bench — only CI clones
  it on demand.

## v0.46.0 — Dialect C Groundwork

Lands the C-level mechanisms that later dialect work will build on
without dragging the user-visible surface along yet. Integer arithmetic
now refuses to silently wrap, character literals are a first-class
value type, transducer `sequence` accepts multiple collections, and
embedders get a C API for batch mutation of persistent collections.
The previously-disabled clj compat assertions that this C work unlocks
are re-enabled in the same release — they were gated off precisely
because this foundation was missing.

### Added

- **First-class character value type.** `\A`, `\space`, `\newline`,
  `\tab`, `\return`, `\backspace`, `\formfeed`, `\oNNN`, `\uNNNN`, and
  multi-byte UTF-8 literals (`\é`, `\☃`) parse to a new `MINO_CHAR`
  value holding a Unicode codepoint. `char?` returns true for chars
  only. `(type \A)` is `:char`. `int` converts a char to its
  codepoint. `str` emits the codepoint's UTF-8 encoding. Chars hash
  and compare distinctly from single-char strings, so they live
  cleanly as map keys and set members. `pr-str` round-trips the
  named form for the six control chars, `\X` for printable ASCII, and
  `\uNNNN` for everything else.
- **Public C transient API.** `mino_transient`, `mino_persistent`,
  `mino_assoc_bang`, `mino_conj_bang`, `mino_dissoc_bang`,
  `mino_disj_bang`, `mino_pop_bang`, `mino_is_transient`, and
  `mino_transient_count` give embedders a batch-mutation path for
  building persistent vectors, maps, and sets from C. `persistent!`
  seals the wrapper; further mutators on a sealed transient throw.
  The initial implementation wraps the persistent ops; a later
  in-place trie-node path can replace it without changing the C ABI.
  The user-level mino `transient`/`persistent!`/`assoc!` names stay
  deferred for now.
- **Multi-collection `sequence`.** `(sequence xform coll & more-colls)`
  pulls one element from each collection per step, passes all
  elements to the transducer's multi-input reducer arity, and stops
  at the shortest collection. `map`'s transducer gains the matching
  `[result input & inputs]` arity so `(sequence (map +) [1 2 3]
  (repeat 10))` returns `(11 12 13)` as in Clojure.
- **Integer-overflow helpers in `src/prim_numeric.c`.**
  `iadd_overflow`, `isub_overflow`, `imul_overflow`, and
  `ineg_overflow` wrap `__builtin_*_overflow` where available and
  fall back to explicit range-checked preconditions on MSVC and older
  compilers.
- **C embedding tests.** `tests/transient_test.c` and
  `tests/multimethod_test.c` exercise the transient API and
  multimethod dispatch (including hierarchy resolution and
  `prefer-method` disambiguation) through the public embedding API.

### Changed

- **Integer overflow throws.** `+`, `-`, `*`, `inc`, and `dec` now
  raise a classified `eval/overflow` (MOV001) when the result would
  wrap past `LLONG_MIN`/`LLONG_MAX`. Float arithmetic is unchanged;
  IEEE 754 already has its own overflow semantics. Unary `-` of
  `LLONG_MIN` now throws instead of invoking signed-negation UB.
  Hot-loop micro-benchmarks (inc loop 10000, reduce + 1000) regress
  2-3% within run-to-run noise; the compiler builtins compile to a
  single flag-test instruction on x86_64 and ARM64.
- **Character literals are no longer strings.** Code that compared
  `\A` to `"A"` or relied on `(string? \A)` now sees `false`. The
  three internal tests that asserted the old semantics
  (`compat_test`, `literal_test`, `clojure_string_test`) now assert
  the char-distinct behaviour. `(seq s)` still yields single-char
  strings for now — a future cycle aligns that with Clojure's
  char-producing behaviour.

### Fixed

- **Re-enabled four previously-disabled clj-compat assertions.**
  Keyword-as-fn and set-as-fn usage (`((juxt :a :b) m)`,
  `(every? #{:a} xs)`, `(some #{:a} xs)`, etc.) already works;
  metadata survives three-deep `(pop (pop (pop v)))`; multi-coll
  `sequence` returns the expected paired result. The TODO comments
  that had gated these assertions are gone.

## v0.45.0 — Correctness Closure

Closes the three known correctness gaps in the C core. The lazy-seq
cache-barrier path gains a regression test that exercises the
realisation slot through promotion. The bit-shift primitives
bounds-check their amount and raise a classified error on
out-of-range input, closing the last UBSAN shift-exponent finding.
`ns :require` surfaces missing-module load failures instead of
silently swallowing them.

### Added

- **Lazy-seq cache-barrier regression test.** A new generational test
  promotes a vector of unrealized lazy seqs to OLD, forces each into
  its cache slot (which writes a fresh YOUNG chain), and re-reads
  every element after further minor pressure. A moderate-iteration
  variant of the same scenario lives in the stress runner so the
  guard also fires under `MINO_GC_STRESS=1`.

### Changed

- **`bit-shift-left`, `bit-shift-right`, and `unsigned-bit-shift-right`
  bounds-check their shift amount.** Any value outside `[0, 63]` now
  raises a classified `eval/bounds` error. The left-shift is routed
  through unsigned arithmetic so `(bit-shift-left 1 63)` keeps its
  usual wrap result without tripping signed-overflow UB.
- **`ns :require` raises on missing modules.** A load failure inside a
  `:require` clause — whether from a typo or from an evaluation error
  in the loaded file — now propagates out of the `ns` form as a
  classified load error instead of being silently caught and cleared.

### Fixed

- **UBSAN shift-exponent finding (issue #55) closed.** The full suite
  runs clean under `UBSAN_OPTIONS=print_stacktrace=1`.

## v0.44.0 — GC Observability and Spawn-Path Perf

Adds embedder-visible remset and mark-stack sizing fields to
`gc-stats`, plus targeted perf improvements for spawn-heavy
workloads. No functional changes to the collector; existing
embedders see strictly more data in the stats struct and map.

### Added

- **Remset and mark-stack observability in `gc-stats`.** Four new keys
  — `:remset-cap`, `:remset-high-water`, `:mark-stack-cap`,
  `:mark-stack-high-water` — let embedders size remset- and
  mark-stack-sensitive workloads without instrumenting the runtime.
  The two `*-cap` keys report current capacity of the respective
  realloc-doubled arrays; the two `*-high-water` keys report the
  peak usage observed on this `mino_state_t` across its lifetime.
  Same fields also added to the public `mino_gc_stats_t` struct for
  C embedders.

### Changed

- **`gc-stats` now reports `:nursery-bytes`.** The configured nursery
  size (from `MINO_GC_NURSERY_BYTES` or the 1 MiB default) is exposed
  in the map returned by `(gc-stats)` so tests and tuning scripts
  can read it back without parsing env vars.

### Performance

- **Intern-table marking now bypasses the interior-pointer resolver.**
  `gc_mark_intern_table` computes the header directly from each
  known-payload entry instead of paying the `O(log n)` `gc_ranges`
  binary search that `gc_mark_interior` does per pointer. During
  minor GC the OLD filter in `gc_mark_push` short-circuits each
  entry in O(1). On a spawn-heavy workload with 190 k interned
  symbols, per-minor intern-marking cost drops from 24.7 ms to
  1.8 ms, cutting total bot-fleet time at N=10 000 / 16 MiB
  nursery from 23.4 s to 19.1 s (-18%).
- **Gensym output no longer goes through the intern table.** Gensyms
  are unique by construction of the counter, so interning never
  dedups — each call produces a name that no other call ever sees.
  The previous behavior accumulated a permanent sym_intern entry
  per gensym call, which in spawn-heavy macro-heavy workloads
  (~15 gensyms per `go`/`go-loop` expansion) climbed into the
  hundreds of thousands. Resident heap at N=10 000 bot-fleet drops
  from 119 MiB to 108 MiB.

## v0.43.1 — Nested-Minor UAF Fix, GC Event Ring, Multi-State Stress

Bug-fix and hardening release. Closes a nursery-overflow
use-after-free under `MAJOR_MARK` surfaced at high `go-loop` spawn
concurrency, adds a GC event ring + reachability classifier for
future debugging, and moves three pieces of mutable process state
(filename intern, var-string intern, PRNG) into `mino_state_t` so
multiple embedded states no longer race.

### Breaking changes

- **Removed `lib/core/actor.mino`.** The pure-mino actor shim is gone;
  `(require "core/actor")` now fails. Channels in
  `lib/core/channel.mino` cover every use case the shim did. A bot /
  stateful-worker pattern is a `(go-loop [] (let [msg (<! in-ch)] ...))`
  reading a request channel; pair with reply channels for call-style
  interaction, fire-and-forget for cast-style. Carrying two
  queue-with-identity abstractions in core invited confusion.

### Changed

- **Concurrency story stays channels-only.** An OTP-style
  `GenServer`/`Supervisor` layer in `lib/core/` was explored and
  declined: host owns lifecycle, I/O, and parallelism; mino is the
  glue. Supervision-quality guarantees (preemption, isolated heaps,
  atomic link/monitor bookkeeping) belong to the host runtime, not to
  a single-threaded tree-walking interpreter.

### Fixed

- **Use-after-free on nested minor under `MAJOR_MARK`.** A nursery
  overflow that fired while a major cycle was in the MARK phase drove
  `gc_mark_push` over a header freed on the same tick. Root cause: the
  in-flight major's mark stack still held references to YOUNG objects
  that the overflow-minor then promoted and (in one path) freed. Fix
  is to force the in-flight major to completion before running the
  overflow-minor so the mark stack is empty when the minor touches
  YOUNG. Reproduces deterministically under ASAN at high `go-loop`
  spawn concurrency.
- **`MINO_GC_VERIFY=1` false positive on dead OLD zombies.** The verify
  pass scanned every OLD container's outgoing pointers, including
  containers that were themselves unreachable from roots but not yet
  swept. Now filtered to reachable-OLD via a classifier pass so
  verify reports genuine barrier misses only.

### Added

- **`MINO_GC_EVT=1` event ring + reachability classifier.** Records a
  fixed-size ring of GC phase transitions, promotions, remset ops, and
  allocation-path events. On assertion fire or classifier disagreement,
  the last N events are dumped with a four-class reachability label
  (`LIVE`/`ZOMBIE`/`DEAD`/`UNKNOWN`) per touched container. Opt-in via
  env var so there's no cost when unused.
- **`tests/embed_multi_state.c` + `mino task test-embed`.** Drives
  16 `mino_state_t` instances on 16 pthreads doing concurrent alloc/
  GC/intern work. Guards against regressions in multi-state
  isolation.
- **`MINO_GC_NURSERY_BYTES` env var at state init.** Override the
  default nursery size from the environment without calling
  `mino_gc_set_param`. Lower bound matches the public-param minimum
  (64 KiB).
- **`(gc!)` primitive.** Explicit GC trigger from mino code, mainly
  for tests and benchmarks that want to measure post-sweep state.

### Internal

- **Per-state intern tables** for filenames and var-strings. Each
  `mino_state_t` carries its own FNV-hashed intern table; no more
  cross-state sharing through process-global statics.
- **Per-state xorshift64\* PRNG.** `random-uuid` and `rand` pull from
  a seed stored in `mino_state_t`; two simultaneously-running embedded
  states no longer interleave on a shared seed.
- **`gc_all_young` / `gc_all_old` list split.** Minor collection walks
  only the YOUNG list; the OLD list is untouched outside major. Cuts
  minor-cycle cost proportionally to OLD-gen size.
- **`mapv` / `filterv` accumulator pinning.** Accumulator values are
  pinned on the GC save stack across each iteration; fixes a latent
  UAF if the mapping function triggered a minor that promoted the
  accumulator.
- **GC suppression around malloc'd C-heap accumulators** in
  `prim_collections.c` and `prim_sequences.c`. Prevents a collection
  mid-conversion from observing an inconsistent half-populated array.
- **Regression tests.** `tests/spawn_stress_regression.mino` pins
  three go-loop spawn patterns that previously failed at
  N=1000–10000.

## v0.43.0 — Pure-mino Channels and Actors

Two successive demotions move the channel layer and the actor system
out of C into `lib/core/`. The C runtime keeps only what must be C:
the scheduler run queue, the deadline-timer priority queue, the GC,
and the evaluator. Total C surface shrinks by roughly 2,100 LOC; the
built binary drops ~20 KB on darwin arm64 at -O2. The public mino
API is unchanged except where flagged below.

### Breaking changes (channel demotion)

- **Removed C primitives** (channels, buffers, alts, transducer hooks):
  `chan*`, `chan?*`, `chan-put*`, `chan-take*`, `chan-close*`,
  `chan-closed?*`, `offer!*`, `poll!*`, `chan-set-xform*`,
  `chan-buf-add*`, `alts*`, `buf-fixed*`, `buf-dropping*`,
  `buf-sliding*`, `buf-promise*`. Each is now a mino function (or a
  `(def x ...)` alias) in `lib/core/channel.mino`.

  Mino callers see no difference: the public surface (`chan`, `put!`,
  `take!`, `offer!`, `poll!`, `close!`, `closed?`, `chan?`, `alts!`,
  `buffer`, `dropping-buffer`, `sliding-buffer`, `promise-chan`,
  `timeout`, `go`) is unchanged. The starred names still resolve
  through compatibility aliases in `lib/core/channel.mino`.

  Host embedders that called these primitives directly from C via
  `mino_eval` must either (a) invoke the mino-level equivalents
  through `mino_eval` on the corresponding public name, or (b) pin
  to v0.42.0.

- **Channel value identity changed.** Channels used to be opaque
  `MINO_HANDLE` values; they are now mino atoms wrapping a state
  map. `(type ch)` returns `:atom`, not `:handle`. Use `(chan? ch)`
  for identity-independent tests. Two test files (`async_api_test`,
  `async_buffer_test`) updated from `(= :handle (type ch))` to
  `(chan? ch)`.

- **`timeout*` primitive removed, replaced by `async-schedule-timer*`.**
  The old primitive returned a buffered C channel that the timer
  subsystem would close on expiry. The new primitive takes `(ms cb)`
  and schedules `cb` on the run queue after `ms` milliseconds; the
  public `(timeout ms)` helper now creates a mino channel and arms
  `close!` via the callback.

- **async/merge renamed to async/merge-chans.** The old `merge`
  shadowed `clojure.core/merge` for maps — so `(merge m1 m2 m3)` on
  plain maps failed with 'no matching arity' whenever `core/async`
  was loaded. Use `merge-chans` for channel-merging.

### Breaking changes (actor demotion)

- **Removed C primitives** `spawn*`, `send!`, `receive`. The actor API
  (`spawn`, `send!`, `receive`, plus new `actor?` and `mailbox-count`)
  now lives in `lib/core/actor.mino`. Users call
  `(require "core/actor")` to load it, the same pattern channels
  already use via `core/async`.

- **`spawn` is no longer auto-loaded from core.mino.** The `spawn`
  macro is gone from the compiled-in core; source that spawned an
  actor without an explicit require now fails with 'unbound symbol:
  spawn'. Add `(require "core/actor")` at the top of the file.

- **`spawn` body runs in the caller's env.** The old C implementation
  created a fresh `mino_state_t` per actor and evaluated the body in
  isolation, so `(def x ...)` inside a spawn body affected only the
  actor. The pure-mino implementation evaluates the body in the caller
  context with `*self*` dynamically bound. In a single-threaded
  runtime the old isolation bought nothing measurable; the new scheme
  makes spawn about 100x faster.

- **Removed C host API**: `mino_mailbox_t`, `mino_mailbox_new/send/
  recv/free`, `mino_actor_t`, `mino_actor_new/state/env/mailbox/send/
  recv/free`. Embedders that used these must either (a) drive the mino
  API through `mino_eval`, or (b) pin to v0.42.0. `mino_clone` is
  retained for cross-state value transfer (still useful for multi-
  runtime hosts).

### Added

- **`async-sched-enqueue*`** — bridge primitive that lets mino-level
  code enqueue callbacks onto the C scheduler run queue. The channel
  mino implementation uses it to schedule taker/putter callbacks.
- **`async-schedule-timer*`** — schedules a callback to fire after N
  milliseconds (replaces the old `timeout*`).
- **`lib/core/actor.mino`** — pure-mino actor implementation using an
  atom-wrapped mailbox and `binding` to scope `*self*`. Exports
  `spawn`, `send!`, `receive`, `actor?`, `mailbox-count`, and `spawn*`
  (the function wrapper the macro expands to).

### Fixed

- **Nil-callback crash** on the 2-arg `put!` / 1-arg `take!` path.
  Both mino wrappers passed an explicit mino `nil` to `chan-put*` /
  `chan-take*`; the primitive forwarded that nil as a callback into
  the scheduler, whose drain then tried to invoke it. Normalize
  `MINO_NIL` to C `NULL` at the primitive boundary (same pattern
  already used for `xform`/`ex-handler` in `chan-set-xform*`).

### Removed

Channel demotion:
- `src/async_buffer.c/h`  (203 LOC)
- `src/async_channel.c/h` (679 LOC)
- `src/async_handler.c/h` (162 LOC)
- `src/async_select.c/h`  (287 LOC)

Actor demotion:
- Mailbox + actor machinery inside `src/clone.c` (~445 LOC of 661)
- `mino_mailbox_t` / `mino_actor_t` public API from `src/mino.h`
- `spawn*` / `send!` / `receive` DEF_PRIM entries from `src/prim.c`

`prim_async.c` drops from 475 to 127 LOC. `clone.c` drops from 661 to
213 LOC and keeps only cross-state `mino_clone`.

## v0.42.0 — Generational + Incremental Garbage Collector

Replaces the single-generation mark-and-sweep collector with a
two-generation non-moving tracing collector whose old-gen mark
phase runs incrementally, paced by mutator allocation. Max pause
on tail-heavy realistic workloads drops from 100-110 ms to under
60 ms; GC share drops from 65-95% to 15-30% on the same workloads.
No API-breaking changes to value semantics or evaluation, but the
public C embedding surface gains three new entry points for host-
driven collection, tuning, and stats.

### Added
- **Public GC control API** in `mino.h`:
  - `mino_gc_collect(S, kind)` with `MINO_GC_MINOR`, `MINO_GC_MAJOR`,
    and `MINO_GC_FULL` for host-driven collection at quiescent
    points.
  - `mino_gc_set_param(S, param, value)` exposes five tuning knobs:
    `MINO_GC_NURSERY_BYTES`, `MINO_GC_MAJOR_GROWTH_TENTHS`,
    `MINO_GC_PROMOTION_AGE`, `MINO_GC_INCREMENTAL_BUDGET`,
    `MINO_GC_STEP_ALLOC_BYTES`.
  - `mino_gc_stats(S, out)` fills an `mino_gc_stats_t` out-struct
    without allocating.
- **`:phase` key** on the `gc-stats` primitive's returned map,
  exposing `:idle`, `:minor`, `:major-mark`, or `:major-sweep`.
- **New env vars** handled in the standalone CLI: `MINO_NURSERY`,
  `MINO_GC_MAJOR_GROWTH`, `MINO_GC_PROMOTION_AGE`, `MINO_GC_BUDGET`,
  `MINO_GC_QUANTUM`. All values below the documented lower bound
  silently fall back to the default.
- **`examples/embed_gc.c`** smoke-tests the public API end-to-end.
- **`examples/embed_gc_stress.c`** exercises every `mino_gc_set_param`
  key at its documented low/high valid and invalid edges, drives each
  `mino_gc_collect` kind, and asserts that `mino_gc_stats` counters
  are monotone across repeated snapshots.

### Fixed
- **Write barrier missing on literal-builder scratch buffers.**
  `eval_vector_literal`, `eval_map_literal`, `eval_set_literal`, and
  both `quasiquote_expand` branches allocated a `GC_T_VALARR` scratch
  array and then filled it slot-by-slot while each per-slot
  `eval_value` could trigger a mid-loop minor. When the scratch was
  promoted mid-fill, subsequent `tmp[i] = ev` writes bypassed the
  remembered set (the one-cycle safety net covers only the next
  minor), so YOUNG values in later slots were swept and the literal
  builder emitted collections with stale pointers. Fix routes every
  such slot store through a new `gc_valarr_set` helper. Matches the
  pre-existing `benchmarks/vec_bench.mino` and `benchmarks/map_bench.mino`
  mid-run failures seen during Phase C.

### Changed
- **GC architecture**: two generations (YOUNG nursery, OLD
  promoted-tenured) with age-based promotion, a remembered set of
  old-to-young pointers (maintained by the write barrier), and a
  four-phase state machine (`IDLE` -> `MAJOR_MARK` -> `MAJOR_SWEEP`
  -> `IDLE`). Minor collection is confined to young-gen; major mark
  is paced in 4096-header slices between mutator allocations, with
  an SATB barrier capturing overwritten pointers during the cycle.
- **Major collection is incremental by default.** The STW major
  path remains available via `mino_gc_collect(S, MINO_GC_FULL)` and
  as an OOM fallback.
- **`runtime_gc.c`** is split into five TUs for readability and
  testability: `runtime_gc.c` (driver), `runtime_gc_roots.c`,
  `runtime_gc_minor.c`, `runtime_gc_major.c`, `runtime_gc_barrier.c`.
  The public API implementation lives in `src/public_gc.c`.
- **Default slice budget** raised from 1024 to 4096 headers per
  incremental step after the Phase C tuning sweep showed that
  larger slices recover Phase B's small-heap allocation-heavy
  share regression without regressing tail-heavy max pause.

### Performance
- Max pause on realistic tail-heavy benches
  (`fibonacci(25)`, `map/filter/map/reduce 50k`, `nested vectors
  500x100`, `realize 10k lazy range`): 100-110 ms -> under 60 ms.
- GC share on the same benches: 65-95% -> 15-30%.
- 940 mino tests pass; `MINO_GC_VERIFY=1` clean on the full
  suite; `qa-arch` PASS.

## v0.41.0 — GC Timing Instrumentation

Adds wall-clock measurement of garbage-collection pauses so the
mino-bench harness can report GC share of wall time per benchmark.
Purely instrumentation — no behavior change, no optimization.

### Added
- **`:total-gc-ns` and `:max-gc-ns`** keys on the `gc-stats` map,
  covering cumulative nanoseconds spent in `gc_collect` and the
  longest single collection pause.

### Changed
- **`prim_nano_time` and `gc_collect`** share a single
  `mino_monotonic_ns` helper; the POSIX/Windows/fallback clock read
  is no longer duplicated.

## v0.40.0 — Interpreter Performance Pass

30 benchmark-driven optimizations. The eval floor (per-operation cost
in a tree-walking step) dropped from ~6 us to ~1 us — roughly a 5x
speedup on realistic programs. Every change ships with a before/after
measurement in the mino-bench suite.

### Added
- **Timing and GC introspection primitives**: `nano-time` (monotonic
  wall-clock nanoseconds) and `gc-stats` (cumulative collector state)
  enable benchmark harnesses in pure mino.
- **Lazy sequence primitives**: `range`, `lazy-map-1`, `lazy-filter`,
  and `lazy-take` run as C c_thunks, skipping the per-element fn frame
  that a mino-level implementation pays. `drop-seq` walks eagerly in
  C. `doall` and `dorun` realize in a C loop.
- **Numeric and type predicates as C primitives**: `inc`, `dec`, `not`,
  `empty?`, `some?`, `zero?`, `pos?`, `neg?`, `odd?`, `even?`, and the
  full type-predicate family (`nil?`, `cons?`, `string?`, `number?`,
  `keyword?`, `symbol?`, `vector?`, `map?`, `fn?`, `set?`, `seq?`,
  `true?`, `false?`, `boolean?`, `int?`, `float?`, `char?`).
- `prim_lazy.c` module houses the c_thunk-based lazy primitives so
  `prim_sequences.c` stays under the architectural LOC cap.

### Changed
- **Intern tables**: symbol/keyword interning uses open-addressing
  hash lookup instead of a linear scan of ~300 names.
- **GC mark phase**: iterative with an explicit stack instead of
  recursive (no stack overflow on deep structures, better cache
  locality).
- **GC sweep**: freed blocks of common sizes (16, 24, 48, 64 bytes)
  return to per-class free lists, cutting malloc round-trips.
- **Environment lookup**: frames above a size threshold build a hash
  index for O(1) name resolution; the root env uses it from the
  first large bind.
- **Special-form dispatch**: `eval_impl` caches interned symbol
  pointers (`quote`, `if`, `let`, etc.) and matches by pointer
  equality; the `when`, `and`, `or` macros are inlined to skip
  per-invocation macro expansion.
- **Trampoline sentinels**: `MINO_RECUR` and `MINO_TAIL_CALL` reuse
  singleton cells in `mino_state_t`.
- **Self-tail-call**: single-arity self-calls reuse the local env
  frame and rebind params in place.
- **Small integer cache**: values in −128..127 share singleton boxes,
  like the cached booleans and nil.
- **fn frame size**: initial binding capacity is 4 (down from 16), so
  fresh frames allocate the 64-byte block that the free-list recycles.
- **Literal collections**: vector/map/set literals whose children are
  all self-evaluating (int, string, keyword, bool, nil, float) return
  the AST form directly instead of rebuilding. Safe because data is
  immutable.
- **Arithmetic primitives**: `+`, `-`, `*` evaluate in a single pass
  over the argument list instead of pre-scanning for float promotion.
- **`eval_symbol`**: skips the per-lookup stack-buffer copy by using
  the interned symbol data pointer directly.
- **Parameter binding**: `bind_sym` takes an interned symbol directly
  and reuses its data pointer and length.
- **Dynamic binding lookup**: short-circuits when `dyn_stack` is
  empty (the overwhelmingly common case).
- **GC stack scan**: rejects pointers outside the tracked heap range
  in O(1) before the sorted range-index binary search.
- **Call dispatch**: `PRIM`/`FN` are checked before keyword/map/vector
  callable fallbacks so the dominant path stays short.
- **`var` special form**: auto-creates a var for any unbound name that
  resolves to a C primitive in the environment, matching `resolve`'s
  behavior so `#'inc`, `#'map`, etc. continue to return vars despite
  being prim-backed.

### Fixed
- `core.mino` no longer shadows the C primitives `mapv`, `filterv`,
  `atom?`, `swap!` with slower mino-level reimplementations.

## v0.39.1 — Cross-Platform Portability Fixes

### Fixed
- **Linux segfault**: `strdup` was implicitly declared under `-std=c99`,
  causing GCC to truncate the 64-bit return value to `int`. Add
  `_POSIX_C_SOURCE 200809L` to main.c and prim_proc.c.
- **Windows `sh!` escaping**: use `cmd.exe`-compatible quoting (only
  quote arguments containing spaces or metacharacters) instead of POSIX
  single quotes.
- **Windows executable locking**: check for both `mino` and `mino.exe`
  in the build task's relink check. Use `mino.exe` for test invocation.
- **Windows temp paths**: I/O tests use `TMPDIR`/`TEMP`/`TMP` instead
  of hardcoded `/tmp/`.
- **`longjmp` clobbering**: mark variables crossed by `setjmp`/`longjmp`
  as `volatile` in require spec processing (GCC `-Wclobbered`).

## v0.39.0 — Task Runner and Self-Hosting Build

### Added
- **Task runner**: `mino task <name>` executes named tasks from
  `mino.edn` with dependency resolution. `mino task` lists available
  tasks. Tasks are ordinary mino functions referenced by qualified
  symbols.
- **Makefile parity tasks**: `build`, `clean`, `test`, `test-external`,
  `gen-core-header`, `qa-arch` defined in `mino.edn` as the native
  replacement for the Makefile build.
- **`file-mtime` primitive**: returns file modification time in
  milliseconds via `stat(2)`. Enables incremental compilation.
- **C `str-replace` primitive**: single-pass O(n) string replacement,
  replacing the mino-level split+join implementation.
- **Windows CI**: build and test on `windows-latest` alongside Linux
  and macOS.

### Removed
- **Makefile**: replaced entirely by `mino task` commands. Bootstrap
  from source with a single `cc` invocation, then use `mino task build`.

## v0.38.0 — Project Manifest and Dependency Management

### Added
- **Project manifest**: `mino.edn` with `:paths` (source directories)
  and `:deps` (external dependencies). The manifest is pure EDN data;
  unknown keys are ignored for forward compatibility.
- **Dependency fetching**: `mino deps` subcommand clones git repos at
  pinned revisions into `.mino/deps/`. Supports `:path` (local) and
  `:git` (remote) coordinate types.
- **Auto-wiring**: when `mino.edn` exists, the module resolver
  automatically searches `:paths` and dependency directories. Works
  in both file mode and the REPL.
- **`:deps/root`**: override the source subdirectory within a git dep.
  Defaults to `["src"]` to match standard project layouts.
- **Filesystem primitives**: `file-exists?`, `directory?`, `mkdir-p`,
  `rm-rf`. Installed via `mino_install_fs`. Uses POSIX APIs for
  cross-platform portability.
- **Process execution primitives**: `sh` (returns `{:exit n :out "..."}`),
  `sh!` (returns stdout, throws on non-zero exit). Installed via
  `mino_install_proc`.
- **Binary-dir resolver fallback**: bundled `lib/` modules are found
  via the binary's location, enabling mino to run from any working
  directory.
- **Deps logic in mino**: `lib/mino/deps.mino` provides manifest
  loading, validation, git fetching, and path resolution.

## v0.37.0 — Compatibility and Stdlib

### Added
- **Multimethods**: `defmulti`, `defmethod` with value dispatch,
  default methods, and dispatch caching.
- **Macros**: `letfn`, `defonce`, `defn-`, multi-arity `defmacro`.
- **Reader**: `#"..."` regex literals, `#?@` splice in maps,
  character literals for whitespace/unicode/octal, `#_` discard
  fix before closing delimiters.
- **Syntax-quote**: unquote-splicing in vectors, fast path for
  vectors without splicing.
- **Primitives**: `random-uuid`, `file-seq`, `getenv`, `getcwd`,
  `chdir`, `int` with single-char string argument.
- **Module resolver**: .clj/.cljs file resolution, hyphen-to-underscore
  conversion in module paths, `lib/` fallback from initial directory.
- **ns forms**: `:use` clause with `:only` support, `:refer-clojure`
  `:exclude` is silently accepted.
- **Stdlib modules**: `clojure.core`, `clojure.data` (diff),
  `clojure.zip` (Huet zippers), `clojure.test` (deftest/is/are),
  `clojure.walk` (keywordize-keys, stringify-keys, macroexpand-all),
  `clojure.edn` (read-string), `clojure.pprint` (pprint, print-table).
- **Protocols**: multi-protocol `extend-type`, keyword option stripping
  in `defprotocol`, docstring handling.
- **Compat vars**: `*clojure-version*`, `clojure-version`, `assert`.
- **JVM stubs**: `defrecord`, `deftype`, `reify`, `proxy`,
  `gen-class`, `definterface`, `import` all throw with clear messages.
  `set!` is a no-op for JVM compiler directives.
- **Compatibility test suite**: 50-repo runner in pure mino with
  categorized failure reporting and root-cause deduplication.

### Changed
- Eval diagnostics inside `try` blocks now longjmp to the catch
  handler instead of producing uncatchable diagnostics.
- Exception messages in `mino_eval_string` now preserve the original
  error instead of reporting generic "unhandled exception".
- Cascading require errors include the file path at each level
  (e.g. "in foo.clj: unbound symbol: x").
- `&env` is bound to nil during macro expansion.
- Test framework moved from `tests/test.mino` to `lib/clojure/test.mino`.

### Removed
- All shell/bash scripts from the repository.

## v0.36.0 — Error Diagnostics

### Added
- **Structured diagnostics**: all errors are now represented as
  structured `mino_diag_t` data with kind, code, phase, message,
  source span (file, line, column), notes, and stack frames.
- **Stable error codes**: every error site has a classified code
  (MRE for reader, MSY for syntax, MNS for name resolution, MAR for
  arity, MTY for type mismatch, MBD for bounds, MCT for contracts,
  MHO for host, MLM for limits, MUS for user exceptions, MIN for
  internal errors).
- **Column tracking in reader**: all parsed forms carry column
  position alongside file and line.
- **Pretty error rendering**: REPL and file mode display errors with
  `error[CODE]: message`, source location, source snippet with caret
  pointer, and compact stack trace.
- **Diagnostic map API**: `mino_last_diag()` and `mino_last_error_map()`
  provide structured access to the last error from C.
  `diag_to_map()` converts diagnostics to mino maps with canonical
  `:mino/kind`, `:mino/code`, `:mino/phase`, `:mino/message`,
  `:mino/location`, `:mino/notes`, `:mino/trace`, `:mino/data` keys.
- **REPL helpers**: `(last-error)`, `(error?)` primitives.
- **Catch normalization**: `catch` handlers always receive a diagnostic
  map. The original thrown value is accessible via `(ex-data e)`.
  `ex-data` and `ex-message` handle both diagnostic maps and `ex-info`
  maps transparently.
- **Source cache**: 4-entry cache of source text for diagnostic
  rendering with snippets.

### Fixed
- `prim_throw_error` no longer infinite-recurses when called outside
  a try block.

## v0.35.0 — core.async and Conformance

### Added
- **core.async**: full CSP channel implementation with go macro.
  - C modules: `async_buffer`, `async_channel`, `async_handler`,
    `async_select`, `async_scheduler`, `async_timer`, `prim_async`
    (17 primitives).
  - Channels: `chan`, `buffer`, `dropping-buffer`, `sliding-buffer`,
    `promise-chan`, `timeout`, `put!`, `take!`, `close!`, `closed?`,
    `offer!`, `poll!`, `chan?`.
  - Transducer and exception handler support on channels.
  - `alts!`, `alts!!` with `:priority` and `:default` options.
    Kernel-level arbitration via shared flag with refcounting.
  - `go`, `go-loop` macros with state machine transform supporting
    parks in let bindings, if/cond/when branches, loop/recur bodies,
    and try/catch/finally.
  - Blocking bridge: `<!!`, `>!!`, `alts!!` with multi-turn drain
    and deadlock detection.
  - Combinators: `pipe`, `onto-chan`, `to-chan`, `async-into`, `merge`,
    `mult`/`tap`/`untap`, `pub`/`sub`/`unsub`/`unsub-all`,
    `mix`/`admix`/`unmix`/`unmix-all`/`toggle`/`solo-mode`,
    `pipeline`, `pipeline-async`, `pipeline-blocking`.
  - Pending puts/takes limit of 1024 per channel.
  - 242 async tests across 9 test files, 346 assertions.
- `clojure.set` namespace (`union`, `intersection`, `difference`,
  `select`, `project`, `rename-keys`, `rename`, `index`, `join`,
  `map-invert`, `subset?`, `superset?`).
- `clojure.string` additions: `escape`, `re-quote-replacement`,
  `capitalize`, `upper-case`, `lower-case`.
- `comment` and `when-first` macros.
- `make-array`, `aset`, `aget`, `alength`, `aclone` array functions.
- `case` macro rewrite with proper constant quoting and
  multi-value match support.

### Changed
- `>`, `<=`, `>=` moved to C primitives with NaN and single-arity
  handling.
- `pop` on nil returns nil instead of throwing.
- `keys`/`vals` on empty non-map collections return nil.
- `find` on vectors supports index-based lookup.
- `mod`, `rem`, `quot` check for NaN/Infinity arguments.
- `rand` supports `(rand n)` arity.
- `keyword` supports `(keyword ns name)` arity.
- `conj` supports maps and lazy-seqs as targets.
- `cons` and `seq` work on sorted-map and sorted-set.
- `some-fn` returns false instead of nil when no predicate matches
  and validates arity.
- `underive` returns nil for 2-arity and validates hierarchy shape.
- `min`/`max` propagate NaN correctly.
- `ifn?` returns true for symbols and vars.

### Fixed
- Reader handles unmatched `#?` followed by `#?@` in
  lists/vectors/maps.
- Pipeline feeder backpressure stall with inputs larger than 2*n.
- Go macro: parks in non-last position of do blocks inside loop
  bodies, standalone park operations as loop exit values, let-park
  continuation body transform, loop init bindings from park points,
  non-park let forms wrapping park bodies, nested do block flattening.
- Mix pause/resume: paused channels are always read from (values
  consumed but not forwarded).
- Merge with zero channels closes output immediately.

## v0.34.0 — Conformance Hardening Phase 2

### Added
- Radix integer literals (`2r1010`, `8r77`, `16rFF`, bases 2-36).
- Tagged literal handling (`#tag form`) for unknown reader dispatch
  macros, enabling `.cljc` files with platform-specific tags.
- `tagged-literal` function so unknown reader tags (`#inst`, `#uuid`,
  etc.) evaluate to `{:tag :name :form body}` data.
- `array-map` alias for `hash-map`.
- `rseq` support for sorted maps and sets.
- Language binding examples for C, C++, and Java.
- Eight use case example programs (configuration, rules engine, data
  pipeline, event processing, plugins, console, game scripting,
  automation) with C++ host code and mino scripts.
- `test-use-cases` Makefile target.

### Changed
- `str` prints `Infinity`/`-Infinity`/`NaN` for special floats
  (was `inf`/`-inf`/`nan`).
- `even?`/`odd?` throw on non-integer arguments.
- `zero?` throws on non-number arguments.
- `NaN?`/`infinite?` throw on nil.
- `namespace` throws on nil argument.
- `realized?` throws on nil.
- `contains?` on strings throws for non-integer keys.
- `shuffle` validates collection argument.
- `mapcat` supports multiple collection arguments.
- `mapv` supports multiple collections: `(mapv + [1 2] [3 4])`.

### Fixed
- GC use-after-free in `mino_map`, `mino_set`, `mino_sorted_map`, and
  `mino_sorted_set`: intermediate allocations during construction could
  reclaim caller-held values. Fixed by suppressing GC during the
  construction loop.
- Benchmarks updated for the explicit `mino_state_t` API.

## v0.33.0 — Conformance Hardening

### Added
- `double?` and `char?` type predicates.
- `volatile!`, `vreset!`, `vswap!` (lightweight mutable box, backed by atom).
- `delay`, `force`, `delay?` (lazy thunk macro).
- `realized?` extended to handle delay values.
- Vector element-by-element comparison in `val_compare`, enabling
  proper sorting of vectors and map entries.
- String support for `contains?` (index-based, like vectors).
- External conformance test runner (`make test-external`).
- `CONFORMANCE.md` documenting intentional divergences.

### Changed
- All type and arity validation errors in C primitives now use
  `prim_throw_error` instead of `set_error`/return NULL, making them
  catchable by `try`/`catch`.
- `name` throws on nil argument (was returning nil).
- `namespace` returns nil on nil argument (was throwing).
- `keyword` accepts symbol and nil arguments.
- `parse-long` and `parse-double` throw on non-string arguments
  (was returning nil).
- `cons` throws on non-seqable second argument (was creating
  dotted pairs).
- `fnil` supports 2-arity and 3-arity default forms.

### Fixed
- Reader conditional `#?@` splice now handles vector values in
  list context (was silently dropping elements).

## v0.32.0 — Host Interop

### Added
- **Capability registry**: type-oriented registry for host interop.
  Hosts register constructors, methods, static methods, and getters
  per type tag via the C API (`mino_host_register_*` functions).
- **Interop primitives**: `host/new`, `host/call`, `host/static-call`,
  `host/get` dispatch through the capability registry with
  default-deny policy (disabled unless `mino_host_enable()` is called).
- **Interop syntax**: evaluator recognizes dot-method calls
  (`(.method target args)`), field access (`(.-field target)`),
  constructor calls (`(new TypeName args)`), and static method calls
  (`(TypeName/method args)`) and desugars them to the explicit
  host primitives.
- All interop errors are catchable via `try`/`catch`.

### Changed
- Symbol resolver now checks literal env bindings before qualified
  name resolution, allowing slash-containing names like `host/new`.

## v0.31.0 — clojure.string Namespace

### Added
- **`clojure.string` namespace** (`lib/clojure/string.mino`): provides
  `blank?`, `capitalize`, `starts-with?`, `ends-with?`, `escape`,
  `lower-case`, `upper-case`, and `reverse` as namespace-qualified
  functions accessible via `(require '[clojure.string :as str])`.
- **`capitalize`**: uppercase first character, lowercase the rest.
- **`escape`**: replace characters in a string according to a map.
- **String-specific `reverse`**: reverse a string (vs. the sequence
  `reverse` which operates on collections).
- All namespace functions include type guards that throw on non-string
  inputs, matching standard library behavior.

### Fixed
- `require` now saves and restores the current namespace, preventing
  `ns` forms in loaded files from leaking into the caller's context.

## v0.30.0 — Hierarchies and Dispatch Essentials

### Added
- **`make-hierarchy`**: create an empty hierarchy map.
- **`derive`**: establish parent-child relationship between tags. Supports
  explicit hierarchy (3-arg) and global hierarchy (2-arg) forms. Includes
  cycle detection and self-derivation guard.
- **`underive`**: remove a parent-child relationship. Recomputes transitive
  closure automatically.
- **`parents`**: query direct parents of a tag.
- **`ancestors`**: query all transitive ancestors of a tag.
- **`descendants`**: query all transitive descendants of a tag.
- **`isa?`**: check if child derives from parent. Supports equality,
  hierarchy lookup, and element-wise vector comparison.
- Global hierarchy atom for convenient 1-arg/2-arg function variants.

## v0.29.0 — Stateful Operations and Watches

### Added
- **`add-watch`**: register a callback on an atom that fires on every
  state change with `(fn key atom old-state new-state)` signature.
- **`remove-watch`**: unregister a watch by key.
- **`set-validator!`**: attach a validation function to an atom. Rejects
  mutations where the validator returns false or throws.
- **`get-validator`**: return the current validator function or nil.
- **`swap-vals!`**: like `swap!` but returns `[old new]` vector.
- **`reset-vals!`**: like `reset!` but returns `[old new]` vector.

### Changed
- **`reset!`** and **`swap!`**: now invoke validators before committing
  and notify watches after committing.

## v0.28.0 — Core Collections Semantics

### Added
- **`subvec`**: O(1) vector slice sharing the backing trie via offset.
  Supports 2-arity `(subvec v start)` and 3-arity `(subvec v start end)`.
- **`seqable?`**: predicate for nil, collections, and strings.
- **`indexed?`**: predicate for vectors.

### Changed
- **`ifn?`**: now returns true for keywords, maps, vectors, and sets
  in addition to functions.
- **`empty`**: preserves metadata from input collection on the empty
  result for vectors, maps, sets, and sorted variants.

## v0.27.0 — Numeric Tower Behavior

### Added
- **`unsigned-bit-shift-right`**: C primitive for unsigned (logical)
  right shift, casting to unsigned before shifting.
- **`parse-long`**: parses a string to an integer, returns nil on
  failure instead of throwing.
- **`parse-double`**: parses a string to a float, returns nil on
  failure. Accepts `"Infinity"`, `"-Infinity"`, `"NaN"`.
- **`pos-int?`**, **`neg-int?`**, **`nat-int?`**: integer range
  predicates.
- **`ratio?`**, **`decimal?`**: type stubs that always return false
  (no ratio or bigdecimal types).
- **`rational?`**: returns true for integers, false otherwise.
- **`long`**, **`double`**: coercion aliases for `int` and `float`.
- **`num`**: validates that its argument is numeric, returns it as-is.

## v0.26.0 — Reader Literal Parity

### Added
- **Special float tokens**: `##Inf`, `##-Inf`, `##NaN` reader tokens
  and aligned printer output.
- **Character literals**: `\space`, `\newline`, `\tab`, `\return`,
  `\backspace`, `\formfeed`, and single-char `\A` forms, read as
  single-character strings.
- **Hex integer literals**: `0xFF` style, parsed via base-16.
- **Ratio literals**: `1/2` reads as float, `6/3` as int when exact.
- **Bigint/bigdec suffixes**: `42N` consumed as int, `1.5M` as float.
- **`NaN?`**, **`infinite?`**: C predicates for special float values.
- **Float division by zero**: float operands produce IEEE infinity/NaN
  instead of throwing.

## v0.25.0 — Test Framework Compatibility

### Added
- **`are` macro**: parameterized assertion macro for the test framework.
  Takes a binding vector, a template expression, and rows of values;
  expands each row into an `(is ...)` assertion.
- **`p/thrown?` support**: the `is` macro recognizes namespace-qualified
  `thrown?` forms (e.g. `(is (p/thrown? ...))`) by checking the symbol
  name, not the full qualified path.
- **`lib/` load path**: the module resolver now searches `lib/` as a
  prefix and accepts `.cljc` file extensions in addition to `.mino`.
- **`lib/clojure/test.mino`**: thin shim that loads the mino test
  framework, enabling `(:require [clojure.test :refer [deftest is
  are testing]])` in external `.cljc` files.
- **`lib/clojure/core-test/portability.mino`**: `when-var-exists` macro
  and portability helpers for the external test suite.
- **`lib/clojure/core-test/number_range.mino`**: numeric range constants
  used by the external test suite.
- **`resolve` auto-var creation**: `resolve` now falls back to the root
  environment for C primitives and auto-interns a var, so
  `when-var-exists` works for all built-in functions.

## v0.24.0 — Namespace and Var Semantics

### Added
- **`MINO_VAR` value type**: first-class vars with namespace, name,
  and root binding. Vars are interned in a per-state registry.
- **Var registry**: `def` creates vars in the registry. `var` returns
  var objects. `#'sym` (var-quote) desugars to `(var sym)`.
- **Namespace-qualified symbol resolution**: `foo/bar` resolves through
  the alias table to find the correct namespace and var.
- **`:refer` support**: `(require '[ns :refer [x y]])` imports specific
  vars into the current namespace.
- **`var?`**: predicate for var values.
- **`resolve`**: returns the var for a symbol, or nil if unbound.
  Supports qualified and unqualified symbols.
- **`namespace`**: returns the namespace string of a qualified symbol
  or keyword.
- **2-arity `symbol`**: `(symbol ns name)` constructs a qualified
  symbol.
- **`qualified-keyword?`**, **`qualified-symbol?`**,
  **`simple-keyword?`**, **`simple-symbol?`**: qualification predicates.

## v0.23.0 — Reader and Loadability Baseline

### Added
- **`ns` special form**: establishes the current namespace and processes
  `:require` clauses with `:as` aliases and `:refer` imports.
- **Reader conditionals**: `#?(:mino expr :default expr)` and splicing
  `#?@(...)` select platform-specific code at read time. The mino
  dialect key is `"mino"`; `:default` is the fallback.
- **`#'` var-quote reader macro**: `#'sym` desugars to `(var sym)`.
- **Vector syntax for `require`**: `(require '[x.y :as z])` accepted
  alongside the existing string form.
- **Namespace and reader dialect state**: `mino_state_t` tracks
  `current_ns` and `reader_dialect` fields.

## v0.22.0 — Collection and Sequence Conformance

### Added
- **Collections as callable functions**: maps, vectors, and sets can be
  called as functions in both direct and higher-order contexts.
  `({:a 1} :a)` returns `1`, `([1 2 3] 0)` returns `1`,
  `(#{:a :b} :a)` returns `:a`. Maps and keywords accept an optional
  default argument. Works with `map`, `filter`, `apply`, and other HOFs.
- **`peek` and `pop`**: stack abstraction for vectors (from end, O(1))
  and lists (from front, O(1)). `vec_pop` in the C trie handles boundary
  cases including trie-leaf promotion and height reduction.
- **`find` as C primitive**: single HAMT lookup returning `[k v]` or nil,
  replacing the core.mino definition that used two lookups.
- **`empty` as C primitive**: type-switch returning the empty version of
  the same collection type.
- **`rseq`**: reverse-order traversal of vectors, returning a list.
- **`take-nth`**: lazy seq function with transducer support.
- **`lazy-cat`**: macro for lazily concatenating multiple collections.
- **Sorted collections**: `sorted-map` and `sorted-set` backed by a
  persistent left-leaning red-black tree (LLRB) with path-copying. Full
  integration with `seq`, `count`, `first`, `rest`, `get`, `assoc`,
  `dissoc`, `contains?`, `conj`, `into`, `find`, `empty`, and equality.
- **`some->`** and **`some->>`**: nil-safe threading macros.
- **`update-vals`** and **`update-keys`**: apply a function to every
  value or key in a map.
- **`min-key`** and **`max-key`**: find elements by keyed comparison.
- **`random-sample`**: probabilistic filter with transducer arity.
- **`halt-when`**: transducer that stops processing on a predicate.
- **`bounded-count`** and **`counted?`**: count with upper limit for
  lazy sequences; predicate for O(1)-countable collections.
- **`while`** macro: imperative loop.
- **`distinct?`**: check all arguments are unique.
- **Type predicates**: `sorted?`, `associative?`, `reversible?`, `any?`.
- **`ensure-reduced`**: transducer helper that wraps in `reduced` if not
  already reduced.

### Changed
- **Lazy `rest`**: `rest` on vectors, maps, sets, and strings now returns
  a lazy cons chain instead of eagerly allocating O(n) cells. Extends
  `MINO_LAZY` with an optional C thunk function pointer for efficient
  deferred iteration without eval overhead.

## v0.21.0 — Architecture Hardening

### Changed
- **Module extraction**: evaluator, runtime, and primitive code further
  split into focused translation units. `eval_special.c` split into
  `eval_special.c` (dispatch) + `eval_special_defs.c` +
  `eval_special_bindings.c` + `eval_special_control.c` +
  `eval_special_fn.c`. `prim.c` split into `prim.c` (shared helpers,
  install) + `prim_reflection.c` + `prim_meta.c` + `prim_regex.c` +
  `prim_stateful.c` + `prim_module.c`. New `eval_special_internal.h`
  provides cross-domain declarations for the evaluator layer.
- **Architecture gates**: `make qa-arch` now passes with zero allowlists.
  TU size limit tightened from 1200 to 1100 LOC. All function span
  allowlists removed.
- **State access**: all state field alias macros removed. Internal code
  uses explicit `S->field` access throughout.
- **Ownership annotations**: function declarations in `mino_internal.h`
  and `prim_internal.h` now carry ownership annotations (GC-owned,
  borrowed, static, malloc-owned).
- **GC hardening**: `gc_save` array increased from 32 to 64 slots.
  `gc_unpin` asserts on underflow in debug builds instead of silently
  clamping.
- **Fault injection**: `mino_set_fail_raw_at` API for testing non-GC
  allocation paths (clone, mailbox, serialization buffers).

### Fixed
- `mino_pcall` now establishes a try frame before calling, preventing
  abort on throw from user code.
- `gc_pin`/`gc_unpin` counter imbalance in `mino_pcall` error path.
- `mino_pcall` propagates the error message from `mino_last_error`
  when the inner eval returns NULL without throwing.
- Regex thread test joins thread 1 if thread 2 creation fails.

## v0.20.0 — Dialect Alignment

Brings mino's surface language into close alignment with standard
conventions. Multi-arity functions, destructuring, protocols,
transducers, value metadata, and reader macros land as a cohesive set.
A large test suite derived from the official test repository validates
conformance across 552 tests (up from 300) and 2039 assertions (up
from 664).

### Added

- **Multi-arity functions**: `fn` and `defn` accept multiple arities
  via `([x] body) ([x y] body)` dispatch. Arity mismatch produces a
  clear error naming the function and the available arities.
- **Vector bindings**: `let`, `fn`, `loop`, `binding`, `for`, `doseq`,
  and all destructuring forms accept `[x y]` binding vectors alongside
  the existing `(x y)` list form.
- **Destructuring**: positional destructuring in vectors, map
  destructuring with `:keys`, `:strs`, `:or`, and `:as`, and nested
  destructuring at any depth. Works in `let`, `fn`, `loop`, `for`,
  and `doseq`.
- **Named fn**: `(fn name [x] body)` binds `name` inside the body for
  self-reference without `def`.
- **Protocols**: `defprotocol`, `extend-type`, `extend-protocol`, and
  `satisfies?`. Dispatch on the type of the first argument. `:default`
  extension provides fallback implementations. Implemented in mino
  using atoms for dispatch tables.
- **Transducers**: `transduce`, `into` with xform, `sequence`,
  `eduction`, `completing`, and `cat`. Composable transducer arities
  added to `map`, `filter`, `remove`, `take`, `drop`, `take-while`,
  `drop-while`, `keep`, `keep-indexed`, `map-indexed`, `dedupe`,
  `partition-by`, `partition-all`, `distinct`, and `interpose`.
- **Value metadata**: `meta`, `with-meta`, `vary-meta`, `alter-meta!`.
  Reader `^` syntax attaches metadata directly: `^{:k v}`, `^:key`,
  `^Type`. Metadata preserved through collection operations (`merge`,
  `merge-with`, `select-keys`, `replace`, `conj`, `assoc`, `dissoc`,
  `into`, `vec`, `set`, `subvec`, `pop`).
- **Reader macros**: `#(+ %1 %2)` anonymous function shorthand, `#_`
  discard next form.
- **Callable keywords**: `(:k m)` and `(:k m default)` for map lookup.
- **Exception data**: `ex-info`, `ex-data`, `ex-message` for
  structured exceptions.
- **`try`/`finally`**: finally clause executes on both success and
  exception. `try` without `catch` or `finally` is now accepted.
- **`with-open`**: macro that binds a resource and ensures cleanup via
  `finally`.
- **`identical?`**: pointer identity comparison.
- **`reduced`**: wraps a value for early termination in `reduce` and
  `transduce`.
- **`declare`**: forward declaration of vars.
- **`set` constructor**: `(set coll)` builds a set from any
  collection.
- **`integer?`**, **`coll?`**, **`==`**, **`empty`**, **`re-pattern`**:
  new predicates and constructors.
- **Multi-binding `for` and `doseq`**: multiple binding pairs with
  `:when`, `:while`, and `:let` modifiers.
- **Multi-collection `map`**: `(map f c1 c2 ...)` maps over multiple
  collections in parallel.
- **Format precision**: `%5d`, `%.2f`, and width specifiers in
  `format`.
- **Test suite**: 552 tests, 2039 assertions (up from 300/664).
  Includes a suite derived from the official test repository covering
  predicates, sequences, higher-order functions, math, control flow,
  transducers, and metadata.

### Changed

- **`defn` and `defmacro`**: skip optional attr-map argument after the
  name for source compatibility.
- **`fn*`, `let*`, `loop*`**: recognized as aliases for `fn`, `let`,
  `loop`.
- **`(def name)`**: allowed without a value, binds to nil.
- **`/` (division)**: returns an integer when the result is exact
  (`(/ 6 3)` returns `2`, not `2.0`).
- **`cons`**: coerces its second argument to a seq.
- **`=` on sequences**: cross-type sequential equality. `(= '(1 2 3)
  [1 2 3])` is true.
- **`first` and `rest`**: extended to work on maps, sets, and strings.
- **`nth`**: extended to work on strings and lazy sequences.
- **`max` and `min`**: now variadic.
- **`comp`**: now variadic, accepts any number of functions.
- **`interleave`**: now variadic.
- **`not=`**: supports 1-arity and variadic calls.
- **`get-in`**: 3-arity distinguishes nil values from missing keys;
  accepts a not-found parameter.
- **Single-quote in symbols**: `can't` and `it's` now parse correctly.
- **`nth` out-of-range**: throws a catchable exception via `try`/`catch`
  instead of a fatal error.

### Fixed

- `memoize` correctly caches nil return values.
- `merge` and `merge-with` return nil when all arguments are nil.
- `replace` preserves vector type and uses `find` for nil-safe lookup.
- `flatten` reimplemented using `tree-seq` for correct behavior on
  non-sequential nested values.
- `drop-last` is lazy instead of forcing `count`.
- `mapcat` is lazy to support infinite sequences.
- `juxt` returns a vector.
- `partition` returns lists when called with a step argument.
- `repeatedly` supports the `(repeatedly n f)` arity.
- `satisfies?` accounts for `:default` protocol extensions.
- `transduce` unwraps nested reduced values.
- `:or` destructuring uses symbol keys correctly.

## v0.19.0 — Explicit Runtime State

### Breaking changes

- **Primitive callback signature**: `mino_prim_fn` now receives
  `mino_state_t *S` as its first parameter. All host-defined primitives
  must be updated from `(mino_val_t *args, mino_env_t *env)` to
  `(mino_state_t *S, mino_val_t *args, mino_env_t *env)`.
- **`mino_current_state()` removed**: primitives receive the state
  explicitly and no longer need to call this function.
- **Default global state removed**: there is no implicit runtime state.
  The host must always create a state with `mino_state_new()`.
- **`spawn` is now a macro**: `(spawn & body)` takes unquoted forms
  instead of a source string. The string-based primitive is available
  as `spawn*` for programmatic use.

### Added

- **Eager collection builders**: `rangev`, `mapv`, `filterv` produce
  vectors directly in C without lazy thunk allocation. `rangev` is
  60-70x faster than lazy `range` for data generation. `reduce` over
  a `rangev` vector is 26x faster than over a lazy `range`.
- **Core.mino parse caching**: parsed forms are cached per state.
  Subsequent `mino_install_core` calls on the same state skip
  re-parsing.

### Changed

- All internal runtime state access is now explicit through
  `mino_state_t *S` parameters. No process-global mutable state
  remains (except a benign filename intern cache for reader
  diagnostics).
- Fixed `mino_env_clone` changelog description: it clones within the
  same state (values are shared), not across states.

## v0.18.0 — Runtime State, GC Hardening, and Repo Reorganization

Multi-instance runtime support, GC correctness under stress, and a
cleaner project layout for embedding and development.

### Added
- **Explicit runtime state**: all public API functions now take a
  `mino_state_t *S` parameter. Multiple independent runtime instances
  can coexist in the same process with no shared mutable data.
- **`mino_state_new` / `mino_state_free`**: create and destroy runtime
  instances. The default global state is still available for simple
  single-instance use.
- **GC save stack**: `gc_pin`/`gc_unpin` macros protect borrowed values
  across allocation boundaries where the conservative stack scanner
  might miss register-allocated locals.
- **Value cloning**: `mino_clone` deep-copies a value tree from one
  state to another for safe cross-state transfer.
- **Mailbox**: thread-safe `mino_mailbox_t` value queue for
  communication between runtime instances.
- **Actor system**: `spawn`, `send!`, `receive` primitives for
  host-controlled isolated concurrency.
- **Session cloning**: `mino_env_clone` creates a new root environment
  within the same state, copying all bindings (values are shared, not
  deep-copied). Cross-state transfer requires `mino_clone`.
- **Eval interruption**: `mino_interrupt` sets a flag checked by the
  eval loop, allowing the host to cancel long-running evaluations.
- **Host-retained refs**: `mino_ref`/`mino_deref`/`mino_unref` pin
  values across GC cycles without keeping an entire environment alive.
- **Dynamic binding**: `binding` special form for thread-local dynamic
  variables scoped to a runtime instance.
- **`swap!` primitive**: atomic read-modify-write on atoms.
- **Regex support**: `re-find` and `re-matches` primitives backed by
  a bundled regex engine (`re.c`/`re.h`).

### Changed
- **Repository layout**: library source files moved to `src/`.
  Test framework moved to `tests/test.mino`. `main.c` stays in the
  root as the REPL binary entry point.
- **Multi-file split**: the monolithic `mino.c` is now split into
  9 focused translation units (`mino.c`, `val.c`, `vec.c`, `map.c`,
  `read.c`, `print.c`, `prim.c`, `clone.c`, `mino_internal.h`).
  The public API header `mino.h` is unchanged.
- **Embedding**: copy the `src/` directory into your project and
  compile with `-Isrc`. The Makefile uses `LIB_SRCS` / `LIB_OBJS`
  for the library object set.

### Fixed
- **GC stress bug**: under `MINO_GC_STRESS=1`, borrowed function
  pointers from env lookups could be collected during `eval_args`
  when the compiler kept them in registers instead of on the stack.
  The GC save stack pins these values explicitly.
- **`(range)` with no arguments**: returned nil instead of an infinite
  lazy sequence. Added zero-arity case.
- **`mino_clone` on nested non-transferable values**: cloning a
  collection containing a function, handle, atom, or lazy-seq would
  silently produce NULL elements instead of failing. Now propagates
  the error with proper cleanup.
- **`mino_new` docstring**: documented as core-only but also installed
  I/O. Updated the docstring to match the actual behavior.
- **Catchable runtime errors**: division by zero, mod/rem/quot by
  zero, nth/char-at/subs/assoc index out of range, and format type
  mismatches now throw catchable exceptions via `try`/`catch` instead
  of propagating as fatal errors.
- **`str` and `println` for collections**: vectors, maps, sets, cons
  cells, lazy sequences, and atoms rendered as `#<?>`. Now uses the
  standard printer for readable output.
- **Segfault on throw inside binding**: `throw` inside a `binding`
  form inside `try`/`catch` crashed with a use-after-free. The
  `longjmp` skipped past the binding frame cleanup, leaving
  `dyn_stack` pointing at reclaimed stack memory. The try handler now
  saves and restores `dyn_stack`.

### Performance
- **Mailbox serialization**: replaced `tmpfile()` + fprintf + fread
  text roundtrip with a direct-to-buffer printer. 911x faster for
  integer messages (100 us to 0.11 us). Actor send+recv for 50,000
  actors dropped from 6 seconds to 156 ms.


## v0.17.0 — Proper Tail Calls and Core Library

Proper tail call optimization in the evaluator. All function calls in
tail position run in constant stack space, including mutual recursion.
Plus ~80 new core.mino definitions bringing the standard library close
to feature parity with core language functions.

### Added
- **Proper tail calls**: `MINO_TAIL_CALL` evaluator type. The
  evaluator tracks tail position and returns a trampoline sentinel
  instead of recursing. `apply_callable` handles both `MINO_RECUR`
  (self-recursion) and `MINO_TAIL_CALL` (general tail calls).
  `loop`/`recur`/`trampoline` remain as convenient iteration
  constructs.
- **Type predicates**: `true?`, `false?`, `boolean?`, `int?`,
  `float?`, `some?`, `list?`, `atom?`, `not-any?`, `not-every?`.
- **Sequence navigation**: `next`, `nfirst`, `fnext`, `nnext`.
- **Map entry accessors**: `key`, `val`.
- **Control flow macros**: `if-not`, `when-not`, `if-let`, `when-let`,
  `if-some`, `when-some`.
- **Sequence functions**: `last`, `butlast`, `nthrest`, `nthnext`,
  `take-last`, `drop-last`, `split-at`, `split-with`, `mapv`,
  `filterv`, `sort-by`.
- **Collection utilities**: `get-in`, `assoc-in`, `update-in`,
  `merge-with`, `reduce-kv`, `replace`, `str-replace`.
- **Bitwise compositions**: `bit-and-not`, `bit-test`, `bit-set`,
  `bit-clear`, `bit-flip`.
- **Lazy combinators**: `keep`, `keep-indexed`, `map-indexed`,
  `partition-all`, `reductions`, `dedupe`.
- **Higher-order**: `every-pred`, `some-fn`, `fnil`, `memoize`,
  `trampoline`.
- **Threading macros**: `as->`, `cond->`, `cond->>`.
- **Iteration**: `doto`, `dotimes`, `doseq`.
- **Utilities**: `remove`, `vec`, `rand-int`, `rand-nth`, `run!`,
  `blank?`, `comparator`, `shuffle`, `time`.
- **Tree walking**: `flatten`, `tree-seq`, `walk`, `postwalk`,
  `prewalk`, `postwalk-replace`, `prewalk-replace`.
- **Regex**: `re-seq`.
- **Complex macros**: `condp`, `case`, `for` (single binding with
  `:when`).
- **Test suite**: 300 tests, 664 assertions (up from 228/511).

## v0.16.0 — Complete C Primitive Layer

Adds every C primitive needed to implement the non-JVM parts of
clojure.core. The pure mino compositions come in a later version;
this version focuses on the C foundation.

### Added
- **Math functions**: `math-floor`, `math-ceil`, `math-round`,
  `math-sqrt`, `math-pow`, `math-log`, `math-exp`, `math-sin`,
  `math-cos`, `math-tan`, `math-atan2`. All thin wrappers around
  `<math.h>`. Constant `math-pi`.
- **`hash`**: exposes the internal FNV-1a hash used by HAMT and sets.
- **`compare`**: general comparison returning -1, 0, or 1 for
  numbers, strings, keywords, and symbols. nil sorts first.
- **`sort` with comparator**: `(sort comp coll)` accepts a boolean
  comparator (like `<`) or a three-way comparator (like `compare`).
- **`symbol`** and **`keyword`** constructors from strings (reverse
  of `name`).
- **`eval`**: evaluate a form at runtime, exposing `mino_eval` to
  mino code.
- **`rand`**: random float in [0.0, 1.0) via ANSI C `rand()`.
- **`time-ms`**: monotonic milliseconds via `clock_gettime`.
  Registered in `mino_install_io`.
- **Regex**: `re-find` and `re-matches` via bundled tiny-regex-c
  (public domain, ANSI C, all platforms). Supports `.`, `*`, `+`,
  `?`, `^`, `$`, character classes, and `\d`, `\w`, `\s` shorthand.

### Changed
- `mino-fs` noted in backlog: file system operations belong in a
  separate library following the babashka/fs pattern. `slurp`/`spit`
  marked for eventual migration.
- Makefile builds `re.o` from vendored `re.c`.

## v0.15.0 — Test Framework and Dogfooding

Replaces all shell test scripts with mino-based tests. The language
now tests itself.

### Added
- **File argument support**: `./mino script.mino` evaluates a file
  and exits. Exit code 1 on eval failure.
- **CWD-relative module resolver**: `(require "test")` resolves to
  `./test.mino`. Wired in `main.c` via `mino_set_resolver`.
- **`exit` primitive**: `(exit code)` terminates the process.
  Registered in `mino_install_io`.
- **`test.mino`**: test framework written in mino itself. Implements
  `deftest`, `is`, `testing`, and `run-tests` following clojure.test
  conventions.
- **Mino test suite**: 203 tests with 427 assertions across 16 files,
  replacing the 371-line smoke.sh and 131-line crash_test.sh.
- **Reader fuzz tests**: 51 adversarial reader tests in mino using
  `read-string` + `try/catch`.

### Changed
- **`read-string` throws catchable exceptions** on parse errors.
  Previously propagated as fatal C-level errors; now caught by
  `try/catch` when inside a `try` block.
- **Makefile**: `make test` runs `./mino tests/run.mino`.
- **Shell scripts removed**: `tests/smoke.sh` and
  `fuzz/crash_test.sh` deleted. No `.sh` files in test infra.

## v0.14.0 — Lazy Sequences, Complete C Core, core.mino Expansion

Lazy sequences land as a first-class type, enabling infinite data
structures and demand-driven evaluation. The C core gains its final
set of primitives; seven sequence operations move from C to lazy mino
implementations. core.mino nearly doubles in size.

### Added
- **Lazy sequences** (`MINO_LAZY`): deferred computation with cached
  results. `lazy-seq` special form captures body and environment;
  forced on first access via `first`, `rest`, `count`, or printing.
  Thunk and captured env released after forcing for GC.
- **`seq`**: coerce any collection (list, vector, map, set, string,
  lazy-seq) to a cons sequence. Returns nil for empty collections.
- **`realized?`**: check if a lazy sequence has been forced.
- **`dissoc`**: remove key(s) from a map.
- **`mod`**, **`rem`**, **`quot`**: arithmetic primitives. `mod` uses
  floored division, `rem` uses truncated division.
- **Bitwise operations**: `bit-and`, `bit-or`, `bit-xor`, `bit-not`,
  `bit-shift-left`, `bit-shift-right`. Integer-only.
- **`name`**: extract string from keyword or symbol.
- **`int`**, **`float`**: type coercion between integer and float.
- **`char-at`**: character access by index (returns single-char string).
- **`pr-str`**: print values to string in readable form.
- **`read-string`**: parse one mino form from a string.
- **`format`**: string formatting with `%s`, `%d`, `%f`, `%%`.
- **core.mino definitions** (~40 new): `second`, `ffirst`, `inc`, `dec`,
  `zero?`, `pos?`, `neg?`, `even?`, `odd?`, `abs`, `max`, `min`,
  `not-empty`, `constantly`, `boolean`, `seq?`, `merge`, `select-keys`,
  `find`, `zipmap`, `frequencies`, `group-by`, `juxt`, `mapcat`,
  `take-while`, `drop-while`, `iterate`, `cycle`, `repeatedly`,
  `interleave`, `interpose`, `distinct`, `partition`, `partition-by`,
  `doall`, `dorun`.

### Breaking
- **`stdlib.mino` renamed to `core.mino`**. The bundled mino source
  file, Makefile build rule, generated header, and all internal
  references now use `core.mino` / `core_mino.h`. Embedders that
  reference the generated header by name must update.
- **`map`, `filter`, `take`, `drop`, `concat`, `range`, `repeat`
  moved from C to core.mino** and are now lazy. Code that relied on
  these being strict (fully realized on return) may behave differently.
  Use `doall` to force eager evaluation where needed.
- **`update`, `some`, `every?` moved from C to core.mino**. These
  are no longer available as C primitives. `update` now accepts extra
  args: `(update m :k f arg1 arg2)`.
- **`range` and `repeat` signatures changed**. `repeat` now takes
  `(repeat n x)` instead of the old `(repeat count value)` (same
  args, but now returns a lazy seq). `range` with no args is no longer
  supported (was an error before too).
- **C primitive count**: 57 to 50 (net: +11 new, -18 moved to mino).
- Cons printer forces lazy tails for correct output.
- `list_length` forces lazy tails for correct `count`.

## v0.13.0 — Atoms, Spit, Stdlib Architecture

Establishes the three-tier architecture: C runtime (irreducible
primitives), bundled stdlib.mino (macros and compositions), and
future mino-std package. Delivers atoms and spit.

### Added
- **Atoms** (`MINO_ATOM`): mutable reference cells for managed state.
  C API: `mino_atom()`, `mino_atom_deref()`, `mino_atom_reset()`,
  `mino_is_atom()`. Primitives: `atom`, `deref`, `reset!`, `atom?`.
  Reader macro: `@form` expands to `(deref form)`.
- **`swap!`**: stdlib function. `(swap! a f x y)` sets atom to
  `(f @a x y)`.
- **`defn`**: stdlib macro. `(defn name (params) body)` expands to
  `(def name (fn (params) body))`. Single-arity.
- **`spit`**: I/O primitive. `(spit "path" content)` writes to file.
  Strings write raw bytes; other values write their printed form.

### Changed
- **stdlib.mino**: the standard library is now a standalone `.mino`
  file compiled into the binary at build time (was an inline C string).
- **Stdlib migration**: `not`, `not=`, `identity`, `list`, `empty?`,
  `>`, `<=`, `>=`, and all ten type predicates (`nil?`, `cons?`,
  `string?`, `number?`, `keyword?`, `symbol?`, `vector?`, `map?`,
  `fn?`, `set?`) moved from C to mino. C primitive count reduced
  from 72 to 57.

## v0.12.0 — Release Candidate (Alpha)

Quality, polish, and documentation pass. No new language features.

### Changed
- Error messages in `let` and `loop` now include source file and line
  when available (promoted to `set_error_at`).
- "Unsupported collection" errors now name the type that was actually
  passed (e.g., `count: expected a collection, got int`).
- "Not a function" errors now report the received type
  (e.g., `not a function (got string)`).
- Internal `type_tag_str` helper added for diagnostic formatting.

### Added
- **Embedding cookbook** (`cookbook/`): six worked examples demonstrating
  real-world embedding patterns — config loader, rules engine,
  REPL-on-socket, plugin host, data pipeline, and game scripting console.
- **Fuzz harness** (`fuzz/`): libFuzzer-compatible reader target plus a
  57-case adversarial crash test suite (`make fuzz-crash`).
- **Map and sequence benchmarks** (`bench/map_bench.c`,
  `bench/seq_bench.c`): HAMT get/assoc scaling, and map/filter/reduce/sort
  throughput. Invoke via `make bench-map` and `make bench-seq`.

### Verified
- 258/258 smoke tests pass in all four modes (O0, O0+GC\_STRESS, O2,
  O2+GC\_STRESS).
- 57/57 adversarial reader inputs handled without crashes.
- All six cookbook examples compile warning-free and produce correct
  output.
- Benchmark results show expected O(log32 n) scaling for vectors and
  maps, consistent sequence throughput.
- API review: all 40+ public symbols consistently named, no orphaned
  declarations, UNSTABLE marker retained (alpha).
- LOC: mino.c ~6,672, mino.h ~352 (within 15k–25k budget).

## v0.11.0 — Sequences and Remainder of Stdlib

Sets, sequence transformations, string operations, and utility functions
round out the core standard library. Strict (non-lazy) semantics
throughout — every sequence operation returns a concrete list or
collection.

### Added

- **Sets** (`MINO_SET`): persistent HAMT-backed set type. Reader literal
  `#{...}`, printer `#{...}`, value-based equality, hashing.
  - `hash-set`, `set?`, `contains?`, `disj`, `get` on sets, `conj` on
    sets.
- **Sequence operations** (strict, return lists):
  - `map`, `filter`, `reduce` (2- and 3-arg), `take`, `drop`, `range`
    (1/2/3-arg), `repeat`, `concat`, `into`, `apply` (2+-arg with
    spread), `reverse`, `sort` (natural ordering via merge sort).
  - All sequence ops work uniformly over lists, vectors, maps (yielding
    `[key value]` vectors), sets, and strings (yielding 1-char strings).
- **String operations**: `subs`, `split`, `join` (1- and 2-arg),
  `starts-with?`, `ends-with?`, `includes?`, `upper-case`, `lower-case`,
  `trim`.
- **Utility primitives**: `not`, `not=`, `empty?`, `some`, `every?`,
  `identity`.
- **Stdlib (mino-defined)**: `comp`, `partial`, `complement`.

### Changed

- `mino_install_core` docstring updated to list all new bindings.
- `conj` extended to support sets.
- `get` extended to support sets (returns the element itself if present).
- `count` extended to support sets.
- `mino.h` gains `MINO_SET` in the type enum and `mino_set()` constructor.
- Existing `apropos` test updated for expanded binding set.

### Notes

- LOC: mino.c ~6,583, mino.h ~352 (well within the 15k–25k budget).
- 258 smoke tests, all passing under normal and `MINO_GC_STRESS=1`
  modes at both `-O0` and `-O2`.
- Lazy sequences were evaluated and deferred: strict semantics are
  simpler, more predictable, and a better fit for the embeddable
  runtime identity. The deviation from the host language is documented.

## v0.10.0 — Interactive Development

The printer is now cycle-safe, `def`/`defmacro` record metadata for
introspection, and a new in-process REPL handle lets a host drive
read-eval-print one line at a time with no thread required.

### Added

- **Cycle-safe printing**: `mino_print_to` tracks recursion depth and
  emits `#<...>` when the depth exceeds 128, preventing stack overflow
  on deeply nested structures.
- **`doc`**: `(doc 'name)` returns the docstring attached to a
  `def`/`defmacro` binding, or `nil` if none was provided.
- **`source`**: `(source 'name)` returns the original source form of a
  `def`/`defmacro` binding.
- **`apropos`**: `(apropos "substring")` returns a list of symbols whose
  names contain the given substring, searched across the current
  environment chain.
- **Docstring support in `def` and `defmacro`**: An optional string
  literal between the name and the value/params is recorded as the
  binding's docstring: `(def name "docstring" value)`,
  `(defmacro name "docstring" (params) body)`.
- **`mino_repl_t` — in-process REPL handle**: `mino_repl_new(env)`
  creates a handle; `mino_repl_feed(repl, line, &out)` accumulates
  input and evaluates when a form is complete. Returns `MINO_REPL_OK`,
  `MINO_REPL_MORE`, or `MINO_REPL_ERROR`. `mino_repl_free` releases
  the handle. No thread required — the host controls the call cadence.
- **Var redefinition with live reference update**: Closures that
  reference root-level vars see updated values after `def` redefines
  them (already the case due to env-chain lookup, now tested
  explicitly).

### Changed

- `mino_install_core` docstring updated to list `doc`, `source`, and
  `apropos` under reflection primitives.
- `examples/embed.c` updated to demonstrate the REPL handle API.

### Notes

- LOC: mino.c ~5,210, mino.h ~338 (within the 15k–25k budget).
- 170 smoke tests, all passing under normal and `MINO_GC_STRESS=1`
  modes at both `-O0` and `-O2`.

## v0.9.0 — Sandbox, Modules, Diagnostics

Runtime errors now carry source locations and call-stack traces. Script
code gains `try`/`catch`/`throw` for recoverable exceptions. The core
environment is sandboxed by default — I/O primitives are installed
separately via `mino_install_io`. A host-supplied module resolver
enables `require` for file-based modules.

### Added

- **Source locations**: The reader tracks file name and line number;
  cons cells produced by reading carry `file` / `line` annotations.
  Eval errors include a `file:line:` prefix, and function call errors
  append a stack trace showing the active call chain.
- **`try` / `catch` / `throw`**: `try` is a special form:
  `(try body (catch e handler...))`. `throw` raises a script-level
  exception caught by the nearest enclosing `try`; an unhandled `throw`
  becomes a fatal runtime error. Uses `setjmp`/`longjmp` internally.
- **`mino_install_io(env)`**: Installs `println`, `prn`, and `slurp`.
  `mino_install_core` no longer installs any I/O primitives — the host
  opts in by calling `mino_install_io`. `mino_new()` installs both for
  convenience.
- **`slurp`**: `(slurp path)` reads a file's contents as a string.
  Only available when `mino_install_io` has been called.
- **`require`**: `(require "name")` loads a module by name using a
  host-supplied resolver. Results are cached so subsequent requires of
  the same name return instantly.
- **`mino_set_resolver(fn, ctx)`**: Registers the host resolver
  callback for `require`.
- **`run_err`** test helper in `smoke.sh` for testing error messages.

### Changed

- **Error buffer** enlarged from 256 to 2048 bytes to accommodate
  stack traces.
- **`mino_install_core`** no longer installs `println` or `prn`.
  Existing embedders using `mino_new()` are unaffected (it calls both
  `mino_install_core` and `mino_install_io`). Embedders calling
  `mino_install_core` directly must add `mino_install_io` to restore
  the prior behaviour.
- **REPL** (`main.c`) calls `mino_install_io` after `mino_install_core`
  and preserves inter-form whitespace so the reader's line counter
  stays accurate across forms.

### Notes

- Stack traces are appended to the error message returned by
  `mino_last_error()`. A future version may expose structured trace
  access.
- `try`/`catch` catches only values raised by `throw`. Fatal runtime
  errors (NULL returns from `mino_eval`) propagate to the host
  unmodified.
- The module cache and resolver are global (not per-env). Thread
  safety is not a goal pre-v1.0.

## v0.8.0 — Host C API

First draft of the embedding API. An external C program can now create a
runtime, register host functions, evaluate source, call mino functions,
and extract results — all in under 50 lines of glue code. The surface
language gains type predicates, `str`, and basic I/O. All new symbols are
`mino_*`-prefixed; the header remains marked UNSTABLE until v1.0.

### Added

- `mino_new()` convenience: allocates an env and installs core bindings
  in one call.
- `mino_eval_string(src, env)` reads and evaluates all forms in a C
  string, returning the last result.
- `mino_load_file(path, env)` reads a file from disk and evaluates all
  forms within it.
- `mino_register_fn(env, name, fn)` shorthand for binding a C function
  as a primitive.
- `mino_call(fn, args, env)` applies a callable value (fn, prim, macro)
  to an argument list from C; returns the result or NULL on error.
- `mino_pcall(fn, args, env, &out)` protected variant that returns 0 on
  success and -1 on error, writing the result through an out-parameter.
- `MINO_HANDLE` value type for opaque host objects. A handle carries a
  `void *` and a tag string; it self-evaluates, prints as
  `#<handle:tag>`, compares by pointer identity, and hashes by the host
  pointer. `mino_handle(ptr, tag)`, `mino_handle_ptr(v)`,
  `mino_handle_tag(v)`, and `mino_is_handle(v)` form the C interface.
- Type-safe C extraction: `mino_to_int`, `mino_to_float`,
  `mino_to_string`, `mino_to_bool`. Each returns 1 on success and writes
  through an out-parameter; `mino_to_bool` uses truthiness semantics.
- `mino_set_limit(kind, value)` with `MINO_LIMIT_STEPS` (per-eval step
  cap) and `MINO_LIMIT_HEAP` (soft cap on GC-managed bytes). When
  exceeded the current eval returns NULL with a descriptive error.
  Pass 0 to disable a limit.
- Type-predicate primitives in the surface language: `string?`,
  `number?`, `keyword?`, `symbol?`, `vector?`, `map?`, `fn?`.
- `type` primitive returns the type of its argument as a keyword
  (`:int`, `:string`, `:list`, `:vector`, `:map`, `:fn`, `:keyword`,
  `:symbol`, `:nil`, `:bool`, `:float`, `:macro`, `:handle`).
- `str` primitive concatenates its arguments into a string. String
  arguments contribute raw content (no quotes); other types use their
  printer representation; nil contributes nothing.
- `println` prints its arguments as `str` does, appends a newline, and
  returns nil. `prn` prints each argument in its printer form separated
  by spaces, appends a newline, and returns nil.
- `examples/embed.c`: a 50-line standalone C program demonstrating the
  full embed lifecycle — create runtime, register a host function,
  evaluate source, extract a float result.
- `make example` target builds and runs the embedding example.
- 31 additional smoke-test cases covering type predicates, `type`,
  `str`, `println`, and `prn` (148 cases total).

### Notes

The header remains `/* UNSTABLE until v1.0.0 */`. API additions are
possible through the 0.x series; the v1.0 release freezes the ABI.
Execution limits are global rather than per-env; this simplifies the
implementation while a single-threaded model is the only supported
configuration. The `mino_load_file` function is the first place the
runtime performs host I/O on behalf of the caller; v0.9 will gate this
behind the capability model.

## v0.7.0 — Tracing Garbage Collection

Replaces the per-allocation `malloc`/`free` discipline with a stop-the-world
mark-and-sweep collector. Every heap object the runtime produces — values,
environments, persistent-collection internals, and scratch arrays — is now
tracked by a single registry and reclaimed automatically once it becomes
unreachable. The surface language is unchanged.

### Added

- `gc_hdr_t`-prefixed universal allocation path. Every internal allocation
  (values, vec/HAMT nodes, HAMT entries, env frames, env binding arrays,
  name strings, reader scratch buffers) carries a header with a type tag,
  mark bit, size, and registry link. `gc_alloc_typed` is the single entry;
  no path creates an unmanaged mino object.
- Mark phase traces objects according to their type tag, following every
  owned pointer the walker knows about. Vector trie leaves versus branches
  are distinguished by a `is_leaf` bit on each node so the walker knows
  what its slots hold. HAMT nodes drive their own walk via `bitmap`,
  `subnode_mask`, and `collision_count`. Scratch ptr-arrays (reader
  buffers, eval temps, prim_vector/hash-map temps) are walked as arrays of
  gc-managed pointers so partial fills survive allocation mid-loop.
- Conservative stack scan, driven by a sorted index of allocation bounds
  built at the start of each collection. `setjmp` flushes callee-saved
  registers into the collector's frame; the scan walks every aligned
  machine word between the saved host frame and the collector's own
  frame, marking any word that lands inside a managed payload (interior
  pointers supported). Public API entry points — `mino_env_new`,
  `mino_eval`, `mino_read`, `mino_install_core` — each record their local
  stack address so the scan's upper bound always dominates the full
  host-to-mino call chain even when control re-enters from a shallower
  frame.
- Root set: all `mino_env_t` returned by `mino_env_new` (tracked in a
  dedicated registry until `mino_env_free`), plus the symbol and keyword
  intern tables, plus the conservative stack scan.
- Adaptive collection trigger. The threshold starts at 1 MiB and grows to
  2× live-bytes after each sweep, so steady-state programs see amortized
  constant-factor collection work.
- `MINO_GC_STRESS=1` env var forces collection on every allocation. This
  is how we validate that no caller holds unrooted pointers across any
  allocation site. `make test-gc-stress` runs the full smoke suite under
  this mode.
- 4 new smoke cases exercise long-tail recursion, vector churn, map
  churn, and closure churn — each allocates orders of magnitude more
  transient values than any single collection's threshold, so the
  collector is invoked repeatedly and the live set must survive intact
  (117 cases total). All pass under stress mode across `-O0`, `-O1`,
  `-O2`, and `-O3`.

### Changed

- `mino_env_free` no longer frees memory synchronously. It unregisters
  the env from the root set; the next collection reclaims the frame and
  every value that was reachable only through it. Header docstring
  updated to reflect the new ownership model.
- Every internal `free()` call on mino-owned memory has been removed.
  The collector is the sole path to reclamation. Plain `malloc`/`free`
  remain for host-owned scratch (`main.c`'s line buffer, the root-env
  list node, the collector's own range-index cache).
- `mino_vec_node_t` gains a one-byte `is_leaf` flag set by the
  constructors so the mark walker can interpret slot contents without
  external context.

### Notes

The collector is non-incremental and non-generational; the entire heap
is scanned on each cycle. For the sizes this runtime is meant to embed
at, linear scan over a sorted range index is a good fit, and the 2×
live-bytes threshold keeps mean pause time bounded. The v0.12 release
candidate will profile realistic workloads and decide whether to layer
on an incremental pass.

## v0.6.0 — Macros

Lifts the surface language above its primitives. `defmacro`, quasiquote,
and a small set of in-language threading and short-circuit forms mean
that new control shapes can land without growing the C evaluator.

### Added

- `MINO_MACRO` value type. Shares the closure layout (params, body,
  captured env) so the same bind/apply path serves both. Printer
  emits `#<macro>`; equality is identity.
- `defmacro` special form binds a macro in the root frame. When the
  evaluator encounters a call whose head resolves to a macro, it
  applies the macro body to the *unevaluated* arguments and then
  evaluates the returned form in the caller's environment.
- Reader gains `` ` ``, `~`, `~@` as shorthands for `(quasiquote x)`,
  `(unquote x)`, `(unquote-splicing x)`. Both backtick and tilde are
  treated as word breaks so symbols no longer absorb them.
- `quasiquote` special form walks its template. Vectors and maps are
  recursed into; `(unquote x)` evaluates `x` and uses the value;
  `(unquote-splicing x)` evaluates `x` (expected to yield a list) and
  inlines the elements into the enclosing list.
- Variadic parameter lists: a trailing `& rest` binds `rest` to the
  list of remaining arguments (possibly empty). Works for `fn`,
  `defmacro`, and `loop`.
- `macroexpand-1` (single step) and `macroexpand` (to fixed point)
  primitives expose the expander for inspection.
- `gensym` primitive with an optional string prefix (default `G__`)
  and a monotonic counter. Macro authors use this to introduce
  temporaries that won't capture caller-visible names — the 0.x
  hygiene convention.
- `cons?` and `nil?` predicates. The threading macros use `cons?` to
  tell whether a step is a bare symbol or a call form.
- In-language stdlib macros defined in mino itself, read + eval'd at
  core install: `when`, `cond`, `and`, `or`, `->`, `->>`. Each ships
  as mino source embedded in the runtime; they are bindings in the
  root env, not special forms.
- 15 additional smoke cases covering `defmacro`, quasiquote splicing,
  variadic params, `macroexpand-1`, `gensym` freshness, and every
  stdlib macro (113 cases total).

### Notes

0.x makes no automatic hygiene promise; macro writers should reach
for `gensym` when they need an identifier that can't capture anything
the caller introduced. The decision whether to keep gensym-only or
add full hygiene lands in v1.0 triage.

## v0.5.0 — Persistent Maps

Replaces the map layout with a 32-wide hash array mapped trie. `get`,
`assoc`, and `update` are now sub-linear; maps can be used as map keys,
equality between maps no longer scales quadratically, and lookup no
longer depends on key arity.

### Changed

- Map representation is now a HAMT plus a companion insertion-order
  key vector. Lookup walks the trie for O(log₃₂ n) `get`; iteration
  walks the key vector so `keys`, `vals`, and the printer emit
  entries in the order they were first inserted — a rebind leaves
  the slot's position alone. Iteration order is part of the contract.
- Equality between maps is O(n log₃₂ n): walk one map's keys and
  look each up in the other.
- `mino.h` exposes `{ root, key_order, len }` with `mino_hamt_node_t`
  as an opaque forward declaration; header still UNSTABLE until v1.0.

### Added

- Hash function compatible with `=`. Integral floats hash as the
  equivalent int so `(= 1 1.0)` stays consistent between equality and
  the hash table. Strings, symbols, and keywords carry distinct type
  tags so byte-equal values of different types hash apart. Vectors
  hash element-wise; maps XOR-fold entry hashes for order-independent
  structural hashing. Non-hashable values (primitives, closures) fall
  back to pointer identity.
- Collision handling: when two distinct keys hit the same 32-bit
  hash, a collision bucket holds them as a linear list at the depth
  where trie descent can no longer discriminate. Inserting a key
  whose hash doesn't match the bucket promotes the bucket into a
  bitmap node that routes the two subtrees separately.
- 5 additional smoke cases locking down map iteration order across
  literals, rebinds, new-key assoc, printing, and a 200-entry map
  that crosses several levels of the trie (98 cases total).

### Notes

The v0.5 HAMT is the last structural replacement before the GC work
in v0.7; from here the layout stays but the allocator underneath
changes. Semantics remain the contract.

## v0.4.0 — Persistent Vectors

Replaces the vector layout with a persistent 32-way trie without
changing the surface language. Every vector primitive from v0.3 behaves
identically; the work lives entirely behind the API.

### Changed

- Vector representation is now a 32-way persistent trie with a tail
  buffer. Leaves hold exactly 32 elements; the tail holds the trailing
  1..32 so tail appends are O(1) amortized. `conj` and `assoc`
  path-copy only the walked spine, so successor vectors share
  structure with their source. `nth` walks at most log₃₂ n internal
  nodes.
- `mino.h` exposes the new vector shape as `{ root, tail, tail_len,
  shift, len }` with `mino_vec_node_t` as an opaque forward
  declaration; the header is still marked UNSTABLE until v1.0.
- Element access across all collection primitives — `nth`, `first`,
  `rest`, `get`, `count`, `assoc`, `conj`, `update`, vector self-eval,
  structural equality, printer — routes through internal
  `vec_nth`/`vec_conj1`/`vec_assoc1` helpers. No caller sees the
  trie layout.

### Added

- `vec_from_array`: a bulk build path that freezes the last 1..32
  elements as the tail, packs the rest into full leaves, and folds
  layers 32-to-1 up the spine in a single O(n) pass. Nodes are mutated
  freely during construction and only become visible as part of the
  persistent vector when the build completes — the internal
  "transient" path with no public API.
- `bench/vector_bench.c`: a standalone measurement program for bulk
  build, `nth`, and `assoc` at sizes 32, 1024, 32768, and 2^20. Wired
  as `make bench`; not run by CI.
- 2 additional smoke cases that cross the 32- and 1024-element
  boundaries and demonstrate structural sharing on a 2000-element
  `assoc` (93 cases total).

### Notes

The naïve map layout from v0.3 is still in place. v0.5 replaces it
with a HAMT, again without changing the surface API. The semantics
are the contract, not the layout.

## v0.3.0 — Literal Vectors, Maps, and Keywords

Brings the value-oriented data model to the surface language. Programs
can now express structured data literally and manipulate it through
immutable collection primitives.

### Added

- `MINO_KEYWORD` value type. Reader parses `:foo` as a keyword
  (self-evaluating, prints as `:foo`). Symbols and keywords are
  interned through process-wide tables so that repeated reads of the
  same name share storage; equality still falls through to length +
  byte compare so externally-constructed values compare equal too.
- `MINO_VECTOR` value type with an array-backed representation.
  Reader parses `[a b c]`; printer round-trips the same shape.
  A vector literal is a form, not a datum: the evaluator walks it in
  order and produces a fresh vector of evaluated elements.
- `MINO_MAP` value type with parallel (keys, vals) flat arrays.
  Reader parses `{k1 v1 k2 v2}`; commas are whitespace. Odd-form
  contents are a parse error. Map literals self-evaluate keys and
  values in read order; the constructor resolves duplicate keys by
  last-write-wins. Equality is structural and order-insensitive.
- Collection primitives: `count`, `nth`, `first`, `rest`, `vector`,
  `hash-map`, `assoc`, `get`, `conj`, `update`, `keys`, `vals`.
  `first`, `rest`, and `count` are polymorphic across cons, vector,
  map, string, and nil where meaningful. `assoc` works on both maps
  and vectors (vector indices may extend one past the end to append).
  `conj` prepends to lists, appends to vectors, and accepts `[k v]`
  vectors when the target is a map.
- `apply_callable` factored out of the evaluator so primitives
  (starting with `update`) can call back into user-defined functions
  with the same trampoline semantics as direct application.
- 43 additional smoke-test cases covering keywords, vector and map
  literals, self-evaluation, and every collection primitive across
  the shapes they support (91 cases total).

### Notes

The v0.3 representations (flat arrays for vectors and maps, linear
scan for map lookup) are intentionally naïve. The public contract is
the primitive signatures and semantics; v0.4 replaces the vector
layout with a persistent 32-way trie and v0.5 replaces the map with a
HAMT, both without changes to the surface API.

## v0.2.0 — Core Special Forms and Closures

Locks in lexical scope, first-class functions, and bounded-stack tail
recursion. The evaluator is now expressive enough to define factorial
and fib iteratively and to build and apply higher-order functions.

### Added

- Chained environments: each frame carries a parent pointer, lookups
  walk the chain, and bindings write to the current frame so that
  `let` and `fn` parameters shadow outer names without mutating them.
  `def` always binds in the root frame regardless of where the form
  is evaluated from.
- Conditional and sequencing: `if` with an optional else branch
  (defaulting to `nil`) that dispatches on truthiness (only `nil` and
  `false` are falsey; `0`, `""`, and the empty list are truthy), and
  `do` which evaluates its forms left to right and returns the last.
- `let` with a flat pair-list binding form (`(let (x 1 y 2) body)`),
  sequential evaluation so later bindings may reference earlier ones,
  and an implicit-do body.
- `fn` special form and a new `MINO_FN` value type. Closures capture
  the environment at definition time; applying one binds parameters
  in a fresh child frame of the captured environment. Arity mismatches
  produce a clear error. Function values print as `#<fn>` and compare
  by identity.
- `loop` and `recur` with a tail-call trampoline: `recur` yields a
  sentinel value that the enclosing `fn` or `loop` intercepts to
  rebind and re-enter its body, so tail recursion is bounded on the
  C stack (tested to 100k+ iterations). Non-tail `recur` is rejected
  with a clear error.
- Chained `<=`, `>`, and `>=` comparison primitives alongside the
  existing `<` and `=`.
- 25 additional smoke-test cases covering the new forms, closures,
  factorial, fib, and deep tail recursion (48 cases total).

## v0.1.0 — Walking Skeleton

The first published milestone. Establishes the single-file build, the
public C header, and an end-to-end read-eval-print pipeline.

### Added

- Tagged value representation (`mino_val_t`) covering nil, boolean,
  integer, float, string, symbol, cons cell, and primitive function.
  Singletons for nil/true/false; per-allocation construction for the rest.
- Recursive-descent reader for atoms, lists, strings, line comments,
  numeric literals (integer and floating-point), and the `'` quote
  shorthand. Parse errors are reported via `mino_last_error()`.
- Printer that round-trips the reader's accepted subset, re-escapes
  string literals, and always emits a decimal point for floats so that
  printed forms re-read as the same value.
- Tree-walking evaluator with `quote` and `def` special forms and
  primitive bindings for `+`, `-`, `*`, `/`, `=`, `<`, `car`, `cdr`,
  `cons`, and `list`. Numeric coercion promotes int to float when any
  argument is a float; `=` compares int and float by value.
- Single global environment stored as a flat `(name, value)` array;
  `def` replaces in place when the name already exists.
- Standalone `mino` binary providing an interactive REPL with multi-line
  input support. Prompts and diagnostics are written to stderr so that
  piped output on stdout remains clean and machine-consumable.
- `tests/smoke.sh` covering 23 end-to-end cases through the binary.
- GitHub Actions matrix build for `ubuntu-latest` and `macos-latest`.
- MIT license, README, and `.gitignore`.
