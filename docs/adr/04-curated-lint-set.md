# ADR 04: The lint lane gates a curated warning set, not -Weverything

Date: 2026-06-06

## Context

The `lint-zig` lane compiles every mino-authored TU under `-Werror`
with zig's newer clang as a third compiler lens. The question was
which warnings to gate. Maximal sets (`-Weverything`, or everything
plausible) look rigorous but force churn at sites that are
deliberate; each suppressed-in-code warning teaches readers that
warnings are noise.

## Decision

Gate a curated high-signal set beyond the build's
`-Wall -Wextra -Wpedantic`: shadow, strict/missing prototypes,
missing variable declarations, pointer-arith, write-strings, undef,
vla, implicit-fallthrough, comma, unreachable-code, nested-externs,
redundant-decls, format=2. Exclude, after site-by-site audits:

- `-Wcast-qual` (~90 sites, all intentional const-drops: GC marking
  const-reachable objects, mutable-cache members like memoized
  hashes, numeric accumulators — gating would force uintptr_t
  laundering across delicate code for near-zero signal),
- `-Wbad-function-cast` (~30 sites, idiomatic function-return casts),
- `-Wmissing-field-initializers` (`{0}` partial-brace zero-init is
  correct C99 and the prim tables rely on it),
- `-Wswitch-enum` (VM dispatch switches carry intentional `default:`
  arms; `-Wswitch` still fires where it matters),
- `-Wformat-nonliteral`, `-Wdouble-promotion` (low signal for an
  IO/numeric library with computed formats and deliberate float math).

The audited exclusions are documented at the `lint-zig-warnings` def
in `lib/mino/tasks/builtin.clj`; reviewers must not re-flag the
excluded idioms.

## Consequences

- The lane stays at zero findings, so any new warning is signal.
- `-Wmissing-prototypes`/`-Wmissing-variable-declarations` drove the
  over-export audit to zero — static-by-default is now mechanical.
- A periodic re-audit is legitimate when the codebase shifts; the
  def's docstring records audit dates.

## Alternatives

- **-Weverything minus suppressions** — inverts the maintenance
  burden: every clang release adds warnings that must be triaged
  under -Werror immediately. Rejected.
- **Inline pragma suppressions at the ~120 audited sites** — makes
  the exclusions visible in code but smears 120 pragma blocks across
  delicate GC/numeric paths. Rejected.
- **No third lens** — the native build already gates -Wall/-Wextra/
  -Wpedantic, but zig's newer clang catches categories gcc misses;
  the lane is cheap (~10 s). Rejected.
