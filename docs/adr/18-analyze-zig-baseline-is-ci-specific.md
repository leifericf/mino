# ADR 18: The analyze-zig baseline is CI-specific (platform-conditional analyzer output)

Date: 2026-08-07

## Context

The `check-analyze-zig` gate (ci-nightly) runs clang's static analyzer
through the pinned `zig cc` (Zig 0.16.0) over every mino-authored
translation unit and fails the job on any finding NOT listed in the
committed baseline at `tools/analyze_baseline.txt`. Because the Zig
version is pinned, it is tempting to assume the analyzer's output is
deterministic on every host.

It is not. The analyzer is zig's bundled clang, and clang's static
analyzer has platform-conditional modeling: the same source produces a
different finding set, and even different checker names for the same
site, on macOS arm64 versus Linux x86_64 at an identical Zig 0.16.0.
Observed while triaging the 2026-08 nightly breakage: the `execvp` site
in `src/prim/proc.c` surfaces as `[core.NonNullParamChecker]` on the
Linux runner and as `[unix.StdCLibraryFunctions]` on a macOS host, and
several other findings appear or disappear between the two.

The practical consequence is that a baseline regenerated on a
developer's Mac does not match what the Linux CI runner emits, so the
gate fails on CI even when the source is unchanged. A local
`check-analyze-zig` run on macOS is therefore not a reliable pre-push
signal, and a Mac-driven `gen-analyze-baseline` produces a baseline that
is wrong for CI.

## Decision

The committed baseline is defined as **the Linux CI output**. It is
regenerated with `./mino task gen-analyze-baseline` on an ubuntu-24.04
runner (the CI environment, or an equivalent Linux host), never on a
developer's Mac.

`check-analyze-zig` is a **CI-only gate**. Local runs on macOS are
advisory; divergences between a local run and the baseline are expected
and are not, on their own, actionable. A contributor who needs to
regenerate the baseline after an intentional change does it on CI (a
`workflow_dispatch` run, or the next nightly) rather than editing the
file from a Mac.

When a change intentionally alters analyzer-visible behavior (a
`noreturn` annotation, a cleanup-label restructure, an `assert`/`abort`
path, a freed-before-throw site), regenerating the baseline is part of
that change, done on CI. The 2026-06-30 `noreturn` annotation on
`gc_oom_throw` (commit `a1298391`) is the negative example: it landed
hours after the last baseline regen and was never followed by another,
so the gate failed every night for over a month on unchanged code.

## Consequences

- A Mac contributor running `check-analyze-zig` locally before push will
  see "new findings" that are mac-analyzer artifacts, not regressions.
  They must not edit `tools/analyze_baseline.txt` from a Mac; the file
  is CI-defined.
- Baseline regeneration is a Linux-CI chore attached to
  analyzer-visible changes. It is cheap (`gen-analyze-baseline` is one
  command) but easy to forget, which is why the nightly skip-guard and
  the embed-test-in-push-CI changes accompany this record.
- The gate retains its value on CI: it surfaced the platform-stable new
  findings (`eval.c`, `proc.c`, `image.c`) that were genuine
  analyzer-visible effects of the June 30 image-serializer and CalVer
  work, exactly the kind of no-regression signal it exists for.
- The triage comments inside `tools/analyze_baseline.txt` remain the
  per-finding record of why each accepted finding is a false positive;
  this ADR records only where the baseline is defined, not its contents.

## Alternatives

- **Pin the analyzer to remove platform variance.** Not possible: the
  analyzer is zig's bundled clang, and the platform-conditional modeling
  is upstream clang behavior the pin cannot disable. Zig is already
  pinned at 0.16.0; the variance persists across platforms at that pin.
- **Run the analyzer in a Linux container locally.** Technically
  possible (Docker), but it adds a heavy dev dependency that
  contradicts the `make` + `cc` bootstrap promise (ADR 01) and the
  container-free macOS dev experience. Rejected: the CI-only definition
  keeps the local path clean at the cost of a CI regen step.
- **Demote the gate to advisory-only.** Rejected: the gate's value is
  failing on genuinely new findings, which it did during this breakage.
  Making it advisory loses the no-regression signal and leaves only the
  noisy raw `analyze-zig` report.
