---
name: check-style
description: Review recipe for the style dimension — naming, idiom, comments, size limits — against the codified C and Clojure standards. Invoked by reviewer agents.
user-invocable: false
---

# check-style

Review the assigned shard against the codified standards:

- C: `references/c-style.md` (in this skill)
- Clojure/mino: `references/clj-style.md` (in this skill)

Those files ARE the checklist — apply them mechanically. Beyond them,
flag only:

1. **Inconsistency with the surrounding file.** A function that names,
   comments, or structures differently from its TU without reason.
2. **Comment debt.** Ownership/GC-safety constraints that are true but
   unstated where the contract requires them (abort rationale,
   "Error classes emitted" blocks); comments that narrate the next
   line instead of stating a constraint; stale comments contradicting
   the code.
3. **Size-limit pressure.** TUs/functions within ~10% of the qa-arch
   limits (1100/250 LOC) — flag as `:low` so factoring can plan, since
   the gate will eventually fire mid-unrelated-change.

Do NOT flag: audited non-gated idioms (the c-style.md "deliberately
NOT gated" list), vendored code under `src/vendor/`, generated files
(`src/core_mino.h`, `src/lib_*.h`, stencil `generated/*.h`).

Style findings are `:level :style`, severity `:low` (or `:medium`
when a misleading comment could cause a future correctness mistake).
These land last, after correctness and factoring waves — never block
on them.
