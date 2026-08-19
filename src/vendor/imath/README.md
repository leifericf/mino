# imath (vendored)

Arbitrary-precision integer arithmetic, vendored for mino's bignum,
ratio, and bigdec towers.

- Upstream: https://github.com/creachadair/imath (M. J. Fromberger)
- Pin: upstream commit
  `f0973961d8a603ad42526ad2d71bf4d23931c219` (2025-01-26). The tree
  was vendored before this repo recorded pins; the revision was
  reconstructed later by diffing against upstream history, not
  recorded at import time. `imath.h` matches that revision exactly,
  `imath.c` modulo the local fixes below.
- License: MIT, see `LICENSE` in this directory (verbatim upstream)

The pin predates the 2026-05 upstream API change that switched
`mp_int_string_len`, `mp_int_count_bits`, `mp_int_binary_len`, and
`mp_int_unsigned_len` from `mp_result` to `mp_size` returns; this
tree still has `mp_result`. An upstream bump reconciles those
deliberately, not piecemeal.

## Layout

- `imath.c` and `imath.h` are the whole tree, compiled directly
  (Makefile wildcard `src/vendor/imath/*.c` and the explicit source
  list in `lib/mino/tasks/builtin.clj`). Nothing is trimmed.

## Local changes

Three narrow fixes in `imath.c`, each marked with a `mino:` comment
for audit on upstream sync; rationale in THIRD_PARTY_LICENSES.md:

- `mp_int_to_int` assembles the magnitude through unsigned
  arithmetic (UBSAN: signed left-shift and negation at
  `MP_SMALL_MIN`).
- `s_realloc` casts the unused `osize` parameter to void
  (`-Wunused-parameter` when `DEBUG` is not defined).
- `s_fake` takes the absolute value through unsigned arithmetic
  (same `MP_SMALL_MIN` class as above).

## Update ritual

1. Re-pin: clone upstream, check out the new revision, copy
   `imath.c`, `imath.h`, and `LICENSE` over, and update the pin
   above.
2. Re-apply the `mino:` fixes, or confirm upstream superseded them;
   drop the ones that are moot.
3. Reconcile API drift (return types above) in one pass with the
   callers in `src/prim/bignum.c`, `src/prim/ratio.c`, and
   `src/prim/bigdec.c`.
4. Re-run the gates: `make`, `./mino task build-asan`,
   `./mino task build-ubsan`, `./mino task test`,
   `./mino task amalgamate`, plus the zig x86_64-windows cross
   compile of the amalgam.
