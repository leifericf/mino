# ADR 39: Conformance generation and triage inherit the session model

Date: 2026-08-31
Status: accepted

## Context

The differential conformance harness in the sibling tests repo compares
mino's printed output with JVM Clojure ground truth, byte for byte. Two
steps in that loop need model judgment rather than mechanics: writing
per-var edge-case forms (laziness, numeric-tower corners, dispatch
edges) and triaging diff output into real bug, intentional divergence,
or harness artifact. Everything between those steps is a deterministic
pipeline of existing scripts.

Three model tiers are available to run those two steps. The highest
general tier reroutes offensive-security content to the audit tier by
provider policy, so it cannot hold a deep security audit, while
high-volume semantic work is exactly where its capacity is cheapest per
finding. Separately, per-dispatch model overrides on subagents are
unreliable in the current tooling (a known upstream issue can silently
serve a smaller model), which makes "pin the model on each worker" a
fragile place to encode routing.

## Decision

Conformance generation and triage run on whatever model the session was
started with, and dispatched workers inherit the session model; no
per-dispatch override is written into worker prompts or skill text. The
routing choice is made once, at session start: conformance cycles start
on the high-capability general tier, and the deep security audit runs
in its own session on the audit tier. A spike on 2026-08-31 verified
that a worker dispatched without an override reports the same model as
its parent session, and that conformance framing does not trip the
security reroute.

## Consequences

Routing lives in one observable place, the session, and survives the
upstream override bug because inheritance is the only mechanism used.
Skills and worker prompts stay model-agnostic, so the same cycle can be
rerun on a cheaper tier by starting a cheaper session. The cost is that
a single session cannot mix tiers per step; work that needs the audit
tier is a separate session by construction, and nothing enforces that a
conformance session was actually started on the intended tier beyond
the operator checking.

## Alternatives

Per-dispatch model overrides on each worker: finer grained and
self-documenting in the dispatch, but currently unreliable (silently
downgraded workers spend the wrong credits and mask the routing), and
it spreads the routing decision across every skill. Rejected until the
upstream issue is fixed. Encoding a model map in the project descriptor:
central and declarative, but the descriptor schema has no model concept
and the spine never dispatches by model; it would be configuration
nothing reads. Rejected as an experiment nothing backs.
