# Changelog

All notable changes to mino are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/).

## [0.9.0] — Sandbox, modules, diagnostics

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

## [0.8.0] — Host C API

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

## [0.7.0] — Tracing garbage collection

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

## [0.6.0] — Macros

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
