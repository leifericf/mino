# Compression and zip

gzip, deflate, and zlib streams plus the zip container, whole-buffer
bytes in and bytes out, native end to end. Design contract:
`docs/adr/29-compression-zip-native-core.md`. The zip facade's full
reference is the docstring:

```
(require '[mino.zip :as zip] '[clojure.repl :refer [doc]])
(doc zip/write)
```

All seven prims are floor prims with no capability bit: every
embedder has them, like the json and html readers. The zip namespace
is `mino.zip`; it never touches `clojure.zip`, the tree zipper.

## The stream prims

`(gzip-compress data opts?)`, `(deflate-compress data opts?)`,
`(zlib-compress data opts?)`, and `(zlib-decompress data opts?)`.
Data must be bytes (the `gzip-decompress` symmetry); `:level`
accepts integers 0 through 9 and rejects everything else, including
10, with `:eval/contract` before any allocation.

| Prim | Opts (defaults) | Returns |
|------|-----------------|---------|
| `gzip-compress` | `{:level 6, :mtime 0, :name nil, :os 255}` | bytes, one RFC 1952 member |
| `deflate-compress` | `{:level 6}` | bytes, raw RFC 1951 |
| `zlib-compress` | `{:level 6}` | bytes, RFC 1950 |
| `zlib-decompress` | `{:max-bytes 67108864}` | bytes |

The gzip header is 10 bytes: XFL is 2 at level 9, 4 at level 1, else
0; OS defaults to 255 (unknown); FNAME is written only when `:name`
is supplied, stripped to its basename, stored as UTF-8. The trailer
is CRC32 plus ISIZE. `zlib-decompress` is the strict sibling of the
raw `deflate-decompress`: CM must be 8, CINFO at most 7, FCHECK a
multiple of 31, the big-endian Adler-32 is verified before return,
and trailing bytes after the one stream are rejected. A stream with
the FDICT preset-dictionary flag set throws `:codec/unsupported`: it
is well-formed zlib that needs a dictionary mino does not have.

`:max-bytes` is a hard output cap. A stream that would inflate past
it throws `:codec/limit`; nothing is truncated.

## The zip prims

| Prim | Shape | Returns |
|------|-------|---------|
| `zip-entries` | `(zip-entries archive)` | vector of read-side entry maps |
| `zip-read` | `(zip-read archive name opts?)`, `{:max-bytes 67108864}` | bytes, one entry |
| `zip-write` | `(zip-write entries opts?)`, `{:zip64 false, :level 6}` | bytes, the archive |

The archive argument accepts bytes or string (a string contributes
UTF-8 bytes, the digest rule). `zip-entries` lists entries in
central-directory archive order. `zip-read` returns the first entry
whose decoded name matches, and throws `:codec/missing` when none
does. The declared entry size is checked against `:max-bytes` before
any inflation: a zip bomb throws `:codec/limit` without allocating,
and CRC32 is verified on the extracted bytes. Nothing writes to any
filesystem; names like `"../x"` are data, inert by construction.

### Write-side entry map

Order is the vector order.

| Key | Default | Meaning |
|-----|---------|---------|
| `:name` | required | string, forward slashes (backslashes rewrite), no leading slash, no NUL |
| `:data` | required | bytes or string (UTF-8); empty with a trailing-slash name writes a directory entry |
| `:mtime` | `0` | epoch seconds; 0 and nil clamp to the DOS minimum 1980-01-01 |
| `:level` | archive default 6 | 0-9, per-entry override |
| `:method` | `:deflate` | or `:store`; anything else throws `:codec/unsupported` |
| `:comment` | `""` | central-directory string, no NUL |

### Read-side entry map

Eight keys: `{:name :size :compressed-size :crc32 :method :mtime
:directory? :comment}`. Field names follow the JVM ZipEntry
accessors and python ZipInfo attributes. `:method` is `:deflate`,
`:store`, or an unknown method's integer code passed through.
`:mtime` is epoch seconds, nil at the DOS minimum and at a zero date
word.

## Names

Entry names decode as UTF-8 when the entry sets the
language-encoding flag (general-purpose bit 11), else as CP437: the
python zipfile behavior, so listing and lookup agree. On write,
non-ASCII names set bit 11 and ASCII names leave it clear.
Archives whose names are raw UTF-8 without bit 11 (writer bugs)
mojibake exactly as they do in python. A leading slash throws
`:eval/contract` (APPNOTE 4.4.17.1).

## Determinism

Same input and options give byte-identical output, for all seven
prims. mino owns the UTC to DOS timestamp conversion: `:mtime` 0 and
nil clamp to 1980-01-01 (the python ZipInfo default), pre-1980
clamps up, post-2107 clamps down, and default-written entries read
back `:mtime` nil.

KNOWN EDGE: a `:mtime` whose UTC fields fall inside the runner's DST
transition window can land one hour off in the DOS minute field. DOS
time is 2-second grain and local-time-defined everywhere in the
ecosystem; full fidelity is not on offer. Goldens pin fixed epochs
away from transitions.

Compressed bodies are mino's own bytes. Cross-tool byte equality is
never claimed; the interop contract is decode only (`gzip -t`,
`unzip -t`, python round-trips), checked where the binaries exist
and self-skipping otherwise.

## zip64

Write switches to zip64 structures automatically at the 4 GiB and
65535-entry thresholds; `{:zip64 true}` forces always-zip64 output.
Sub-threshold archives carry no zip64 markers. Read accepts zip64
structures unconditionally. The entry-count and central-directory
ceilings throw `:codec/limit`, never truncate. A 4 GiB member needs
a 4 GiB live input: whole-buffer scope, no streaming.

## Errors

One `:codec` family, ex-info with `:mino/kind`, codes MGC001
through MGC007. Nothing escapes as `:internal` except OOM.

| Kind | Meaning |
|------|---------|
| `:codec/magic` | not a gzip or zlib header, no zip EOCD |
| `:codec/truncated` | input ends mid-structure |
| `:codec/corrupt` | malformed stream, LOC/CDH disagreement, overlapping entries, trailing bytes |
| `:codec/crc` | CRC32, ISIZE, or Adler-32 mismatch |
| `:codec/limit` | `:max-bytes`, zip64 ceilings, unaddressable declared sizes |
| `:codec/missing` | `zip-read` name not found |
| `:codec/unsupported` | encrypted entries, methods other than 0 and 8, unknown write `:method`, FDICT zlib |

## The mino.zip facade

`mino.zip` carries the entry-map vocabulary over the prims:
`(zip/entries archive)`, `(zip/read archive name opts?)`, and
`(zip/write entries opts?)`. Thin aliases with docstrings, no second
entry shape. The bb idiom:

```
(zip/write [{:name "report.csv" :data csv}
            {:name "plot.png"  :data png}])
```

The compress prims get no facade; the names are the API, like the
existing decompressors.
