# Clojure Dialect Roadmap (Step-Based)

This roadmap is ordered by dependency: each step unlocks the next.

## How to use this roadmap (for downstream implementation agents)

- Treat each step as a separate delivery slice that can ship independently.
- Avoid mixing steps in one PR unless a tiny shim is required by a hard dependency.
- Before changing behavior, add characterization tests for current behavior when possible.
- Prefer simple, explicit semantics over clever shortcuts.
- Preserve embeddability constraints (deterministic behavior, clear errors, no hidden host side effects).

### Recommended validation loop for every step

- Run mino internal suite: `make test`
- Run targeted external test files first (only files related to changed semantics).
- Then run full external suite sweep and bucket failures by category.
- Record pass/fail deltas in step notes before/after implementation.

### Suggested implementation touchpoints

- Reader/parsing: `src/read.c`
- Evaluation/special forms: `src/eval_special.c`, `src/eval_special_*.c`
- Core library/runtime vars/macros: `src/core.mino`, `src/prim*.c`
- Module loading and namespace-adjacent resolution: `src/prim_module.c`
- Value model/type operations/printing: `src/val.c`, `src/print.c`

---

## ~~Step 1 - Reader + Loadability Baseline~~

### Smaller steps
- Add `ns` as a recognized top-level form that establishes current namespace.
- Add Clojure-style `require` form parsing (`:as`, `:refer` subset).
- Implement reader conditionals `#?` and splicing conditionals `#?@`.
- Implement var-quote reader macro `#'sym`.
- Add initial dialect feature flags used by reader conditionals (at minimum `:default` and mino dialect key).
- Add parse/load diagnostics for malformed namespace and reader-conditional forms.

### Implementation notes for downstream agent
- Implement `#?`/`#?@` as reader-level selection, not eval-time branching.
- Keep unsupported platform tags deterministic (`:default` fallback behavior must be stable).
- Parse `require` syntax now even if only a subset is executable at first.
- Keep reader changes table-driven (dispatch table for `#` forms) to avoid brittle branching.

### Known risks / pitfalls
- Reader conditional splicing can corrupt surrounding list/vector assembly if shape checks are weak.
- Accepting `require` syntax without symbol normalization can cause later resolver inconsistencies.
- `ns` should not silently mutate unrelated global state.

### Definition of done
- Any file in `clojure-test-suite/test/clojure/**/*.cljc` can be read without immediate reader failure on `ns`, `#?`, `#?@`, or `#'`.
- `(require '[x.y :as z])`-style forms no longer fail at syntax level.
- Error messages for malformed `ns`/`require`/reader-conditional input are deterministic and location-aware.

---

## ~~Step 2 - Namespace + Var Semantics~~

### Smaller steps
- Introduce runtime namespace objects and current-namespace tracking (`*ns*` equivalent behavior).
- Implement symbol resolution order (locals -> current ns -> referred aliases -> core fallback).
- Add alias table management from `ns`/`require` forms.
- Add first-class vars (`var`, `var?`, deref-able var identity).
- Implement `resolve` and basic namespace-qualified symbol resolution.
- Add dynamic var support sufficient for common test portability helpers.

### Implementation notes for downstream agent
- Keep namespace identity and var identity separate from symbol identity.
- Maintain explicit root binding table vs dynamic binding stack.
- Make var deref and symbol resolution semantics observable and debuggable.
- Resolve should return nil when unresolved, not generic hard error, where Clojure expects that behavior.

### Known risks / pitfalls
- Conflating vars with raw env bindings will break watch/dynamic behavior later.
- Alias resolution recursion can create cycles if not guarded.
- Dynamic bindings leaking across eval boundaries will create flaky tests.

### Definition of done
- Namespace-qualified and aliased symbols resolve consistently across files.
- `#'sym`, `var`, and `var?` behave as var identity operations, not plain symbol wrappers.
- Tests that only fail on missing namespace/var semantics now execute past resolution.

---

## ~~Step 3 - `clojure.test` Compatibility~~

### Smaller steps
- Implement `deftest` registration and execution model.
- Implement `is`, `testing`, and `are` macros with expected expansion semantics.
- Add assertion result reporting API compatible with suite usage.
- Implement thrown-assert behavior expected by portability helpers.
- Add test runner that can run namespace sets and print summary counts.
- Ensure failures/errors preserve source location and form context.

### Implementation notes for downstream agent
- Preserve macro expansion-time behavior of assertion forms.
- Keep reporting payloads data-first (`:type`, `:message`, `:expected`, `:actual`).
- Build minimal but extensible assertion multimethod hook points for portability code.
- Ensure nested `testing` contexts accumulate cleanly in reports.

