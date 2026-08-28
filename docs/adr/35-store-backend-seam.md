# ADR 35: Store backends are plain maps of fns behind a five-op seam

Date: 2026-08-29
Status: accepted

## Context

mino.store has one durability story: a file at a path, written and read
by C primitives (store-open*, store-commit*, store-checkpoint*,
store-close*, store-clock*, store-read-snapshot*, store-read-wal*), with
the db value a persistent map and all db logic (replay, schema,
compaction) in Clojure (ADR 10, ADR 11). store/open takes a path or
nothing; there is no way to substitute another storage medium. Other
media (sqlite, postgres) are wanted eventually; the seam should exist
before any second backend, so routing and format preservation can be
proved separately from new storage.

## Decision

A backend is a plain map of fns tagged :kind and validated by backend?:
mino has no defprotocol, and store.clj already dispatches through maps
of fns and keywords. A backend is a dumb segment store: it owns bytes
and their durability ordering, never db logic. Five ops:

- `:initial` takes no args and returns the snapshot segment as a db
  value, or nil when absent.
- `:wal-entries` takes no args and returns the WAL segment as a vector
  of parsed tx-info (nil when absent); a torn final line stops the read
  at the parse edge.
- `:commit` takes [conn new-db tx-info], owns the
  durable-append-before-publish ordering, and returns the published db.
- `:checkpoint` takes [conn], writes the snapshot, deletes the WAL.
- `:close` takes [conn], performs the final checkpoint, releases the
  handle.

Two backends ship. `:memory` is the default: `:initial` and
`:wal-entries` return nil; `:commit`, `:checkpoint`, `:close` are plain
publish or no-ops. `:file` wraps the existing C prims as its native
edge, one prim per op, zero C changes. The ADR 11 format holds
byte-for-byte: the seam reroutes calls, never bytes.

The file backend wraps C prims because mino cannot express its contract
in pure Clojure: spit lacks fsync, there is no rename primitive for the
atomic tmp-plus-rename checkpoint, and there is no data-only EDN reader
(segment parsing rides the C evaluator). A non-file backend must bring
its own durable put.

open binds a conn to its backend through an atom registry keyed by conn
(the listener-registry pattern); close deregisters. GC-finalized conns
leak registry entries exactly as they leak listener entries today:
accepted precedent, not a new hazard.

## Consequences

- A third party supplies a backend by passing a map with a fresh :kind
  keyword and the five ops; malformed maps (missing op, non-keyword
  or missing :kind, non-fn op, non-map) throw classified errors at
  validation.
- The db value stays a persistent map and replay, schema, and
  compaction stay in mino.store; a backend cannot drift db semantics.
- Durability ordering is stated once at the seam instead of at every
  call site.
- The registry costs one atom lookup per transact and one leaked entry
  per abandoned conn, matching the listener registry.

## Alternatives

- **defprotocol Backend.** Idiomatic Clojure, but mino has no
  defprotocol and store.clj already uses maps of fns; protocol
  machinery for two implementations is weight without benefit.
- **No seam, backend fns at call sites.** Cheapest today, but every
  future backend forks open, transact, checkpoint, and close.
- **Pure-Clojure file backend.** Not expressible without new C surface
  anyway (fsync, rename, safe EDN read); wrapping the existing prims is
  the smaller native edge.
