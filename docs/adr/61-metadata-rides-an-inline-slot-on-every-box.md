# ADR 61: Metadata rides an inline slot on every box, not a side table

Date: 2026-09-07

## Context

Every heap-allocated value carries an inline metadata slot. `struct
mino_val` (`src/values/layout.h`) places `mino_val *meta` as its second
field, right after the type tag and before the value union, so the slot
exists on every box whatever its type. `mino_meta` reads it and
`mino_with_meta` returns a shallow copy with the slot repointed
(`src/mino.h:1046-1047`, `src/prim/core/meta.c`). Meta access is a
single field load; attaching meta is a box copy.

Not every value type honors meta. `supports_meta` covers symbols, cons,
vectors, maps, sets, fns, and macros; `meta_readable` adds the
identity-tied types (atom, agent, ref, var) for read and in-place
`alter-meta!` (`meta.c:25-44`). Reader-built list cells do not even use
the slot for their source position: line, column, and file live in
dedicated `cons` union fields and a `{:line :column :file}` map is
synthesized on demand (`meta.c:54-74`), so the common list literal
pays nothing for source tracking beyond those fields. The question the
decomplection pass raised is whether metadata should stay in the inline
slot (fast access, a fixed per-box cost) or move to a side table keyed
by value identity (no cost on the meta-less common case, slower meta
ops, and an identity question for value-typed keys).

This is a measured decision. Measured on this build:

- `sizeof(struct mino_val)` is 80 bytes; the meta slot is 8 of them, at
  offset 8. The slot is 10% of every box, and removing it would not
  shrink the box below the next 16-byte alignment step in the common
  cases, so the realizable saving is smaller than the raw 8 bytes
  suggests.
- Values are meta-less until `with-meta` or `vary-meta` is applied
  explicitly. A fresh `[1 2 3]`, `{:k 1}`, or `"str"` all report `nil`
  meta; only the value returned by `with-meta` carries any. In ordinary
  evaluation the fraction of live boxes that ever carry meta is small:
  meta appears on values the reader annotates and on values a program
  explicitly tags, not on the cons, vector, map, and string boxes that
  dominate a running heap.

## Decision

Keep the inline slot. The common value is meta-less, so a side table
would win on memory only for boxes that never consult it, while every
meta read and every equality or print path that must ask "does this
carry meta" would pay a table lookup instead of a field load. The
inline slot makes the meta-less case a NULL-pointer check and the
meta-bearing case a direct dereference, which is the right trade when
meta reads are far more frequent than meta writes and the writes already
copy the box.

The measured 8-byte cost is real but bounded: 10% of an 80-byte box, on
boxes whose count is what it is regardless of meta, and immediates
(ints, chars, bools, nil) are tagged pointers that never box and never
pay it. The condition that would flip the decision is a workload where
the meta-bearing fraction is provably tiny and the box count is the
dominant memory pressure, and where shrinking the box by 8 bytes crosses
an allocation-size-class boundary that actually reduces resident memory.
Absent that measured pressure, the side table trades a hot-path field
load for a lookup and buys memory only on values that would not have
consulted the table anyway.

## Consequences

Meta access stays a single load and meta presence stays a NULL check, so
equality, hashing, and printing test for meta without indirection. Every
box pays 8 bytes whether or not it will ever carry meta, which is the
accepted cost. Identity-tied types keep in-place `alter-meta!` on their
own slot, which a side table keyed by identity could also serve but with
the added question of what identity means for value-typed keys that are
equal-but-not-same; the inline slot sidesteps that question entirely
because the slot travels with the box. If a future cycle measures the
box count as the binding memory constraint, this record is the place a
side-table ADR would supersede, and it names the measurement that
decision would need.

## Alternatives

A side table keyed by value identity, holding meta only for the values
that carry it. Its strength is that the meta-less common box drops 8
bytes. Rejected on the measurement: the saving lands on boxes that never
read the table, while the frequent meta reads and the presence checks in
equality and print grow a lookup, and the table raises an identity
question for value types (two equal vectors that are not the same box)
that the inline slot never has to answer. The trade is worse on the hot
path to save memory on the case that was already cheap.

A meta slot only on the types that support it, via per-type structs
rather than one union. Its strength is that atoms and the numeric boxes
that cannot carry meta would not reserve the field. Rejected because the
single `struct mino_val` body with a shared header is what lets the
runtime treat any value uniformly through one pointer, and specializing
the box per type to reclaim 8 bytes on some types would fracture that
uniformity across the collector, printer, and equality walkers for a
saving the measurement does not justify.
