# ADR 54: Ratio accessors narrow to int when the component fits

Date: 2026-09-05

## Context

`numerator` and `denominator` read the ratio's stored bigint slots. An
earlier fix returned those slots directly so components past 64 bits
stay exact, but every result then carried bigint identity and printed
with the N suffix: `(numerator (/ 2 3))` printed `2N`. Canon's
accessors return the host big-integer type, which prints as plain
digits, and the ground-truth runtime prints `2` for the same call. The
nightly ClojureDocs diff probe flagged the divergence
(numerator:0, expected `2`, got `2N`), a silent print divergence on a
function a user reaches for on day one. mino cannot print a bigint
without the suffix; the suffix is how bigint identity survives a
re-read. But mino already has a narrowing rule for exactly this shape:
ratio construction collapses a denominator-1 result to `:int` when the
value fits a long.

## Decision

The accessors reuse that collapse rule. `numerator` and `denominator`
return `:int` when the component fits a long, and the stored bigint
only past 64 bits. Every realistic component now prints canon-identical
plain digits; oversized components stay exact and fall under the
existing bigint N-suffix print divergence, which is visible and
already the documented family, not a new silent one. This supersedes
the earlier keep-bigint-identity choice, whose rationale assumed the
probe could not observe the suffix; the probe observed it.

## Consequences

`(type (numerator 1/2))` is `:int`, not the canon big-integer type; a
type check ported from canon sees the difference, while every equality
and arithmetic use is unaffected because mino compares integer tiers
by value. Components past 64 bits still return `:bigint` and print
with N where canon prints plain digits, the shared fate of every
oversized integer mino prints.

## Alternatives

**Keep bigint identity on every component.** Type-faithful to canon
and uniform across magnitudes. Rejected: it makes every
numerator/denominator print divergent for the values users actually
hold, the worst-ranked class of divergence, to preserve a type
distinction mino's numeric equality already erases.

**Print bigints without the N suffix.** Would fix the text without
touching the accessors. Rejected: the suffix is load-bearing for
read/print round-trip of bigint identity everywhere else.
