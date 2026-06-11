---
name: maintain-toolchain
description: Toolchain maintenance rituals — stencil regeneration, zig pin bumps, cross-builds, qemu execution, reproducibility gate — and the Zig boundary law. Invoked by verifier agents and maintainers' dispatches.
user-invocable: false
---

# maintain-toolchain

The authority is `docs/MAINTAINER_TOOLCHAIN.md`; this skill is the
operational recipe. Two laws frame everything:

1. **Zig is a toolchain, never a source language.** No `.zig` files —
   runtime, tests, or tooling. Tests are C + mino. The embedder path
   is `make` + any C compiler, always.
2. **The pin is the pin.** Maintainer lanes run only the pinned zig
   (`check-zig-version` enforces; `./mino task doctor` diagnoses).
   Never substitute a host clang to make a lane pass — that is an
   infrastructure failure to report, not to work around.

## Rituals

**Stencil regeneration** (after editing `src/eval/bc/stencils/*.c` or
JIT ABI headers):
```
./mino task gen-stencils-all
git diff --stat src/eval/bc/stencils/generated/   # review the bytes moved
./mino task check-stencils-fresh-all              # must be clean after commit
```
Commit regenerated headers together with the source change.

**Zig pin bump** (deliberate, its own commit):
1. Update `zig-version-pin` in `lib/mino/tasks/builtin.clj`, the
   `mlugg/setup-zig` `version:` in `.github/workflows/ci.yml` AND
   `release-build.yml`, and the pin in `docs/MAINTAINER_TOOLCHAIN.md`
   — all in lockstep.
2. `./mino task gen-stencils-all` and commit the regenerated headers
   in the same change; `check-stencils-fresh-all` must pass.
3. Re-check the darwin TLV tripwire notes in MAINTAINER_TOOLCHAIN.md —
   a pin bump can change the Mach-O mis-bind pattern.

**Cross + execution validation:**
```
./mino task cross-build          # Linux amd64/arm64 + Windows amd64 -> dist-cross/
./mino task test-cross-qemu      # run the arm64 artifact (skips politely without binfmt)
./mino task check-binary-reproducible
```

**Static analysis baseline** (only after intentional changes):
```
./mino task check-analyze-zig    # the gate
./mino task gen-analyze-baseline # regenerate; review the diff critically
```
Never regenerate the baseline to silence a finding you haven't read.

## Don't re-spike the ceiling

PGO, ASan-via-zig, MSan, zig libFuzzer, and coverage rendering are
all structurally unavailable on zig 0.16 (no LLVM runtime libs, no
llvm-profdata/llvm-cov) — investigated and documented in
MAINTAINER_TOOLCHAIN.md "The toolchain ceiling". ASan stays on host
`cc`; fuzzing stays on host clang in mino-bench.
