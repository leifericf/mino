# Maintainer Toolchain

There are two distinct audiences, and the toolchain line between them is firm:

- **Building or embedding mino** needs only an **ANSI C compiler** plus
  **`make`**, with no external dependencies. That is the promise embedders rely
  on (`README.md`), and nothing here weakens it. Zig is **never** required for
  this path — not for bootstrap, not for embedding, not for a from-source
  standalone build.
- **Developing mino itself** — cutting a release, regenerating stencils, or
  running the reproducible QA lanes — **requires the pinned `zig cc`**. This is
  a hard requirement for maintainers, not an optional extra. It stays entirely
  off the embedder path above.

Run **`./mino task doctor`** to check your toolchain: it reports the C compiler
(required for everything) and the pinned `zig` (required for the maintainer
workflow), with install guidance for whatever is missing or mismatched.

The maintainer tasks that require the pinned `zig cc`:

| Task | Purpose |
|---|---|
| `gen-stencils-all` / `check-stencils-fresh-all` | regenerate / gate the committed CPJIT stencil byte headers |
| `cross-build` (alias `release-cross`) | cross-compile the Linux + Windows release artifacts |
| `build-all` | build every target (native + cross) from one host |
| `sanitize-zig` | reproducible UBSan + TSan suite run |
| `lint-zig` | curated strict-warning lane (a third compiler lens) |
| `analyze-zig` | advisory clang static-analyzer report |

Each hard-fails with a clear message (`check-zig-version`) when zig is absent or
off the pinned version, rather than silently using a different compiler.

## Pinned Zig version

A few maintainer tasks shell out to [`zig cc`](https://ziglang.org) — a bundled,
pinned Clang with first-class cross-compilation. Zig is **pre-1.0**: a minor
version bump can shift the bundled LLVM and therefore the exact machine code it
emits, so the version is pinned and must move deliberately.

**Pinned version: `0.16.0`**

This single source of truth is mirrored in two places that must stay in
lockstep with the value above:

- `zig-version-pin` in `lib/mino/tasks/builtin.clj` (enforced at runtime by
  `./mino task check-zig-version`).
- `mlugg/setup-zig` `version:` in `.github/workflows/ci.yml`
  (`stencil-determinism` job) and `.github/workflows/release-build.yml`
  (`cross-build-validate` job).

To bump: change all of the above together, run `./mino task gen-stencils-all`,
and commit the regenerated `src/eval/bc/stencils/generated/*.h` in the same
change. `./mino task check-stencils-fresh-all` must be clean afterward.

Installing a specific Zig is just a tarball extract; nothing about it needs to
be on `PATH` for a normal build. Override the compiler with
`STENCIL_CC=clang ./mino task gen-stencils-all` to use a host Clang instead
(this opts out of byte reproducibility — the version pin is skipped).

## Stencil regeneration

The copy-and-patch JIT uses per-target stencils compiled to machine-code byte
blobs and **committed** as headers under
`src/eval/bc/stencils/generated/stencils_<arch>_<os>.h`. Normal builds and
embedders consume those committed bytes and never invoke a stencil compiler.

- `./mino task gen-stencils-all` — regenerate every target's header from one
  host via the pinned `zig cc`. Stencil sources are hermetic (only project
  headers plus Clang-resource `<stdint.h>`/`<stddef.h>`), so all five targets —
  including both macOS ones — cross-compile with no platform SDK.
- `./mino task gen-stencils` / `gen-stencils-x86-64-linux` / … — regenerate a
  single target. Thin wrappers over the same table (`stencil-targets`).
- `./mino task check-stencils-fresh-all` — regenerate all targets and fail if
  `git diff` is non-empty. This is the **determinism gate**, run in CI on one
  pinned-Zig host (`stencil-determinism`). It must never run on the per-OS
  matrix: host compiler-version skew is exactly what forced the original
  determinism job to be removed.

The targets and their `--target=` triples live in the `stencil-targets` table
in `lib/mino/tasks/builtin.clj`. Windows uses the `-gnu` environment (not
`-msvc`): stencil objects are compiled `-c` only and never linked, and x86_64
PE/COFF symbol naming is ABI-agnostic.

## Cross-compiled release binaries

- `./mino task cross-build` (alias `release-cross`) — cross-compile the Linux
  (amd64/arm64) and Windows (amd64) release binaries from one host into
  `dist-cross/`. Each binary mirrors the native `make` config, so a cross-built
  artifact matches the corresponding native-matrix release binary.

Two deliberate boundaries:

- **macOS is native-only.** The full runtime links libSystem and pulls system
  headers; Zig bundles no macOS SDK, so darwin release builds stay native
  (`release-build.yml` matrix). Tier-1 darwin *stencils* are unaffected — they
  need no SDK.
- **Windows drops the `-static` hack.** zig's mingw links the compiler runtime
  statically by default, so the cross-built PE imports only the system
  Universal CRT + `KERNEL32` — no `libgcc_s_seh-1.dll` /
  `libwinpthread-1.dll`, and thus no `STATUS_DLL_NOT_FOUND` on a clean Windows
  install. CI asserts this in the `cross-build-validate` job.

The native release matrix in `release-build.yml` remains the source of the
published artifacts and the portability canary (gcc + Apple Clang + mingw).
`cross-build` runs *alongside* it as validation, never as a replacement.

## Guardrails

- The embedder line is absolute: `zig` is never required to build or embed
  mino, nor for a from-source standalone build. `make` + `cc` stays the
  canonical bootstrap. zig is a *maintainer* requirement only — it gates the
  release / stencil / QA tasks listed above, never the embedder path.
- Adopting `zig cc` only ever *adds* a compiler to CI; it never removes one.
  gcc, Apple Clang, and mingw stay in the matrix as portability canaries, and
  an informational MSVC compile canary (`msvc-compile-canary`) was added.
- Build orchestration stays in the self-hosted Clojure task runner; it shells
  `zig cc` underneath rather than moving anything into `build.zig`.
