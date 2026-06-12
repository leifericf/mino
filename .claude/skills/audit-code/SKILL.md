---
name: audit-code
description: Review and fix existing code at any scope - six-dimension reviewer fan-out, triaged punch list, editor fleet in worktrees, rounds until a round finds nothing new.
disable-model-invocation: true
---

# audit-code

Input: a scope — a module (`src/gc`), a list of modules, or a theme
("everything that calls realloc"). Output: an audited feature branch
where the last review round found nothing new, plus an escalation
list for what agents could not safely fix.

The topology and conflict rules are
`.claude/skills/implement-change/references/worktree-model.md`; the
per-round procedure is the run-review-round skill. This skill is the
loop around it:

1. **Initialize.** Branch `audit/<scope-slug>` off main;
   `./mino tools/run_state.clj init .local/runs/audit-<scope-slug> --kind audit --scope <scope> --branch audit/<scope-slug>`.
2. **Shard the scope** by module ownership (one shard = one module
   directory + its internal.h). For a theme-scope, grep the theme to
   a file list first, then group by module.
3. **Rounds until dry.** Dispatch `review-round-runner` with the run
   dir, branch, shards, and round number. It returns one line. Repeat
   while it says `continue`; stop at `dry` (or at a round budget you
   state up front — default 4 — to bound cost; say so if hit).
   Resume after a crash: `./mino tools/run_state.clj status` says
   which round was in flight; findings/proposals on disk are not
   redone.
4. **Report.** Flatten first:
   `./mino tools/flatten_branch.clj <run-dir> --repo <dir> --target <branch>`
   (linear history, tree verified bit-identical, integrated branches
   deleted). Then: rounds table (the one-line summaries), what was
   fixed by level, `escalations.edn` contents with both-diff context,
   and the changelog lines merged. Offer the flat branch for merge;
   merging is the maintainer's call.

Scope discipline: an audit fixes what reviews find inside the scope.
Findings outside the scope are recorded
(`<run-dir>/out-of-scope.edn`) and reported, not chased.
