# BearSSL (vendored)

TLS client library vendored for the HTTPS support in mino's net layer.

- Upstream: https://www.bearssl.org/git/BearSSL (Thomas Pornin)
- Release tag: v0.6 (2018-08-14)
- Pinned commit: `8ef7680081c61b486622f2d983c0d3d21e83caad`
- License: MIT, see `LICENSE` in this directory (verbatim upstream
  `LICENSE.txt`)

## Layout

- `inc/` and `src/` are the upstream v0.6 sources, unmodified, trimmed
  to the client-relevant set (see below). They are regeneration input
  only; nothing in the build compiles them directly.
- `bearssl_client.c` is the single translation unit the build consumes
  (Makefile wildcard `src/vendor/bearssl/*.c` and the explicit source
  list in `lib/mino/tasks/builtin.clj`). It is generated and committed;
  see the ritual below.
- `tools/make_amalgam.py` generates `bearssl_client.c` from `inc/` and
  `src/`. Deterministic: sorted walks, stable paste order, no
  timestamps.

The TLS code that uses this library lives in the runtime; until it
lands, nothing calls into BearSSL at runtime.

## Why a generated single TU

BearSSL units are independent TUs that reuse file-local names across
units (`K`, `ROTR`, `TLEN`, `cond_negate`, `t1`..`t10`, and identical
anonymous typedefs). Pasting them naively into mino's single-file
amalgamation produces hundreds of redefinition errors. The generator
renames each unit's file-local identifiers (macros, static functions
and objects, typedefs, struct/union/enum tags) with a per-unit `_u<idx>`
suffix; per-TU semantics guarantee every reference is intra-unit. It
also applies two targeted edits, recorded here so an upstream bump
rechecks both:

- `MIN`/`MAX` static inlines in `inner.h` become `br_MIN`/`br_MAX`
  (host `sys/param.h` macros collide; BearSSL never calls them).
- `#define BR_ENABLE_INTRINSICS 1` is hoisted to the top of the
  amalgam. Units with x86 or POWER8 intrinsics normally set it before
  including `inner.h`; with collapsed includes the global define
  replaces the per-unit ones and is inert elsewhere.
- The MSVC `#pragma comment(lib, "advapi32")` line in
  `src/rand/sysrng.c` is dropped (unknown-pragma noise under
  clang/gcc).

A scoped `#pragma GCC diagnostic ignored "-Wunused-function"` wraps the
pasted public headers only: collapsing them into one TU exposes their
static inline helpers without callers. Guarded by
`#if defined(__GNUC__) || defined(__clang__)` so MSVC never sees it.
No warning suppression is needed for any `.c` unit; under the project's
strict flags the vendored code is clean on host cc, zig x86_64-linux
and zig x86_64-windows.

## Trim list

Excluded from upstream v0.6:

- Server side: all `ssl_server*`, `ssl_scert_*` TUs.
- Key generation: `ec_keygen.c`, `src/rsa/*keygen*`.
- PEM codec (`pemdec`, `pemenc`), `skey_decoder`, x509 encoder TUs.
- `hkdf`, `eax`, `aesctr_drbg`.
- `tools/`, `test/`, `samples/`, the T0 compiler sources, docs.
- Upstream `Rand` file `sysrng.c` is **kept** for now: the SSL engine
  references `br_prng_seeder_system`. It is why Windows links
  `-ladvapi32` (CryptGenRandom). The plan is for the TLS runtime layer
  to supply its own seeder over getentropy (POSIX) and
  BCryptGenRandom (Windows), drop `sysrng.c` from the generator's file
  list, and remove the advapi32 dependency.

Included beyond the strict minimum (TLS client needs or the default
selectors reference them): 3DES suites, single-RSA and single-EC
client certificates, the ssl_lru session cache, and every
runtime-dispatched implementation (x86ni, pwr8, sse2, pclmul, ctmulq);
non-applicable arch code self-gates to nothing.

## Update ritual

1. Re-pin: clone the canonical repo, check out the new release tag,
   copy `LICENSE.txt` over `LICENSE`, refresh `inc/` and `src/` per the
   trim list, and update tag and commit sha above.
2. Regenerate: `python3 src/vendor/bearssl/tools/make_amalgam.py`, then
   diff `bearssl_client.c`. Rename-suffix counts changing is expected;
   anything else in the diff deserves a read.
3. Re-run the gates: `make`, `./mino task build-asan`,
   `./mino task build-ubsan`, `./mino task test`,
   `./mino task amalgamate`, plus the zig x86_64-windows cross
   compile of the amalgam.
4. Changelog line rides the release that first ships the new pin.
