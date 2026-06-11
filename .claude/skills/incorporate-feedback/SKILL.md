---
name: incorporate-feedback
description: Promote captured guidance into the shared standards - reference files, check-* skills, known-bugs.md, or deterministic gates - and empty the inbox. Run periodically or before a big run.
disable-model-invocation: true
---

# incorporate-feedback

Input: `.claude/guidance/inbox.edn` (entries written by
capture-guidance). Output: each entry promoted to its durable home,
the inbox emptied, one commit.

Per entry, in inbox order:

1. **Pick the home by strength.** Strongest first:
   - **Deterministic gate** (qa-arch check, lint-zig warning,
     a spine validation) — when the rule is mechanically checkable,
     a gate beats prose. This needs a code change + test (TDD, see
     write-clj); do it, don't downgrade to prose for convenience.
   - **Reference file** (`c-style.md`, `clj-style.md`,
     `known-bugs.md`, `worktree-model.md`) — rules reviewers and
     writers must apply. New shipped-bug patterns always go to
     known-bugs.md with commit hash and repro.
   - **check-* / write-* skill body** — when it changes what a
     dimension looks for or how a recipe proceeds.
   - **Decision record** (`docs/adr/`, via the record-decision
     skill) — when the entry is a *why* (a choice between
     alternatives) rather than a *how*. Often paired: the ADR holds
     the reasoning, a reference holds the resulting rule.
   - **Agent body / entry skill** — only for dispatch-contract or
     orchestration changes.
2. **Resolve conflicts.** An entry with `:conflicts-with` (or one you
   discover contradicts current text): present both rules to the
   maintainer and apply the decision; never keep both. The resolution
   is a decision — record it via record-decision.
3. **Write it in place**, in the file's voice — a rule, not a
   changelog of the conversation. Delete the entry from inbox.edn in
   the same change.
4. **Cross-check.** If the new rule invalidates guidance elsewhere
   (a now-banned idiom shown in an example), fix those sites too.

Commit everything as one
`Skills: Incorporate captured guidance (<n> entries)` commit (gates
get their own TDD commits first). Report a table: rule → home.
