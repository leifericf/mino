# ADR 19: clojure-census is the source of truth for Clojure surface and mino divergences

Date: 2026-08-12

## Context

Three artifacts describe how mino relates to Clojure's surface: the
`clojure-census` dashboard (`output/mino/dashboard.edn`, with a
divergences registry and behavior catalog), the mino in-tree coverage
test (`tests/clojure_coverage_test.clj`, a hand-pinned var manifest),
and three hand-written `mino-site` pages (the compatibility matrix,
the intentional-divergences catalog, and the coming-from-Clojure tour).

They had drifted apart. The census carried divergences
(`compare` sign-normalized, the single-integer tier, `type` as a
keyword, float-32 distinct) that the site's intentional-divergences
page never named. The coverage test pinned the Clojure **1.11** surface
while census compared against **1.12.4**, so the two completeness
numbers measured different Clojures. The compatibility matrix cited a
test-file path (`tests/clj_*_test.clj`) that does not exist. Three
sources of truth, each partially stale, none authoritative.

## Decision

`clojure-census` is the single source of truth for the Clojure surface
minio compares against and for mino's documented divergences. The Clojure
baseline is **1.12** (where census already captures the reference surface
at `clojure/1.12.4-surface.edn`).

Everything else derives from it and must not be hand-maintained in ways
that can disagree:

- The mino coverage manifest is regenerated from the census reference
  surface by `tools/gen_coverage_manifest.bb`; the committed
  `expected-*` sets are its output, re-derived whenever the baseline
  moves.
- The three `mino-site` pages render the divergence list, the surface
  verdicts, and the divergence anchors from the census dashboard
  payload. Hand-written prose stays; the data does not.
- The completeness number (surface parity percent) and the behavior
  totals are census's, not a second count kept elsewhere.

## Consequences

- A divergence or surface verdict is recorded once, in census. Editing
  it on a page or in the manifest is a drift bug, not a shortcut.
- Moving the Clojure baseline is one change in census (a regenerated
  reference surface) followed by a `gen_coverage_manifest.bb` run, not
  a hand-edit of the manifest.
- The cost is a render pipeline (census payload into the site pages)
  and a generator for the manifest. Both are small and pay back every
  time Clojure ships a release or mino settles a new divergence.
- The 9.1 percent of non-parity surface is split into intentional by
  design (JVM-bound, annotated in census) and genuine gap, surfaced
  through the generated matrix; the number stays interpretable.

## Alternatives

- **Keep the pages hand-written and reconcile by hand.** Rejected: it is
  how the drift above happened. Three hand-maintained copies of the same
  data rot, and the rot is invisible until a reader notices a mismatch.
- **Make the mino test read the census surface at run time.** Rejected:
  the mino test runs inside the mino runtime with no path to the census
  file, and vendoring the surface into the test tree is just a second
  copy. A committed generator that materializes the manifest from the
  census file keeps one source of truth and a reproducible rebase.
- **Pin the baseline at 1.11 to match the old manifest.** Rejected:
  census already compares against 1.12.4; aligning the manifest up to
  1.12 is strictly less work and stops the two numbers from measuring
  different Clojures.
