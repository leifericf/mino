# Vendored tzdata snapshot

`zoneinfo.bundle` is the timezone database snapshot the timezone
prims (ADR 27) are generated from: the 598 canonical TZif files of
a macOS `/usr/share/zoneinfo` (macOS ships no symlinks, so there
are no IANA link aliases), one `name<TAB>base64(tzif)` line per
zone, sorted by name. Text form because the mino generator reads it
with slurp and base64-decode, the mozilla-roots PEM precedent.

The data is public domain: IANA tzdata (https://www.iana.org/
time-zones), as compiled into TZif by the host libc. No license
entries change.

Snapshot of record: 598 zones, 946352 bytes,
sha256 e03189e1595d7a30d510b334d058915d5797a0dd9f3e920025ba164b0382f6ef.

## Update ritual

Regeneration is a maintenance task, never a build step:

1. Copy a fresh canonical TZif set over the bundle (rebuild it from
   `/usr/share/zoneinfo` or an IANA release the same way: sorted
   names, base64 lines, no timestamps).
2. `./mino task gen-tzdata` -- regenerates
   `src/prim/tzdata_blob.c` and `src/prim/tzdata_blob.h`, stamping
   the bundle's sha256 into the header comment.
3. Run it twice and check the output is byte-identical (the
   determinism gate).
4. Commit bundle and generated files together, and update the
   snapshot-of-record line above.

The generator is deterministic by construction: sorted input, fixed
stream ordering by first appearance, and no timestamps anywhere.
