# Backlog

What remains for mino to be complete at its intended scale. Tooling
(tree-sitter-mino, mino-lsp, mino-nrepl) is done. These are core
language and embedding API items.

## Language

### Critical

~~**Atoms**~~ -- Done in v0.13.0.

**Destructuring** -- pattern matching in binding forms.

Vector destructuring: `(let [[a b & rest] xs] ...)`. Map
destructuring: `(let [{:keys [a b]} m] ...)` and
`(let [{a :a, b :b} m] ...)`. Applies to `let`, `fn`, `loop`, and
`defmacro` parameter lists. Currently only symbol bindings work.
Refactor `bind_params` to recursively destructure. ~300 lines.

~~**Lazy sequences**~~ -- Done in v0.14.0. `MINO_LAZY` type, `lazy-seq`
special form, `seq`, `realized?`. Seven sequence ops moved to lazy mino.

**Named anonymous functions** -- optional name in `fn` form.

`(fn name (x) (name (- x 1)))` for self-referencing anonymous
functions. The name is only bound in the function's own body. Common
in recursive anonymous fns and matches the form supported by most
Lisps. Small change to the `fn` special form parser. ~30 lines.

### Important

**Multi-arity functions** -- multiple parameter lists per function.

`(fn ([x] ...) ([x y] ...) ([x y & rest] ...))`. Dispatch by argument
count at call time. Common pattern for default arguments. ~150 lines.

~~**`defn` macro**~~ -- Done in v0.13.0 (single-arity). Multi-arity
and docstring support deferred to multi-arity fn.

~~**`spit`**~~ -- Done in v0.13.0.

### Nice-to-have

**Protocols** -- polymorphic dispatch on first argument's type.
`defprotocol`, `extend-type`, `extend-protocol`. Enables extensible
abstractions. ~400 lines.

~~**Regex**~~ -- Done in v0.16.0. `re-find`, `re-matches` via bundled
tiny-regex-c. `re-seq` implemented in core.mino.

**Value metadata** -- arbitrary key-value metadata on any value.
`with-meta`, `meta`, `vary-meta`. Reader syntax `^{:key val} form`.
Currently only docstrings on `def`/`defmacro` via the internal
`meta_table`. Requires adding a metadata pointer to `mino_val_t`.
~300 lines.

~~**String formatting**~~ -- Done in v0.14.0. `format` with `%s`, `%d`,
`%f`, `%%`.

**`identical?`** -- pointer identity comparison. C primitive checking
`a == b` without value comparison. ~10 lines.

**`declare`** -- forward declaration. `(def x)` without a value, or
`(declare x y z)`. Needed for mutually recursive definitions without
`defn`. ~20 lines.

**`with-open`** -- resource management. `(with-open (f (open path)) body)`.
Needs `try/finally` in the evaluator. ~50 lines for `finally` + ~20
for the macro.

**Dynamic bindings** -- `binding` form for thread-local dynamic vars.
`(def ^:dynamic *x* 1)` + `(binding (*x* 2) ...)`. Requires metadata
or a dedicated dynamic var mechanism. ~200 lines.

**Transducers** -- `transduce`, `eduction`, `into` with xform.
Composable algorithmic transformations. Requires protocol-level
abstractions or a simpler approach. ~400 lines.

**Multi-binding `doseq`/`for`** -- nested iteration with multiple
bindings. Currently single-binding only. Requires destructuring or
recursive macro expansion over binding pairs. ~100 lines.

## C/C++ Interop

mino is a GC'd language embedded inside unmanaged C/C++ programs. This
creates interop concerns that JVM-hosted dialects do not face.

### Handle finalizers

`MINO_HANDLE` wraps a `void*` pointer with a tag string. When the GC
sweeps a handle (`gc_sweep`, mino.c:2712), it calls `free(h)` with no
cleanup hook. The C-side resource (file descriptor, struct, GPU buffer)
leaks.

Add `mino_handle_with_finalizer(ptr, tag, cleanup_fn)` where
`cleanup_fn` is `void (*)(void *)`, called during sweep before
freeing. ~60 lines.

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

### Thread safety

mino uses ~15 static global variables: `reader_line`, `reader_file`,
`error_buf`, `gc_all`, `gc_bytes_*`, `gc_threshold`, `gc_depth`,
`gc_stack_bottom`, `gc_root_envs`, `sym_intern`, `kw_intern`,
`try_stack`, `call_stack`, `module_cache`, `meta_table`, `limit_steps`,
`limit_heap`. Two threads calling `mino_eval` concurrently corrupt all
of them.

Options: (a) `_Thread_local` on all globals (C11, ~100 lines) or
(b) a `mino_state_t` context struct passed to all API calls (portable,
cleaner, ~500 lines, breaks current API). Not urgent for v1.0.

Workaround: confine all mino calls to a single thread and use a
message queue for cross-thread communication.

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

## Performance

~~### Mailbox binary serialization~~ -- Done. Replaced tmpfile
roundtrip with direct buffer printer. 911x faster for integers.

### Cache parsed core.mino

`mino_install_core` re-parses and re-evaluates `core_mino_src`
(~800 lines) for every new environment. Measured at 524 us per
`mino_new` call. Every actor pays this cost. Read the source once
into a cached form list, then eval-into-env from the cache on
subsequent calls. Or pre-evaluate into a template env and clone it.
~100 lines.

### Lazy sequence per-element overhead

`(into [] (range 1000))` costs 8 ms (8 us/element). Each step
allocates a lazy thunk, forces it (full eval), and produces a cons
cell. For tight data-processing loops this dominates runtime. Options:
(a) add eager C-level variants for hot paths (`range` returning a
vector, `map`/`filter` with eager fallbacks for non-lazy input),
(b) accept the cost as inherent to lazy-by-default. Design decision,
not a simple fix.

## Architecture

### mino-std package

Separate repo for rich mino library code loadable via `require`.
Candidates: lazy sequence combinators (once the C type exists),
protocols, transducers. Create when enough content justifies it.

### mino-fs package

Separate repo for file system operations, following the babashka/fs
pattern. Candidates: `file-exists?`, `delete-file`, `rename-file`,
`mkdir`, `directory?`, `glob`, `file-size`, directory listing.

`slurp` and `spit` should eventually migrate from `mino_install_io`
into mino-fs. They currently live in the C core for bootstrapping
convenience but are conceptually library-level I/O.

~~### C primitive migration (phase 2)~~ -- Done in v0.14.0.

`map`, `filter`, `take`, `drop`, `range`, `repeat`, `concat` moved to
lazy mino. `update`, `some`, `every?` also moved. C core at ~50
primitives. `reduce`, `into`, `reverse`, `sort` remain in C (strict,
efficient).

### Imperative-style standard library

Separate package providing familiar API names for users coming from
imperative embedded scripting backgrounds. Backed by mino's
persistent data structures. Separate repo, low priority.

~~### Language-level actor syntax~~ -- Done in v0.19.0. `spawn` is now
a macro that takes body forms directly. `spawn*` retains the
source-string primitive for programmatic use.

~~### nREPL state-per-session concurrency~~ -- Done in v0.19.0. Each
session owns its own `mino_state_t`. Sessions share nothing. The
server is still single-threaded but the architecture enables future
concurrent eval.
