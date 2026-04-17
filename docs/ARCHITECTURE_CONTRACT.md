# Architecture Contract

This document defines the invariants that must remain true across all
refactoring and readability work. No change may violate these constraints
without explicit justification, isolated commits, and tests.

## 1. Value Semantics

mino is a value-oriented language in the Clojure family.

**Must stay true:**

- All collections (list, vector, map, set) are persistent and immutable.
  Mutation is only possible through atoms (`atom`, `reset!`, `swap!`).
- Code is data. All mino forms are represented as mino values (cons lists,
  vectors, maps, symbols, keywords, strings, numbers).
- `nil` and `false` are falsey. Everything else is truthy.
- Equality is structural (`mino_eq`). Identity comparison is separate
  (`identical?`).
- Keywords and symbols are interned per-state. Two symbols with the same
  name within the same state are pointer-equal.
- Lazy sequences are transparent: they behave like lists once forced.
  Forcing is implicit on sequence operations.

## 2. Embedding Boundary

mino is an embeddable runtime. The host drives everything.

**Must stay true:**

- The host creates and owns `mino_state_t`. No global or ambient state exists.
- The host creates environments (`mino_env_t`) and chooses which primitives
  to install. `mino_install_core` provides the language; `mino_install_io`
  is opt-in.
- The host drives evaluation by calling `mino_eval`, `mino_eval_string`,
  `mino_call`, or `mino_repl_feed`. No background threads are started by the
  runtime.
- Values returned from eval are borrowed: they survive until the next GC cycle.
  The host must use `mino_ref`/`mino_unref` to retain values across calls.
- The host can register C functions as primitives via `mino_register_fn`.
- The host can set execution limits (steps, heap) and interrupt evaluation.
- Handles (`MINO_HANDLE`) let the host attach opaque pointers with type tags
  and optional finalizers.

## 3. Isolation and Concurrency

**Must stay true:**

- Each `mino_state_t` is fully isolated. No mutable data is shared between
  states. States may coexist in the same process without synchronization.
- Cross-state value transfer uses serialization (`mino_clone`,
  `mino_mailbox_send`/`recv`). No pointers cross state boundaries.
- The mailbox is the only concurrency primitive that uses a mutex. It is
  safe to call `send` and `recv` from different threads simultaneously.
- Actors bundle state + env + mailbox. The host drives actors; the runtime
  does not manage threads.
- `mino_interrupt` is the only function safe to call from a thread other
  than the one running eval.

## 4. Ownership Model

Two ownership categories exist in the C implementation:

### GC-owned (managed)

All `mino_val_t`, `mino_env_t`, `mino_vec_node_t`, `mino_hamt_node_t`,
and `hamt_entry_t` values are allocated through `gc_alloc_typed` and
managed by the mark-sweep collector.

- The GC traces roots: registered environments, the gc_save stack, the
  conservative C stack scan, host refs, dynamic bindings, and intern tables.
- Values are valid until the next collection. The gc_save stack (`gc_pin`/
  `gc_unpin`) protects temporaries across allocation points.
- Handles receive a finalizer callback before the GC frees them.

### Host-owned (malloc)

These structures use direct `malloc`/`calloc`/`realloc`:

- `mino_state_t` itself (allocated in `mino_state_new`)
- `root_env_t` registry nodes
- `mino_ref_t` retention nodes
- `dyn_binding_t` dynamic binding frames
- `mino_mailbox_t` and its message queue
- `mino_repl_t` and its line buffer
- Module cache and metadata table arrays
- Intern table arrays
- GC range index
- Clone serialization buffers (`sbuf_t`)

**Naming conventions (target, not yet fully applied):**

- `*_new` / `*_alloc` = caller owns the result
- `*_free` / `*_destroy` = caller releases ownership
- `*_get` / `*_peek` = borrowed pointer, do not free
- `*_take` = ownership transfers to caller

## 5. Error Model

Two error classes exist:

### Class I: Invariant-fatal (abort)

Used when no recovery is possible because the runtime is in an
inconsistent state or no state exists to report through.

Current abort sites (all verified Class I):

- `mino.c:23` -- `mino_state_new` initial alloc failure (no state exists)
- `mino.c:238,249` -- `gc_alloc_typed` OOM with no try frame
- `mino.c:585,662` -- GC range index realloc inside GC (mid-collection)
- `mino.c:1031` -- unexpected `setjmp` return value (corrupted stack)
- `prim.c:3693,3706,3717,3725,3739` -- core.mino bootstrap failures

**Rule:** Every `abort()` must have a comment explaining why recovery is
impossible. New abort sites require explicit justification.

### Class II: Recoverable (error return)

Used for conditions the host or mino code can handle:

- Argument/type errors in primitives: `prim_throw_error` (catchable via
  try/catch in mino code).
- Resource failures during eval (OOM after try frame is established):
  `set_error` + return NULL, surfaced via `mino_last_error`.
- Reader errors: return NULL + error message.
- File I/O errors: return NULL + error message.

**Rule:** No user-triggerable input may reach a Class I path. If user input
can cause an abort, that is a bug.

## 6. Special Forms

These are recognized directly by the evaluator and are not installed as
environment bindings:

    quote  quasiquote  unquote  unquote-splicing
    def  defmacro  declare
    if  do  let  let*  fn  fn*
    loop  loop*  recur
    try  catch  finally
    binding  lazy-seq

**Must stay true:** Adding or removing a special form requires explicit
justification. Special forms cannot be shadowed by environment bindings
(the evaluator checks them first).

## 7. GC Invariants

- Collection may occur on any `gc_alloc_typed` call (and always does under
  `MINO_GC_STRESS=1`).
- Any value held across an allocation boundary must be either (a) reachable
  from a root, or (b) protected via `gc_pin`.
- `gc_pin`/`gc_unpin` must be paired. The gc_save stack has 64 slots
  (`GC_SAVE_MAX`); overflow increments the counter but does not store the
  pointer. Debug builds assert on underflow.
- The conservative stack scanner provides a safety net but must not be
  relied upon as the sole protection mechanism.
- `gc_depth` prevents recursive collection. Allocations during GC use a
  pending-range buffer flushed after collection completes.

## 8. Module System

- `require` loads a file once and caches the result per-state.
- The host registers a resolver via `mino_set_resolver` to map module names
  to file paths.
- Module loading evaluates forms in the caller's environment.

## 9. ANSI C Portability

- All runtime code targets C99 (`-std=c99`).
- Platform-specific code is isolated: currently mutex shims in `clone.c`
  (`_WIN32` vs pthreads) and GCC/Clang volatile hints in `mino.c`.
- The regex engine (`re.c`) is a self-contained translation unit with its
  own header.
- No compiler extensions are required for core functionality.
