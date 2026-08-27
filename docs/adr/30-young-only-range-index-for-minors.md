# ADR 30: Minor collections touch a young-only range index

Date: 2026-08-26

## Context

The range index is one sorted array (gc.ranges) plus a small unsorted
pending buffer, mapping payload address spans to owning headers for the
conservative stack scan and interior-pointer marking. Every minor
collection maintains the whole array: gc_range_compact_after_minor_mark
walks one entry per live header, young and old alike, and
gc_range_merge_pending pays O(ranges_len + K) worst case folding new
allocations in. Under a big live tree the cost is old-proportional:
measured 2M entries walked per minor, with the json corpus parse
profile spending all of its collection time in the compact. This is
the remaining driver of ki-21: Linux shard-2 suite RSS peaks at 3.49GB
against a 3GB gate, and the mac suite runs about 125s against a 78s
baseline. Generation lives in gc_hdr_t.gen, not in the index entry;
gc_find_header_for_ptr never reads generation from the index, so which
array a pass walks changes no lookup semantics.

## Decision

The index splits by generation into a young pair ranges_y +
ranges_y_pending and an old pair ranges_o + ranges_o_pending, owned by
a new src/gc/ranges.c translation unit. Minor-side merge and compact
walk young data only. A promoted header's entry rides one cycle in the
young array; the next compact routes entries whose header flipped OLD
into a sorted ranges_o_pending that folds at index rebuild.
promotion_age defaulted to 1 when this ADR landed; the follow-up
measurement (2026-08-27, recorded in docs/RANGE_INDEX.md) raised the
default to 2 on the split index: promotion volume and major count fall
about 10x on the json/regex workload, the shard-2 glibc peak-RSS top
cluster drops under the 3 GiB gate with no measured regression, and
p99 pauses shrink. Age 3 was measured and rejected (peak-RSS tail
regressed). The raise rides the split; alone it would not remove the
O(live) term the split removes.

## Consequences

- Ptr-to-header lookup pays up to two binary searches (young array,
  old array, sorted old pending) plus a linear scan of the small young
  pending, instead of one search. Lookup is not the measured cost; the
  per-minor compact walk is.
- Young array size is nursery-bounded. Old pending is bounded by
  between-major promotion volume, with a generous hard cap and an
  amortized fold if that volume ever exceeds it.
- One more translation unit owns the index; roots.c keeps root
  enumeration and the conservative scans.

## Alternatives

Raising promotion_age alone keeps young sets smaller but grows the OLD
population the compact still walks; it does not remove the O(live)
term, and it widens the remset window the one-cycle safety net has to
cover. Remembered-set or card marking addresses old-to-young edges,
which is marking work; the compact is index maintenance, and
reclaiming dead young slots still needs an O(n) pass or accepts
unbounded growth. Tombstone or epoch deferral postpones the walk to a
fold point but keeps the eventual O(n) pass and allows unbounded entry
growth between folds.
