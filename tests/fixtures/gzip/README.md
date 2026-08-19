# gzip and deflate test fixtures

Generated once and checked in; mino has no compression prim, so the
compressed side of every round trip lives here. The generation
commands (run from the repo root) use gzip(1):

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

head -c 10240 /dev/urandom > $d/random10k.bin
gzip -n -9 -c $d/random10k.bin > $d/random10k.gz
head -c 1048576   /dev/zero | gzip -n -9 > $d/zeros1m.gz
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
(mtime is pinned to 0 by -n, XFL is 2 from -9). Regenerating
`random10k.*` draws fresh random bytes.

gzip containers here exercise the no-flag and FNAME header shapes;
tests synthesize the FEXTRA / FCOMMENT / FHCRC shapes from `hello.gz`
parts inline.

Contents:

- `empty.gz` / `a.gz` / `hello.gz` -- gzip of b"", b"a",
  b"hello, gzip\n" (mtime 0)
- `hello-fname.gz` -- same payload with the FNAME header flag
- `random10k.gz` + `random10k.bin` -- incompressible 10 KB payload
  and its plaintext
- `zeros1m.gz` -- 1 MB of zeros (1051 bytes compressed)
- `zeros200m.gz` -- 200,000,000 zeros (194,421 bytes compressed);
  the decompression-bomb fixture for the output cap
- `hello.raw` -- raw deflate (RFC 1951, no zlib wrapper) of
  b"hello, gzip\n"
- `hello.zlib` -- zlib-wrapped deflate (RFC 1950) of the same bytes;
  deflate-decompress rejects this shape
