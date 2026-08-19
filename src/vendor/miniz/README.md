# miniz (vendored)

The inflate side of miniz, vendored for gzip and deflate response
body decoding in mino's HTTP client.

- Upstream: https://github.com/richgel999/miniz
- Release tag: 3.1.2 (2025-01-04)
- Pinned commit: `77d0dce8627735138c51770d1799a1ef48f2117d`
- License: MIT, see `LICENSE` in this directory (verbatim upstream)

## Layout

- `upstream/` holds the 3.1.2 headers (`miniz.h`, `miniz_common.h`,
  `miniz_tdef.h`, `miniz_tinfl.h`, `miniz_zip.h`) and
  `miniz_tinfl.c`, unmodified. Nothing in the build compiles them
  directly; they are include input only.
- `miniz_inflate.c` is the single translation unit the build consumes
  (Makefile wildcard `src/vendor/miniz/*.c` and the explicit source
  list in `lib/mino/tasks/builtin.clj`). It is hand-maintained, not
  generated: it defines `MINIZ_NO_ZLIB_COMPATIBLE_NAMES` (which drops
  the static zlib-name wrappers `miniz.h` would otherwise declare
  unused in this TU), includes `upstream/miniz_tinfl.c`, and carries
  `mz_crc32` copied verbatim from upstream `miniz.c` (the gzip
  container's CRC-32 check needs it; the rest of `miniz.c` is zlib
  wrappers and stays out).
- `miniz_export.h` is a stub for the header upstream CMake builds
  generate via GenerateExportHeader; the export macros are empty and
  guarded.

## Trim list

Excluded from upstream 3.1.2:

- Compression: `miniz_tdef.c` (the header `miniz_tdef.h` stays
  because `miniz.h` includes it; declarations only, no code).
- ZIP archive handling: `miniz_zip.c` (header stays for the same
  reason).
- `miniz.c`: everything except `mz_crc32`, extracted into
  `miniz_inflate.c` (zlib-style stream wrappers, adler32, deflate
  init glue).
- `examples/`, `tests/`, CMake and pkg-config files, ChangeLog,
  readme.

The decompressor needs no allocator hooks in this build:
`tinfl_decompress` itself never allocates, and mino's prim layer
drives it with caller-owned buffers (the `tinfl_decompress_mem_*`
heap helpers compile but are unreferenced).

## Notes

- miniz is C89-era portable C; under mino's strict `-std=c99
  -Wall -Wpedantic -Wextra -Werror` flags the vendored files are
  clean on host cc, zig x86_64-linux and zig x86_64-windows.
  `__STRICT_ANSI__` (set by `-std=c99`) neutralizes the
  `MZ_FORCEINLINE` GNU attribute branch, and the endian and unaligned
  load paths are compile-time guarded.
- 3.1.2 over 3.0.2: the 3.1 line carries an inflate robustness fix
  ("Guard against code_len==0 infinite loop in tinfl_decompress")
  plus warning fixes, and 3.1.2 is the current release tag.

## Update ritual

1. Re-pin: clone upstream, check out the new release tag, copy
   `LICENSE` over and refresh everything under `upstream/`, update
   tag and commit sha above.
2. Re-extract `mz_crc32` from the new `miniz.c` into
   `miniz_inflate.c` (the function sits next to `mz_adler32` at the
   top of the file; copy the table-driven variant).
3. Re-run the gates: `make`, `./mino task build-asan`,
   `./mino task build-ubsan`, `./mino task test`,
   `./mino task amalgamate`, plus the zig x86_64-windows cross
   compile of the amalgam.
4. Changelog line rides the release that first ships the new pin.
