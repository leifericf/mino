# ADR 64: The amalgamation is the C++ embedding boundary; internal test APIs stay private

Date: 2026-09-09

## Context

Migrating mino-examples onto the amalgamation (ADR 62) surfaced three
couplings to mino's private tree that the C boundary, `dist/mino.h`, did
not cover. The C++ examples included `src/mino.hpp`, the RAII wrapper,
which the `amalgamate` task did not emit at all: a C++ embedder consuming
`dist/` had no wrapper. `fault_inject_test.c` used
`mino_set_fail_alloc_at` and `mino_set_fail_raw_at` from the private
`mino_internal.h` companion header. `regex_thread_test.c` drove the
internal tiny-regex C engine (`re_compile` / `re_matchp` / `re_free` from
`regex/re.h`), the implementation detail behind the public `re-find`
language surface. The question was whether to widen the public surface to
cover these, or treat the two tests as misfiled.

## Decision

The amalgamation is the C++ embedding boundary as well as the C one:
`amalgamate` now also emits `dist/mino.hpp` (the license banner plus the
wrapper, which includes `mino.h`), so a C++ embedder consumes
`dist/mino.{c,h,hpp}` and nothing else. The two test-only internal
surfaces stay private, and an embedding-examples repo builds against the
public API only. The OOM fault-injection probe moved to mino-tests'
whitebox C harness, which legitimately links mino internals; the regex
thread-safety example was rewritten against the public API, as N isolated
`mino_state` threads each evaluating `re-find`, proving the same property
through the supported concurrency model with zero internal headers.

## Consequences

C++ embedders get the wrapper through the amalgam, pinned by an
examples-amalgam C++ compile the way the C boundary is pinned. The public
C and C++ surface stays minimal: allocation-failure injection and the
byte-level regex engine remain implementation details, free to change
without breaking an embedder. Whitebox tests that need internals live in
mino-tests, where that coupling is intended; the examples repo
demonstrates only the public contract, which is also a better example.
The amalgamate task now emits three files plus the license and README.

## Alternatives

Promote the fault-injection hooks and the tiny-regex C API to the public
`mino.h`. Rejected: it publishes and freezes test-only internals to
satisfy two misfiled tests, permanently enlarging the surface for no
embedder need.

Keep the two examples linking `mino/src` as documented partial-link
exceptions. Rejected: it leaves two private-tree couplings that a future
reorg can still break, the exact class this cycle removes.

Ship a separate C++ distribution or a `mino.hpp`-only side channel.
Rejected: mino already chose the single-file amalgam as its embedding
boundary; the wrapper simply needed to travel with it.
