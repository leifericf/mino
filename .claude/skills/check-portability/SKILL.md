---
name: check-portability
description: Review recipe for the portability dimension — strict C99, platform splits, endianness/width assumptions, libc variance. Invoked by reviewer agents.
user-invocable: false
---

# check-portability

Review the assigned C shard for anything that breaks the promise:
**any ANSI C compiler + make builds mino** on Linux/macOS/Windows,
gcc/clang/mingw (MSVC compile canary), x86_64/arm64.

Look for:

1. **C99 violations.** GNU/C11isms that one matrix compiler will
   reject: VLAs (gated by -Wvla), statement expressions, anonymous
   structs/unions, `typeof`, non-portable pragmas outside the two
   audited sites (`gc/driver.c`, `prim/install.c`).
2. **Platform splits.** POSIX calls without a `_WIN32` alternative
   (`lstat` needed one); Windows paths (drive letters, `\\`) in path
   logic; assumptions that `fork`/signals/pthreads exist (threading
   goes through `host_threads.c`'s shims).
3. **Width/endianness assumptions.** `long` assumed 64-bit (LLP64
   Windows!); pointer↔int casts not via `intptr_t`/`uintptr_t`;
   byte-order-dependent serialization (clone buffers, bits syntax,
   stencil extraction); `size_t` vs `int` mixing in loop bounds.
4. **libc variance.** Behavior that differs across glibc/musl/msvcrt:
   `strtod`/`strtol` endptr edge cases (musl bit us once — commit
   3a6aaee), locale sensitivity, `%zu` availability is fine (C99) but
   `%n` and friends are not used.
5. **JIT boundary.** Code that assumes the JIT exists — everything
   must build and pass with `MINO_CPJIT` disabled (`build-lean` is a
   release gate).

Severity: breaks-a-matrix-target is `:high`; silently-wrong-on-one-
platform is `:high`; theoretical-but-gated is `:low`. Level is
`:correctness`.
