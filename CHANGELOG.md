# Changelog

All notable changes to mino are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/).

## [0.5.0] — Persistent maps

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

## [0.4.0] — Persistent vectors

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

## [0.3.0] — Literal vectors, maps, and keywords

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

## [0.2.0] — Core special forms and closures

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

## [0.1.0] — Walking skeleton

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
