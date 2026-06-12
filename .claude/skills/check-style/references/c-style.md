# mino C style — the checkable standard

The build is the first gate; this file is what reviewers check beyond
"it compiles". Sources of truth: the Makefile CFLAGS, `./mino task
qa-arch`, `./mino task lint-zig`, and `docs/ARCHITECTURE_CONTRACT.md`.
When this file and a gate disagree, the gate wins — fix this file.

## Language level

- ANSI C99, no compiler extensions: every TU compiles under
  `-std=c99 -Wall -Wpedantic -Wextra -Werror` (gcc, Apple clang,
  mingw, MSVC canary, and pinned zig clang all gate in CI).
- No `.zig`, no C11/GNU features (no anonymous unions, no statement
  expressions, no `__attribute__` outside existing guarded uses).
- Platform splits live behind `_WIN32` guards, isolated to the files
  that already carry them; POSIX calls that follow symlinks (`stat`)
  vs not (`lstat`) are chosen deliberately and the choice commented.
- Truth values are plain `int` (0/1); no `<stdbool.h>`, `_Bool`, or
  `bool` typedefs — the keyword's status varies across C versions and
  toolchains.
- No identifiers with a leading underscore: they are reserved (the
  adapted regex header's `_TINY_REGEX_C` guard is legacy, not
  precedent).

## Size discipline (qa-arch gates, hard numbers)

- Translation unit ≤ 1100 LOC. Approaching the limit means the TU is
  carrying two responsibilities — split by domain (`numeric_math.c`,
  `sequences_seq.c` are the precedent), don't shave lines.
- Function body ≤ 250 LOC. Same rule: split by phase or case family.

## Linkage and prototypes (lint-zig gates)

- `static` by default. Anything non-static needs a prototype in the
  owning `internal.h` (`-Wmissing-prototypes`,
  `-Wmissing-variable-declarations` are -Werror).
- Also gated: `-Wshadow -Wstrict-prototypes -Wpointer-arith
  -Wwrite-strings -Wundef -Wvla -Wimplicit-fallthrough -Wcomma
  -Wunreachable-code -Wnested-externs -Wredundant-decls -Wformat=2`.
