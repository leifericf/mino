# ADR 42: mino.store is the in-box database; sqlite waits behind the seam

Date: 2026-09-03

## Context

mino ships a durable, transactional fact store: mino.store, an EAVT
store with an EDN snapshot plus WAL on disk (ADRs 10, 11, 16) and a
five-op backend seam that lets any medium supply bytes and durability
ordering without touching db logic (ADR 35). Users of other scripting
runtimes frequently ask for an embedded sql engine; several runtimes
now bundle one. The reference engine is roughly 150 thousand lines of
C, an order of magnitude larger than mino's entire runtime, and mino's
pitch to embedders is a small ANSI C dependency. The seam in ADR 35
was cut before any second backend existed, precisely so a heavier
medium could arrive later without a rewrite.

## Decision

mino.store is the answer the box gives for durable structured data;
no sql engine is vendored. When an sql-file backend is wanted, it
arrives through the ADR 35 seam as an optional backend that embedders
compile in, never as a default dependency, and that work is scoped and
measured on its own. Should demand center on reading databases other
programs produce rather than on storage, the recorded fallback is a
read-only database-file reader: a native prim in the ADR 23 to 28
reader tradition, parsing the file format directly at a small fraction
of the engine's weight. Neither piece is scheduled; this record fixes
the shape either takes.

## Consequences

- The tiny-C embed pitch holds: a default build carries no sql engine.
- Scripts that need durable structured data use mino.store; scripts
  that need to query an existing sql database shell out or wait for
  the read-only reader.
- Any future backend inherits the ADR 35 contract: bytes and
  durability only, db semantics stay in mino.store, so an sql-backed
  store cannot drift the data model.
- The demand signal stays open: this record settles how, not whether
  never; a later record can schedule either piece without reversing
  this one.

## Alternatives

- **Vendor the engine.** The strongest compatibility story, one users
  ask for by name, and the file format is the de facto interchange
  standard for local structured data. Rejected: the dependency would
  dwarf the runtime it joins and contradict the small-C promise made
  to embedders.
- **Read-only database-file reader now.** Covers the interchange case
  at reader cost, fits the native-reader record. Not taken now:
  demand so far names the engine out of habit, not a concrete file
  to read; building it ahead of a real use risks an unused surface.
- **Nothing on record.** Cheapest today. Rejected: the question
  recurs, and without a record each recurrence reopens the vendoring
  debate the seam already answered.
