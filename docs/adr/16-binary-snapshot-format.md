# ADR 16: EDN text is the stable on-disk format for mino.store v1

Date: 2026-06-28

## Context

ADR 11 fixed the store's on-disk format as a one-byte version header
followed by EDN text, with `0x00` denoting "EDN text follows" and
`0x01` reserved for a future binary encoding. The WAL is
line-delimited EDN, one transaction per line.

EDN text has carried the store through v1. Snapshots are human-readable,
debuggable with `cat`, `head`, and `grep`, and tolerant of additive
schema change because the reader skips unknown keys. Profiling so far
shows store open and checkpoint dominated by file I/O and GC allocation
on the parsed structure, not by the text parser itself. No embedder has
reported snapshot size or parse time as a constraint.

A binary format would pack integers, tag values, and deduplicate common
substrings, reducing both parse time and file size. The question is
whether v1 should commit to shipping it.

## Decision

EDN text (version byte `0x00`) is the stable on-disk format for
mino.store v1. A binary format is not committed for v1.

The version byte remains the evolution mechanism, exactly as ADR 11
specified. Byte `0x01` stays reserved for a binary encoding; the reader
path that checks the first byte stays in place. Nothing in v1 writes
`0x01`, and no code branches on it beyond the version check.

This record reinforces ADR 11. It does not change the format, the
version-byte scheme, or the torn-write recovery protocol. It records
the v1 scope decision so that "why no binary in v1" has a citable
answer.

## Consequences

- v1 ships with one on-disk format. There is no second code path to
  test, no encoder/decoder pair to keep in sync, and no migration tool
  to ship.
- The switching criteria for adding binary are explicit and measured,
  not vibes-driven. Binary lands when at least one holds:
  - profiled EDN parse cost exceeding 10% of store-open time on a store
    with more than 10K entities; or
  - concrete embedder demand for smaller or faster snapshots on a
    measured workload.
- The migration path is already designed in. A future ADR that
  allocates `0x01` writes the encoder, widens the reader's version
  switch, and ships a one-shot converter. Existing `0x00` files keep
  loading unchanged.
- Store files remain text-debuggable through v1, which matters for
  support, postmortem, and the deliberately-small tooling surface.

## Alternatives

- **Commit to binary for v1.** Tighter files and faster parse on paper,
  but premature: no profiling data justifies the encoder/decoder, the
  dual read path, or the migration tooling. Rejected.
- **Defer the decision without reserving the version byte.** Leaves the
  migration path underspecified. Reserving `0x01` costs nothing (one
  constant and one branch already in the reader) and keeps the format
  honest about its evolution mechanism. Rejected.
- **Hybrid: EDN snapshot, binary WAL (or vice versa).** Two formats in
  one store doubles the test surface and the debugging story for no
  measured gain. Rejected.
