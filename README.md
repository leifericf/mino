# mino

[![Clojure parity](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/leifericf/clojure-census/main/output/mino/badge.json)](https://clojure-census.leifericf.com/dialects/mino/readiness/)

An embeddable Lisp in ANSI C. Link it into a C or C++ application to add a programmable extension layer. The standalone executable is for development; the embedding API is the primary interface.

Requires only an ANSI C compiler and `make`.

```
make
./mino
```

`make` is the bootstrap step only: it generates the bundled-source headers and compiles the binary from a clean checkout. Every other build, test, and tooling task runs through the binary itself:

```
./mino task            # list available tasks
./mino task build      # incremental rebuild (used during development)
./mino task test       # run the test suite
./mino task build-asan # ASan-instrumented build
```

### Maintainer toolchain

`make` + an ANSI C compiler is the canonical build path for embedders, from-source builds, and CI. Building or embedding mino does not require any other toolchain.

*Developing* mino is different: cutting a release, regenerating stencils, or running the reproducible QA lanes requires a pinned [`zig cc`](https://ziglang.org) (a bundled Clang with cross-compilation support). Maintainer-only tasks that need it include:

- `./mino task gen-stencils-all` regenerates the committed copy-and-patch JIT stencil byte tables for every target from one host, reproducibly. The bytes are checked in, so normal builds never invoke this.
- `./mino task cross-build` cross-compiles the Linux (musl-static, amd64/arm64) and Windows release binaries from one host. macOS stays a native build (Zig bundles no macOS SDK).
- `./mino task sanitize-zig` / `lint-zig` / `analyze-zig` reproducible UBSan+TSan, a curated strict-warning lens, and an advisory static-analyzer report.

Run `./mino task doctor` to check your toolchain. The pinned Zig version and the full task list are in `docs/MAINTAINER_TOOLCHAIN.md`. These tasks fail without the pinned `zig`; embedder-facing paths are unaffected.

Documentation: [mino-lang.org](https://mino-lang.org)

## Versioning

mino uses calendar versioning ([CalVer](https://calver.org)): `YYYY.MM.DD[-prerelease]`, e.g. `2026.08.08-alpha1`. A release on the same day bumps the prerelease suffix (`-alpha2`, `-alpha3`, ...; `-betaN`, then the unqualified date for a stable release).

Stable releases (`YYYY.MM.DD` with no suffix) aim to be backward compatible within the language and the embedding API. Anything labelled `-alphaN` or `-betaN` is a preview and may change or break before the stable release on that date.

The ABI freeze is scheduled for the v1.0 cycle; until then, `src/mino.h` continues to carry evolving-API language.

## License

MIT