### Known risks / pitfalls
- Implementing `are` naively can produce incorrect source attribution and poor diagnostics.
- Thrown assertions must distinguish failure-to-throw vs wrong runtime error path.
- Reporter side effects can interfere with REPL output if not isolated.

### Definition of done
- The external suite can run to completion (pass/fail/error output) instead of failing before execution.
- `deftest`, `is`, `testing`, and `are` forms execute with recognizable Clojure-style semantics.
- Summary reports include deterministic totals for tests/assertions/failures/errors.

---

## Step 4 - Reader Literal Parity

### Smaller steps
- Add character literals (`\\space`, `\\newline`, `\\tab`, `\\A`, etc.).
- Add ratio literals (`1/2`, `-3/4`).
- Add bigint and bigdec literals (`1N`, `1M`).
- Add hexadecimal integer literals (`0xFF`, signed variants where valid).
- Add special float tokens (`##Inf`, `##-Inf`, `##NaN`).
- Align print/read behavior for new literal families where round-trip is expected.

### Implementation notes for downstream agent
- Introduce literal parsers as independent helpers, then dispatch from atom reader.
- Define canonical internal representations before wiring operations.
- Add strict literal validation to avoid ambiguous token fallback to symbol.
- Add print rules early so debugging output remains legible during implementation.

### Known risks / pitfalls
- Ratio parsing conflicts with symbol tokens if tokenizer boundaries are vague.
- Big number parsing must avoid silent truncation.
- `##NaN` equality/compare semantics should follow Clojure behavior, not host defaults blindly.

### Definition of done
- Literal-focused core tests no longer fail at read phase.
- Numeric and char literals evaluate to stable runtime types with consistent printing.
- Invalid literals produce precise reader errors (not generic unbound symbol errors).

---

## Step 5 - Numeric Tower Behavior

### Smaller steps
- Define canonical numeric coercion matrix across int/float/ratio/bigint/bigdec.
- Align arithmetic ops (`+ - * /`) with Clojure arity and type-promotion behavior.
- Align comparison behavior (`=`, `==`, `<`, `<=`, `>`, `>=`) across numeric types.
- Implement predicates (`integer?`, `ratio?`, `rational?`, `decimal?`, etc.) to match semantics.
- Implement conversions/parsers (`int`, `long`, `double`, `bigint`, `bigdec`, `parse-*`) per target behavior.
- Normalize overflow/underflow and divide-by-zero exception behavior.

### Implementation notes for downstream agent
- Write a conversion/coercion matrix table in code comments or docs and enforce it in one place.
- Route arithmetic operations through shared numeric dispatch instead of ad hoc per-primitive branching.
- Add cross-product tests for mixed-type arithmetic and comparisons.
- Keep exception classes/messages stable and pattern-matchable.

### Known risks / pitfalls
- Mixed numeric semantics can regress silently if each primitive owns its own coercion logic.
- Overflow behavior differences across host C types can leak unless normalized.
- Parser functions (`parse-*`) may require stricter input handling than arithmetic casts.

### Definition of done
- Numeric test files run with majority pass rate and no systematic type-promotion mismatches.
- Cross-type arithmetic and comparisons are deterministic and documented.
- Numeric edge-case failures are isolated, not structural.

---

## Step 6 - Core Collections Semantics

### Smaller steps
- Align `get`, `assoc`, `dissoc`, `conj`, `contains?`, `find`, `empty`, `count` edge cases.
- Align sequence boundary behavior for `seq`, `first`, `rest`, `next`, `nth`, `subvec`.
- Align callable semantics for keywords/maps/vectors.
- Align nil/empty collection behavior in collection and sequence APIs.
- Ensure metadata-preserving behavior where Clojure expects it.
- Harden exception behavior for invalid collection operations.

### Implementation notes for downstream agent
- Build behavior tables for each core function: valid inputs, nil behavior, exception paths.
- Centralize sequential coercion (`seq`-ability checks) to avoid drift.
- Validate callable collection arity and default-value behavior in one shared call path.
- Confirm metadata propagation on all operations that return new collections.

### Known risks / pitfalls
- Nil edge cases often differ between callable and non-callable paths.
- Metadata can be dropped unintentionally during structural updates.
- Sequence APIs can trigger accidental eager realization if not careful.

### Definition of done
- Collection and sequence core tests stop showing broad semantic drift.
- Nil/empty/callable-collection behavior matches Clojure expectations in common and edge paths.
- Remaining failures are narrow function-specific corner cases.

---

## Step 7 - Stateful + Transients + Watches

### Smaller steps
- Align transient lifecycle (`transient` -> transient ops -> `persistent!`).
- Implement transient ops (`assoc!`, `conj!`, `disj!`, `dissoc!`, `pop!`) with validity checks.
- Add watch APIs (`add-watch`, `remove-watch`) and callback invocation model.
- Align atom/var watch event payload shape and ordering expectations.
- Ensure dynamic binding interactions with stateful operations are correct.
- Validate safety/error behavior when transients are used after persistence.