- Deliberately NOT gated (don't "fix" these): partial-brace zero-init
  `{0}`, `default:` arms in VM switches, const-dropping casts in GC
  marking, function-return casts. They are audited idiom.

## Naming and ownership (ARCHITECTURE_CONTRACT §4)

- `*_new` / `*_alloc` — caller owns the result.
- `*_free` / `*_destroy` — caller releases ownership.
- `*_get` / `*_peek` — borrowed pointer, do not free.
- `*_take` — ownership transfers to caller.
- State access is explicit `S->field`; no alias macros. The only
  macros that implicitly need a local `S` are `gc_pin`/`gc_unpin`.
- GC-owned (via `gc_alloc_typed`) vs host-owned (`malloc`) is a hard
  boundary: never `free` a GC-owned value, never let a host-owned
  struct hold the only reference to a GC value across an allocation.

## Includes

- Path-qualified: `#include "runtime/internal.h"`, never bare
  `#include "internal.h"` (every subsystem owns one).
- Respect the dependency directions in `docs/INTERNAL_MODULE_MAP.md`;
  no new cross-module includes without a cross-module escalation.

## Control flow and expressions

- Validate early, return early: invalid input and failure cases exit
  at the top; the normal path runs at low indentation, not nested
  inside success conditionals.
- One important action per line. No assignment inside conditionals,
  no nested ternaries, no multi-effect loop conditions, no
  short-circuit chains that hide real work.
- When resources accumulate, use a single cleanup path (one
  `goto cleanup` label) that is safe under partial initialization —
  not mirrored cleanup blocks in every error branch.
- One function, one job, locally understandable: a reader should not
  need the whole program to follow it. Too many flags, branches, or
  ownership transitions means split by phase or case family.
- Prefer the simplest correct version over the clever one, even when
  the clever form is technically right.

## Memory, sizes, and bounds

- Check every allocation result; free owned memory exactly once;
  never return pointers to dead stack storage.
- Initialize before use; never read uninitialized memory. Prefer
  `{0}` initializers for zeroing new structs — `{0}` yields correct
  null pointers and `0.0` by the standard, while `memset` relies on
  the all-bits-zero platform assumption. The supported matrix
  satisfies that assumption, so existing `memset`-zero sites are
  accepted idiom, not findings.
- Large or input-sized buffers go on the heap (`-Wvla` is gated); a
  stack overrun is more dangerous than a heap overrun.
- Guard size/index arithmetic before allocation and pointer math:
  `SIZE_MAX` overflow checks precede the multiply/add they protect.
- Buffers travel with lengths; terminator and truncation rules are
  explicit at the API. Parsing checks before reading, before
  advancing, before copying, and rejects malformed lengths early.
- Bounds checks sit adjacent to the access they protect — including
  opcode, constant-table, and register indices. Malformed internal
  state fails closed.

## Types and conversions

- `size_t` for sizes and counts; signed types only where a negative
  value means something. No careless signed/unsigned mixing, no
  silent narrowing.
- Never cast to silence a warning — fix the types or add a checked
  conversion helper. A required cast stays narrow and justified at
  the call site (the NOT-gated list above is the audited exception).
- Fixed-width types wherever representation matters (value encodings,
  packed fields, wire and file formats).

## Undefined behavior (runtime core)

Eval, GC, values, and bytecode paths are held to these without
exception; the rest of the tree follows them too:

- Never rely on signed overflow. Fast numeric lanes keep overflow
  behavior explicit and tested.
- Never shift by a negative amount or by >= the bit width; bit
  packing and field extraction use unsigned operands; encoding
  assumptions are documented at opcode/value boundaries.
- No strict-aliasing violations, no pointer punning through
  incompatible types.
- Relational pointer comparison (`<`, `>=`) is defined only within
  one object; cross-object address ordering (heap bounds, arena
  membership) goes through `uintptr_t`, as the GC already does.
  Equality (`==`/`!=`) is fine across objects.

## Floating point

- `==` on floats is meaningful only for assigned or copied values,
  never for computed results — compare with an explicit tolerance or
  restructure the check.
- NaN compares false with everything including itself: `f != f` (or
  C99 `isnan`) is the test; remember it when writing comparison and
  sort paths.
- Don't expect bit-identical float results across platforms or
  optimization levels; tests asserting on printed floats must
  tolerate this.

## Error handling

- Every `abort()` carries a comment explaining why recovery is
  impossible (qa-arch inventories abort sites; new ones need explicit
  justification).
- User-triggerable input must never reach a `MINO_ERR_CORRUPT` path.
- Classify via the three-class model in `src/diag/diag_contract.h`;
  per-subsystem `internal.h` files carry "Error classes emitted"
  blocks — keep them current when you add paths.
- Diagnostics name the failing operation and the reason ("length
  exceeds maximum supported value"), never a bare "error"/"failed".

## Assertions and invariants

- Assertions cover internal invariants in trusted paths — impossible
  states, contract violations. They are never the only validation for
  external input (user, file, host); that gets runtime checks.
- Debug-only invariant checks at frame/register/GC boundaries are
  encouraged; invariant comments are contracts and must stay
  synchronized with the code.

## Macros

- `static` functions over function-like macros. Macros are for
  constants, include guards, config gates, portability shims, and
  tiny encoding helpers.
- A macro that allocates, frees, branches, returns, or evaluates an
  argument more than once should be a function (`gc_pin`/`gc_unpin`
  are the audited exception).
- Parenthesize parameters and the whole expansion; no hidden control
  flow or ownership semantics.

## Core and shell (decide, then act)

mino already separates deciding from doing in its load-bearing
places; new code follows the same shape (the pattern is
functional-core/imperative-shell, adapted to C):

- **Compute a plan, return it, let the caller act.** The compiler
  (`bc/compile.c`) produces bytecode the VM executes; the GC driver
  tick *picks* minor vs major vs incremental-slice and the collectors
  carry it out; the error path *classifies* (`MINO_ERR_*`) and then
  reports through one channel. A decision in C is an enum, a small
  struct, or a filled out-parameter — make the caller's switch
  explicit rather than burying the choice inside the effectful loop.
- **Pragmatic, not pure.** Core/decision functions read state
  directly — `S->field` access and walking live values is expected,
  exactly like the rest of mino. Purity theater (handle indirection,
  wrapper layers, function-pointer seams introduced only so a test
  can intercept) is a factoring defect, not a virtue.
- **What decision code must not do:** perform I/O, mutate GC state,
  signal the host, or consult *operational* bookkeeping (retry
  counters, trace stats) to make a *semantic* choice.
- **Keep the shell boring.** The function that loops, calls out, and
  writes results should contain no branching a test would want to
  reach; if it does, that branch is a decision that belongs in the
  core.
- **Test the core, not the shell.** Decision functions get direct
  tests; effectful shells are covered by the suite end to end. mino
  has no mocks and that is deliberate.

## Performance work

- Hot-path changes need measured evidence — before/after numbers, not
  intuition. No noise-level wins.
- Isolate the fast path, comment the invariant that makes it safe,
  add a targeted test per tricky invariant, and keep the slow path
  boring and correct first.

## Change scoping and abstraction

- One logical change per commit: behavior changes never carry
  drive-by cleanup, renames, or style churn in untouched code. When a
  task splits naturally, land mechanical prep, refactor, behavior,
  and tests as separate steps.
- Follow the local subsystem's conventions; consistency inside the
  file beats abstract preference.
- Abstraction must earn its place: a helper exists to reduce mistakes
  or preserve an invariant across call sites, not to hide a few
  repeated lines. Duplication is cheaper than the wrong abstraction.

## Comments

- Comment density matches the surrounding file. Comments state
  constraints the code can't (ownership, GC-safety windows, why an
  abort is unrecoverable) — not what the next line does.
- TU-top block comment states the file's single responsibility.
