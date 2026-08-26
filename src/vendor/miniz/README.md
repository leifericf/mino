# miniz (vendored)

The deflate and zip core of miniz, vendored for mino's compression
and archive prims (gzip, deflate, zlib framing and the zip
container; see ADR 29).

- Upstream: https://github.com/richgel999/miniz
- Release tag: 3.1.2 (2025-01-04)
- Pinned commit: `77d0dce8627735138c51770d1799a1ef48f2117d`
- License: MIT, see `LICENSE` in this directory (verbatim upstream)

## Layout

- `upstream/` holds the 3.1.2 headers (`miniz.h`, `miniz_common.h`,
  `miniz_tdef.h`, `miniz_tinfl.h`, `miniz_zip.h`) and the three
  implementation files `miniz_tinfl.c`, `miniz_tdef.c`, and
  `miniz_zip.c`, unmodified. Nothing in the build compiles them
  directly; they are include input only.
- `miniz_core.c` is the single translation unit the build consumes
  (Makefile wildcard `src/vendor/miniz/*.c` and the explicit source
  list in `lib/mino/tasks/builtin.clj`). It is hand-maintained, not
  generated: it defines `MINIZ_NO_STDIO` (drops every file-based
  archive path; mino reads and writes archives in memory only) and
  `MINIZ_NO_ZLIB_COMPATIBLE_NAMES` (which drops the static
  zlib-name wrappers `miniz.h` would otherwise declare unused in
  this TU), deliberately does NOT define `MINIZ_NO_TIME` (the
  explicit `last_modified` write path needs the time field in the
  internal stat; every `time()` side door is bypassed by passing
  explicit times), includes the three upstream `miniz_*.c` files,
  and carries `mz_crc32`, `mz_adler32`, and the three default
  allocator hooks (`miniz_def_alloc_func` and siblings) copied
  verbatim from upstream `miniz.c` (the gzip and zlib framings need
  the CRC-32 and Adler-32, tdefl also references `mz_adler32`
  externally, and the zip heap paths take the allocator hooks as
  function pointers, so the copies are link requirements; the rest
  of `miniz.c` is zlib stream wrappers and stays out).
- `miniz_export.h` is a stub for the header upstream CMake builds
  generate via GenerateExportHeader; the export macros are empty and
  guarded.

## Trim list

Excluded from upstream 3.1.2:

- `miniz.c`: everything except `mz_crc32`, `mz_adler32`, and the
  three default allocator hooks, extracted into `miniz_core.c`
  (zlib-style stream wrappers, deflate init glue).
- `examples/`, `tests/`, CMake and pkg-config files, ChangeLog,
  readme.

One TU over the three implementation files because the zip reader
references tinfl symbols and the writer tdefl, both externally
linked; splitting buys only compile-time parallelism (ADR 29).

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
- The heap archive paths (`mz_zip_reader_init_mem`,
  `mz_zip_writer_init_heap`) allocate through miniz's `MZ_MALLOC`
  (plain `malloc`); this TU installs no allocator overrides.

## Update ritual

1. Re-pin: clone upstream, check out the new release tag, copy
   `LICENSE` over and refresh everything under `upstream/`, update
   tag and commit sha above.
2. Re-extract `mz_crc32`, `mz_adler32`, and the three default
   allocator hooks from the new `miniz.c` into
   `miniz_core.c` (crc32 and adler32 sit at the top of the file;
   copy the table-driven crc32 variant; the allocator hooks sit a
   little below adler32; keep the one-level de-indent the TU uses).
3. Re-run the gates: `make`, `./mino task build-asan`,
   `./mino task build-ubsan`, `./mino task test`,
   `./mino task amalgamate`, plus the zig x86_64-windows cross
   compile of the amalgam.
4. Changelog line rides the release that first ships the new pin.
