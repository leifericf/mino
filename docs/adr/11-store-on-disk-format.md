# ADR 11: Store on-disk format — EDN text with version header, line-delimited WAL

Date: 2026-06-27

## Context

ADR 10 shipped an EAVT fact store with snapshot-on-checkpoint durability.
The v1 snapshot format is bare EDN text: `mino_print_to` writes the db
value, `mino_eval_string` reads it back. There is no WAL — transactions
between checkpoints are lost on crash.

Wave 1 adds a WAL for per-transaction durability and needs a stable
on-disk format for both snapshot and WAL files. The format must be
forward-compatible (future versions can add binary encoding without
breaking existing files) and debuggable (a developer can `cat` or
`head` the file and read it).

A binary format (Nippy-style tagged encoding) was considered for
compactness and speed. Nippy is not available in mino, and writing a
bespoke binary encoder/decoder is ~300 lines of C for a benefit that
does not materialize at KB-scale store sizes.

## Decision

**Snapshot files**: a 1-byte format-version header followed by EDN text.

```
byte 0:  0x00  (format version = EDN text follows)
bytes 1+: EDN text of the db value, as produced by mino_print_to
```

On read, the first byte is checked. If it is `0x00`, the rest of the
file is parsed as EDN. If it is any other byte, the entire file is
parsed as EDN (backward compatibility with v1 snapshots that have no
header — the first character of a bare-EDN snapshot is always `{`,
which is `0x7B`, never `0x00`).

Future versions reserve `0x01` for binary encoding. The version byte
is the only mechanism needed to evolve the format.

**WAL files**: line-delimited EDN, one transaction per line.

```
{:tx 0 :instant 1719480000123 :tx-data [:db/add 1 :name "Alice"]}
{:tx 1 :instant 1719480005456 :tx-data [[:db/add 2 :name "Bob"] [:db/add 3 :name "Carol"]]}
```

Each line is a map with `:tx` (transaction number), `:instant`
(wall-clock milliseconds), and `:tx-data` (the original transaction
input in any form accepted by `parse-tx-data`). On replay, `apply-tx`
is called for each entry in order.

**WAL path**: `<snapshot-path>.wal` (e.g., `data.db` → `data.db.wal`).

**Torn-write recovery**: on open, the WAL is read line by line. If a
line fails to parse (torn write from a crash mid-append), parsing
stops and the entries read so far are used. The malformed trailing
line is discarded.

**Checkpoint protocol**:

1. Write the snapshot (version header + EDN) to the path.
2. Delete the WAL file.

If the process crashes between 1 and 2, the snapshot has the new state
and the WAL has stale entries. On reopen, the tx-number filter skips
WAL entries whose `:tx` is below the snapshot's `:tx`, preventing
double-application. This is safe even for `:many` cardinality
attributes.

**Crash safety guarantee**: data committed to the WAL (via
`store-commit*`) survives a crash. Data committed in-memory but not yet
WAL-flushed is lost. The WAL append (`fopen("ab")` + `mino_print_to` +
`fputc('\n')` + `fclose`) flushes to OS buffers on `fclose`; an explicit
`fsync` is the host's responsibility.

## Consequences

- Snapshot and WAL files are human-readable. A developer can inspect
  them with standard text tools.
- The format is forward-compatible via the version byte. Adding binary
  encoding later requires only a new version constant and a conditional
  in the read path.
- WAL replay uses the same `apply-tx` pipeline as live transactions, so
  schema validation and cardinality handling are applied identically.
  There is no separate replay code path.
- Per-transaction WAL entries store the original `tx-data`, not
  pre-parsed facts. This keeps the WAL compact and avoids coupling the
  on-disk format to the internal fact representation.
- String values containing literal newlines would break the
  line-delimited WAL format. `mino_print_to` escapes newlines as `\n`
  in string literals, so this is safe as long as the printer correctly
  escapes. (Verified: the printer uses `\\n` for control characters.)

## Alternatives

- **Binary encoding (Nippy-style).** More compact, faster to parse.
  Rejected for v1: ~300 lines of C for a tagged binary encoder/decoder,
  no measurable benefit at KB-scale store sizes, loses debuggability.
  The version byte leaves the door open.
- **Full-db WAL (write entire db value per transaction).** Simpler
  replay (read last line). Rejected: WAL grows O(transactions × db_size)
  instead of O(transactions × tx_size). For 3600 transactions between
  checkpoints with a 10 KB db, that is 36 MB vs 360 KB.
- **External WAL engine (Hirsla).** Rejected per ADR 02 and ADR 10.
