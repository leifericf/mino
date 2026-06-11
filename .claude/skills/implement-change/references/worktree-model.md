# Worktree collaboration model

How parallel agents share one repo without trampling each other.
Used by `implement-change`, `audit-code`, and `run-review-round`.
The spine scripts (`tools/run_state.clj`, `tools/triage_findings.clj`,
`tools/integrate_fixes.clj`, `tools/merge_proposals.clj`) own the
bookkeeping; this file is the topology and the rules.

## Topology

- The **feature branch** is the meeting point. It is created once per
  run (`audit/<scope>`, `change/<slug>`, `fix/<slug>`) off `main`.
- Every **mutating agent** (editor, writer) works in its own git
  worktree on its own branch forked from the feature branch. mino's
  in-tree `*.o` build artifacts make worktrees naturally isolated —
  each worktree builds independently.
- `tools/integrate_fixes.clj` lands the branches on the feature branch
  in dependency order, one `--no-ff` merge per unit.
- Expensive lanes run **once per landing wave** on the feature branch,
  not per worktree. Per-worktree verification is the cheap set only
  (build + the tests covering the touched module).
- `main` receives one fast-forward (or PR) at the end. Never force-push.

## Ordering rules

- **Tests land before implementation**, so integrate-time history
  proves fail→pass.
- **Fix levels are barriers**: correctness/security branches land and
  verify before factoring/structure begins; style/mechanics last.
  Parallelism is *within* a level across disjoint modules, never
  across levels.
- Reviewers get no worktrees — they read the feature branch between
  waves, when the tree is stable.

## Ownership boundaries

- A unit of work owns **one module directory plus its own
  `internal.h`** (boundaries per `docs/INTERNAL_MODULE_MAP.md`).
- Touching another module's header, `src/public/`, or `src/mino.h`
  is a cross-module change: the agent returns
  `needs-cross-module <reason>` instead of editing, and the round
  runner sequences one dedicated branch for it after the module waves.

## Serialized files

- Agents NEVER edit `CHANGELOG.md` or version fields — guaranteed
  conflicts. Changelog lines travel as proposal EDN
  (`<run-dir>/proposals/<branch-slug>.edn`) and
  `tools/merge_proposals.clj` appends them under `## Unreleased`
  exactly once at land time.

## Conflicts

- `integrate_fixes.clj` never resolves a conflict by guessing: the
  merge is aborted, recorded in `<run-dir>/escalations.edn`, and the
  remaining branches still land. An escalation goes to a fresh editor
  dispatched with both diffs — or to the maintainer if it is a real
  design collision.

## Commit format — category first

The full form, in order of the message:

1. **`Category: ` prefix.** A capitalized noun naming the area or
   effort the commit belongs to (`GC:`, `Security:`, `Tooling:`,
   `Docs:`, `Build:`) — chosen so the log scans and related commits
   group. The category slot is for humans; that is why Conventional
   Commits is rejected here (it spends the most valuable part of the
   message on machine-readable tokens). An occasional uncategorized
   commit is acceptable when no meaningful category fits — never
   invent a hollow one.
2. **Imperative, sentence-case subject** describing the *effect* of
   the change, not the diff: `GC: Trace fn.wraps_prim in the
   FN/MACRO walker`, not `GC: Add PUSH call to gc_handlers.c`.
   Capital first letter, no trailing period, ~70 characters — exceed
   that for clarity rather than compressing into noise. The category
   already gives context, so the subject doesn't repeat it.
3. **Body (optional).** The context a future reader needs: why, what
   broke, what was considered. Constraints and causes, not a prose
   restatement of the diff.
4. **Trailers at the very bottom.** Machine-readable lines
   (`Co-Authored-By:`, `[skip ci]`) — order among them doesn't
   matter, but they come after everything human-readable.

No version numbers in commit messages — versions are release
metadata. One commit per finding or per tightly-related group.

## Run state

- Everything lives under `.local/runs/<run-id>/` (gitignored):
  `state.edn`, `findings/`, `proposals/`, `punch-list.edn`,
  `merged.edn`, `escalations.edn`. A killed session resumes with
  `./mino tools/run_state.clj status <run-dir>` — nothing is held
  only in a model's context.
