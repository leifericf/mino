# Zig Toolchain Max-Out — Roadmap

mino uses Zig strictly as a **toolchain** (compile / instrument / analyze /
cross / distribute the C), never as a source language. The compiler, cross,
sanitizer, lint, analyzer, and stencil-determinism lanes are already in place
(see `MAINTAINER_TOOLCHAIN.md`). This doc tracks the remaining items that
genuinely extend that axis, in execution order. Each lands as its own task +
CI lane and is documented in `MAINTAINER_TOOLCHAIN.md` when it ships.

The dividing line is unchanged: nothing here touches the embedder path. Every
item is a maintainer-side use of `zig cc` (or a binfmt emulator on a CI host);
`make` + `cc` remains the canonical build, and no item requires Zig of anyone
embedding mino.

## 1. Execute cross artifacts under QEMU  — *correctness, low risk*

**Gap:** `cross-build-validate` builds and **publishes** the
`linux-arm64-musl` artifact, but on the x86_64 runner it only `readelf`-inspects
it (`verify-cross-static`). The binary we ship to ARM users is never executed in
CI — a miscompile, bad relocation, or arch-specific UB trap would ship undetected.

**Mechanism:** register `qemu-user-static` binfmt on the x86_64 runner so the
static AArch64 ELF runs transparently, then reuse the existing
`run-suite-with-test-bin` (which exports `MINO_TEST_BIN`) to run the full suite
against `dist-cross/mino_linux_arm64_musl`. No qemu prefix threading needed —
binfmt makes execution transparent, including the suite's subprocess re-execs.

- Task: `test-cross-qemu` (skips with a note when no aarch64 binfmt handler is
  present; runs natively on an arm64 host).
- CI: a step in `cross-build-validate` (`release-build.yml`), **informational
  first** (`continue-on-error`), promoted to gating after a green streak — the
  same playbook as `darwin-zig-canary` / `jit-host-canary`.

## 2. Whole-binary reproducible build gate  — *supply-chain trust* — SHIPPED

**Gap:** determinism stopped at the committed stencil bytes
(`check-stencils-fresh-all`). The shipped binaries were **not** reproducible:
each published `linux-*-musl` artifact embedded ~470 references to the builder's
absolute paths (zig install dir, cache hash, cwd), so two machines produced
different bytes.

**Root cause (not what the original plan guessed):** the paths are **not** from
`__DATE__`/`__FILE__` or our compile dir — they live in the **DWARF** carried by
zig's bundled `libunwind` / `compiler_rt` objects, which `-ffile-prefix-map`
cannot reach (those objects are compiled by zig, not through our `cc` flags). So
prefix-map / `SOURCE_DATE_EPOCH` / `zig ar` were all no-ops here.

**Fix:** `-Wl,--strip-debug` on the ELF (Linux) cross targets in
`cross-build-one`. It's the only cross-arch, single-host option — host binutils
`strip` can't read a foreign-arch ELF, and `zig objcopy --strip-debug` is
unimplemented in 0.16. lld drops `.debug_*` **and** `.symtab` for this static
link, giving a fully stripped, smaller, reproducible artifact. Symbols aren't
lost for good: the build is now reproducible, so a symbolised twin is recovered
by rebuilding *without* the flag (identical `.text`, addresses align for
`addr2line`). The Windows PE keeps its paths for now (COFF linker rejects the
GNU flag) — a follow-up.

- Task: `check-binary-reproducible` — builds `linux-amd64-musl` twice, asserts
  byte-identical **and** no embedded `$PWD`/`$HOME` (a host-independent
  cross-machine-reproducibility proxy).
- CI: `binary-reproducible`, a single-host pinned-zig job (like
  `stencil-determinism`).

## 3. PGO release build  — *performance, gated on parity*

**Mechanism:** `-fprofile-generate` → run the `mino-bench` corpus →
`llvm-profdata merge` → `-fprofile-use` rebuild. Targets the VM dispatch loop.
Maintainer-release-only; the shipped binary is still ordinary compiled C, so
embedders are unaffected.

- **Guard:** `test-jit-parity` must stay byte-identical; keep profile data off
  the stencil/JIT boundary. Do this last, highest care.
- Task: `build-pgo` (+ wire into the release build once proven).

## The honest ceiling

"Maxed" means doing the above and then stopping where zig 0.16.0 structurally
can't help — not faking coverage the toolchain can't back:

- **ASan** — zig ships no asan runtime; stays on host `cc` (`release-gate`).
- **MSan** — needs an MSan-instrumented libc zig's musl doesn't provide.
- **zig-native libFuzzer** — fuzzer runtime missing in 0.16; fuzzing stays on
  host clang (`mino-bench` `fuzz-build-libfuzzer`).
- **Coverage rendering** — `zig cc` can instrument (`-fcoverage-mapping`) but
  zig exposes no `llvm-cov` / `llvm-profdata` subcommands to render reports.
- **Debugger / profiler** — zig ships none; use lldb/gdb, perf/valgrind.
