# ADR 49: Long division wraps at the minimum over divisor -1

Date: 2026-09-03

## Context

Two's-complement long division has exactly one overflowing input:
the long minimum divided by -1, whose true quotient exceeds the long
maximum by one. Canon long division wraps there, answering the long
minimum again with remainder zero. mino's long tier instead promoted
that corner to bigint and returned the mathematically true
9223372036854775808N, a value no other long division can produce.
Every other long-tier `quot`, `rem`, and `mod` result in mino agrees
with canon byte for byte, and the conformance edge corpus compares
printed output byte for byte. The divergence surfaced in conformance
round B follow-up probing: code ported from canon that divides longs
observed a bigint where it expected a long, silently, only at the
single most extreme input.

## Decision

`quot`, `rem`, and `mod` on two longs wrap at the long minimum over
divisor -1: the quotient is the long minimum, the remainder and
modulus are zero, and all three stay in the long tier. The bigint,
ratio, and bigdec tiers are untouched; arbitrary-precision division
remains truthful, and a caller who wants the exact quotient asks for
it by promoting an operand (`(quot -9223372036854775808N -1)`).

## Consequences

The long tier is closed under division, as canon promises, and the
one silent value divergence in the division surface is gone; the
corpus tuple for this corner asserts the wrap. The cost is a
mathematically wrong answer at one input, the same wrong answer canon
gives, inherent to fixed-width division. Checked `+`, `-`, `*`, `inc`,
`dec`, and `-` continue to throw on long overflow; division differs
because canon itself wraps only here, and matching canon's observable
behavior is the default this project chose (least surprise ranks
silent value divergences worst).

## Alternatives

**Truthful promotion to bigint.** Mathematically honest, consistent
with mino's checked-arithmetic stance of never returning a wrapped
long, and arguably the better answer in isolation. Rejected: it is a
silent value divergence from canon at an input real ported code can
reach, the result type changes only at one corner so callers cannot
anticipate it, and the promotion was unreachable from the documented
overflow story (checked ops throw, they do not promote).

**Throw on the overflowing corner.** Loud, consistent with checked
`+`/`*` overflow behavior. Rejected: canon does not throw here, so
ported code that relies on the wrap (hash mixing, modular reductions)
would break; a throw is a louder divergence, not a smaller one.
