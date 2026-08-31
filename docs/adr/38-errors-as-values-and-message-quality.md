# ADR 38: Errors as values, and best-in-class messages

Date: 2026-08-31
Status: accepted

## Context

mino's error representation is a diagnostic **data map** carrying `:mino/kind`
(ADR 32, ADR 37): open, keyword-dispatched, no class hierarchy. That settles
*what* an error is. It does not settle two questions the library code answers
inconsistently today:

1. **Signal by throwing, or by returning?** The bundled libraries mix models.
   `clojure.string`, `mino.html`, `mino.template`, and `mino.cli` raise the
   `:mino/kind` map; `mino.http` throws `(ex-info "..." {})` with an empty data
   map on validation faults; `mino.http.server`, `mino.deps`, `clojure.zip`, and
   several `core` sites throw **bare strings**; `mino.cli/auto-coerce` used the
   full reader (`read-string`) and let a raw `ratio: division by zero` escape on
   hostile input. A caller cannot write one `catch` on `:mino/kind` across the
   surface, and some faults carry no structured data at all.

2. **How good is the message?** JVM Clojure's weakest ergonomic point is its
   errors: `class java.lang.String cannot be cast to class clojure.lang.IFn`
   names the machinery, not the mistake. mino should not inherit that.

## Decision

**Signal recoverable failure as a value; throw only at the boundary; make every
message specific.**

- **Recoverable, caller-branches-on-it → return data.** A failure the caller is
  expected to handle (a parse that can fail, a validation, a lookup that can
  miss) is returned, not thrown: `nil` for a plain miss, or a
  `{:mino/kind ... :mino/message ... :mino/data ...}` map where the caller needs
  the reason. This is the functional-core discipline: pure logic takes data and
  returns data, errors included, so it composes and tests without `try`.
- **Exceptional, stop-and-report → throw the same data map.** Bad public input
  at an entry point, or a broken invariant, throws the `:mino/kind` diagnostic
  so it unwinds to the boundary/REPL and is catchable by kind (ADR 37).
- **No bare-string and no empty-`ex-info` throws.** They are the worst of both
  worlds: opaque and unstructured. Every raise carries a `:mino/kind` and a
  specific message.
- **Best-in-class messages.** `:mino/message` names the offending value, the
  expectation, and where. Not `"invalid number"` but
  `"--x: cannot coerce \"1/0\" to a number; pass an integer like 8080"`. A
  generic or machinery-shaped message is a finding. This applies to returned
  error maps and thrown diagnostics alike.
- **Faithful ports keep their throw contract.** `lib/clojure/*` reimplement
  Clojure libraries whose documented contract is to throw (e.g. `clojure.zip`
  on "insert at top"); they continue to throw, but upgrade to a specific
  message and, where they raised bare strings, to the `:mino/kind` map.

This is not a proposal to remove `throw`/`try`/`catch` from the language: user
code keeps full exception semantics. It governs how mino's own library code
signals failure.

## Consequences

One error model across the surface: a caller can branch on returned error data
in the core and `catch` by `:mino/kind` at the boundary, and every message is
actionable. The conversion is mechanical where a bare-string or empty-`ex-info`
throw exists; the judgement is per-site whether a fault is recoverable (return)
or exceptional (throw). The cost is churn across `mino.http`, `mino.http.server`,
`mino.deps`, `clojure.zip`, and the `core` throw sites, done incrementally with a
test per fixed site.

## Alternatives

**Values everywhere, throw never** (every public fn returns `{:ok}`/`{:error}`):
forces error-threading at every call site, taxes the happy path a Clojure
dialect is expected to compose cleanly, and cannot apply to the faithful ports
without breaking fidelity. Rejected as more complecting, not less. **Keep
throwing everywhere, only fix messages:** leaves the pure core untestable
without `try` and keeps failure off the value channel where the core wants it.
Rejected: message quality is necessary but not sufficient.
