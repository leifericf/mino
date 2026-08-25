# ADR 26: YAML subset, reader first

Date: 2026-08-25

## Status

Accepted

## Context

`mino.yaml` (scripting-essentials p8) is the biggest reader in the
campaign: indentation-scoped block collections, flow collections,
three scalar quoting families plus two block scalar kinds, comments,
and multi-document streams. Full YAML also carries anchors and
aliases, tags, complex keys, and directives, and it is the
interoperability of those pieces that makes the format expensive.

The campaign context (Drive-1) prescribes the order for exactly this
case: land a mino-side Clojure reader first, single pass over byte
indices, no per-character `subs`, gate it against an absolute budget
over a realistic 1 MB config-shaped document (about 2 s standalone),
and if the gate cannot be met, the follow-up is a native prim with
its own record (the ADR 23 JSON, ADR 24 CSV, ADR 25 TOML line), never
a generous budget over slow code. TOML just measured that wall again:
a correct linear mino-side reader at 74 s for 835 KB, 35x over
budget, in interpreter dispatch and per-call regex compiles.

The behavior oracle is the official yaml-test-suite examples for the
in-subset shapes, with python3 pyyaml (installed here, YAML 1.1)
cross-checking only where 1.1 and 1.2 agree. Resolution follows the
YAML 1.2 core schema, so pyyaml's yes/no booleans, octal `017`,
underscored integers, and sexagesimals are divergences the spec wins.

## Decision

### The subset

In for v1:

- Block mappings and block sequences by indentation, including the
  compact forms: a mapping starting on a `- ` entry line, sequences
  under a key at the same indentation as the key, empty entries.
- Flow sequences `[a, b]` and flow mappings `{k: v}` nesting to any
  depth, over multiple lines, with comments between entries, and the
  single-pair mapping entry inside flow sequences (`[ a : b ]`).
- Plain scalars with multi-line folding, single-quoted scalars with
  `''`, and double-quoted scalars with the YAML escape set including
  `\x`, `\u`, `\U`, and the escaped line break.
- Literal `|` and folded `>` block scalars with chomping (`-`, `+`,
  clip default) and explicit indentation indicators, including
  zero-indented top-level blocks.
- Comments, alone or trailing, with the rule that `#` starts a
  comment only after whitespace or at line start.
- `---` document starts (with content allowed on the marker line),
  `...` document ends, and bare documents; `parse-string` reads the
  first document, `parse-string-all` reads the stream.
- Quoted keys, empty keys (`: value`), numeric-looking and
  symbol-laden plain keys, and keys with spaces around the colon.
- Duplicate keys: last one wins (the snakeyaml default clj-yaml
  rides).

Out for v1, each a thrown error with its own reason, never a silent
misparses:

- Anchors `&a` and aliases `*a` (`:unsupported-alias`,
  `:unsupported-anchor`).
- Tags `!!str`, `!foo`, `!<verbatim>` (`:unsupported-tag`).
- Complex keys, explicit `? ` entries and collection keys
  (`:unsupported-complex-key`).
- Directives `%YAML`, `%TAG` (`:unsupported-directive`).

### The surface

`mino.yaml/parse-string` in the clj-yaml shape: `(parse-string s)`
and `(parse-string s opts)` with `{:keywords true|false}`, keyword
keys by default, applied recursively. `(parse-string-all s)` returns
every document in the stream. Scalars resolve through the 1.2 core
schema: `true`/`false` only (no yes/no), decimal, `0x`, and `0o`
integers to signed 64-bit, floats including `1e3`, `.5`, `.inf`,
`.nan`, `null`/`~`/empty to nil, everything else a string. Errors are
thrown ex-info with `:kind :yaml/parse`, a `:reason` keyword, and
`:location {:line :col}` over bytes.

### The plan

The reader lands mino-side first: one cursor over the string's
bytes, indentation tracked as a stack, scalars cut as byte spans.
The golden vectors from the test suite pin the semantics green
before any measurement. The scaling gate then decides: if the 1 MB
config-shaped document cannot hold the absolute budget, the reader
becomes the native `yaml-parse` prim in `src/prim/yaml.c` following
the toml.c structure (same facade, same golden vectors, the Clojure
parser deleted), registered in the floor domain beside `toml-parse`
with no capability bit, and this record carries the measured
outcome.

## Consequences

- The subset is stated as errors, so out-of-subset documents fail
  loudly instead of half-parsing; a later phase can widen it by
  turning reasons off one at a time.
- 1.2 core resolution means `yes` is the string "yes" and `017` is
  seventeen; scripts coming from 1.1 parsers see those as divergences
  by design, pinned in the golden vectors.
- If the native fallback fires, the scaling gate, the nightly
  exclusion entry, and the headroom budget follow the TOML precedent
  (ADR 25) exactly, including the in-suite GC pressure allowance.
