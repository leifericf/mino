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

**Status:** Item 1 (QEMU cross-execution) and Item 2 (reproducible artifacts)
are **shipped**. Item 3 (PGO) was **investigated and deferred** — it sits below
the zig-0.16 ceiling and would need a coupled second LLVM toolchain; see below.
With that, the zig *toolchain* axis is maxed to the ceiling.

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

## 3. PGO release build  — INVESTIGATED, DEFERRED (below the ceiling)

The original plan was `-fprofile-generate` → train → `llvm-profdata merge` →
`-fprofile-use`, guarded by `test-jit-parity`. A hands-on spike shows this is
**not achievable on the pinned zig toolchain**, on three independent counts —
all verified against zig 0.16.0 (clang 21.1):

1. **No profile runtime.** zig ships no `libclang_rt.*` at all, so
   `-fprofile-generate` fails to link (`undefined symbol:
   __llvm_profile_instrument_memop`). The runtime lives in clang-21/compiler-rt,
   outside zig.
2. **No `llvm-profdata`.** zig exposes only `ar`/`objcopy` — raw profiles can't
   be merged without an external, version-matched `llvm-profdata-21`.
3. **Driver doesn't wire the flow.** Even hand-supplying both
   `libclang_rt.profile-x86_64.a` and `llvm-profdata-21` from an external
   LLVM-21 package, the instrumented binary writes **zero** `.profraw` — zig's
   clang driver doesn't register the profile `atexit` writer, because PGO isn't
   part of the toolchain it manages.

Making PGO work would require adopting a **version-coupled second LLVM-21
toolchain** (runtime + profdata, tracking zig's bundled clang major) — which
contradicts the mandate that pinned `zig` is the *only* special maintainer
dependency (`docs/MAINTAINER_TOOLCHAIN.md`). The payoff is also weak: the gain
is unquantified, and mino's actual hot path — the copy-and-patch JIT — is
**uninstrumentable by PGO anyway**, since stencils ship as committed
machine-code bytes, not recompiled C. So PGO is deferred to the ceiling below,
not pursued. (Revisit if zig bundles the profile runtime + a `profdata`
subcommand, at which point it becomes a pure-toolchain win.)

## The honest ceiling

"Maxed" means doing items 1–2 and then stopping where zig 0.16.0 structurally
can't help — not bolting on a coupled second toolchain or faking coverage the
toolchain can't back:

- **PGO** — needs the LLVM profile runtime + `llvm-profdata`, neither shipped by
  zig; see item 3. The JIT is uninstrumentable regardless.
- **ASan** — zig ships no asan runtime; stays on host `cc` (`release-gate`).
- **MSan** — needs an MSan-instrumented libc zig's musl doesn't provide.
- **zig-native libFuzzer** — fuzzer runtime missing in 0.16; fuzzing stays on
  host clang (`mino-bench` `fuzz-build-libfuzzer`).
- **Coverage rendering** — `zig cc` can instrument (`-fcoverage-mapping`) but
  zig exposes no `llvm-cov` / `llvm-profdata` subcommands to render reports.
- **Debugger / profiler** — zig ships none; use lldb/gdb, perf/valgrind.

The pattern is consistent: zig 0.16 is a superb *compiler + cross + linker*, but
ships **none of the LLVM runtime libraries** (asan, msan, fuzzer, profile) nor
the **LLVM tool binaries** (`llvm-profdata`, `llvm-cov`). Anything that needs
those stays on a host LLVM/clang, off the pinned-zig path — by design, not
oversight.
