# ADR 31: Unicode case tables generated from vendored Unicode data

Date: 2026-08-27

## Context

`clojure.string/upper-case`, `lower-case`, and `capitalize` are C
prims that case-convert byte by byte with `toupper`/`tolower`. For any
non-ASCII input this is a silent no-op: each byte of a UTF-8 sequence
passes through unchanged, so a user casing Norwegian or Vietnamese
text gets the original string back with no error. The JVM fns case
Unicode properly, so this is a real divergence, not a stylistic one.

The string prims are otherwise UTF-8 aware (codepoint iteration,
substring, reverse), so the gap is isolated to case mapping.

## Decision

Generate Unicode simple case mapping tables from a vendored copy of
the Unicode character database (`vendor/unicode/UnicodeData.txt` and
`SpecialCasing.txt`), commit the generated `src/prim/unicode_case.h`
like the tzdata blob (ADR 27) and the zip CP437 table, and drive the
casing prims' non-ASCII path through them. The generator
(`tools/gen_unicode_case.py`) reads only the vendored files, so
regeneration (`./mino task gen-unicode-case`) needs no network. The
tables cover 1:1 simple mappings plus the unconditional 1:1 entries
of SpecialCasing; every mapping keeps the codepoint's UTF-8 byte
length, which the generator asserts.

Excluded, deliberately: multi-character mappings (sharp s to SS) and
locale-sensitive mappings (Turkish dotted i), which JVM
`String.toUpperCase` applies through locale defaulting. mino has no
locale machinery and adding it for two casing edges is not worth the
surface; the divergence is recorded in the clojure-census registry.

## Consequences

Casing fns become correct for every 1:1 Unicode mapping; ASCII keeps
a byte fast path so hot ASCII strings pay nothing. The vendored data
adds about 1.9 MB to the repository and a regeneration step when the
Unicode version moves. Strings with special or locale-dependent
casings still diverge from the JVM, visibly and documented rather
than silently.

## Alternatives

Throw on non-ASCII input: honest but regresses working calls and
diverges harder from the JVM. Case-fold via host `setlocale` tricks:
non-reentrant, platform-divergent, and locale state is process
global. Java-identical full casing including multi-char output:
requires length-changing buffers and a locale model mino does not
have.