### Implementation notes for downstream agent
- Model transient validity as explicit state flag; check on every transient op.
- Keep watch callback invocation deterministic and isolated from mutation primitive internals.
- Define callback failure policy (propagate, aggregate, or isolate) and keep it consistent.
- Add stress tests for repeated watch add/remove cycles.

### Known risks / pitfalls
- Allowing post-`persistent!` transient usage can corrupt data guarantees.
- Watch callback exceptions can cause partial state updates if ordering is not controlled.
- Dynamic binding leakage is common in stateful code paths.

### Definition of done
- Transient tests pass for valid mutation paths and fail correctly for invalid lifecycle usage.
- Watch tests pass with expected callback payloads and sequencing.
- Stateful semantics are stable under repeated test runs.

---

## Step 8 - Hierarchies + Dispatch Essentials

### Smaller steps
- Implement hierarchy construction and update APIs (`make-hierarchy`, `derive`, `underive`).
- Implement query APIs (`parents`, `ancestors`, `descendants`).
- Add minimal protocol/dispatch primitives required by current test coverage.
- Ensure hierarchy operations are immutable/value-oriented where expected.
- Align error behavior for invalid derive/underive inputs.

### Implementation notes for downstream agent
- Keep hierarchy structure immutable and explicit (no hidden singleton mutations by default).
- Use cycle detection when deriving relationships.
- For protocol/dispatch support, implement only tested surface first, then extend.
- Add compact cache invalidation strategy if dispatch caches are introduced.

### Known risks / pitfalls
- Cycles in hierarchy relations can produce non-terminating ancestor queries.
- Global mutable hierarchy state can create test-order dependency.
- Over-implementing protocol features too early increases complexity and regressions.

### Definition of done
- Hierarchy-related tests execute and pass for standard parent/ancestor/descendant cases.
- Protocol/dispatch tests covered by suite no longer fail on missing runtime primitives.
- Hierarchy mutation does not leak inconsistent global state.

---

## Step 9 - `clojure.string` Namespace Parity

### Smaller steps
- Provide `clojure.string` namespace with canonical var names.
- Implement tested functions (`blank?`, `capitalize`, `starts-with?`, `ends-with?`, `escape`, `lower-case`, `upper-case`, `reverse`).
- Align behavior on nil, non-string, and unicode edge cases used by the suite.
- Ensure namespace alias usage (`[clojure.string :as str]`) resolves correctly.

### Implementation notes for downstream agent
- Keep `clojure.string` functions namespaced; do not rely on global aliases for correctness.
- Validate unicode behavior explicitly for case and boundary operations.
- Mirror Clojure nil/error behavior even when host C string APIs differ.
- Separate core string primitives from namespaced wrappers for maintainability.

### Known risks / pitfalls
- Locale-sensitive host behavior can drift from Clojure expectations.
- Nil and non-string coercion behavior is easy to get subtly wrong.
- Namespace aliasing bugs may masquerade as function bugs.

### Definition of done
- `test/clojure/string_test/*.cljc` runs end-to-end under namespace aliasing.
- Behavior matches expected pass/fail outcomes from suite edge cases.
- No fallback to global unqualified string vars required to pass tests.

---

## Step 10 - Conformance Hardening

### Smaller steps
- Run full external suite repeatedly and bucket failures by semantic category.
- Fix remaining edge-case mismatches (exceptions, predicate corners, print forms, arity details).
- Add regression tests in mino's internal suite for each fixed mismatch.
- Document intentional divergences with rationale (only if truly required).
- Add CI gate for external-suite pass-rate trend and no-regression checks.

### Implementation notes for downstream agent
- Maintain a failure taxonomy (`reader`, `resolver`, `numeric`, `collections`, `exceptions`, `test-harness`).
- Fix highest-frequency categories first, then high-impact corner cases.
- For each fixed external failure, add one internal regression test to avoid churn.
- Keep a living compatibility matrix with "implemented", "partial", "intentional divergence" states.

### Known risks / pitfalls
- Chasing long-tail failures without categorization wastes effort.
- Exception-message overfitting can make behavior brittle.
- Lack of regression capture causes repetitive reopenings of previously fixed issues.

### Definition of done
- External suite reaches agreed compatibility target (for example, >95% assertion pass rate).
- No broad category-level failure remains (only isolated, documented exceptions if any).
- Compatibility contract is published and maintained with regression automation.

---

## Optional handoff checklist for each implementation PR

- Step ID and sub-step IDs covered.
- Files touched and rationale.
- Behavior changes (before/after examples).
- External suite namespaces/files run.
- Internal tests added.
- Known follow-ups intentionally deferred to next step.
