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

Each session owns its own `mino_state_t` and `mino_env_t`. Sessions share
nothing: each has independent GC, intern tables, module cache, and bindings.
Evaluating or defining names in one session does not affect another. This
isolation model enables concurrent eval across sessions (one thread per
session).


## Session lifecycle

### Creation

When a client sends a `clone` request (or the first `eval` without an
explicit session), the server creates a new session with its own runtime:

```c
mino_state_t *S   = mino_state_new();
mino_env_t   *env = mino_new(S);         /* core + I/O included    */
```

Each session gets a unique string ID that the client uses in subsequent
requests.

### Destruction

When a client sends a `close` request, the server frees the session's
environment and state:

```c
mino_env_free(S, env);
mino_state_free(S);
```


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

Implementation: creates a new `mino_state_t` and `mino_env_t` with
core + I/O bindings. The new session is independent of all other sessions.

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

Each session has its own state, so `mino_interrupt` targets the specific
session being interrupted.

### `stdin`

Feed input to a running evaluation that reads from stdin.

This operation is not yet supported. It will require a host-side input
buffer wired into the state.


## Threading model

Each session owns its own `mino_state_t`, so sessions are fully isolated.
The current server is single-threaded and serializes eval across sessions.
However, the one-state-per-session architecture enables a future threading
model where each session evaluates on its own thread without locks.

`mino_interrupt` is safe to call from any thread and targets a specific
session's state.


## Output capture

The server installs custom `println` and `prn` primitives that write to a
per-eval buffer instead of stdout. After eval completes, the buffer contents
are sent as `out` messages, followed by a `value` (on success) or `err`
(on failure) response.

This is implemented by registering replacement primitives at session
creation time:

```c
mino_register_fn(S, env, "println", capture_println);
mino_register_fn(S, env, "prn",     capture_prn);
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
