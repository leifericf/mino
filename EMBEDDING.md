# Embedding mino

This document specifies the contract between mino and a host program that
embeds it. It covers the state lifecycle, value ownership, GC interaction,
error handling, capability control, and threading rules.

The API is declared in `mino.h` and is **unstable until v1.0.0**.


## State lifecycle

Every mino session begins with a runtime state:

```c
mino_state_t *S = mino_state_new();
```

The state owns the garbage collector, intern tables, module cache, singletons,
and all GC-managed objects allocated within it. Multiple states may coexist in
the same process; they share no mutable data.

Create an environment and install bindings:

```c
mino_env_t *env = mino_new(S);          /* core bindings included */
mino_install_io(S, env);                /* opt in to I/O          */
```

Or step by step:

```c
mino_env_t *env = mino_env_new(S);      /* empty environment      */
mino_install_core(S, env);              /* pure functions only     */
```

Tear down in reverse order:

```c
mino_env_free(S, env);
mino_state_free(S);
```

`mino_state_free` releases every GC-managed object, every intern table entry,
the module cache, and any outstanding refs. Handle finalizers fire during this
sweep (see Handles below).


## Value ownership

**Borrowed by default.** Values returned by constructors (`mino_int`,
`mino_string`, etc.) and by `mino_eval` / `mino_eval_string` are borrowed.
A borrowed value is valid until the next allocation that may trigger GC.
In practice this means: use it or store it before calling another mino
function that allocates.

**Ref to retain.** To keep a value alive across allocations or GC cycles,
root it with a ref:

```c
mino_ref_t *r = mino_ref(S, val);       /* root val               */
/* ... any number of allocations / evals ... */
mino_val_t *v = mino_deref(r);          /* get the value back     */
mino_unref(S, r);                       /* release the root       */
```

Refs are owned by the state. If the host forgets to unref, the ref (and its
value) are freed when the state is freed. But holding refs longer than needed
prevents the collector from reclaiming objects, so unref promptly.

**Environments are roots.** Any value reachable through a live environment
survives collection. The host does not need to ref values that are bound in
an environment it has not freed.


## Garbage collection

The collector is a conservative mark-and-sweep that runs when cumulative
allocation crosses a threshold. It also runs during `mino_state_free`.

### What triggers collection

Any function that allocates a GC-managed object may trigger a sweep. This
includes all constructors, `mino_eval`, `mino_eval_string`, `mino_read`,
and `mino_load_file`.

### What survives

An object survives if it is reachable from any root:

- Registered environments (via `mino_env_new` / `mino_new`)
- Host refs (via `mino_ref`)
- The intern tables (symbols and keywords)
- The module cache
- The metadata table
- The try/exception stack (during eval)
- The conservative stack scan (pointers on the C stack)

### GC stress mode

Set `MINO_GC_STRESS=1` in the environment to force collection on every
allocation. This is slow but catches any caller that holds an unrooted
pointer across an allocation boundary. Use it during development.


## Error handling

Most functions that can fail return `NULL` on error. The error message is
available via `mino_last_error(S)`, which returns a pointer to an internal
buffer valid until the next eval or read call on the same state.

For structured error handling from C, use `mino_pcall`:

```c
mino_val_t *result;
if (mino_pcall(S, fn, args, env, &result) != 0) {
    fprintf(stderr, "error: %s\n", mino_last_error(S));
}
```

From mino code, `try` / `catch` and `throw` provide exception handling.
`mino_pcall` catches any throw.


## Handles

A handle wraps an opaque host pointer and a type tag string:

```c
mino_val_t *h = mino_handle(S, my_ptr, "my-resource");
```

The tag is a static or interned string (not GC-owned). The host retrieves
the pointer and tag with `mino_handle_ptr(h)` and `mino_handle_tag(h)`.

### Finalizers

A handle may carry a finalizer that fires when the handle is collected by
the GC or when the state is freed:

```c
void my_cleanup(void *ptr, const char *tag) {
    fclose((FILE *)ptr);
}

mino_val_t *h = mino_handle_ex(S, fp, "file", my_cleanup);
```

If the handle is rooted (via ref or environment), the finalizer will not fire
until the root is released and the handle becomes unreachable. The finalizer
receives the same pointer and tag that were passed to `mino_handle_ex`.

