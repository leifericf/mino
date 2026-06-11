# ADR 03: Defer PGO until zig ships the profile runtime and tooling

Date: 2026-06-10

## Context

Profile-guided optimization looked like the next step on the zig
toolchain axis: `-fprofile-generate` → train on the bench corpus →
`llvm-profdata merge` → `-fprofile-use`, gated by `test-jit-parity`.
A hands-on spike against the pinned zig 0.16.0 (clang 21.1) tested
the assumption.

## Decision

PGO is deferred, not pursued. Three independently verified blockers:
zig ships no profile runtime (`-fprofile-generate` fails to link with
undefined `__llvm_profile_*` symbols), no `llvm-profdata` to merge
raw profiles, and zig's clang driver never registers the profile
`atexit` writer — even with both supplied from an external LLVM-21,
the instrumented binary writes zero `.profraw`. Working around all
three means adopting a version-coupled second LLVM toolchain,
contradicting the single-pin mandate (ADR 02). Revisit only if zig
bundles the profile runtime plus a `profdata` subcommand.

## Consequences

- No PGO build lane; release binaries are plain `-O2`.
- The pinned zig remains the only special maintainer dependency.
- The expected payoff was weak anyway: mino's hot path is the
  copy-and-patch JIT, whose emitted code ships as committed
  machine-code bytes PGO cannot instrument.
- The same ceiling inventory (no ASan/MSan/libFuzzer/coverage in
  zig) is recorded in `docs/MAINTAINER_TOOLCHAIN.md` so nobody
  re-spikes it.

## Alternatives

- **Second LLVM toolchain (runtime + profdata) tracking zig's clang
  major** — works mechanically, but every zig bump now drags an
  external package in lockstep; one pin becomes two. Rejected.
- **PGO via host gcc/clang on the native matrix only** — splits the
  release artifact story (some binaries PGO'd, some not) for an
  unquantified gain on a JIT-dominated profile. Rejected.
- **Wait** — costs nothing; the blockers are upstream and visible.
  Chosen.
