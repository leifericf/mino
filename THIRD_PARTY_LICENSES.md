# Third-Party Licenses

mino bundles source from the following third-party projects. Each project's
license is preserved in the source files under `src/vendor/` and is reproduced
below.

## imath

Vendored arbitrary-precision integer arithmetic library. Source at
`src/vendor/imath.h` and `src/vendor/imath.c`, fetched from
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

The vendored `src/vendor/imath.c` contains two narrow changes, each
marked with a `mino:` comment for audit on upstream sync:

- `s_realloc` casts the unused `osize` parameter to void to silence
  `-Wunused-parameter` warnings when `DEBUG` is not defined.
- `s_fake` takes the absolute value through unsigned arithmetic so
  negation at `MP_SMALL_MIN` wraps cleanly in two's complement instead
  of tripping signed-overflow UB (caught by UBSAN). The fix produces
  the same result as upstream for all non-MIN inputs, and the
  documented MIN case now produces the correct unsigned magnitude.

No other changes were made to the upstream source.
