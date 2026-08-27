# ADR 32: Classed catch clauses dispatch on diagnostic kind

Date: 2026-08-27

## Context

mino's catch grammar is `(catch e body...)`: no class slot, catches
everything. JVM Clojure requires `(catch ExceptionType e body...)` and
filters by class, so most real-world Clojure code (and every example
on ClojureDocs) fails to load on mino with "unbound symbol". The
omission was deliberate: mino has one exception representation, a
diagnostic map with `:mino/kind` and `:mino/code`, and no class
hierarchy. But the syntax gap costs more compatibility than the type
filter ever bought honesty.

## Decision

Accept both shapes. A catch clause with exactly one leading symbol
stays the bare catch-all. A clause with two leading symbols is
classed: `(catch Type e body...)`, where Type maps to a predicate over
the thrown diagnostic's `:mino/kind`:

| Type                                                    | matches                  |
|---------------------------------------------------------|--------------------------|
| Throwable, Exception, :default                          | anything                 |
| ExceptionInfo, clojure.lang.ExceptionInfo               | :user                    |
| Error                                                    | :internal                |
| ClassCastException, ArithmeticException, NullPointerException, NumberFormatException | :eval/type |
| IllegalArgumentException                                | :eval/arity, :eval/contract |
| UnsupportedOperationException                            | :eval/contract           |
| IndexOutOfBoundsException, StringIndexOutOfBoundsException | :eval/bounds           |
| IllegalStateException                                   | :eval/state              |

Multiple catch clauses run first-match-wins, as on the JVM; a bare
clause may follow classed ones. A class name outside the table is a
compile error naming the class, mirroring the JVM's unknown-class
failure, rather than silently binding the wrong symbol. The `:default`
keyword follows ClojureScript's precedent. Both execution tiers
(tree-walker and bytecode VM) share one partition and dispatch
implementation. The mapping is approximate where the JVM's class
granularity exceeds mino's kind granularity; code needing exactness
uses the bare clause and reads `:mino/kind`.

## Consequences

Clojure code with classed catches loads unmodified. The single-catch
limitation in both try parsers relaxes to a clause list. Catches now
decline to handle diagnostics outside their class, so previously
over-broad handlers tighten. The table is C data shared by both tiers
and grows only with new diagnostic kinds.

## Alternatives

Rewrite every classed catch to a bare catch at read time: loads more
code but silently widens handlers, which is exactly the bug class the
JVM filter exists to prevent. Implement a class hierarchy in the
diagnostic map: a whole type system for one special form.
