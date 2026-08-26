# ADR 29: Compression write side and zip container, native end to end

Date: 2026-08-26

## Context

ki-23 plus the zip half of ki-19: mino reads gzip and raw deflate
but writes neither, and has no zip container, while zip is a
documented top babashka use case and every peer runtime (JVM
java.util.zip, python gzip/zlib/zipfile, Ruby Zlib, bb fs/zip)
defaults to the RFC 1950 zlib wrapper mino cannot express. The
compressor core is already vendored C: miniz 3.1.2 (MIT), pinned at
77d0dce8627735138c51770d1799a1ef48f2117d, inflate-only through one
hand-maintained TU. Every prior wire-format reader measured
mino-side Clojure 35-41x over an absolute budget and was rewritten
native (ADR 23 json, 24 csv, 25 toml, 26 yaml, 28 html/xml). The
API contract is settled by the ecosystem research: names mirroring
the existing decompressors, bytes in and bytes out, keyword opts,
a thin mino.zip facade over entry maps shaped by JVM ZipEntry and
python ZipInfo.

## Decision

The compress prims (gzip-compress, deflate-compress, zlib-compress,
zlib-decompress) and the zip container prims (zip-entries, zip-read,
zip-write) are native C in src/prim/compress.c and src/prim/zip.c
over the widened vendored miniz core: whole-buffer bytes in and out,
single pass, one output allocation, no event or token API and no
streaming this campaign. Output is deterministic by contract: same
input and options produce byte-identical output, with mino owning
the UTC DOS timestamp conversion. The Clojure surface is the thin
mino.zip facade. One :codec error family covers everything.

## Consequences

- One vendor TU compiles about 7k upstream lines; the payback is a
  single vendor seam to re-pin and audit.
- Whole-buffer scope means a 4 GiB zip64 member needs a 4 GiB live
  input. The nightly test is the tripwire, not a streaming license;
  the revisit trigger is a real multi-GB user.
- tdefl output bytes are mino's own. Cross-tool byte equality of
  compressed bodies is never claimed, only decode interop (gzip -t,
  unzip -t, python round-trips).
- The DST-transition mtime edge is accepted and pinned: a :mtime
  whose UTC fields fall inside the runner's DST window can land one
  hour off in the DOS minute field. DOS time is 2-second grain and
  local-time-defined everywhere in the ecosystem; full fidelity is
  not on offer.
- CP437 decode rewrites legacy Latin-1-intended names exactly as
  python does; archives whose names were raw UTF-8 without bit 11
  (writer bugs) mojibake identically.

## Alternatives

