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
- **Automatic reverse index.** The db carries
  `{:reverse {ref-attr {target-eid #{source-eid}}}}`. It is maintained
  incrementally on every transaction that adds or retracts a `:ref`
  fact. Reverse traversal is exposed via `store/referring` (which eids
  does this attr point at?) and `store/referred-by` (which eids point
  at this one through this attr?); both are O(1) index lookups.
- **Nil-on-retraction, no cascade.** When an entity ceases to exist
  (its last attribute is retracted), every `:ref` attribute elsewhere
  that pointed at it is nilled automatically — retracted from the
  source entity's `:v` set (or, for `:one`, the attribute is removed).
  The reverse index makes this cheap: it lists exactly the source facts
  that need rewriting. Source entities themselves are not retracted.
- **Lazy correctness over eager work.** The reverse index is updated as
  a side effect of applying a transaction, not as a separate rebuild.
  There is no "reindex" operation; the index is always consistent with
  the materialized db view.

## Consequences

- Referential integrity is enforced: dangling refs throw at apply time,
  not silently stored. A caller who writes `[:db/add e :friend 9999]`
  against a db with no entity `9999` gets an exception, not a time bomb.
- Reverse traversal is O(1) index lookup, not O(n) entity scan. The
  agent-memory "who has this entity in its queue?" query is constant
  time regardless of db size.
- Entity retraction is safe: no dangling refs remain after a target is
  removed. A reader never has to defensive-check whether a `:ref` value
  resolves.
- No cascade delete. Retracting an entity does not retract entities
  that refer to it. Callers who want cascade must explicitly retract the
  sources, typically by reading `store/referred-by` first.
- The reverse index adds a constant per-`:ref`-fact memory cost. For
  stores dominated by `:ref` relationships this is significant; for
  stores dominated by scalar attributes it is negligible. It is always
  proportional to the number of `:ref` facts, never to entity count.
- Snapshot and WAL formats (ADR 11) carry the reverse index as part of
  the db value. No format change is needed — the index is just data.

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
