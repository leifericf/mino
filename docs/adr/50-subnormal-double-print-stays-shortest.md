# ADR 50: Subnormal double print keeps the shortest round-trip form

Date: 2026-09-03

## Context

mino's double printer emits the shortest decimal string that re-reads
to the same bits, uniformly across the double range. The reference
runtime's legacy printer promises only round-trip, not shortness, and
at the subnormal extremes it emits a longer spelling: the smallest
positive double prints there as "4.9E-324" and its double as
"9.9E-324", where mino prints "5.0E-324" and "1.0E-323". Both
spellings re-read to identical bits; the printed text differs only in
the subnormal range. The conformance edge corpus caught a knock-on:
`rationalize` on a double rationalizes its printed decimal, so
`(rationalize 4.9E-324)` yields a different exact ratio in mino than
in canon, and the corpus held tuple rationalize:12 pending on the
choice. The reference JavaScript-hosted dialect also prints its own
host form here, so canon's dialects already disagree with each other
at these extremes.

## Decision

The printer keeps the true shortest round-trip form everywhere,
including the subnormals. The divergence from the legacy reference
spelling at the subnormal extremes is intentional; the corpus tuple
rationalize:12 moves to the conformance allowlist citing this record,
and the pinned print tests assert the mino spelling.

## Consequences

Printed subnormals differ textually from the legacy reference output,
so byte-for-byte output comparisons against it fail in the subnormal
range, and `rationalize` of such a double yields a different exact
ratio than canon (1/(2*10^323) rather than 49/10^325 at the smallest
positive double). Numeric equality after re-read is unaffected: every
printed double still re-reads to identical bits in both runtimes. The
printer stays one uniform algorithm with no range-special cases.

## Alternatives

**Reimplement the legacy subnormal spelling.** Byte-for-byte parity
with reference output, closing the rationalize divergence too.
Rejected: it grafts a known quirk of the legacy printer onto a printer
whose contract is shortest-round-trip, adds a subnormal-only special
case to otherwise uniform code, and matches one reference dialect
while diverging from the other; the parity gained is textual only,
with no value-level difference after re-read.
