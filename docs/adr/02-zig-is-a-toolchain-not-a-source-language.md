# ADR 02: Zig is a toolchain, never a source language

Date: 2026-06-04

## Context

`zig cc` (a bundled, pinned clang with first-class cross-compilation)
solves real maintainer problems: deterministic stencil regeneration
for all JIT targets from one host, Linux+Windows release
cross-builds, hermetic sanitizer lanes, and a third compiler lens for
linting. That utility creates standing temptation to write `.zig`
files — build scripts, test harnesses, eventually runtime code — each
one eroding ADR 01's promise from a different angle.

## Decision

Zig is adopted fully as a *toolchain* and banned fully as a *source
language*. Maintainer tasks shell out to the pinned `zig cc`
(`gen-stencils-all`, `cross-build`, `sanitize-zig`, `lint-zig`,
`analyze-zig`, and friends); no `.zig` files exist in the runtime,
the tests, or the tooling. Tests stay C + mino. Build orchestration
stays in the self-hosted Clojure task runner — nothing moves into
`build.zig`. The single sanctioned future exception is a
`build.zig.zon` package smoke test. Zig is required for maintainers
only; the embedder path (ADR 01) never sees it.

## Consequences

- One pinned binary (`zig-version-pin`, enforced by
  `check-zig-version`) replaces a zoo of cross-toolchains; bumping it
  is a deliberate, lockstep ritual (see `docs/MAINTAINER_TOOLCHAIN.md`).
- The pure-C promise stays intact and mechanically checkable
  (`find . -name '*.zig'` is empty).
- Capabilities zig 0.16 doesn't ship (ASan runtime, MSan, libFuzzer,
  PGO/coverage tooling) stay on host compilers rather than being
  forced through zig (see ADR 03).

## Alternatives

- **Adopt build.zig as the build system** — richer caching and a
  uniform entry point, but makes zig a hard dependency and moves
  build logic out of mino itself (the task runner is dogfood).
  Rejected.
- **Allow .zig for tests/tooling only** — the line would not hold;
  test surface law (language behavior in tests/*_test.clj, ABI in C
  embed tests) already covers the need. Rejected.
- **No zig at all** — keeps maximal purity but forfeits
  deterministic stencils and single-host cross-builds, which were
  recurring release pain. Rejected.
