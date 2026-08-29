# ADR 36: The HTTP server is Ring maps over the net prims

Date: 2026-08-29
Status: accepted

## Context

mino ships the client half of HTTP (ADR 20): a C wire codec, a net
layer under `MINO_CAP_NET`, client TLS, and the `mino.http` namespace
as plain maps. The server half reuses that base. The Clojure
convention for servers is Ring: a handler function from a request map
to a response map, middleware as higher-order functions. mino's own
surface already speaks plain maps everywhere: the client, the store,
the ADR 35 backends. The runtime offers worker threads, chans,
futures, agents, tail recursion, and parked blocking C calls proven
GC-safe, so the Erlang server patterns are expressible without new
runtime machinery.

## Decision

A server is one function over plain maps, in the ADR 20 vocabulary.
`(run-server handler opts)` returns `{:port :stop}`; the handler takes
a request map and returns a response map; unknown `opts` keys are an
error, not silently ignored, mirroring `http/request`.

Scope, settled:

- HTTP/1.1 only, origin-form request targets only. Absolute,
  authority, and asterisk forms are rejected at the codec.
- Keep-alive is in v1 with an idle budget. `Connection: close` is
  honored and HTTP/1.0 defaults to close.
- Chunked request bodies are in v1; the shared parser already rejects
  the smuggling shapes (both framing headers, conflicting lengths,
  obs-fold). Chunked responses are out: responses are Content-Length
  framed or bodiless, because handler bodies are whole values.
- Server TLS is out v1 (`tls.c` is client-only; a BearSSL server
  context is a future campaign). The connection handle rides the
  request map as `:conn`, keeping the transport upgrade path open.
- Websocket is out v1; `:conn` stays first-class for the same reason.
- `Expect: 100-continue` is ignored; the body is read normally.
- No GenServer prim. Stateful handlers compose agent plus chan (the
  documented pattern); a connection loop is a tail-recursive function
  with its state in loop locals, blocked in a parked read between
  iterations.
- The codec is two pure C prims in `src/prim/http.c`:
  `http-parse-request` (the untrusted-input boundary, one parser
  shared with the response path, leftovers exposed for keep-alive) and
  `http-encode-response`. The server owns Content-Length,
  Transfer-Encoding, Connection, and Date on the response wire;
  handler supply of those is rejected.

Concurrency: N acceptor futures race `net-accept` on one shared
listener (the net layer is accept-race-safe by design); each accepted
connection gets a worker future; a `:max-conns` permit chan bounds
in-flight connections, parking acceptors when exhausted so the kernel
backlog absorbs the rest. The single evaluation lock means handlers
interleave at yield points rather than run in parallel. The OTP
mapping: ranch becomes the shared listener with racing acceptors;
process-per-connection becomes a worker future with a try/catch 500
boundary at the connection edge; active-once flow control becomes the
permit chan plus a bounded accept chan; idle timeouts become
read-polling under `:idle-timeout` with a `:request-timeout`
wall-clock deadline against slow readers.

## Consequences

- A throwing handler yields a 500 text/plain response and a close;
  one connection's crash never kills the server.
- Everything is bounded: header bytes and count, body bytes, live
  connections, idle time, request wall-clock time.
- No streaming or chunked responses in v1; a handler returns whole
  bodies. The chunk frame encoder exists if a later phase reverses
  this.
- `:remote-addr` is omitted until `net-accept` widens (accept is
  called with a null address today); widening is a forward task.
- `:acceptors` beyond the host thread grant adds no throughput on a
  single-threaded embedder; the concurrency is for IO interleaving.
- Consistency with ADR 20 holds key for key: plain maps in and out,
  unknown-key errors, the net layer capability-gated, one vocabulary
  shared by client and server.

## Alternatives

- **One evented accept loop (the Node shape).** Single-threaded
  simplicity, but the runtime already gives futures and chans, and
  process-per-connection with isolation is the proven server shape.
- **A GenServer prim.** A generic loop abstraction was weighed against
  agent plus chan composition; the composition covers shared state,
  registries, and presence without new C surface, and loop locals
  cover per-connection state.
- **Streaming responses in v1.** Deferred; whole-value bodies keep the
  handler contract data-only and testable, and no user story demands
  streams yet.
- **Accepting absolute-form targets (proxy shape).** Rejected; a
  scripter's server serves origin-form, and the stricter parse is the
  safer default at the trust boundary.
