# mino.http

One HTTP client, plain maps in, plain maps out. Design contract:
`docs/adr/20-one-http-client-plain-data-vendored-tls.md`. The full
option reference is the docstring:

```
(require '[mino.http :as http] '[clojure.repl :refer [doc]])
(doc http/request)
```

The namespace needs the net capability (`MINO_CAP_NET`). The CLI
binary installs it; the sandbox preset (`MINO_CAP_DEFAULT`) excludes
it, so embedders pass the bit explicitly.

## Requests

`(http/request m)` takes one plain map. Verb sugar builds the map:
`(http/get uri opts?)` and friends `head`, `post`, `put`, `delete`,
`patch`, each `(into {:method :verb :uri uri} opts)`. `:uri` is
canonical; `:url` is accepted as an alias; supplying both is an error.
Unknown keys are an error naming them.

| Key | Default | Meaning |
|-----|---------|---------|
| `:method` | `:get` | keyword or string |
| `:uri` / `:url` | required | target URL (`:url` is an alias) |
| `:headers` | `{}` | string or keyword names, string values; user headers win over layer defaults; Host, Content-Length, Transfer-Encoding are owned by the layer |
| `:query-params` | none | map; string or integer values, vector values repeat the key, nested maps are rejected |
| `:body` | none | string or bytes |
| `:form-params` | none | map, urlencoded into the body (exclusive with `:body`) |
| `:basic-auth` | none | `[user pass]` or `"user:pass"` |
| `:oauth-token` | none | Bearer token (exclusive with `:basic-auth`) |
| `:accept` | none | media-type keyword or string |
| `:content-type` | none | media-type keyword or string; names the body type, sent even on bodyless requests |
| `:as` | `:string` | `:string`, `:bytes`, or `:json` |
| `:throw` | `true` | 4xx/5xx throw ex-info with the response map as ex-data |
| `:timeout` | `30000` | read timeout, ms |
| `:connect-timeout` | `10000` | connect timeout, ms |
| `:follow-redirects` | `true` | follow 3xx Location hops |
| `:max-redirects` | `10` | hop budget; the 3xx past it is returned as data |
| `:user-agent` | `"mino/<version>"` | |
| `:keepalive` | `120000` | pool TTL, ms; 0 or less sends Connection: close |
| `:insecure?` | `false` | skip TLS verification, local fixtures only |
| `:decompress-body?` | `true` | send accept-encoding gzip,deflate and decode |
| `:async` | `false` | `true` returns a mino future |

## Responses

`{:status :body :headers :request-time :request :trace-redirects}`.
`:request` is the request map as passed, `:uri` canonicalized (default
ports dropped). `:trace-redirects` is the vector of absolute hop URLs.

4xx and 5xx statuses throw ex-info whose ex-data is the full response
map (message `"HTTP <status>"`); `:throw false` returns them as data,
with a failed body coercion falling back to raw bytes so the status
stays visible. Transport failures throw ex-info with ex-data
`{:error {:kind ...}}` where kind is one of `:dns`, `:connect`,
`:tls`, `:timeout`, `:http`.

`:async true` wraps the run in a mino future; timeouts and throws fire
inside deref.

`:as :json` decodes the body through clojure.data.json with keyword
keys and needs the json capability installed.

## Redirects

303 switches to GET; 301 and 302 rewrite non-GET/HEAD to GET; 307 and
308 preserve method and body. A redirect from https down to http stops
the chain. Authorization and Cookie headers are stripped when the
target changes host. A hard cap of 32 hops backs `:max-redirects`.

## Keep-alive and gzip

Connections pool per scheme, host, and port with expiry; a pooled
socket is liveness-polled before reuse, and any mid-request error
closes it instead of returning it to the pool. gzip, x-gzip, and
deflate bodies decode transparently while `:decompress-body?` is
true; an unknown content-encoding returns the body undecoded.

## Security

SNI is always sent from the host; there is no toggle. Chain and
hostname verification run by default against a vendored Mozilla root
snapshot. `:insecure?` skips verification for local fixtures only.

KNOWN LIMITATION: an https URL with an IP-literal host fails hostname
verification. BearSSL v0.6 matches dNSName SANs only, never IP
address SANs. Use DNS names.

## Scope

HTTP/1.1 only. No streaming bodies, SSE, websockets, or proxies in
v1.
