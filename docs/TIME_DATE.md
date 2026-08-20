# mino.time

Time and date in plain data. Design contract:
`docs/adr/21-time-date-own-core-epoch-ms.md`. The full reference is
the docstring:

```
(require '[mino.time :as t] '[clojure.repl :refer [doc]])
(doc t/parse)
```

The namespace is ungated: reading a clock is info-only, so every
embedder has it. The representable range is years 1..9999; anything
outside throws `:time/range`. There is no named-zone database and no
locale anywhere; an offset is a fixed number of minutes east of UTC,
carried as data.

## The vocabulary

An instant is an integer: epoch milliseconds since
1970-01-01T00:00:00Z, the same value `inst-ms` produces.
Broken-down time is a plain map:

```
{:year 2026 :month 8 :day 20 :hour 3 :min 12 :sec 0 :ms 500
 :wday 4 :offset-min 0}
```

Months are 1-based. `:wday` is 0 = Sunday, computed. `:offset-min`
shifts the rendered fields; the map carries it so
`epoch->time-map` and `time-map->epoch` round-trip as data.

Every verb that takes an instant accepts three shapes: the integer,
a parse result, and a time map (`t/instant` coerces).

## Reading clocks

| Call | Returns |
|------|---------|
| `(t/now)` | wall clock, epoch-ms |
| `(t/now-s)` | wall clock, epoch seconds |
| `(t/monotonic-ms)` | monotonic elapsed ms (time code with this, not `now`) |
| `(t/cpu-ms)` | process CPU time, ms (a work metric, not a clock) |

The distinct monotonic clock answers the classic `os.clock` footgun:
wall time for timestamps, monotonic time for durations.

## Parsing

`(t/parse s)` accepts ISO 8601 / RFC 3339 (date-only, datetime,
`T`/`t`/space separator, optional seconds, fractional seconds kept
to milliseconds, `Z`/`z` and `+HH:MM` / `+-HHMM` offsets) and the
RFC 1123 / 2822 comma form (`Sun, 06 Nov 1994 08:49:37 GMT`,
optional day name, case-insensitive month and zone names, zones
GMT/UT/UTC or `+-HHMM`). Returns
`{:epoch-ms :offset-min :format :date-only?}` where `:format` is
`:iso8601`, `:rfc1123` (alphabetic zone), or `:rfc2822` (numeric
zone). HTTP `Date` and `Last-Modified` headers parse directly.

Strictness: impossible dates (February 30th), day names that
contradict the date, named non-UTC zones (EST is ambiguous without a
database), 2-digit years, trailing junk, embedded NULs, and input
over 64 characters throw `:time/parse` with the byte position.
Leap second `:60` folds to `:59`; `:61` rejects. Malformed input
never normalizes silently the way C `mktime` does.

## Formatting

`(t/format t fmt? offset-min?)` with keyword formats only:

| Keyword | Output |
|---------|--------|
| `:iso8601` (default) | `2026-08-20T10:00:00.123Z` (`.SSS` only when nonzero) |
| `:iso8601-date` | `2026-08-20` |
| `:rfc1123` | `Thu, 20 Aug 2026 03:12:00 GMT` (HTTP Date; always GMT) |
| `:rfc2822` | `Thu, 20 Aug 2026 05:12:00 +0200` |

No strftime-style pattern strings: they are a locale trap and a
second parser. Compose custom formats from the time map and `str`.

## Arithmetic

`(t/add t units)` with `:ms`, `:days` (exact 86400000-ms days; the
model has no DST), and `:months` (calendar months with day
clamping: January 31 plus one month is February 28 or 29). Units
apply in the order ms, days, months. Integer inputs answer
integers; maps answer maps at their own offset. Unknown units are
an error naming them.

`(t/diff a b)` decomposes into `{:months :days :ms}`, whole
calendar units first: `months-between` is defined as the largest n
with `(add-months a n) <= b`, so a January 31 to February 28 gap
counts as one month.

`(t/human t b?)` renders the largest unit: `"just now"`,
`"3 days ago"`, `"in 5 minutes"`, with full singular and plural
words; calendar months before twelve, then years.

Calendar facts: `(t/leap-year? y)`, `(t/days-in-month y m)`,
`(t/weekday t)`.

## inst interop

`(t/from-inst #inst "...")` converts an inst (the component map the
reader produces) to epoch-ms, strictly: impossible dates reject,
offsets are honored, seconds 60 fold to 59, nanoseconds truncate to
milliseconds. `(t/to-inst ms)` converts back carrying the instant
marker, so `pr-str` prints a `#inst` literal the reader
round-trips. JSON bodies keep ISO strings as strings; there is no
automatic coercion.
