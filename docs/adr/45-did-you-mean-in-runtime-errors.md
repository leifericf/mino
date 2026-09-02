# ADR 45: Did-you-mean lives in the runtime's own errors, not a library

Date: 2026-09-03

## Context

The most common interactive failure is a name typo: an unresolved
symbol, an unknown keyword arg, a mistyped namespace. mino's error
doctrine says every message names the offending value, the
expectation, and where (ADR 38), and the unresolved-symbol site
already knows the candidate set: the vars in scope, the namespace's
mappings, the loaded namespace names. Separately, users of other
scripting runtimes download general fuzzy-matching libraries, which
suggests a stdlib gap. One reference runtime answered this years ago
by wiring suggestions into the runtime's own name errors rather than
shipping a matching toolkit, and that shape is what users actually
experience as "did you mean".

## Decision

Did-you-mean is error-message work, not a library. The unresolved-
symbol and unresolved-namespace raise sites gain a suggestion step: a
small bounded edit-distance pass over the in-scope candidate set, with
the near misses appended to `:mino/message` ("did you mean ...?") and
carried as data under the diagnostic map so tooling can read them. The
distance fn stays an internal detail of the error path; no public
fuzzy-match namespace ships. The wiring is future work scoped by this
record and lands under the ADR 38 message-quality bar.

## Consequences

- The typo experience improves exactly where typos happen, at the
  REPL and in scripts, with no API for the user to learn.
- The suggestion pass runs only on the error path, so the happy path
  pays nothing; the pass itself must stay bounded (candidate count
  and distance cutoff) so a huge environment cannot make failing
  slower than users tolerate.
- Suggestions ride the diagnostic map, so keyword-catch dispatch
  (ADR 37) and message formatting stay unchanged; tooling reads the
  candidates without parsing prose.
- General fuzzy matching over arbitrary user data remains out of the
  box; a script that needs it writes the few lines of edit distance
  itself or motivates a separate record with a concrete workload.

## Alternatives

- **Ship a fuzzy-match namespace.** Serves search-and-suggest uses
  beyond errors, and the demand signal exists. Rejected: the demand
  is overwhelmingly the typo case, which a library alone does not
  fix; a public matching API is surface to maintain for a use the
  error path already covers.
- **Suggestions in the REPL only.** Smaller blast radius. Rejected:
  scripts hit the same typos; the raise site is the one place that
  covers both.
- **Prose-only suggestions, no data.** Simpler map. Rejected: editors
  and the language server want the candidates structurally; appending
  prose while carrying data costs one extra key.
