---
name: implement-change
description: Build a specified change end to end - plan units, parallel test+implementation writers in worktrees, integration, review rounds until dry, one clean landing on main.
disable-model-invocation: true
---

# implement-change

Input: a spec (argument or conversation). Output: the change landed on
a feature branch, review-clean, ready for main.

Read `references/worktree-model.md` (in this skill) before
orchestrating — it is the topology, ordering, and conflict law.

1. **Initialize.** Pick a slug; create feature branch `change/<slug>`
   off main;
   `./mino tools/run_state.clj init .local/runs/<slug> --kind change --scope <scope> --branch change/<slug>`.
2. **Plan units.** First scan the decision index (`docs/adr/README.md`)
   for records the spec touches: a plan that contradicts an ADR goes
   to the maintainer before any dispatch (supersede the decision or
   change the plan — never silently override). A real choice made
   while planning, where the rejected alternative would have been
   reasonable, gets recorded via the record-decision skill.
   Then split the spec into units, each owning one module
   (gather-module-context per module). Every unit gets: a test unit
   and an implementation unit. State each unit's spec in 3–6 lines.
   Size every unit to be completable by one agent in one sitting —
   if a unit needs a second dispatch to finish, it was two units.
   Surface the plan to the maintainer if the spec is ambiguous —
   ambiguity is cheaper to resolve now.
3. **Write in parallel.** Dispatch `writer` agents (worktrees off the
   feature branch): test units first wave (skill write-tests), then
   implementation units (write-c / write-clj) — implementation
   writers are told their failing tests' paths. Each dispatch prompt
   carries the unit's complete spec plus its module brief
   (gather-module-context): self-contained, so the writer never needs
   this session's context — the prompt IS the plan's hand-off.
4. **Integrate.** `./mino tools/integrate_fixes.clj` with tests
   ordered before their implementations;
   `./mino tools/merge_proposals.clj`. Escalations → one fresh editor
   each, then maintainer.
5. **Verify.** One `verifier`: full landing-wave set (verify-lanes).
   FAIL → dispatch an editor on the failure, re-verify (twice max,
   then stop and report).
6. **Review rounds.** Dispatch `review-round-runner` on the feature
   branch scope; repeat until a round returns `dry`
   (`./mino tools/run_state.clj status` tracks rounds). Keep only the
   one-line round summaries in this session.
7. **Land.** Show the maintainer: branch, commit list, changelog
   lines added, round summaries. Merging to main is the maintainer's
   call — offer, don't assume.

Keep this session lean: dispatch prompts and one-line returns only;
details live in `.local/runs/<slug>/` and the agents' contexts.