Finalizers must not call back into the mino API. They run during GC sweep
or state teardown, when the runtime is not in a safe state for re-entry.


## Capability model

`mino_install_core` provides pure functions with no side effects. The host
opts in to I/O by calling `mino_install_io`, which adds `println`, `prn`,
`slurp`, `spit`, and `exit`.

A sandboxed environment starts with `mino_env_new` + `mino_install_core`
and exposes no I/O. The host can register custom primitives with
`mino_register_fn` to provide controlled access to host resources.

```c
mino_env_t *sandbox = mino_env_new(S);
mino_install_core(S, sandbox);
mino_register_fn(S, sandbox, "query", my_query_fn);
```


## Execution limits

The host can cap eval steps and heap usage:

```c
mino_set_limit(S, MINO_LIMIT_STEPS, 100000);  /* max eval steps */
mino_set_limit(S, MINO_LIMIT_HEAP, 8 * 1024 * 1024);  /* 8 MB   */
```

Step limits reset at the start of each `mino_eval` or `mino_eval_string`.
When a limit is exceeded the current eval returns `NULL` and
`mino_last_error` reports the cause. Pass 0 to disable a limit.


## Modules

The host registers a resolver to map module names to file paths:

```c
const char *my_resolver(const char *name, void *ctx) {
    /* return a file path, or NULL if unknown */
}
mino_set_resolver(S, my_resolver, my_ctx);
```

When mino code calls `(require "name")`, the resolver maps the name to a
path. The file is loaded once; subsequent requires return the cached value.


## Threading rules

**One thread per state at a time.** A `mino_state_t` is not thread-safe.
The host must not call into a state from multiple threads concurrently.
Different states may be used from different threads simultaneously since
they share no mutable data.

Refs, environments, and values belong to their state. Do not pass raw
pointers between states; use `mino_clone` or a mailbox instead.


## Value cloning

To copy a value from one state to another:

```c
mino_val_t *copy = mino_clone(dst, src, val);
```

Clone performs a deep copy. Transferable types: nil, bool, int, float,
string, symbol, keyword, cons, vector, map, set. Non-transferable types
(fn, macro, prim, handle, atom, lazy-seq) cause the clone to return NULL
with an error on the destination state.

Nested collections are cloned recursively. A single non-transferable
element anywhere in the tree fails the entire clone.


## Mailbox

A mailbox is a thread-safe FIFO queue for passing values between states
that may live on different threads:

```c
mino_mailbox_t *mb = mino_mailbox_new();

/* Thread A: send a value */
mino_mailbox_send(mb, state_a, val);

/* Thread B: receive into a different state */
mino_val_t *msg = mino_mailbox_recv(mb, state_b);

mino_mailbox_free(mb);
```

The mailbox serializes values on send and deserializes on receive, so no
cross-state pointers ever exist. The same transferability rules apply as
for `mino_clone`. `mino_mailbox_recv` returns NULL when the queue is empty.

The mailbox is the one place in mino that uses a mutex. It is safe to call
`send` and `recv` from different threads concurrently. The mailbox is owned
by the host, not by any state.


## Actors

An actor bundles a state, an environment (with core bindings installed),
and a mailbox into a single entity. Actors provide the isolation model:
each actor is a failure domain with its own GC, its own bindings, and
no shared mutable state.

```c
mino_actor_t *a = mino_actor_new();
```

The host drives the actor by sending messages and calling eval. No
background thread is started; the host controls scheduling entirely.

```c
/* Send a value from the host's state to the actor. */
mino_actor_send(a, host_state, val);

/* Receive the next message in the actor's state. */
mino_val_t *msg = mino_actor_recv(a);

/* Evaluate code in the actor's context. */
mino_eval_string(mino_actor_state(a), src, mino_actor_env(a));
```

The actor's state, environment, and mailbox are accessible individually
when the host needs fine-grained control:

```c
mino_state_t   *s  = mino_actor_state(a);
mino_env_t     *e  = mino_actor_env(a);
mino_mailbox_t *mb = mino_actor_mailbox(a);
```

Free the actor when done. This frees the mailbox, environment, and state:

```c
mino_actor_free(a);
```

### Actors from mino code

Three primitives expose the actor model to mino code:

