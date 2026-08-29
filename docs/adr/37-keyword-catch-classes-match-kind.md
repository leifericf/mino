# ADR 37: Keyword catch classes match diagnostic kind by equality

Date: 2026-08-29
Status: accepted

## Context

ADR 32 gave `catch` a class slot: `(catch Type e body...)`, where Type is
a name in a fixed C table (`mino_catch_classes`) mapping a
source-compatibility class token to the diagnostic `:mino/kind` values it
accepts. A name outside that table is a compile error, mirroring an
unknown class.

That table exists so existing source that writes catches like
`(catch ExceptionInfo e ...)` or `(catch IllegalArgumentException e ...)`
loads unmodified. But those names are not classes in mino. mino has no
class hierarchy: its only error representation is a diagnostic **data
map** carrying `:mino/kind`. The table is a compatibility veneer that
translates legacy class tokens onto that one native model.

The libraries mino bundles raise domain errors with their own kinds
(`:time/zone`, `:json/parse`, `:toml/parse`, ...), and user code raises
its own (`{:mino/kind :myapp/oops ...}`). None of these appear in the
class table. Under ADR 32 alone the only way to catch one selectively is
the bare `(catch e ...)` clause followed by a hand-written `:mino/kind`
test. There was no way to say "catch this kind" directly, and adding
every library kind to the C table would drag mino's open, data-shaped
taxonomy into the frozen compat veneer and still exclude user-defined
kinds.

## Decision

A **keyword** catch class that is not a table alias matches the thrown
diagnostic's `:mino/kind` by equality:

```clojure
(try (parse-zone s)
  (catch :time/zone e (recover e))   ; matches {:mino/kind :time/zone ...}
  (catch :default   e (log e)))      ; :default stays catch-all
```

This is mino's native error dispatch applied directly to the catch
grammar. Rules:

- A **symbol** catch class is unchanged: it names an entry in the frozen
  compat table, and an unknown symbol is still a compile error (the
  typo-safety ADR 32 provides for legacy class names).
- A **keyword** catch class is first looked up in the table so the
  existing keyword alias `:default` (catch-all, per ADR 32) keeps
  working. A keyword that is *not* in the table is not an error; it is a
  kind literal, matched against `:mino/kind` by equality. No hierarchy,
  no widening, no approximation.
- Matching is exact keyword equality. A diagnostic with no `:mino/kind`,
  or a different kind, is declined and the next clause is tried.
  First-match-wins across clauses, as before; a bare clause may follow.

Both execution tiers share the mechanism. The tree-walker records the
clause as the sentinel `MINO_CATCH_CLASS_KIND` plus the keyword and
matches via `mino_catch_kind_matches`. The bytecode tier emits a new
two-word `OP_CATCH_MATCH_KIND` (the trailing word carries the keyword's
constant-pool index, mirroring `OP_CALL_CACHED`); it is not
JIT-stencilled, so try bodies stay on the interpreter exactly as under
ADR 32.

## Consequences

Library- and user-defined kinds are catchable with the same syntax as
the built-in compat classes, and the C core stays ignorant of any
library's taxonomy: kinds live in data, not in the table. The class-name
table stays frozen and small.

The cost is that a mistyped keyword kind (`:time/zoen`) silently never
matches, with no compile-time check. This is the same tradeoff Clojure
accepts for `(:type (ex-data e))` dispatch; the bare `(catch e ...)`
clause plus a read of `:mino/kind` remains the escape hatch for code that
wants to inspect rather than dispatch. The split is principled: symbols
are the closed compat surface (unknown = error), keywords are open data
(unknown-in-table = match by kind).

## Alternatives

Enumerate every library kind in the C catch-class table: keeps ADR 32's
typo-safety but couples the core to each optional, capability-gated
library's error taxonomy, cannot express user-defined kinds, and imports
the closed-hierarchy shape mino exists to avoid. Rejected. Desugar
keyword-kind catches into a bare catch plus an `=` test at read time:
loads more code but reintroduces the hand-written dispatch this ADR
removes and complicates the shared catch dispatcher.
