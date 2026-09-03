# ADR 52: The format grouping separator is a fixed comma

Date: 2026-09-03

## Context

The `,` flag on the format prim's `%d` inserts a thousands separator.
Canon resolves that separator through the host's locale machinery, so
the same program prints `1,234,567` on an en host and `1.234.567` or
`1 234 567` elsewhere. mino has no locale machinery anywhere: number
printing, string casing, and collation are all locale-free by design,
and mino output is byte-identical across hosts (the same reasoning
that fixed `%n` to `"\n"`). A locale-correct `%,d` would need a new
subsystem serving exactly one flag.

## Decision

`%,d` groups with a hardcoded `,` on every host. The divergence is
documented at the prim and pinned by unit tests: output matches canon
on en-locale hosts and differs from canon's localized output
elsewhere, in exchange for byte-identical mino output everywhere.

## Consequences

- Scripts render the same bytes on every machine, consistent with
  mino's cross-host output guarantee; snapshot tests and golden files
  never flake on the host locale.
- Users on non-en locales get `,` where their locale convention says
  otherwise; code that needs localized digit grouping must format it
  explicitly.
- A conformance probe capturing ground truth on a non-en host would
  flag `%,d`; the divergence is on record here for that triage.

## Alternatives

- **Read the host locale.** Canon's actual behavior and what a
  localized CLI would want. Rejected: it imports a locale subsystem
  for one flag and breaks the byte-identical-output guarantee the
  rest of mino keeps deliberately.
- **Reject the `,` flag.** No divergence, no subsystem. Rejected:
  grouping is the flag's entire value and the common en rendering
  matches canon; removing it helps no one.
