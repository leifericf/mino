# ADR 24: CSV reader in C, writer stays Clojure

Date: 2026-08-25

## Status

Accepted

## Context

`clojure.data.csv` (the scripting-essentials port) landed as a pure
mino-Clojure reader: an index-carried single pass over the input with
`nth` for character access and `subs` for field slicing, with golden
vectors captured from the python3 csv module as the RFC 4180 oracle.

The scaling gate (a 1MB / 100k-row document) exposed that the mino
string prims cannot carry a linear reader, on three measured counts:

1. `nth` on strings costs O(offset from the string's start): the
   per-character delimiter scan pays O(position) per character.
2. `subs` is a copying slice, O(remaining length) at varying
   positions, so re-basing the scan window per record is quadratic
   too.
3. There is no codepoint-indexed O(1) character access and no string
   byte view available to scripts (`char-at` is byte-indexed and
   returns one-byte strings, which desynchronizes indices against
   `count`/`subs` codepoint space on multibyte text).

Re-basing per record through O(1)-at-fixed-position `subs` brought a
20k-row document from 128s to 2.4s, but 60k rows still took 37s: the
copy cost dominates. A generous absolute budget would have papered
over a reader that is quadratic in input size; a 10MB file would take
hours.

This is the same wall ADR 23 hit for JSON: the per-step cost is not
algorithm but primitive contract, and every peer implementation
(python's csv, babashka's bundled data.csv, the JVM over readers)
parses CSV in native code.

## Decision

The CSV reader becomes a C primitive, `csv-parse`, in
`src/prim/csv.c`: a single-pass byte-cursor parser that walks UTF-8
bytes directly, matches the separator and quote as (possibly
multibyte) byte sequences, and allocates row vectors and field
strings straight from byte spans. It preserves the exact oracle
semantics the golden vectors pin: lenient about stray quotes
(characters after a closing quote join the field raw, an
unterminated quote takes the remainder), lone \r ends a record,
blank lines yield empty rows, and a separator at end of input closes
an empty trailing field.

`clojure.data.csv/read-csv` delegates to the primitive for both
string input and cursor atoms (a cursor is parsed whole and emptied).
The result is an eager vector of vector rows rather than the
canonical lazy seq, the same trade ADR 23 made for `read-str`.
The writer stays Clojure: it is linear through the transient row
builder and the C-backed `join`, and is not on any gate.

## Consequences

- The 1MB / 100k-row gate passes with the same headroom class as the
  json gates; `tests/csv_perf_test.clj` pins it.
- The pure-mino reader is removed from csv.clj as dead code (the
  ADR 23 precedent), so there is one parser to keep on the oracle.
- `csv-parse` registers in the floor domain list beside `json-parse`
  and follows the prim registration conventions (`k_prims_csv`
  table, classified arity/type errors).
- The mino string primitive contracts (`nth` O(offset), `subs`
  copying) remain open as a scripting-surface limitation; a future
  byte-view or O(1) indexing primitive would let readers like this
  one stay in mino-side Clojure.
