# ADR 17: SLAD forward compatibility via skip-and-warn, plus an offline migration tool

Date: 2026-06-28

## Context

ADR 12 specified the save-lisp-and-die (SLAD) image format: a versioned
text file with a value pool, a roots section, and a CRC32 trailer. The
format is designed to grow: new `MINO_*` type tags and new payload
shapes arrive as mino adds value types, and the magic string embeds a
version number that bumps on incompatible change.

That design handles older-image-on-newer-runtime loading cleanly (the
newer reader knows the old version). It does not handle the reverse:
an image saved by a newer mino, loaded by an older mino. The older
reader encounters type tags and payload shapes it has never heard of,
and under a strict reader the whole load fails. Rolling upgrades
(canary a new build against images produced by the current build) and
graceful rollback (ship a new build, revert, keep the images the new
build already wrote) both need the older runtime to do something
better than refusing the file.

## Decision

The SLAD loader uses skip-and-warn for unknown type tags, and an
offline `mino_migrate_image(old, new)` tool is committed for runtime
upgrades. This amends ADR 12.

**Skip-and-warn on unknown type tags.** When the loader hits a value
line whose type tag it does not recognize, it substitutes nil for that
ID, records the ID and tag in a warning log, and continues. References
to that ID from other values resolve to nil. The image loads; the
unknown-typed values are lost; the loss is visible in the log.

**Offline migration tool.** `mino_migrate_image(old, new)` reads an
image with an older magic and rewrites it with the current magic,
re-encoding values into the current format. It is a host-invoked,
pre-deployment step, not something the loader does at open time. An
embedder upgrading runtimes runs the tool over its image library before
pointing the new runtime at them.

The identity table format from ADR 12 is unchanged. Skip-and-warn
operates on value lines; the ID-to-pointer table, the roots section,
and the CRC32 trailer behave exactly as before.

## Consequences

- An image saved by a newer mino loads on an older mino, with data loss
  on unknown-typed values clearly warned. Rolling upgrades and rollbacks
  degrade gracefully instead of failing hard.
- Unknown-typed values become nil transitively: any value that
  references an unknown ID also sees nil in that slot. The log is the
  authoritative record of what was lost; embedders running mixed
  versions should check it.
- The migration tool is the supported upgrade path. Embedders who want
  zero data loss across a runtime upgrade run it before switching; the
  skip-and-warn path is the safety net for when they do not, or cannot.
- Auto-migration is explicitly not done at load time. The loader never
  writes the image file; it only reads. This keeps the load path
  side-effect-free and the image file immutable from the loader's
  perspective.
- The quiesce protocol, the CRC32 integrity check, and the trust model
  from ADR 12 are unchanged. Skip-and-warn is a read-side tolerance,
  not a relaxation of the safety properties.

## Alternatives

- **Hard-reject unknown tags (strict reader).** Cleanest for the loader,
  but brittle for rolling upgrades and rollback: any unknown tag fails
  the whole load, even when the unknown values are irrelevant to the
  embedder's workload. Rejected.
- **Auto-migrate on load.** The loader rewrites the image in the current
  format as it reads. Rejected for two reasons: it mutates the image
  file at load time, which is surprising and irreversible, and it makes
  the load path do write I/O, breaking the clean read-only loader
  contract. Migration is a separate, explicit, host-driven step.
- **Versioned side-by-side images (keep N versions).** Pushes the
  problem to the embedder with no runtime help. Rejected: skip-and-warn
  plus a migration tool gives the same upgrade safety with one image
  per state.
