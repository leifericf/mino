# ADR 01: Any ANSI C compiler + make builds mino — C99, no extensions

Date: 2026-06-11 (standing since inception; recorded retroactively)

## Context

mino is an embeddable runtime. Embedders pick it up precisely because
it imposes nothing: no build system beyond `make`, no dependency tree,
no toolchain requirements beyond a C compiler. The C ecosystem's
portability story is real but only at the C99-without-extensions
level; every extension adopted (GNU statement expressions, C11
atomics, `__attribute__`s) shrinks the set of compilers and platforms
that build cleanly, and each exception invites the next.

## Decision

All runtime code targets strict C99 and compiles under
`-std=c99 -Wall -Wpedantic -Wextra -Werror` with no compiler
extensions required for core functionality. `make` + any ANSI C
compiler is the canonical and only required build path. Platform
splits live behind `_WIN32` guards in the files that already carry
them; the gcc / Apple clang / mingw / MSVC-canary / pinned-zig CI
matrix enforces the promise from five compiler perspectives.

## Consequences

- Embedders integrate with a Makefile fragment or the single-file
  amalgamation; nothing to install.
- Some code is more verbose than GNU-C would allow (no typeof, no
  statement expressions, manual cleanup paths).
- Concurrency support routes through small shims (`host_threads.c`,
  clone.c mutexes) instead of C11 `<threads.h>`/atomics.
- Every new compiler that appears tends to just work.

## Alternatives

- **C11 baseline** — gains atomics, `_Static_assert`, anonymous
  unions; loses older toolchains and (at the time of adoption) full
  MSVC C11 coverage was still uneven. Rejected: the wins are
  conveniences, the losses are embedders.
- **GNU extensions where convenient** — pragmatic short-term, but the
  promise is binary: either any compiler builds mino or it doesn't.
  Rejected.
- **C++ core with C API** — richer abstraction toolkit, but doubles
  the toolchain demands on embedders and contradicts the
  single-translation-unit amalgamation story. Rejected.
