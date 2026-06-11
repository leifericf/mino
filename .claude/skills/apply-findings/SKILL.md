---
name: apply-findings
description: Recipe for fixing punch-list findings — smallest sufficient edit, level discipline, module boundary, verify-then-land. Invoked by editor agents in worktrees.
user-invocable: false
---

# apply-findings

Fix the punch-list items assigned to you (one module, one fix level),
in your worktree, one commit per finding or tightly-related group.

Per finding:

1. **Confirm it first.** Read the cited code; reproduce if a repro is
   suggested (`./mino -e`, `MINO_GC_STRESS=1`, ASan build). A finding
   you cannot confirm goes back as `FAILED <id>: not reproducible —
   <why>` rather than a speculative edit.
2. **Smallest sufficient edit.** Fix exactly the defect. At
   `:correctness` level, do not also rename/refactor — that is the
   factoring wave's job and it makes your diff unreviewable. At
   `:factoring`/`:style` levels the finding itself defines the scope.
3. **Stay in bounds.** Your module directory + its `internal.h`. If
   the real fix lives in another module or the public surface, return
   `needs-cross-module <id>` — do not write the tempting local
   workaround; that is exactly the kind of fix the no-workarounds
   rule bans.
4. **Test the fix.** A correctness fix gets a regression test in the
   right surface (see write-tests) unless one already covers it —
   "the reviewer found it" means the suite didn't.
5. **Verify.** `./mino task build`, the module's tests, and the
   relevant cheap lane (qa-arch if sizes moved). Full lanes run at
   the landing wave.
6. **Land.** Commit `Category: Imperative summary`; write your
   proposal EDN (`{:branch ... :changelog [...] :commits [...]}`) —
   never touch CHANGELOG.md or version fields.

If two of your findings conflict (one wants the code the other
removes), fix in punch-list order and note the second as overtaken:
`FAILED <id>: overtaken by <first-id>`.
