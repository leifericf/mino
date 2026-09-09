# ADR 63: Defer mino's internal source-list deduplication

Date: 2026-09-09

## Context

mino describes its own source set in more than one place internally: the
bootstrap Makefile enumerates the sources it compiles, and
`lib/mino/tasks/builtin.clj` holds `lib-srcs` (the wired amalgam
manifest, guarded so an unwired source fails the build). These are
hand-synced. It is the same enumerate-mino's-sources pattern the
amalgam-migration cycle removed from consumers (ADR 62), and arguably its
root: the habit that taught consumers to enumerate mino's sources. The
cycle asked whether to unify mino's internal lists as part of the same
work.

## Decision

Leave the bootstrap-Makefile-versus-`builtin.clj` duplication untouched
in the amalgam-migration cycle. That cycle's scope is the consumer
boundary. The internal dedup is a separate refactor with its own
constraint: the bootstrap Makefile must build mino before the task runner
exists, so it cannot simply read `lib-srcs`. It blocks no consumer. The
amalgam already carries the single wired manifest (`lib-srcs`) plus the
guard, so consumers depend on one source of truth regardless of mino's
internal duplication.

## Consequences

The amalgam-migration cycle stays small and focused on the four
consumers. mino keeps two internal source enumerations for now; a
mino-internal reorg still touches both the bootstrap Makefile and
`builtin.clj`, but that is a maintainer cost inside one repo, not a
cross-repo break. A follow-on cycle can unify the lists, for example by
emitting the bootstrap Makefile's source set from `lib-srcs` or having
both read a generated manifest, removing the last instance of the
pattern.

## Alternatives

Unify now, in the same cycle. Attractive because it kills the root cause
in one pass. Rejected: it couples a cross-repo consumer migration to a
mino-internal build refactor with a real bootstrap-ordering constraint,
enlarging the blast radius and delaying the consumer fix that already had
proven spikes.

Never unify. Rejected: the internal duplication is a live maintenance
cost and the natural next cycle; deferring is not abandoning.
