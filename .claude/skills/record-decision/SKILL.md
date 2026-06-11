---
name: record-decision
description: Write a decision record in docs/adr/ when an architecture decision is made - by the maintainer in conversation, or by an agent whose work settled a real choice between alternatives. Lightweight by design - a text file, written while the context is fresh.
---

# record-decision

A decision record is a text file in `docs/adr/` capturing context,
decision, consequences, and the alternatives weighed — written when
the decision happens, not reconstructed later. Style and field rules:
`.claude/skills/check-style/references/prose-style.md` (the "Decision
records" section is the template).

## When to invoke

Humans: whenever a discussion ends in "we'll do X, not Y" and X
constrains future work.

Agents: invoke this skill when your work settles one of these —
- an **escalation** resolved by a design ruling (not a mechanical
  rebase);
- a **plan-time choice** in implement-change between real
  alternatives, where the rejected option would have been reasonable;
- an **incorporate-feedback conflict** the maintainer resolved;
- a **review finding rejected as deliberate** with no existing ADR to
  cite — the rejection rationale IS the record.

Do not record: choices with one reasonable option, reversible
implementation details, anything an existing ADR already covers
(supersede it instead if the answer changed).

## Procedure

1. Next number: read `docs/adr/README.md`, take max + 1 (two digits).
2. Write `docs/adr/NN-slug.md`:

   ```markdown
   # ADR NN: <the decision, readable in the index>

   Date: YYYY-MM-DD

   ## Context
   ## Decision
   ## Consequences
   ## Alternatives
   ```

   One screenful. Consequences include the costs. Alternatives get
   their real strengths before the rejection. Superseding: say
   "Supersedes ADR NN" in the Decision; never edit the old record.
3. Add the one-line row to the `docs/adr/README.md` index.
4. If the decision changes how agents work (a banned idiom, a new
   ritual), also route a rule to the right skill or reference —
   the ADR holds the why, the skill holds the how. Cross-cite.
5. Commit both files: `Docs: Record ADR NN, <short title>` (or fold
   into the change's commit series when recorded mid-change).

Agents working in a worktree: write the ADR in your worktree like any
other file; it lands with your branch.
