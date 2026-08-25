# ADR 27: Timezones over embedded tzdata

Date: 2026-08-25

## Status

Accepted (follow-on to ADR 21)

## Context

ADR 21 delivered the civil-time core on one deliberate omission: "UTC
plus fixed numeric offsets only. No IANA named zones... The additive
path if named zones are ever genuinely needed: vendor the public-
domain tzdata the way the Mozilla roots are vendored, as a conversion
layer over epoch-ms." Scripts now parse API timestamps from other
continents, render local wall times, and answer "what offset is Oslo
in August", and each script that hand-rolls it gets DST wrong.

The requirement is exactly the additive layer ADR 21 reserved for:
the civil core (the nine time-map keys, the parser's strictness, the
formatters, epoch-ms) must not move; named zones arrive as one more
way to state an offset. The Mozilla roots precedent
(src/vendor/bearssl/mozilla-roots.pem -> tools/gen_ca_roots.clj ->
committed roots.c) supplies the vendoring shape: a committed data
snapshot, a generator run on demand as a maintenance task (never a
build step), and a compact compiled-in blob the runtime consumes
directly.

The behavior oracle is python3 zoneinfo on this machine (the campaign
oracle rule; vectors derived by running it, never from memory). One
oracle fact shaped the design: python evaluates the TZif POSIX footer
for instants past the last stored transition (America/New_York in
year 2500 answers -04:00 in July), so a blob of transitions alone
cannot cover ADR 21's years 1..9999 correctly.

## Decision

### The surface (additive over ADR 21)

A `:zone` option on the existing converters, taking either a fixed
offset in minutes (an integer, the ADR 21 arithmetic path) or an IANA
zone name (a string, or a keyword whose text is the name):

- `(epoch->time-map ms {:zone z})` renders the map at the zone's
  offset at that instant; the map carries the resolved `:offset-min`
  exactly as the integer-offset form does. The nine-key map surface
  is unchanged: a zone name is never stored in a time map.
- `(time-map->epoch m {:zone z})` reads the map's fields as local
  wall time in the zone.
- `(parse-time s {:zone z})` interprets an offset-less input (a
  naive datetime or a date-only string) in the zone; an input that
  carries its own offset together with `:zone` is a conflict and
  throws `:time/field`, the strictness stance (explicit beats
  implicit, and the RFC forms always carry a zone token).
- `(format-time ms fmt? {:zone z})` renders the offset-capable
  forms at the zone's offset; `:rfc1123` stays always-GMT and
  rejects a zone like it rejects an offset.
- `(zone-offset-mins z ms)` is the one new prim: the zone's offset
  at an instant, in minutes. The option map is accepted at the
  existing option positions (an argument that can only be a keyword
  fmt or an integer offset), so no arity changes and no positional
  booleans.

Unknown names throw `:time/zone` carrying the name (ex-info with
`:zone` in the data from the mino.time facade). Zone prims stay
info-only and ungated in the floor domain beside the rest of
`k_prims_time`: a database lookup mutates nothing and exfiltrates
nothing.

### The data

The snapshot is the 598 canonical TZif files of the host
/usr/share/zoneinfo (macOS carries no symlinks, so canonical names
only; aliases like US/Eastern are not in the snapshot and resolve as
unknown). The files are public domain (IANA tzdata). They are
vendored as one committed text bundle, `src/vendor/tzdata/
zoneinfo.bundle`: sorted `name<TAB>base64(tzif)` lines, so the
generator reads text (the roots-generator constraint: mino reads the
snapshot through slurp and base64-decode) and mac/linux TZif drift
cannot move the tests.

`./mino task gen-tzdata` (src/vendor/tzdata/tools/gen_tzdata.clj)
parses the bundle's TZif v2 64-bit blocks at generation time and
emits `src/prim/tzdata_blob.c` + `tzdata_blob.h`, both committed.
Nothing parses TZif at runtime; the runtime zone database is the
blob and nothing else.

The blob is little-endian with a fixed layout: a name-sorted zone
table over a NUL-terminated name block, a stream table, i32 offset
tables, transition arrays as sign-extended 40-bit absolute seconds
(random access, so lookup is a plain binary search) plus parallel
u8 type indices, and NUL-terminated POSIX footer strings. Zones
with identical (transitions, types, footer, initial type) share one
stream: 599 zones collapse to 339 streams, and the compiled data
lands at 171 KB (598 zones, 341 streams). Per zone the type before the first transition
is the first non-DST type (the RFC 8536 rule python follows);
macOS files carry no sentinel transitions.

### The semantics

- UTC->offset: binary search the transition table; after the last
  stored transition the zone's POSIX footer governs (std/dst offsets
  plus M/J/day rules with w/s/u suffixes, evaluated with the civil
  core), which is what makes years 2038..9999 answer correctly.
- local->UTC (parse and time-map->epoch in a zone): fold-0
  semantics, matching python zoneinfo's default. In a fall-back
  overlap the first occurrence wins (the earlier instant, the
  pre-transition offset); in a spring-forward gap the pre-transition
  offset is used, so the nonexistent wall time maps forward past the
  gap. Beyond the table the footer's candidate offsets are tried
  larger-offset-first with the same fallback.
- Offsets are minute-granular, the ADR 21 vocabulary: the blob
  carries seconds, resolution rounds to the nearest minute, and the
  epoch math uses the rounded minutes so maps round-trip exactly.
  The only divergence this creates is sub-minute LMT offsets before
  roughly 1900 (Oslo's +00:53:28 rounds to +00:53); the vocabulary
  is minute-based by ADR 21 and the error is under half a minute in
  the railway era.
- DST is nothing but transitions: no rule engine runs where table
  data exists, and the footer only governs where the table ends.

### The update ritual

Generation is a maintenance task, not a build step (the mozilla
roots precedent): fetch or copy a tzdata snapshot over the bundle,
rerun `./mino task gen-tzdata`, commit bundle and generated files
together. The generator is deterministic by construction (sorted
input, no timestamps) and stamps the bundle's sha256 into the
generated header; running it twice is byte-identical, which the
determinism check pins.

## Consequences

- ADR 21's surface is untouched: every existing test, map, and prim
  behaves identically; `:zone` is one more way to state an offset.
- The binary grows by the blob (171 KB) and the footer evaluator;
  no tzdata update ritual is forced (the snapshot ages gracefully;
  footers keep future instants right regardless).
- mino.time gains `in-zone` and `zone-offset-mins` sugar over the
  prims, documented against inst-ms: the instant is the same
  epoch-ms everywhere, a zone only chooses the wall clock you read
  it on.
- Zone-name lookup is untrusted-input surface: comparisons are
  length-bounded against NUL-terminated names inside the blob, and
  the echoed name in errors is capped, per the check-security
  stance.
- Sub-minute historical offsets and missing aliases are the two
  recorded approximations; both are pinned in the tests as documented
  behavior rather than silent gaps.
