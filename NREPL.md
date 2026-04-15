# nREPL protocol for mino

This document defines how a mino nREPL server maps standard nREPL operations
to the mino C API. It is the reference for the mino-nrepl implementation.


## Concepts

| nREPL concept | mino equivalent |
|---------------|-----------------|
| Runtime (VM) | `mino_state_t` |
| Session | `mino_env_t` within a state |
| Eval | `mino_eval_string(S, code, env)` |
| Clone | `mino_env_clone(S, env)` |
| Interrupt | `mino_interrupt(S)` |
| Error | `mino_last_error(S)` |

A single `mino_state_t` serves all sessions. Each session is an independent
environment created with `mino_env_clone` from a base environment. Sessions
share the state's GC, intern tables, and module cache, but have independent
bindings. Evaluating or defining names in one session does not affect another.


## Session lifecycle

### Creation

The server creates a base environment at startup:

```c
mino_state_t *S = mino_state_new();
mino_env_t *base = mino_new(S);
mino_install_io(S, base);
```

When a client sends a `clone` request (or the first `eval` without an
explicit session), the server creates a new session:

```c
mino_env_t *session = mino_env_clone(S, base);
```

Each session gets a unique string ID that the client uses in subsequent
requests.

### Destruction

When a client sends a `close` request, the server frees the session
environment:

```c
mino_env_free(S, session);
```

The base environment and state remain alive until the server shuts down.


## Operations

### `describe`

Returns server capabilities. The response includes:

- `ops`: list of supported operations
- `versions`: `{"mino": "x.y.z"}`

No state interaction required.

### `eval`

Evaluate code in a session.

Request fields:
- `code` (required): source string to evaluate
- `session` (optional): session ID; creates one if absent

Implementation:

```c
mino_val_t *result = mino_eval_string(S, code, session_env);
if (result == NULL) {
    /* send err response with mino_last_error(S) */
} else {
    /* print result to string, send value response */
}
```

Output capture: the server redirects `println` and `prn` output to a buffer
and sends it as `out` messages before the final `value` or `err` response.

### `clone`

Clone an existing session or the base environment.

Request fields:
- `session` (optional): source session to clone; defaults to base

Implementation:

```c
mino_env_t *src = session_id ? lookup(session_id) : base;
mino_env_t *clone = mino_env_clone(S, src);
/* assign new session ID, return it */
```

### `close`

Close a session and free its resources.

Request fields:
- `session` (required): session ID to close

Implementation:

```c
mino_env_t *env = lookup(session_id);
mino_env_free(S, env);
/* remove from session table */
```

### `interrupt`

Interrupt a running evaluation.

Request fields:
- `session` (required): session being interrupted

Implementation:

```c
mino_interrupt(S);
```

The interrupt flag is checked on each eval step. The running eval returns
NULL with `mino_last_error` reporting "interrupted". The flag is cleared
at the start of the next `mino_eval` or `mino_eval_string` call.

Since all sessions share one state, an interrupt stops whatever is currently
evaluating in that state. The server must ensure that only one eval runs at
a time per state.

### `stdin`

Feed input to a running evaluation that reads from stdin.

This operation is not yet supported. It will require a host-side input
buffer wired into the state.


## Threading model

A `mino_state_t` is single-threaded: only one eval may run at a time. The
server serializes eval requests per state. The one exception is
`mino_interrupt`, which is safe to call from any thread.

For nREPL servers that need concurrent evaluation across sessions, the
recommended approach is one `mino_state_t` per thread, with sessions
partitioned across states. This preserves isolation without locks.


## Output capture

The server installs custom `println` and `prn` primitives that write to a
per-eval buffer instead of stdout. After eval completes, the buffer contents
are sent as `out` messages, followed by a `value` (on success) or `err`
(on failure) response.

This is implemented by registering replacement primitives before eval:

```c
mino_register_fn(S, session_env, "println", nrepl_println);
mino_register_fn(S, session_env, "prn", nrepl_prn);
```


## Completions

The server can implement code completion by iterating the session
environment's bindings. The `apropos` primitive already provides prefix
matching from mino code:

```
(apropos "str")  ;=> ("str" "string?" "starts-with?" ...)
```

A native implementation can walk the env binding array directly for
better control over the response format.


## Error reporting

Eval errors are reported through `mino_last_error(S)`, which returns a
string including the error message and a stack trace when available. The
server sends this as the `err` field in the response.

For structured error data (file, line, column), the server can parse
the error string or use `mino_pcall` for finer control.
