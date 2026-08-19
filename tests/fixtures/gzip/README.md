# gzip and deflate test fixtures

Generated once by python3 and checked in; mino has no compression
prim, so the compressed side of every round trip lives here. The
generation commands (run from the repo root):

```
python3 - <<'EOF'
import gzip, io, os, zlib

d = "tests/fixtures/gzip"

def gz(data, mtime=0, fname=None):
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9,
                       mtime=mtime, filename=fname) as f:
        f.write(data)
    return buf.getvalue()

def raw_deflate(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()

payload = os.urandom(10240)
files = {
    "empty.gz":       gz(b""),
    "a.gz":           gz(b"a"),
    "hello.gz":       gz(b"hello, gzip\n"),
    "hello-fname.gz": gz(b"hello, gzip\n", fname="name.txt"),
    "random10k.gz":   gz(payload),
    "random10k.bin":  payload,
    "zeros1m.gz":     gz(b"\x00" * 1048576),
    "zeros200m.gz":   gz(b"\x00" * 200000000),
    "hello.raw":      raw_deflate(b"hello, gzip\n"),
    "hello.zlib":     zlib.compress(b"hello, gzip\n"),
}
for name, data in sorted(files.items()):
    open(os.path.join(d, name), "wb").write(data)
EOF
```

Regenerating `random10k.*` draws fresh random bytes; the other files
are byte-deterministic (mtime pinned to 0). gzip containers here
exercise the no-flag and FNAME header shapes; tests synthesize the
FEXTRA / FCOMMENT / FHCRC shapes from `hello.gz` parts inline.

Contents:

- `empty.gz` / `a.gz` / `hello.gz` -- gzip of b"", b"a",
  b"hello, gzip\n" (mtime 0)
- `hello-fname.gz` -- same payload with the FNAME header flag
- `random10k.gz` + `random10k.bin` -- incompressible 10 KB payload
  and its plaintext
- `zeros1m.gz` -- 1 MB of zeros (1051 bytes compressed)
- `zeros200m.gz` -- 200,000,000 zeros (194,421 bytes compressed);
  the decompression-bomb fixture for the output cap
- `hello.raw` -- raw deflate (RFC 1951, no zlib wrapper, wbits=-15)
  of b"hello, gzip\n"
- `hello.zlib` -- zlib-wrapped deflate (RFC 1950) of the same bytes;
  deflate-decompress rejects this shape
