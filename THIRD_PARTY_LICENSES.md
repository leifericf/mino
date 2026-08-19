# Third-Party Licenses

mino bundles source from the following third-party projects. Each project's
license is preserved in the source files under `src/vendor/` and is reproduced
below.

## imath

Vendored arbitrary-precision integer arithmetic library. Source at
`src/vendor/imath/imath.h` and `src/vendor/imath/imath.c`, fetched from
<https://github.com/creachadair/imath>.

```
Copyright (C) 2002-2007 Michael J. Fromberger, All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The vendored `src/vendor/imath/imath.c` contains two narrow changes, each
marked with a `mino:` comment for audit on upstream sync:

- `s_realloc` casts the unused `osize` parameter to void to silence
  `-Wunused-parameter` warnings when `DEBUG` is not defined.
- `s_fake` takes the absolute value through unsigned arithmetic so
  negation at `MP_SMALL_MIN` wraps cleanly in two's complement instead
  of tripping signed-overflow UB (caught by UBSAN). The fix produces
  the same result as upstream for all non-MIN inputs, and the
  documented MIN case now produces the correct unsigned magnitude.

No other changes were made to the upstream source.

## BearSSL

Vendored TLS client library, pinned at release v0.6, upstream commit
`8ef7680081c61b486622f2d983c0d3d21e83caad`. Source under
`src/vendor/bearssl/`, fetched from
<https://www.bearssl.org/git/BearSSL>. The upstream license notice is
preserved verbatim in `src/vendor/bearssl/LICENSE`.

```
Copyright (c) 2016 Thomas Pornin <pornin@bolet.org>

Permission is hereby granted, free of charge, to any person obtaining 
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be 
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The upstream sources under `src/vendor/bearssl/inc/` and
`src/vendor/bearssl/src/` are unmodified. The committed
`bearssl_client.c` is generated from them by
`src/vendor/bearssl/tools/make_amalgam.py`, which applies mechanical,
recorded transforms (per-unit file-local identifier renames, a
`MIN`/`MAX` to `br_MIN`/`br_MAX` rename, hoisting one macro define, and
dropping one MSVC `#pragma comment` line); see
`src/vendor/bearssl/README.md` for the full list and the update ritual.

## miniz

Vendored inflate (decompression) side of miniz, pinned at release
3.1.2, upstream commit `77d0dce8627735138c51770d1799a1ef48f2117d`.
Source under `src/vendor/miniz/`, fetched from
<https://github.com/richgel999/miniz>. The upstream license notice is
preserved verbatim in `src/vendor/miniz/LICENSE`.

```
Copyright 2013-2014 RAD Game Tools and Valve Software
Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC
All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

The upstream sources under `src/vendor/miniz/` are unmodified. The
committed `miniz_inflate.c` selects the inflate-only trim with one
upstream-provided define and carries `mz_crc32` copied verbatim from
upstream `miniz.c`; see `src/vendor/miniz/README.md` for the trim
list and the update ritual.

## Mozilla CA root certificates

The file `src/vendor/bearssl/mozilla-roots.pem` is a snapshot of the
Mozilla CA root store as republished by curl at
<https://curl.se/ca/cacert.pem> (Mozilla data as of Thu Aug 13 03:12:01
2026 GMT; sha256
`f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9`).
`src/vendor/bearssl/roots.c` is generated from it and embeds the same
certificates as DER data. The certificates are copyright Mozilla and
individual contributors and are distributed under the Mozilla Public
License 2.0; the license text is at
<https://mozilla.org/MPL/2.0/>.

