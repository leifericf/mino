# Backlog

What remains for mino to be complete at its intended scale. Tooling
(tree-sitter-mino, mino-lsp, mino-nrepl) is done. These are core
language and embedding API items.

## C/C++ Interop

mino is a GC'd language embedded inside unmanaged C/C++ programs. This
creates interop concerns that JVM-hosted dialects do not face.

### GC-safe value pinning

C hosts must keep values reachable from a registered env or hope the
conservative stack scanner finds them. Stack scanning depends on
compiler register allocation and optimization level.

Add `mino_pin(val)` / `mino_unpin(val)` to maintain an explicit root
set the GC always marks. ~40 lines.

### Argument marshaling helpers

Every `mino_prim_fn` manually walks the cons-list args, checks types,
and extracts values. This is repetitive and error-prone.

Add `mino_args_parse(args, "ii", &x, &y)` that validates arity and
types in one call, setting `mino_last_error` on mismatch. ~100 lines.

### Handle method dispatch

Operating on a handle requires global primitives that check the tag
with `strcmp`. This does not compose when multiple libraries register
handles.

Add `mino_register_method(tag, name, fn)` so that mino code can call
methods on handles by tag. Connects to the protocols item above.
~200 lines.

### Structured error propagation

C primitives return NULL and set a string via `mino_last_error`. There
is no way to throw a structured exception from C that mino's
`try`/`catch` can catch with data attached.

Expose the internal `throw_val` mechanism: `mino_throw(val)`. ~30 lines.

### C++ patterns

`mino.h` has `extern "C"` guards. C++ exceptions must not propagate
through mino's eval (C has no unwinding). C++ primitives must wrap in
try/catch. A thin `mino.hpp` header with RAII wrappers for
`mino_env_t*` and pinned values would help C++ adoption. ~100 lines.

## Data Structures (Deferred)

### Array map

Small-map optimization (linear scan for maps under ~8 entries).
mino's HAMT is already fast for small maps, so the payoff is minimal.

### Transients

Batch mutation API for bulk collection building. `vec_from_array`
covers the main internal case. Adds complexity without proportional
embedded-scale benefit.

### Chunked sequences

32-at-a-time lazy evaluation. Adds complexity to the seq machinery;
mino's element-at-a-time approach is fine at embedded scale.

### `rest` vs `next` distinction

Would require empty list `()` to be distinct from `nil`, which is a
fundamental change to mino's value model. mino's `rest` has `next`
semantics (returns nil when exhausted). Style guides already recommend
`next` for nil-punning, so this rarely surprises in practice.

## Core Library (Needs C Support)

### `sorted-map-by` / `sorted-set-by`

Custom comparator sorted collections. The rbtree infrastructure already
has a `comparator` field and `rb_compare` accepts an optional comparator,
but the C constructors (`mino_sorted_map`, `mino_sorted_set`) hardcode
`NULL`. Need new C primitives that accept a comparison function. ~30 lines.

### `subseq` / `rsubseq`

Range queries on sorted collections. Need C-level in-order traversal
with lower/upper bounds on the rbtree. ~80 lines.

### `pr` / `print` / `newline`

Output without trailing newline. `println` and `prn` always append a
newline. Need C primitives for no-newline output. ~20 lines each.

## Architecture

### mino-std package

Separate repo for rich mino library code loadable via `require`.
Candidates: lazy sequence combinators, protocols, transducers. Create
when enough content justifies it.

### mino-fs package

Separate repo for file system operations. Candidates: `file-exists?`,
`delete-file`, `rename-file`, `mkdir`, `directory?`, `glob`,
`file-size`, directory listing.

`slurp` and `spit` should eventually migrate from `mino_install_io`
into mino-fs. They currently live in the C core for bootstrapping
convenience but are conceptually library-level I/O.

### Imperative-style standard library

Separate package providing familiar API names for users coming from
imperative embedded scripting backgrounds. Backed by mino's
persistent data structures. Separate repo, low priority.
