# ADR 51: The format char directive takes an int codepoint

Date: 2026-09-03

## Context

The format prim follows the canonical formatter surface. Canon's `%c`
accepts a character or a boxed 32-bit integer codepoint and rejects
the 64-bit box; since integer literals there are 64-bit, `(format
"%c" 97)` throws in canon even though the codepoint is valid. mino
has a single integer tier: there is no 32-bit box to accept and no
64-bit box to reject, so the canon rule cannot be reproduced, only
approximated by rejecting all integers or by accepting them all.
mino's `%c` today accepts a char or an integer codepoint in the
Unicode range [0, 0x10FFFF] and throws outside it.

## Decision

`%c` (and `%C`) keep accepting an integer codepoint alongside chars,
bounded to the Unicode scalar range with a classified throw outside
it. The accommodation is documented at the prim and pinned by unit
tests; the divergence is visible, not silent: every accepted call
formats the same glyph canon's 32-bit path would produce, and no call
that canon accepts is rejected.

## Consequences

- `(format "%c" 97)` returns "a" where canon's 64-bit literal path
  throws; code ported from canon that relied on that throw for
  validation must bound-check explicitly.
- The single int tier stays coherent: the same integer works in
  `char`, `%c`, and the codepoint functions without a cast ceremony.
- Out-of-range and negative codepoints still throw `:eval/type`, so
  the directive cannot emit malformed UTF-8.

## Alternatives

- **Reject all integers for `%c`.** Closest to what canon's literal
  path does in practice, and the strictest reading. Rejected: it
  discards the useful half of canon's contract (the 32-bit box that
  does format) and makes the single int tier strictly less capable
  than canon for no gain.
- **Accept any integer and mask into range.** Never throws. Rejected:
  silently mapping invalid codepoints to glyphs is the silent-wrong-
  answer class this project ranks worst.
