# ADR 20: One HTTP client, plain data in and out, vendored TLS

Date: 2026-08-18

## Context

mino scripts have no HTTP client. The scripting-essentials review (ki-12)
rates it the top gap: every nontrivial script sooner or later talks to a
web service. The ecosystem evidence is unanimous on what to copy and what
to avoid. JVM Clojure standardized on library vocabulary (hato, http-kit)
with Ring-style plain maps. Babashka shipped two clients (bb.curl, then
bb.http-client) and the migration cost is the cautionary tale: two
vocabularies, two behaviors, years of divergence. http-kit taught that
SNI must never be optional (turning it off breaks most of the modern
web). Janet taught the layering: a capability-gated net layer under the
convenience API. The design investigation is recorded in the
http-client research run (constraints, five-ecosystem survey,
synthesized vocabulary).

The maintainer set two binding constraints: exactly one client, complete
with TLS from the first shipped version (no interim client, no curl
shell-out, no plaintext stopgap), and requests are plain data: maps in,
maps out, verb functions build maps.

## Decision

One namespace, `mino.http`, one entry point plus verb sugar:

- `(http/request {...})` takes a plain map and returns a plain map.
  `(http/get url opts)` is `(into {:method :get :uri url} opts)` and
  friends for head/post/put/delete/patch. `:uri` is canonical; `:url`
  is accepted as an alias.
- Options: `:headers :query-params :body :form-params :basic-auth
  :oauth-token :accept :content-type :as (:string | :bytes | :json)
  :throw :timeout :connect-timeout :follow-redirects :max-redirects
  :user-agent :keepalive :insecure?`. Anything else is an error, not
  silently ignored. Nested params (`a[b]=c`) are rejected with an
  explicit error.
- Response: `{:status :body :headers :request-time :request
  :trace-redirects}`. Status 4xx/5xx throws ex-info with the response
  map as ex-data; `:throw false` opts out. Transport failures throw
  ex-info with `{:error {:kind :dns | :connect | :tls | :timeout}}`.
  This diverges deliberately from Babashka's unexceptional-status set
  (a curl-era artifact that omits 308); with redirects followed by
  default, 3xx never surfaces, and a 3xx returned as data when
  redirects are off is data, not an error.
- Sync by default; `:async true` returns a mino future (worker-thread
  promise machinery already in the runtime).

Layering under the namespace, bottom up:

1. Net layer in C: TCP sockets as opaque host-handle values
   (pointer plus type tag plus GC finalizer that closes the fd),
   gated by a new `MINO_CAP_NET` capability bit (next free, 34),
   installed for the CLI, excluded from the sandbox preset. DNS via
   getaddrinfo on the calling thread, like the fs prims.
2. TLS in C: BearSSL vendored under `src/vendor/bearssl/` at a pinned
   release, trimmed to the client-relevant TUs, inlined through the
   existing amalgamation. SNI is always set from the host; there is no
   toggle. Hostname and chain verification against a vendored Mozilla
   root snapshot; `:insecure?` skips verification for local fixtures
   only. Entropy from getentropy (POSIX) or BCryptGenRandom (Windows).
3. HTTP/1.1 in C: request serialization and response parsing as pure
   functions over buffers (untrusted input, property and fuzz tested),
   per-endpoint keep-alive pool with expiry and liveness checks,
   redirect policy (303 always GET, 301/302 rewrite non-GET to GET,
   307/308 preserve method and body, max 10), gzip via vendored miniz
   inflate.
4. `lib/mino/http.clj`: normalization, verb sugar, coercion, throw
   policy, `:as :json` through the bundled JSON reader.

Out of scope for v1, deliberately: streaming bodies, SSE, websockets,
HTTP/2, proxies, unix sockets, multipart, chunked upload.

## Consequences

- The first shipped version is complete: TLS, redirects, gzip,
  keep-alive, async. No migration from an interim client ever.
- Plain-map contract keeps the client scriptable, testable with mock
  executors, and hostable from the store without new value types.
- Vendoring BearSSL adds roughly 300 KB of C to the amalgam and a
  security-update ritual: pin, regenerate, bump, changelog line at
  release. The same ritual covers the CA root snapshot.
- `:as :stream` stays blocked until an io-stream value type exists.
- Windows needs a winsock path (WSAStartup at install, different
  timeout socket options); the MSVC canary gates every vendored TU.

## Alternatives

- Platform TLS dispatch (Secure Transport, schannel, OpenSSL): three
  code paths, three audit surfaces, license exposure on some targets;
  rejected for one small constant-cost client library everywhere.
- mbedTLS: the fallback if the BearSSL spike fails a build gate; no
  official single-TU amalgam makes it second choice.
- curl shell-out or a plaintext stopgap: rejected by constraint.
- Two-phase API (interim then final): rejected by constraint; the
  Babashka history is the counterexample.

## Spike verdict (2026-08-18)

The BearSSL spike passed gates 1 through 3 (host strict C99, zig cross
for linux and windows, single-TU inlining dry run) against release
v0.6 with zero warnings. Gate 4 (real cl.exe) rides the CI canary when
the vendoring lands. File list, sizes, and exact commands:
`~/.agentic-sdk/mino/runs/http-client/spike-report.md`. mbedTLS stays
the documented fallback, unused.