- `(spawn src)` creates a new actor, evaluates the source string `src`
  in the actor's state, and returns an opaque actor handle.
- `(send! actor val)` sends a value to an actor's mailbox.
- `(receive)` receives the next message from the current actor's mailbox.
  Requires that `*self*` is bound to an actor handle in the current
  environment (set automatically by `spawn`).


## Concurrency primitives

### Atoms and swap!

Atoms are mutable reference cells. `reset!` sets the value directly;
`swap!` applies a function to the current value and stores the result:

```
(def counter (atom 0))
(swap! counter + 1)     ;=> 1
(swap! counter + 10)    ;=> 11
(deref counter)         ;=> 11
```

`swap!` accepts extra arguments that are passed after the current value:
`(swap! a f x y)` is equivalent to `(reset! a (f (deref a) x y))`.

### Dynamic binding

The `binding` special form establishes dynamic bindings that are visible
to all code called within the body, regardless of lexical scope:

```
(def *verbose* false)

(defn log (msg)
  (when *verbose* (println msg)))

(binding (*verbose* true)
  (log "this prints"))     ; *verbose* is true inside binding

(log "this does not")      ; *verbose* is false again
```

Dynamic bindings are per-state and stack correctly with nesting. They
shadow lexical bindings of the same name for the duration of the body.

### Agents

Agents are mutable reference cells updated via function application.
Within a single runtime, updates are synchronous. The API provides a
familiar abstraction that maps to asynchronous patterns in multi-runtime
contexts:

```
(def a (agent 0))
(send-to a + 10)          ;=> 10
(send-to a + 5)           ;=> 15
(deref a)                 ;=> 15
```

### What mino does not provide

mino does not implement shared-memory concurrency primitives (STM, refs,
dosync). Concurrency in mino happens between runtimes via message passing,
not within a single runtime via coordinated mutation. This is a deliberate
design choice: the isolation model is simpler, safer, and maps naturally
to the embedding context where the host controls scheduling.


## REPL handle

The in-process REPL lets the host drive evaluation one line at a time
without managing a read buffer:

```c
mino_repl_t *repl = mino_repl_new(S, env);

const char *line = "(+ 1 2)";
mino_val_t *result;
int rc = mino_repl_feed(repl, line, &result);

switch (rc) {
    case MINO_REPL_OK:    /* result is ready     */ break;
    case MINO_REPL_MORE:  /* need more input      */ break;
    case MINO_REPL_ERROR: /* see mino_last_error  */ break;
}

mino_repl_free(repl);
```


## Inside a primitive

Host-defined primitives receive `(mino_val_t *args, mino_env_t *env)`.
To call constructors or other API functions that require a state, use
`mino_current_state()`:

```c
mino_val_t *my_prim(mino_val_t *args, mino_env_t *env) {
    mino_state_t *S = mino_current_state();
    return mino_int(S, 42);
}
```

The current state is set by the runtime before calling the primitive and
is valid for the duration of the call.


## Quick reference

### State lifecycle

| Function | Description |
|----------|-------------|
| `mino_state_new()` | Create a new isolated runtime state |
| `mino_state_free(S)` | Free the state and all its resources |
| `mino_current_state()` | Get the active state (inside primitives) |

### Environments

| Function | Description |
|----------|-------------|
| `mino_env_new(S)` | Create an empty environment |
| `mino_new(S)` | Create an environment with core bindings |
| `mino_env_free(S, env)` | Unregister and free an environment |
| `mino_env_set(S, env, name, val)` | Bind a name |
| `mino_env_get(env, name)` | Look up a name (NULL if unbound) |
| `mino_install_core(S, env)` | Install pure core bindings |
| `mino_install_io(S, env)` | Install I/O bindings |
| `mino_register_fn(S, env, name, fn)` | Bind a C function as a primitive |

### Eval and read

| Function | Description |
|----------|-------------|
| `mino_eval(S, form, env)` | Evaluate one form |
| `mino_eval_string(S, src, env)` | Read and evaluate all forms in a string |
| `mino_load_file(S, path, env)` | Read and evaluate a file |
| `mino_read(S, src, end)` | Read one form from a string |
| `mino_call(S, fn, args, env)` | Call a callable value |
| `mino_pcall(S, fn, args, env, out)` | Protected call (catches errors) |
| `mino_last_error(S)` | Get the last error message |

