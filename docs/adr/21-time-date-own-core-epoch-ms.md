# ADR 21: Time and date, own civil core, epoch-ms instants

Date: 2026-08-20

## Context

mino scripts have no time library beyond monotonic clocks (`nano-time`,
`time-ms` in io.c) and the clojure.instant port that reads `#inst`
literals into component maps. The known-issues tracker (ki-13) rates
time/date the next built-in gap after the HTTP client. Scripts parse
timestamps from APIs, format headers, add days and months, and print
human-readable differences; today each script hand-rolls that and gets
it wrong.

The ecosystem evidence is consistent. Babashka wraps java.time and
inherits a type zoo (Instant, ZonedDateTime, LocalDate, Duration,
Period) plus a full IANA database it never asked to maintain; the zoo
is the top complaint. Janet ships roughly 150 lines of C (os/time,
os/date, os/mktime, os/strftime) and that covers 90 percent of
scripting value; its most-cited wart is zero-based months. Lua has no
parser, so every script hand-rolls os.date patterns; its top footgun
is os.clock being CPU time while scripts assume wall time. The
load-bearing subset everywhere: epoch now, a parser, add days/months,
human diff, broken-down fields.

The vendor option was surveyed and rejected on verified evidence.
Every candidate license was checked from primary sources this run:
musl is MIT but its date code is two tiny integer functions (algorithm
reference, not a library); HowardHinnant/date is MIT but C++11 with a
242 KB header; c-dt is BSD-2 but 17 file pairs, dormant since 2015,
and parses no RFC forms; TimSC/iso8601lib is MIT but parse-only; SQLite
date.c is public domain but welded to sqliteInt.h; GNU dateutils is a
GPL-mixed CLI suite; glibc and newlib are LGPL. Nothing vendorable
covers the surface. A spike proved the own-core alternative: 432 lines
of pure C99 integer math (Hinnant's civil algorithms as published
papers, no code copied) passing dense round trips over years 1-9999,
200 thousand random epochs against gmtime_r, RFC 1123 byte-identical
to strftime across 138 thousand samples, strict C99 -Werror clean on
cc and on the zig Windows cross. The spike is preserved in the
time-date run directory; the land copies its math byte for byte.

## Decision

Own civil core in C, one vocabulary in plain data:

- **Canonical instant: epoch milliseconds as int64.** This matches
  `inst-ms` and the JS ecosystem, and every Clojure epoch-ms idiom.
  Wall clock reads: `(now)` epoch-ms, `(now-s)` epoch-seconds. CPU
  time: `(cpu-ms)`. The monotonic clock stays `nano-time` (distinct
  clock, distinct name; conflating them is the Lua footgun).
- **Broken-down time is a plain map:**
  `{:year :month :day :hour :min :sec :ms :wday}` with 1-based months
  (Janet's wart rejected), computed `:wday` (0=Sunday). Converters:
  `epoch->time-map`, `time-map->epoch`. Validation is strict: unknown
  keys throw, field ranges throw, `:wday` when supplied must match
  the computed weekday, missing time-of-day fields default to zero.
  An optional `:offset-min` shifts the map to a fixed offset; it is
  data in the map, applied by the converters.
- **Representable range: years 1 through 9999** (the 4-digit ISO
  year). Outside the range every prim throws `:time/range`. Negative
  and expanded years are out of scope.
- **UTC plus fixed numeric offsets only. No IANA named zones.** A
  fixed offset is arithmetic, not a database. The additive path if
  named zones are ever genuinely needed: vendor the public-domain
  tzdata the way the Mozilla roots are vendored, as a conversion
  layer over epoch-ms. That adds a capability without breaking one
  map or one prim, which is the whole reason the vocabulary is
  offsets-only now.
- **One parser, three format families.** `(parse-time s)` accepts
  ISO 8601 / RFC 3339 (date-only, datetime, `T`/`t`/space separator,
  optional seconds, fractional seconds preserved to milliseconds and
  truncated past three digits, `Z`/`z` and `+-HH:MM`/`+-HHMM`
  offsets) and the RFC 1123 / 2822 comma form (optional day name,
  1-2 digit day, case-insensitive month and zone names, 4-digit
  years, zones GMT/UT/UTC or `+-HHMM`). Leap second 60 is accepted
  and folded to 59; 61 rejects. Trailing junk rejects. Input is
  capped at 64 chars. Returns
  `{:epoch-ms :offset-min :format :date-only?}`. A day name that
  contradicts the date rejects. Named non-UTC zones (EST and
  friends) reject: they are ambiguous without a database. Errors are
  classified `:time/parse`.
- **Formatting is keywords, not patterns.** `(format-time ms fmt?)`
  with `:iso8601` (default; emits `.SSS` only when the millisecond
  part is nonzero), `:iso8601-date`, `:rfc1123` (HTTP Date; always
  GMT), `:rfc2822` (numeric offset form), plus an optional offset
  argument for the offset-capable forms. No strftime-style pattern
  strings; the pattern zoo is a locale trap and a second parser to
  fuzz. Custom strings compose from the time map and `str`.
- **Calendar arithmetic:** `add-days`, `add-months` (day clamping:
  January 31 plus one month is February 28 or 29), `days-between`,
  `months-between` (whole calendar units, floor semantics),
  `leap-year?`, `days-in-month`, `weekday`. Days are exact 86400000 ms
  because the model has no DST.
- **`(human-diff a b?)`** renders the largest unit under 30 days as
  full words ("3 days ago", "in 5 minutes"), then calendar months,
  then years; sub-second is "just now" / "in a moment"; `b` defaults
  to `(now)`. The exact thresholds are pinned by the tests.
- **Errors** are classified `:time/parse`, `:time/range`,
  `:time/field` on the house ex-info shape. Malformed input never
  normalizes silently (musl mktime normalizes garbage; we throw).
- **No locale anywhere.** Month and day names in the RFC formats are
  fixed English tokens per those RFCs.
- **Capabilities: ungated.** Reading a clock is info-only, cheap,
  and side-effect free, the same reasoning as Janet's os/time. The
  sandbox preset loses nothing that mutates or exfiltrates.

The namespace `mino.time` wraps the prims with idiomatic names
(`parse`, `format`, `add`, `diff`, `human`, `today`), composes
multi-unit adds, and interops with clojure.instant: `from-inst`
converts an inst map to epoch-ms (strict day-in-month validation,
nanoseconds truncated to ms), `to-inst` converts back carrying the
`:mino/instant` marker so `pr-str` prints a `#inst` literal. HTTP
integration stays data-only: Date and Last-Modified header strings
are parseable via `parse-time`, JSON ISO strings stay strings (no
auto-coercion).

## Consequences

- The civil core lands in `src/prim/time.c` as pure static C99
  functions with provenance comments (Hinnant's papers as algorithm
  source, musl as cross-check reference); no third-party license
  entries change.
- The parser is untrusted-input surface: it ships seeded garbage and
  mutation fuzz tests (parse-or-classified-throw, never crash) and
  round-trip properties (every output of format-time parses back to
  the same instant), the codec test exemplar discipline.
- `clojure.instant` is untouched: `#inst` still reads to component
  maps. mino.time is a layer over the same epoch-ms the instant
  functions produce.
- No IANA data ships, so no tzdata update ritual, no zone-expires
  surfaces, and the binary stays free of a megabyte of timezone
  database.
- Superfluous precision (nanoseconds) is truncated at the mino.time
  boundary by design: the vocabulary is epoch-ms, and carrying more
  invites a second instant representation.
