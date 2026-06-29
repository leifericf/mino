# ADR 13: References and reverse indexes

Date: 2026-06-28

## Context

mino.store's EAVT model (ADR 10) uses explicit entity-ids as the `:e`
and, potentially, as the `:v` of any attribute. Without a distinguished
reference type, references are untyped: any value could be an eid, no
validation catches a stale or wrong-type value at apply time, and there
is no way to traverse relationships backward without a linear scan of
every entity in the db.

For the agent-memory and game-NPC use cases that motivated the store,
relationships are first-class. An agent remembers which NPCs it has
spoken to; an NPC remembers which quests it has issued. The store must
answer "who refers to this entity?" cheaply, and it must not let the db
drift into a state where a `:ref` value points at an eid that no longer
exists.

## Decision

Add a schema type `:ref`, an automatically maintained reverse index,
and nil-on-target-retraction semantics.

- **`:ref` schema type.** An attribute declared `{:value-type :ref}`
  validates, at apply time, that each `:v` it would store is an existing
  eid in the current db. Storing a dangling or non-eid value throws at
  apply time rather than being silently persisted.
- **`:cardinality :many` interacts with `:ref`.** A `:many` `:ref` holds
  a set of eids; each element is validated independently. The reverse
  index records one source→target pair per element.
- **Automatic reverse index.** The db exposes
  `{:reverse {ref-attr {target-eid #{source-eid}}}}`. It is **rebuilt
  lazily** by `store/referring` (which eids does this attr point at?)
  and `store/referred-by` (which eids point at this one through this
  attr?) on each call from the current `:entities` view, not maintained
  incrementally per transaction. See the revision note below.
- **Nil-on-retraction, no cascade.** When an entity ceases to exist
  (its last attribute is retracted), every `:ref` attribute elsewhere
  that pointed at it is nilled automatically — retracted from the
  source entity's `:v` set (or, for `:one`, the attribute is removed).
  The reverse index makes this cheap: it lists exactly the source facts
  that need rewriting. Source entities themselves are not retracted.
- **Lazy correctness over eager work.** The reverse index is a pure
  function of the materialized db view, recomputed when a reverse
  query needs it. There is no incremental per-tx maintenance and no
  persisted index to drift; the index is always consistent with
  `:entities` because it is derived from it on demand.

## Consequences

- Referential integrity is enforced: dangling refs throw at apply time,
  not silently stored. A caller who writes `[:db/add e :friend 9999]`
  against a db with no entity `9999` gets an exception, not a time bomb.
- Reverse traversal rebuilds the index on each call, so `store/referring`
  and `store/referred-by` are O(entities) per call, not O(1). This is
  acceptable for v1's stated scale (agent-memory and game-NPC stores with
  modest entity counts); incremental maintenance is the obvious follow-up
  if a workload makes reverse queries hot. See the revision note below.
- Entity retraction is safe: no dangling refs remain after a target is
  removed. A reader never has to defensive-check whether a `:ref` value
  resolves.
- No cascade delete. Retracting an entity does not retract entities
  that refer to it. Callers who want cascade must explicitly retract the
  sources, typically by reading `store/referred-by` first.
- The reverse index is not persisted: it is derived from `:entities` on
  demand, so the snapshot and WAL formats (ADR 11) do not carry it. The
  persisted db value is just the entities and the log.
- The reverse index adds a constant per-`:ref`-fact memory cost only
  while a reverse query holds the rebuilt index; it is not retained on
  the db value between queries.

## Alternatives

- **Untyped references.** Any attribute value could be an eid; integrity
  is the caller's problem. Rejected: no integrity guarantee, no reverse
  index without an O(n) scan, and every reader has to defensive-check.
  This is the failure mode the store exists to prevent.
- **Manual reverse index via `:indexes`.** Let callers opt in to
  maintaining a reverse map by hand, the way they would maintain any
  derived index. Rejected: too easy to forget, doesn't validate the
  forward direction, and the index drifts from the facts the moment a
  transaction forgets to update it. The whole point of an EAVT store is
  that derived views are derived, not hand-maintained.
- **Cascade delete on retract.** When an entity is retracted, retract
  every entity that refers to it, transitively. Rejected for v1:
  surprising, hard to undo (the retracted sources are gone from the
  fact log only via retractions, not additions), and the transitive
  case can delete far more than the caller intended. A later
  `:on-delete :cascade` per-attribute opt-in is the door left open.
- **Reverse index as a separate C-side structure.** Build it in C next
  to the db value rather than as a Clojure-side map. Rejected: the db
  value is plain EDN data, snapshot and WAL serialize it for free, and
  `store/referring` / `store/referred-by` are pure functions of that
  data. A C-side index would duplicate state, complicate clone, and
  break the "host-owned handles, GC-owned values" split from ADR 10.

## Revision (2026-06-29)

The original Decision and Consequences described the reverse index as
maintained incrementally on every transaction, carried in the snapshot
and WAL, and queried in O(1). The shipped v1 does not do this:
`store.clj` rebuilds the reverse index lazily inside `referring` and
`referred-by` (`build-reverse-index` over `:entities`), does not retain
it on the db value, and does not persist it. The bullets above have
been corrected to match the implementation. Rationale: incremental
maintenance adds per-tx bookkeeping and snapshot/WAL format surface
that v1 does not need at its stated scale; a lazy derived index keeps
the persisted db value small and the referential-integrity guarantee
intact. Incremental maintenance is the documented follow-up if reverse
queries become hot. Recorded during the unpushed-store-image audit.