### Constructors

| Function | Description |
|----------|-------------|
| `mino_nil(S)` | The nil singleton |
| `mino_true(S)` / `mino_false(S)` | Boolean singletons |
| `mino_int(S, n)` | Integer value |
| `mino_float(S, f)` | Float value |
| `mino_string(S, s)` | String (copies `s`) |
| `mino_string_n(S, s, len)` | String with explicit length |
| `mino_symbol(S, s)` | Interned symbol |
| `mino_keyword(S, s)` | Interned keyword |
| `mino_cons(S, car, cdr)` | Cons cell |
| `mino_vector(S, items, len)` | Persistent vector |
| `mino_map(S, keys, vals, len)` | Persistent hash map |
| `mino_set(S, items, len)` | Persistent hash set |
| `mino_prim(S, name, fn)` | Primitive function |
| `mino_handle(S, ptr, tag)` | Opaque host handle |
| `mino_handle_ex(S, ptr, tag, fin)` | Handle with finalizer |
| `mino_atom(S, val)` | Mutable atom |

### Predicates and accessors

| Function | Description |
|----------|-------------|
| `mino_is_nil(v)` | Test for nil |
| `mino_is_truthy(v)` | Truthiness (everything except nil and false) |
| `mino_is_cons(v)` | Test for cons cell |
| `mino_eq(a, b)` | Structural equality |
| `mino_car(v)` / `mino_cdr(v)` | Cons accessors |
| `mino_length(list)` | Length of a list |
| `mino_to_int(v, out)` | Extract integer (returns 1 on success) |
| `mino_to_float(v, out)` | Extract float |
| `mino_to_string(v, out, len)` | Extract string pointer and length |
| `mino_to_bool(v)` | Truthiness as C int |
| `mino_is_handle(v)` | Test for handle |
| `mino_handle_ptr(v)` | Get handle pointer |
| `mino_handle_tag(v)` | Get handle tag |
| `mino_is_atom(v)` | Test for atom |
| `mino_atom_deref(a)` | Read atom value |
| `mino_atom_reset(a, val)` | Set atom value |

### Printer

| Function | Description |
|----------|-------------|
| `mino_print(S, v)` | Print to stdout (no newline) |
| `mino_println(S, v)` | Print to stdout with newline |
| `mino_print_to(S, out, v)` | Print to a FILE* |

### Refs

| Function | Description |
|----------|-------------|
| `mino_ref(S, val)` | Root a value (survives GC) |
| `mino_deref(ref)` | Get the rooted value |
| `mino_unref(S, ref)` | Release the root |

### Modules

| Function | Description |
|----------|-------------|
| `mino_set_resolver(S, fn, ctx)` | Register a module resolver |

### Limits

| Function | Description |
|----------|-------------|
| `mino_set_limit(S, kind, value)` | Set step or heap limit (0 to disable) |

### Cloning

| Function | Description |
|----------|-------------|
| `mino_clone(dst, src, val)` | Deep-copy a value between states |

### Mailbox

| Function | Description |
|----------|-------------|
| `mino_mailbox_new()` | Create a thread-safe message queue |
| `mino_mailbox_send(mb, S, val)` | Serialize and enqueue a value |
| `mino_mailbox_recv(mb, S)` | Dequeue and deserialize (NULL if empty) |
| `mino_mailbox_free(mb)` | Free the mailbox and queued messages |

### Actors

| Function | Description |
|----------|-------------|
| `mino_actor_new()` | Create an actor (state + env + mailbox) |
| `mino_actor_state(a)` | Get the actor's runtime state |
| `mino_actor_env(a)` | Get the actor's environment |
| `mino_actor_mailbox(a)` | Get the actor's mailbox |
| `mino_actor_send(a, S, val)` | Send a value to the actor's mailbox |
| `mino_actor_recv(a)` | Receive next message (NULL if empty) |
| `mino_actor_free(a)` | Free the actor and all its resources |

### REPL

| Function | Description |
|----------|-------------|
| `mino_repl_new(S, env)` | Create a REPL handle |
| `mino_repl_feed(repl, line, out)` | Feed one line of input |
| `mino_repl_free(repl)` | Free the REPL handle |
