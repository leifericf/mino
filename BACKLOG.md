# Backlog

What remains for mino to be complete at its intended scale. Tooling
(tree-sitter-mino, mino-lsp, mino-nrepl) is done. These are core
language and embedding API items.

## Language

### Nice-to-have

**Variadic `comp`** -- compose N functions. Currently `comp` only
accepts two arguments: `(comp f g)`. The reference language supports
`(comp f g h ...)`. Change from fixed 2-arity fn to a reduce-based
variadic form. ~10 lines in core.mino.

**`reduced`** -- early termination for `reduce`. `(reduce (fn [acc x]
(if (> x 5) (reduced acc) (+ acc x))) 0 (range 100))`. Requires a
`Reduced` wrapper type and check in the reduce loop. ~40 lines.

**`set` constructor function** -- `(set coll)` to create a set from a
collection. Currently only set literals `#{...}` and `(into #{} coll)`
work. ~10 lines.

**`ex-info` / `ex-data`** -- structured exceptions. `(throw (ex-info
"msg" {:key val}))` with `(ex-data e)` in catch. Currently `throw`
takes any value, but `ex-info` provides a standard structure. ~30 lines.

**Multi-collection `map`** -- `(map f coll1 coll2 ...)` applying `f`
to corresponding elements of multiple collections. Currently `map`
only accepts one collection. ~30 lines in core.mino.

**`format` precision specifiers** -- `(format "%.2f" 3.14)` currently
returns the literal `"%.2f"`. The format parser handles `%d`, `%s`,
`%f` but not width/precision modifiers like `%.2f` or `%04d`. ~40 lines.

**Protocols** -- polymorphic dispatch on first argument's type.
`defprotocol`, `extend-type`, `extend-protocol`. Enables extensible
abstractions. ~400 lines.

**Value metadata** -- arbitrary key-value metadata on any value.
`with-meta`, `meta`, `vary-meta`. Reader syntax `^{:key val} form`.
Currently only docstrings on `def`/`defmacro` via the internal
`meta_table`. Requires adding a metadata pointer to `mino_val_t`.
~300 lines.

**`identical?`** -- pointer identity comparison. C primitive checking
`a == b` without value comparison. ~10 lines.

**`declare`** -- forward declaration. `(def x)` without a value, or
`(declare x y z)`. Needed for mutually recursive definitions without
`defn`. ~20 lines.

**`with-open`** -- resource management. `(with-open (f (open path)) body)`.
Needs `try/finally` in the evaluator. ~50 lines for `finally` + ~20
for the macro.

**Transducers** -- `transduce`, `eduction`, `into` with xform.
Composable algorithmic transformations. Requires protocol-level
abstractions or a simpler approach. ~400 lines.

**Multi-binding `doseq`/`for`** -- nested iteration with multiple
bindings. Currently single-binding only. Requires destructuring or
recursive macro expansion over binding pairs. ~100 lines.

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
