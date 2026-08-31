# ADR 40: Conformance probe stays in the tests repo; lanes reach it via MINO_BIN

Date: 2026-08-31
Status: accepted

## Context

The differential conformance probes (corpus fixtures, ground-truth
captures, differ scripts, allowlist) live in the sibling tests repo.
Capturing ground truth needs a JVM and other host toolchains that mino
itself must never depend on. mino's verification lanes, defined in the
project descriptor, today cover build, sanitizers, and the native test
suite, but nothing land-blocking checks conformance: a change can
silently diverge from canonical output and land green.

The conformance corpus is growing a curated edge tier beside the
documentation-derived tier, and the point of curating it is that a
divergence, once fixed, stays fixed. That only holds if some lane runs
the probe against the freshly built binary before landing.

## Decision

The probe and all its fixtures remain owned by the tests repo; the mino
repo gains a task that locates the sibling checkout, points `MINO_BIN`
at the freshly built `./mino`, and runs the edge-corpus differ from the
tests repo root. That task joins the pre-land lane. Ground-truth
fixtures are committed in the tests repo, so lane time needs no JVM and
no network; the lane compares one binary against recorded canon. A
missing or unreadable sibling checkout is a hard lane failure with a
message naming the expected path, never a skip.

## Consequences

Fixed divergences become permanent land-blocking regressions at the
cost of one subprocess sweep over a small curated corpus (seconds).
The pre-land gate now depends on a sibling checkout being present and
reasonably current; a developer without the tests repo cannot run the
full gate, which is accepted because the same is already true of the
sanitizer toolchains. Corpus refresh (new tuples, new ground truth)
stays entirely in the tests repo and never blocks on a mino release.
The failure mode where the sibling is stale (probe green against old
fixtures) is bounded by the tests repo's own CI running the same probe.

## Alternatives

Keep the probe solely in the tests repo's CI: no cross-repo coupling in
the lanes, and CI still catches divergence eventually, but after land
rather than before, so a bad change is reverted instead of rejected and
the require-tests-before-land hook never sees conformance at all.
Rejected because it leaves the gate advisory. Vendor the corpus and a
minimal differ into the mino repo: self-contained lanes, but it forks
the harness, duplicates fixtures that then drift, and drags test-only
assets into the language repo. Rejected for drift.
