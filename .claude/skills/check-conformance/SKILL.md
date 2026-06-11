---
name: check-conformance
description: Review recipe for the conformance dimension — behavior must match JVM Clojure, deviations must be deliberate and documented. Invoked by reviewer agents over C primitives or Clojure-side lib code.
user-invocable: false
---

# check-conformance

Review the assigned shard for divergence from canonical Clojure. The
spec is JVM Clojure's observable behavior; mino's job is to match it
or document why not (see the deviation comments in `src/prim/stm.c`
for the documentation idiom).

Look for:

1. **Semantic drift.** Edge cases where the implementation's behavior
   would differ from JVM Clojure: nil handling, empty-collection
   cases, laziness boundaries, arity errors vs nil returns, numeric
   tower promotion/overflow behavior, ordering guarantees
   (sorted vs hash collections), metadata propagation.
2. **Undocumented deviations.** Behavior that differs on purpose but
   has no deviation comment at the implementation site.
3. **Error-shape mismatches.** Wrong diagnostic kind, an exception
   where JVM Clojure returns a value (or vice versa), uncatchable
   errors for things `try`/`catch` should see.
4. **Doc/implementation gaps.** Docstrings in the prim tables that
   promise JVM behavior the C doesn't deliver.

Classify every gap using the taxonomy (this is the no-workarounds law
applied to review): a **real mino gap** is a finding; an **upstream
platform difference** (libc/OS variance) needs a site comment, not a
shim; an **infrastructure issue** (harness, runner) is a finding
against the harness. Never suggest special-casing a caller or test to
hide a gap.

Severity: silent wrong answers are `:high`; wrong error shapes
`:medium`; doc gaps `:low`. Level is `:correctness` (doc gaps:
`:style`). Cross-check candidates against `tests/conformance_test.clj`
and `tests/clojure_coverage_test.clj` before filing — it may be a
known, tracked gap.

Before filing, scan the decision index (`docs/adr/README.md`): a
divergence an ADR decided is not a finding (cite the ADR if the site
lacks its deviation comment — that's a `:style` doc gap); code that
*violates* an ADR is a `:high` finding citing `ADR-NN`.
