---
name: write-tests
description: Recipe for writing mino tests — surface selection (language vs ABI), the suite's shared-namespace rules, spec-first tests that land before implementation. Invoked by writer agents.
user-invocable: false
---

# write-tests

Write tests for mino. Pick the surface first — this is law:

- **Language behavior** → `tests/<area>_test.clj`. Flat file:
  `(require "tests/test")`, `deftest`/`is`, `(run-tests-and-exit)` at
  the bottom, registered in `tests/run.clj`.
- **C embedding ABI** → C embed tests (`tests/*.c` driven by tasks,
  `examples/embed_*.c` built by `./mino task examples`).
- **Never `.zig` tests**, ever.

Rules of the suite:

1. **One shared namespace.** Top-level `def`s/`defn-`s need a
   file-unique prefix (`gc-tmp-dir`, not `tmp-dir`) or they clobber
   another file's fixtures when the whole suite runs.
2. **Self-cleaning fixtures.** Scratch state under a test-owned /tmp
   path, recreated at the top of each deftest (`rm-rf` first) — tests
   must pass in any order and when rerun.
3. **Spec-first.** When the implementation doesn't exist yet, write
   the test against the intended behavior (JVM Clojure's, or the
   design's), land it first, and let it fail — the integrate order
   proves fail→pass. Mark nothing as skipped.
4. **Behavior, not implementation.** Assert observable results, error
   kinds (`:type/...`, `:arity/...`), and printed forms — not
   internals that factoring is allowed to change.
   Corollary (core-and-shell, see the style references): test the
   decision functions directly and never mock; effects run against
   real scratch state (a /tmp dir, a scratch git repo, a fresh
   mino_state). A shell whose branches feel untestable is a factoring
   finding — move the branch into the core, don't build a mock.
5. **Edge cases are the point.** nil, empty, single, boundary sizes
   (the 64-slot gc_save stack, 32-way trie boundaries at 32/33,
   chunk boundaries), unicode in strings, negative/overflow numerics.
6. **GC-sensitive tests** get a `MINO_GC_STRESS=1` note in a comment
   and should pass under it (the suite runs stress lanes in CI).

No workarounds, no exceptions: a test that can't pass reveals either
a real gap (file it, fix the source), an upstream platform difference
(document at site), or harness debt (fix the harness). Skip-lists and
weakened assertions are never the fix.
