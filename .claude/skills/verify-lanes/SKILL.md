---
name: verify-lanes
description: The verification lane table — which ./mino task lanes run per worktree, per landing wave, and pre-merge, in what order. Invoked by verifier agents and the round runner.
user-invocable: false
---

# verify-lanes

Run lanes in order; report one line per lane; on failure capture the
first error and the repro command, then stop (unless dispatched with
"run all").

## Cheap set — every worktree, before its branch lands

| # | Command | Gate |
|---|---------|------|
| 1 | `./mino task build` | compiles under `-std=c99 -Wall -Wpedantic -Wextra -Werror` |
| 2 | `./mino tests/<owned>_test.clj` (each test file covering the touched module; `./mino task test` when ownership is unclear) | owned tests pass |
| 3 | `./mino task qa-arch` | TU/function size + abort inventory (only when C changed) |

## Landing wave — feature branch, once per integrated wave

| # | Command | Notes |
|---|---------|-------|
| 1 | `./mino task test` | full suite |
| 2 | `./mino task qa-arch` | architecture gates |
| 3 | `./mino task test-jit-parity` | byte-identical stdout, JIT vs lean |
| 4 | `./mino task lint-zig` | third-compiler strict-warning lens (pinned zig) |
| 5 | `./mino task sanitize-zig` | UBSan + TSan suite, auto + eager JIT |
| 6 | `./mino task check-analyze-zig` | static-analyzer findings vs baseline |

Conditional additions:
- public surface / embed API touched → `./mino task test-embed` and
  `./mino task examples`
- JIT or stencil sources touched → `./mino task check-stencils-fresh-all`
  and `./mino task test-jit-host`
- build-lean-relevant changes (JIT gates, `MINO_CPJIT` conditionals)
  → `./mino task build-lean`

## Pre-merge to main / pre-tag

| Command | Notes |
|---------|-------|
| `./mino task release-gate` | composite: stencil/registry/reloc gates + tests + parity, fail-fast |

## Reporting

One line per lane: `PASS <lane>` or
`FAIL <lane>: <first error line> (repro: <command>)`, then
`VERDICT: PASS|FAIL`. Never re-run a failed lane more than once, never
downgrade a failure to acceptable — that judgment belongs upstream.
Sanitizer/zig lanes hard-fail with a clear message when the pinned zig
is absent (`./mino task doctor` explains); report that as
`FAIL <lane>: pinned zig missing`, not as a pass.
