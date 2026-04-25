# Changelog

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
  the source files, and the top-level `NOTICE` plus
  `THIRD_PARTY_LICENSES.md` carry the copyright and license text.
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
