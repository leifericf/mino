# ADR 14: Schema migration

Date: 2026-06-28

## Context

mino.store's schema is set at `open` time and stored in the db value.
Once a store is open the schema is treated as immutable: every apply
path reads it to validate `:db/add` operations, and the reverse index
(ADR 13) is shaped by which attributes are declared `:ref`. Without a
migration API, changing the schema requires closing the store, editing
the open options, and reopening. That path loses the WAL (a reopen
reads the snapshot and starts a new log), silently drops the in-flight
db value, and pushes data migration onto the caller with no validation
support.

The store needs a controlled evolution path: validate the existing
facts against the candidate schema, optionally coerce values that would
violate it, and publish the new schema through the same transaction
machinery that publishes facts. Migration is not a special mode; it is
a transaction-shaped operation that swaps one db field.

## Decision

Add `store/migrate`, a programmatic API that validates existing data
against the new schema before publishing.

- **Validation-gated publish.** `store/migrate` walks every fact in the
  current db value and checks it against the candidate schema. A
  type-constraining change (narrowing `:value-type`, adding
  `:cardinality :one` where many values exist, declaring a `:ref` where
  a non-eid is stored) that conflicts with existing data throws
  `::migration-conflict` with the offending facts, before the db value
  is touched.
- **`:coerce` for in-place fixing.** The caller may pass
  `{:coerce {attr fn}}`. For each fact that would violate the candidate
  schema the fn is applied to its `:v`; the returned value replaces the
  old one in the candidate db. Coercion runs after validation collects
  the conflict set and before the publish check, so the final db is
  re-validated against the candidate schema.
- **`:force` for acknowledged breakage.** When passed, conflicting
  facts are retracted from the candidate instead of throwing. `:force`
  and `:coerce` combine: coerce first, force the remainder.
- **`:data` for structural migration.** A tx may be passed via `:data`
  and run on the candidate db after the schema swap but before publish.
  This is the path for splitting an attribute, renaming, or backfilling
  a derived field, all within the same migration call.
- **Atomic publish via `store-commit*`.** The migration result replaces
  the db value's `:schema` (and optionally `:indexed-attrs`) in one
  `store-commit*` call. On durable stores the commit writes to the WAL;
  the next checkpoint persists the new schema to the snapshot. A failed
  migration throws before any write, so the on-disk db is never left in
  a half-migrated state.

## Consequences

- Schema evolution is safe: violations surface before the schema
  changes, preventing the silent data corruption that reopen-and-edit
  invites.
- Coercion handles common widening moves (string-to-long,
  keyword-to-ref) without manual data editing or a separate repair
  script.
- `:data` keeps structural changes in the same atomic boundary as the
  schema swap, so an attribute split cannot half-commit.
- Migration is durable through the existing WAL and checkpoint path; no
  new on-disk format is introduced, consistent with ADR 11 and ADR 16.
- The `:entity-specs` and `:history` policies survive migration
  unchanged; they live alongside `:schema` in the db value and are not
  rewritten by the migration path.
- Migration is single-state, single-store, and synchronous, matching
  the isolation model from ADR 10. There is no cross-store migration
  tool.

## Alternatives

- **Schema-as-data (Datomic's `:db.install/attribute`).** Treat schema
  edits as ordinary transactions on bootstrap entities. Rejected as too
  complex for mino's map-based schema (ADR 10): the schema is a plain
  map read on every apply, and resolving it through a fact layer would
  add indirection on the hottest path for no gain at mino's cardinality.
- **Reopen with a new schema.** Close the store, edit the open options,
  reopen. Rejected: loses the WAL (a reopen reads the snapshot and
  starts fresh), performs no validation, and provides no coercion. It
  is strictly worse than `store/migrate` on safety and equal on
  durability.
- **Automatic type widening only.** Allow widening `:value-type` (e.g.
  `:keyword` to `:string`) without an API, rejecting anything else.
  Rejected as too narrow: it does not handle index changes (`:indexed`
  flips), cardinality moves, or the structural data migration that
  schema changes routinely need. Widening is a special case of
  `store/migrate` with no `:coerce` and no `:force`.