A Clojure container assembler over a native tdefl prim was the
research's provisional shape and is superseded: per-entry boundary
crossings with no measurable upside. ADR 23's "writer stays
Clojure" line does not apply; it was earned by a linear Clojure
JSON writer, while here the compressor core is vendored C from day
one. Multiple vendor TUs, vendoring the amalgamated miniz.c, and
editing upstream files to split symbols are rejected for the vendor
seam. Vendor localtime timestamp paths are rejected as TZ-flaky
through goldens; emitting the LOC/CDH/EOCD structures ourselves is
rejected because zip64 extra fields are exactly where silent
corruption lives. Always-zip64 by default loses legacy-reader interop.
mino.archive is rejected (bb and JVM scripters say zip; the
clojure.zip namespace is untouched). A :zip/* error subfamily is
rejected for one codec-universal family. Raw byte passthrough for
non-EFS names is rejected for mojibake plus incoherent lookup.
Keeping the zlib wrapper out is rejected against the ecosystem
evidence.

## The eight aspects

### Native cores end to end

Both cores are C prims in the ADR 23/25/26/28 lineage: values built
C-side, whole-buffer scope, single pass. Decompression reuses the
bounded growing-buffer inflate core promoted out of gzip.c as the
shared mino_inflate_raw; compression allocates once at the vendor's
own worst-case bound (mz_deflateBound) with no growth loop. gzip.c
itself stays the frozen read side that http.c consumes. No
streaming this campaign; a token-stream API is rejected until a
customer exists (the ADR 28 line). A generic archive abstraction
tar could plug into is rejected; the container is zip-shaped only.

### One widened vendor TU

miniz_inflate.c becomes miniz_core.c, same pattern widened: it
defines MINIZ_NO_STDIO and MINIZ_NO_ZLIB_COMPATIBLE_NAMES, includes
upstream miniz_tinfl.c, miniz_tdef.c, and miniz_zip.c unmodified as
include input, and carries mz_crc32 plus mz_adler32 copied verbatim
from upstream miniz.c; the adler copy is also a link requirement,
tdefl references it externally. MINIZ_NO_TIME is deliberately not
defined (the explicit last_modified write path needs the stat
field); every time() side door is bypassed by passing explicit
times. One TU over three because the zip reader references tinfl
symbols and the writer tdefl, both externally linked; splitting
buys only compile-time parallelism.

### Write-side determinism

Same input and options produce byte-identical output. miniz
converts epoch to DOS through localtime and DOS back through
mktime, so vendor-path timestamps vary with the runner's timezone;
mino owns the conversion instead. Write passes the desired epoch's
UTC civil fields compensated so miniz's localtime lands on exactly
those fields; :mtime 0 and nil clamp to the DOS minimum 1980-01-01
(the python ZipInfo default); read decodes the CDH time/date words
from the archive bytes and returns :mtime nil at the DOS minimum.
Goldens split by direction: python3-oracle fixtures pin the read
side, self-frozen bytes pin write determinism, and tdefl bodies are
never compared against gzip(1) or python. gzip-compress strips
:name to its basename (RFC 1952 intent).

### zip64

Write rides miniz's automatic switch at the 4 GiB and 65535-entry
thresholds; {:zip64 true} forces always-zip64 structures; read
accepts zip64 unconditionally. The entry-count and
central-directory-size ceilings throw :codec/limit, never truncate.
Evidence is threefold: a forced-zip64 byte golden (64-bit EOCD plus
locator, which python3 zipfile opens and reads), the 65535-entry
auto-switch test riding the perf file, and a nightly memory-guarded
test-zip64 task for the 4 GiB member that self-skips with a printed
acceptance note when the host cannot allocate the working set.

### Namespace and entry maps

mino.zip carries the entry-map vocabulary: write entries are
{:name :data :mtime :level :method :comment}, read entries add
:size, :compressed-size, :crc32, and :directory?, field names
following the JVM ZipEntry accessors and python ZipInfo attributes
so bb and JVM scripters guess right. The facade is thin aliases
with docstrings, no second entry shape, no capability bit (the
json/toml/yaml/html floor pattern). Duplicate names resolve to the
first central-directory match in archive order; unknown :method
integers pass through on read.

### Error taxonomy

One :codec family, symmetric with gzip.c's read-side keys, extended
with zip semantics: :codec/magic, :codec/crc, :codec/truncated,
:codec/limit, :codec/name, plus new kinds :codec/missing (zip-read
name not found) and :codec/unsupported (encrypted entries, methods
other than 0 and 8, unknown write :method), codes MGC006 and MGC007
extending MGC001-005 contiguously. Every zip and zlib error
classifies; nothing escapes :internal except OOM.

### CP437 fallback

Entry names decode as UTF-8 when the entry sets the
language-encoding flag (bit 11), else as CP437: the python zipfile
behavior, so oracle fixtures and vendor archives agree and
zip-read's lookup uses the same decoding the listing showed. The
128-entry high-half table is generated from the python cp437 codec
into src/prim/zip_cp437.h with a provenance header (the ADR 28
entity-table discipline: generated from the oracle, nothing
vendored). Write: non-ASCII names set bit 11, backslashes rewrite
to forward slashes, a leading slash throws. "../" and absolute
names round trip verbatim; the reader returns bytes and writes no
filesystem, so traversal is inert by construction.

### The zlib RFC 1950 pair

zlib-compress and zlib-decompress are in scope, widening the
earlier deflate-is-raw-only stance: JVM Deflater, python zlib,
Ruby Zlib, and miniz itself all default to the RFC 1950 wrapper,
and a scripter crossing from any of them is surprised exactly once
without it. The raw prims keep their documented behavior;
zlib-decompress is the strict sibling (CM=8, CINFO at most 7,
FCHECK a multiple of 31, big-endian Adler-32 verified, :max-bytes
default 64 MiB). The widening is recorded here so the tracker
reconciles.
