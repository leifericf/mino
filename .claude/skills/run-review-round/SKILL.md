---
name: run-review-round
description: The authoritative step-by-step for one review round — lanes, reviewer fan-out, triage, level-ordered editor waves, integration, verification, state advance. Invoked by the review-round-runner agent.
user-invocable: false
---

# run-review-round

One round = everything from "stable feature branch" to "stable feature
branch with this round's findings fixed and verified". The spine
scripts own the bookkeeping — call them, don't reimplement them.

Inputs from your dispatch: run dir, feature branch, scope (module
shards), round number.

## Steps

1. **Deterministic lanes first** (free findings): run
   `./mino task qa-arch` and `./mino task lint-zig` on the feature
   branch. Convert hard failures into findings EDN yourself (dimension
   `:style` or `:factoring` per the gate, level per the defect) and
   write them to `<run-dir>/findings/lanes-round<N>.edn`.
2. **Reviewer fan-out.** One `reviewer` agent per (dimension × module
   shard) via the Agent tool, in parallel. Dimensions: memory,
   security, conformance, style, factoring, portability — skip
   dimensions that cannot apply (e.g. memory on a pure .clj shard).
   Use gather-module-context once per shard and reuse the brief in
   all six dispatches. Each dispatch names the check-* skill, the
   shard, and the findings path
   `<run-dir>/findings/<dimension>-<shard-slug>.edn`.
3. **Triage.** `./mino tools/triage_findings.clj <run-dir>` → punch
   list. Zero items → step 7 with found-new false.
4. **Editor waves, level order.** For each level present
   (correctness → factoring → style):
   - Group punch-list items by module; dispatch one `editor` per
     module in parallel (each in a worktree branched off the feature
     branch; branch name `fix/<module-slug>-r<N>`).
   - Collect returns. `needs-cross-module` items: hold until the
     module waves of this level finish, then dispatch ONE editor with
     the union of those items on its own branch.
   - Integrate the wave:
     `./mino tools/integrate_fixes.clj <run-dir> --repo . --target <feature-branch> --branches <landed,in,test-first-order>`
     then `./mino tools/merge_proposals.clj <run-dir>`, then COMMIT the
     merged changelog on the feature branch
     (`Changelog: Land round-<N> proposal lines under Unreleased`) —
     merge_proposals edits the working tree only; an uncommitted
     CHANGELOG.md leaks across branch switches.
   - Escalations in `<run-dir>/escalations.edn`: dispatch one fresh
     editor per escalation with both diffs; if it fails again, leave
     it recorded for the maintainer and continue.
   - Barrier: do not start the next level until this wave verified
     (next step) clean.
5. **Verify the wave.** One `verifier` agent on the feature branch:
   cheap set after correctness/factoring waves; the full landing-wave
   set after the last wave of the round (see verify-lanes). A FAIL
   feeds back: triage the failure as a finding, dispatch an editor,
   re-verify — at most twice before recording an escalation and
   stopping the round.
6. **Worktree hygiene.** Remove merged worktrees and branches
   (`git worktree remove`, `git branch -d`); escalated branches stay.
7. **Advance.**
   `./mino tools/run_state.clj advance <run-dir> --found-new <true|false>`
   — true iff triage produced ≥1 item this round (regardless of how
   many were fixed).

## Return

Exactly one line:
`ROUND <n>: <findings> findings, <fixed> fixed, <escalated> escalated, verify <PASS|FAIL>, <continue|dry>`
