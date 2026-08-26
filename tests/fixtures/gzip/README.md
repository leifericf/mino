# gzip and deflate test fixtures

Oracle goldens only: the compressed side of cross-tool decode
pins, generated once with gzip(1) and checked in. Round-trip payloads
are generated and compressed in-process by tests/gzip_test.clj
(deterministic LCG bytes, zeros); nothing here is regenerated at test
time and no test shells out to gzip(1) (compress_test's self-skipping
decode cross-check excepted, pinned by a grep-clean assertion).

Generation commands (run once, from the repo root; gzip(1)
provenance):

```
d=tests/fixtures/gzip

printf ''              | gzip -n -9 > $d/empty.gz
printf 'a'             | gzip -n -9 > $d/a.gz
printf 'hello, gzip\n' | gzip -n -9 > $d/hello.gz

# FNAME header flag: gzip stores the file's name and mtime, so the
# mtime is pinned to epoch 0 through a UTC touch.
printf 'hello, gzip\n' > name.txt
TZ=UTC touch -t 197001010000.00 name.txt
gzip -9 -c name.txt > $d/hello-fname.gz && rm name.txt

head -c 200000000 /dev/zero | gzip -n -9 > $d/zeros200m.gz

# Raw deflate (RFC 1951): the gzip body with header and trailer
# stripped (10-byte header, 8-byte CRC32+ISIZE trailer).
dd if=$d/hello.gz of=$d/hello.raw bs=1 skip=10 \
   count=$(( $(wc -c < $d/hello.gz) - 18 ))

# Zlib wrapper (RFC 1950): 78 9c, the raw deflate body, then
# adler32("hello, gzip\n") big-endian (1c c3 04 25).
{ printf '\x78\x9c'; cat $d/hello.raw; printf '\x1c\xc3\x04\x25'; } \
  > $d/hello.zlib
```

The checked-in files carry 0xff (unknown) in the header's OS byte
from their original minting; gzip(1) stamps its own OS id there, so
a regeneration differs in that one header byte and nothing else
(mtime is pinned to 0 by -n, XFL is 2 from -9). The python3 oracle
streams for the compress prims live under tests/fixtures/compress/
(tools/gen_zip_oracle.py).

gzip containers here exercise the no-flag and FNAME header shapes;
tests synthesize the FEXTRA / FCOMMENT / FHCRC shapes from `hello.gz`
parts inline.

Contents:

- `empty.gz` / `a.gz` / `hello.gz` -- gzip(1) of b"", b"a",
  b"hello, gzip\n" (mtime 0); the decode-side oracle goldens
- `hello-fname.gz` -- same payload with the FNAME header flag
- `zeros200m.gz` -- 200,000,000 zeros (194,421 bytes compressed);
  keeps the decompression-bomb role for the output cap
- `hello.raw` -- raw deflate (RFC 1951, no zlib wrapper) of
  b"hello, gzip\n"
- `hello.zlib` -- zlib-wrapped deflate (RFC 1950) of the same bytes;
  deflate-decompress rejects this shape

Removed from the checked-in set, now in-process: `random10k.bin` /
`random10k.gz` (seeded LCG payload, compressed through gzip-compress)
and `zeros1m.gz` (1 MiB of zeros, same).
