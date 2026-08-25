# ADR 25: TOML reader in C, facade stays Clojure

Date: 2026-08-25

## Status

Accepted

## Context

`mino.toml` (scripting-essentials p7) landed as a pure mino-Clojure
reader first, exactly as the campaign context prescribed: a single
pass over line splits, anchored regexes per construct, all content
extracted through regex captures, byte cursors advanced by
re-find-from offsets, no per-character `subs`. The golden vectors
(python3 tomllib oracle) pinned the semantics and the suite went
green before any measurement, so the algorithm was already linear.

The pre-landing scaling gate (the binding Drive-1 rule) then measured
it: a generated pyproject.toml-shaped document parsed at roughly
70 ms per kilobyte (835 KB in 74 s), linear in size but 35x over the
2 s absolute budget for 1 MB. Profiling the shape showed why: the
reader spends its time in interpreter dispatch and per-call regex
compilation, about 15 pattern compiles plus a few dozen evaluated
forms per line. The regex engine's compile-per-call contract and the
evaluator's per-form cost are the floor, not the algorithm; this is
the third reader to hit the same wall (ADR 23 for JSON at ~50 us per
token, ADR 24 for CSV through the string prims).

The campaign context pre-recorded the answer for exactly this case:
if the mino-side reader cannot meet an absolute budget, the follow-up
is a native prim with its own ADR, never a generous budget over slow
code.

## Decision

The TOML reader becomes a C primitive, `toml-parse`, in
`src/prim/toml.c`: a single-pass byte-cursor recursive-descent parser
that normalizes CRLF up front (parsing in place when no carriage
return is present), decodes escapes into one bounded scratch buffer
per string, allocates maps, vectors, strings, and numbers directly
from byte spans, and threads table updates through the persistent
map/vector constructors. Recursion is depth-capped like json.c.

The parser keeps the exact semantics the golden vectors pin from
tomllib: tables, arrays of tables with last-element navigation,
dotted keys, quoted keys with escapes, inline tables (immutable,
newline-restricted), the four string kinds with the first-terminator
plus two-quote fold and the line-ending backslash trim, radix and
underscore integer forms checked to signed 64-bit (a recorded
divergence: tomllib accepts wider literals), floats including
inf/nan, and RFC 3339 values kept as raw source strings.

Bookkeeping (declared tables, arrays of tables, dotted-created
tables, inline-table paths) is one persistent map from path vector
to flag bits, mirroring the Clojure reader's four sets.

`mino.toml/parse-string` stays the public API and stays Clojure:
argument and `{:parse-values f}` validation, the `:parse-values`
leaf hook invoked from C via `mino_call` (the json `:key-fn`
precedent), and the ex-info error shape. The C prim returns either
the parsed map or an error descriptor vector; the facade throws the
ex-info, so the error contract lives in one place. Error locations
are byte-based line/column pairs.

The pure-mino reader is removed from toml.clj as dead code (the ADR
23/24 precedent), so there is one parser to keep on the oracle.

## Consequences

- The 1 MB gate passes with native-reader headroom;
  `tests/toml_perf_test.clj` pins it and joins the nightly
  MINO_TEST_EXCLUDE list like the other perf gates.
- `toml-parse` registers in the floor domain list beside
  `json-parse` and `csv-parse`; the mino.toml facade rides the
  bundled-lib machinery in the floor like mino.env and mino.log, so
  no capability bit changes.
- The mino string/regex primitive contracts remain the known
  scripting-surface limitation; a Clojure-side reader of this shape
  stays viable for small documents if ever needed, but readers with
  throughput ambitions start native now.
