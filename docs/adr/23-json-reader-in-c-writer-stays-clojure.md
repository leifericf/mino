# ADR 23: JSON reader in C, writer stays Clojure

Date: 2026-08-22

## Status

Accepted

## Context

`lib/clojure/data/json.clj` opened with a deliberate constraint:
"Pure Clojure, no C primitive." The reader was a per-character
pushback loop over `subs`, `count`, and 1-char string compares, and
the gate that mattered was `clojuredocs-refresh` in the mino-tests
satellite: a 3.7MB `clojuredocs-export.json` corpus that
`json/read-str` never finished parsing (ki-22, blocking ki-4).

Three layers of cost were measured and fixed on the way to the
gate, each necessary but not sufficient:

1. The reader was quadratic on mixed-ASCII content: any non-ASCII
   byte flips `subs` into a codepoint walk from index 0, so the
   per-character loop paid O(position) per character. A regex
   tokenizer (`re-seq` over one pattern) removed the per-character
   walk.
2. The regex layer itself was quadratic in two places:
   `re-find-from` recomputed the text's full codepoint count and
   walked codepoints from 0 on every call (fixed: byte-indexed
   contract), and top-level alternation ran each branch as its own
   unanchored scan, so every token paid the distance to the next
   match of every later-matching branch (fixed: one forward scan,
   branches tried anchored per position).
3. Even linear, the Clojure tokenizer-plus-parser floor was ~50us
   per token (~347K tokens in the corpus): 17 seconds best case for
   the 3.7MB corpus against a 2-second gate, with GC range-index
   compaction (a separate known issue, see Consequences) multiplying
   that under heap pressure.

A pure-Clojure reader cannot close an order-of-magnitude gap; the
per-token cost is interpreter dispatch, not algorithm. Every peer
implementation (JVM Clojure's cheshire/data.json over Jackson,
babashka's built-in) parses JSON in native code.

## Decision

The JSON reader becomes a C primitive, `json-parse`, in
`src/prim/json.c`: a single-pass byte-cursor recursive-descent
parser that allocates values directly (no intermediate token
strings), preserves the Clojure reader's exact error messages and
semantics (string-keyed maps by default, `:key-fn` invoked per key
via `mino_call`, EDN-compatible number tower including bigint
literals, no surrogate-pair joining), and keeps the same
`read-str` API in `clojure.data.json`. The writer stays Clojure:
it is already linear through the `str` builder and is not on any
gate.

The regex engine fixes (1) and (2) stay regardless: they were real
quadratic defects in `re-find-from`'s contract and the alternation
scan strategy, pinned by `tests/regex_perf_test.clj`. The Clojure
tokenizer and token-stream parser are removed from json.clj as
dead code; `tests/json_perf_test.clj` pins the 3.7MB gate.

## Consequences

- The <2s corpus gate is met with two orders of magnitude of
  headroom; `clojuredocs-refresh` unblocks (ki-4).
- json.clj's header comment changes from a "no C primitive" pledge
  to a statement of the split: reader native, writer Clojure.
- The GC range-index cost seen under this workload (minors pay
  O(live headers) compacting one unified index) remains open as
  the known suite-RSS issue; the json workload is no longer a
  victim of it, but the suite still is.
- A new prim means new capability surface: `json-parse` registers
  under the existing data/codec capability grouping and follows
  the prim registration conventions (`k_prims_json` table,
  arg-count and type checks with classified errors).
