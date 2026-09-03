# ADR 53: The format date and hash directives stay absent

Date: 2026-09-03

## Context

Canon's formatter has two directive families mino's format prim does
not implement. `%t`/`%T` is a date/time sub-language: some forty
suffix conversions over the host's date types, resolved through the
host's calendar and locale machinery. `%h`/`%H` prints the host
runtime's identity hash of the argument in hex, a value that is not
stable across runs, hosts, or versions even in canon. mino already
has a boundary for host-runtime-state surface: the statics layer
implements what has a real mino-native equivalent and leaves what
depends on host runtime state absent by design, surfacing a clear
resolve error rather than a faked, wrong-shaped result. Hash output
is squarely in that family; the date sub-language is a locale-coupled
subsystem mino keeps out of format the same way `%,d` keeps out
locale lookup (ADR 52).

## Decision

`%t`/`%T` and `%h`/`%H` stay absent. They throw the same classified
`:eval/type` unsupported-directive error as any unknown directive, at
the first use, with nothing passing through as literal text. The
throw shape is pinned by unit tests. Date formatting belongs to the
time library, which formats explicitly and locale-free; hash output
tied to a host runtime's identity hash has no truthful mino
equivalent to offer.

## Consequences

- Ported format strings using `%t` or `%h` fail loudly at first use,
  the cheapest failure mode to diagnose; nothing renders wrong bytes.
- Date rendering goes through the time library's explicit formatters
  instead of a second, locale-coupled date sub-language inside format.
- If a real need for `%t` appears, the honest route is a named subset
  over the time library, recorded in its own ADR, not a silent
  partial implementation.

## Alternatives

- **Implement `%t` over the time library.** Real utility for ported
  code. Rejected: the suffix sub-language is large and locale-coupled;
  a partial or locale-free rendition would diverge silently, the worst
  class, where the absent directive diverges loudly.
- **Implement `%h` over mino's own hash.** Trivial. Rejected: the
  output would look like canon's while never matching it; canon
  itself does not promise a stable value, so no program can portably
  rely on it, and faking it violates works-or-throws.
