# ADR 44: Unicode normalization and case folding via generated tables

Date: 2026-09-03

## Context

Two strings can render identically yet compare unequal: a precomposed
accented letter and its base-plus-combining-mark spelling are distinct
codepoint sequences until normalized. macOS filenames arrive
decomposed while user input is usually precomposed, so filename and
user-text comparison is wrong without NFC/NFD support. Case-insensitive
comparison has the same shape: correct equality needs case folding,
not round-tripping through the ADR 31 case mappings. mino already
vendors the Unicode character database for those case tables, and the
required normalization data (canonical decompositions, combining
classes, composition exclusions) and the folding data live in the same
vendored dataset. The generated-table pattern is established: a
generator reads only vendored files, the header is committed, and
regeneration needs no network (ADRs 27, 31).

## Decision

mino adopts NFC and NFD normalization and simple case folding, built
the ADR 31 way: the generator grows to emit decomposition, combining-
class, composition, and fold tables from the already vendored data,
the header is committed beside the case tables, and native prims
expose normalize (with the form as an argument) and a fold-aware
equality helper on the string surface. Canonical forms only; the
compatibility forms (NFKC, NFKD) are excluded until a concrete use
appears, and folding is the simple 1:1 fold, matching the ADR 31
exclusion of multi-character and locale-sensitive mappings. The work
is green-lit as its own future change; this record fixes the approach.

## Consequences

- Filename and user-text comparison can be made correct on all three
  platforms, closing the macOS decomposed-filename trap.
- The committed header grows by the normalization and fold tables, a
  small fixed cost against the vendored data already in the tree; no
  new vendor directory, no new generator dependency.
- The full-composition algorithm (canonical ordering, Hangul
  composition) is real implementation weight; the prims owe the same
  property treatment the casing prims got, checked against the
  published normalization test file in the vendored dataset.
- Locale-tailored folding stays out, consistent with ADR 31: the
  divergence is visible and documented, not silent.

## Alternatives

- **No normalization; document the trap.** Zero cost, and ASCII-only
  scripts never notice. Rejected: comparison silently wrong on
  non-ASCII text is the silent-divergence class this project ranks
  worst, and the fix needs no new vendored data.
- **Full compatibility forms and locale-aware folding.** The complete
  answer. Rejected: NFKC/NFKD and locale tailoring serve identifier
  and search pipelines mino does not have; canonical forms cover the
  filename and equality cases that motivated this.
- **Normalize implicitly inside string equality.** No new API.
  Rejected: equality on codepoint sequences is the primitive; hiding
  normalization inside it changes the meaning of every existing
  comparison and its cost.
