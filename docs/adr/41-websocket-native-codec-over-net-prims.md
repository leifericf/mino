# ADR 41: One websocket surface, mino.ws over the net prims, codec native

Date: 2026-09-02

## Context

mino ships one HTTP client (ADR 20: plain maps in and out, a
capability-gated net layer, vendored client TLS) and the server half
as Ring maps over the same prims (ADR 36), which left websocket out of
server v1 while keeping the connection handle first-class on the
request map for the upgrade path. RFC 6455 layers a framed
bidirectional protocol over an HTTP/1.1 Upgrade exchange: the client
sends a random Sec-WebSocket-Key nonce, checks the SHA-1-derived
Sec-WebSocket-Accept echo, then exchanges masked frames carrying
64-bit payload lengths, control frames (ping, pong, close with a
code), and fragmented messages. Wire-format readers of untrusted input
are native in this codebase, five for five (ADRs 23 through 28). The
runtime already has `secure-rand-bytes` under `MINO_CAP_RANDOM`,
`sha1`, and the net and TLS prim quads; capability bit 53 is free
between SIGNAL (52) and TAR (54).

## Decision

Websockets are one namespace, `mino.ws`, in the ADR 20 vocabulary:
`(ws-connect url opts)` returns a plain handle map (the net-listen
precedent, no new opaque type beyond the socket it wraps), and
`ws-send`, `ws-recv`, `ws-close` operate on it. Text and binary frames
both pass; ping/pong and the closing handshake run below the public
API (pong replies are automatic), and close codes surface as data.
Errors are `:mino/kind` data maps (ADR 38). One entry point selects
transport by scheme: `ws://` rides net-connect/net-read/net-write/
net-close, `wss://` rides the tls-* quad. The frame codec (encode and
decode) and the Sec-WebSocket-Accept key computation are native prims
in `src/prim/ws.c`, because they parse untrusted network bytes at a
native edge; the Upgrade exchange reuses `http-encode-request` and
`http-parse-response`, accept verification uses `sha1`, and `mino.ws`
stays a thin shell over these prims. The surface is gated by a new
`MINO_CAP_WEBSOCKET` (bit 53), which joins `MINO_CAP_DEFAULT` in the
same commit as the prims. A later change gives `mino.http.server`
(ADR 36) the server-side upgrade, reusing the same codec and handshake
prims: one vocabulary, one implementation, no second client.

## Consequences

- The client masking key and the Sec-WebSocket-Key nonce come from
  `secure-rand-bytes`, never `rand`; a predictable mask defeats the
  cache-poisoning defense the mask exists for.
- The native decoder enforces RFC 6455 at the trust boundary: bounded
  payload lengths with 64-bit overflow rejected, masked-client and
  unmasked-server enforcement, close-code validation, UTF-8 validation
  on text frames, reserved-bit rejection, and capped fragmented-message
  reassembly, the cap defaulted like the HTTP body cap.
- A new prim file and capability bit widen the C and audit surface;
  the codec owes the property and fuzz treatment the other readers got.
- Pong-below-the-API means `ws-recv` reads through control traffic;
  there is no application-level ping hook in v1.

## Alternatives

- **A pure-mino frame codec.** No new C, and the frame header is
  small and regular. Rejected: it parses untrusted wire bytes with
  payload-scaling risk, exactly the case the ADR 23-28 record sends
  native.
- **A second dedicated websocket client library.** Free to evolve its
  own callback shape. Rejected: one vocabulary is the ADR 20
  constraint; two clients diverge and never converge.
- **Surfacing ping/pong to the caller.** Explicit liveness control.
  Rejected: every consumer would reimplement the automatic pong the
  RFC expects; a heartbeat hook can be added later without breaking
  the handle contract.
