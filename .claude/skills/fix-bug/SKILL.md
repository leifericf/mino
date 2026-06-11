---
name: fix-bug
description: Fix one bug with the house discipline - reproduce, failing test, source-level fix, lanes, one commit. Inline for small bugs; agents only when the fix fans out.
disable-model-invocation: true
---

# fix-bug

Input: a bug report (expression, failing test, crash, or description).
Output: one commit (or one small series) on a `fix/<slug>` branch with
a regression test that failed before and passes after.

The discipline, in order — do not skip steps:

1. **Reproduce.** Reduce to the smallest `./mino -e '<expr>'` (or
   test file / embed snippet) that shows the wrong behavior. For
   GC-suspects: `MINO_GC_STRESS=1`. For memory unsafety:
   `./mino task build-asan` and run the repro under `./mino_asan`.
   No fix lands without a confirmed repro — "can't reproduce" goes
   back to the reporter with what you tried.
2. **Failing test first.** Write the regression test in the right
   surface (write-tests: language → `tests/*_test.clj`, ABI → embed
   tests). Run it; watch it fail for the expected reason. Commit it
   first (`tests: ...`) so history proves fail→pass.
3. **Find the cause, not the symptom.** Check
   `.claude/skills/check-security/references/known-bugs.md` — most
   shipped bugs are recurrences of five patterns. Fix at the source:
   the C or core.clj defect, never a caller-side special case, never
   a test adjustment. Classify per the taxonomy: real gap (fix here),
   upstream platform difference (document at site), infrastructure
   (fix the harness).
4. **Fix smallest-sufficient** (apply-findings discipline). If the
   cause is in another module than expected, follow it — but say so.
5. **Verify.** Cheap set always (build + suite); if the fix touched
   pointer-heavy C, sanitizer lanes; if it touched the JIT path,
   `test-jit-parity` (lane table: verify-lanes).
6. **Commit.** `Category: Imperative summary` (e.g.
   `gc: Pin the data buffer across alloc_val in mino_string_n`).
   Changelog line in the commit body or proposed to the maintainer —
   security fixes always get one.

Solo by default. Fan out only when the bug is actually several bugs
(then: audit-code on the affected module) or the repro hunt needs
parallel hypotheses (dispatch reviewers with explicit hypotheses).
