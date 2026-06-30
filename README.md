# mino

A tiny, embeddable Lisp in pure ANSI C.

Drop it into a C or C++ application and gain a programmable extension layer. The standalone executable is a convenience for development; the embedding API is the product.

Requires only an ANSI C compiler and `make`. No external dependencies.

```
make
./mino
```

`make` is the bootstrap step only — it generates the bundled-source headers and compiles the binary from a clean checkout. Every other build, test, and tooling task runs through the binary itself:

```
./mino task            # list available tasks
./mino task build      # incremental rebuild (used during development)
./mino task test       # run the test suite
./mino task build-asan # ASan-instrumented build
```

### Maintainer toolchain

`make` + an ANSI C compiler is the one, canonical build path — for embedders, from-source builds, and CI. **Building or embedding mino never needs anything else.**

*Developing* mino is different: cutting a release, regenerating stencils, or running the reproducible QA lanes **requires a pinned [`zig cc`](https://ziglang.org)** (a bundled Clang with cross-compilation support). Maintainer-only tasks that need it include:

- `./mino task gen-stencils-all` — regenerate the committed copy-and-patch JIT stencil byte tables for every target from one host, reproducibly. The bytes are checked in, so normal builds never invoke this.
- `./mino task cross-build` — cross-compile the Linux (musl-static, amd64/arm64) and Windows release binaries from one host. macOS stays a native build (Zig bundles no macOS SDK).
- `./mino task sanitize-zig` / `lint-zig` / `analyze-zig` — reproducible UBSan+TSan, a curated strict-warning lens, and an advisory static-analyzer report.

Run `./mino task doctor` to check your toolchain. The pinned Zig version and the full task list are in `docs/MAINTAINER_TOOLCHAIN.md`. These tasks hard-fail without the pinned `zig`; every embedder-facing path is unaffected.

Documentation: [mino-lang.org](https://mino-lang.org)

## Versioning

mino uses calendar versioning ([CalVer](https://calver.org)): `YYYY.MM.DD[-prerelease]`, e.g. `2026.06.30-alpha1`. A release on the same day bumps the prerelease suffix (`-alpha2`, `-alpha3`, ...; `-betaN`, then the unqualified date for a stable release).

Stable releases (`YYYY.MM.DD` with no suffix) aim to be backward compatible within the language and the embedding API. Anything labelled `-alphaN` or `-betaN` is a preview and may change or break before the stable release on that date. Every change of note is called out under the corresponding version heading in `CHANGELOG.md` so embedders can audit the delta before upgrading.

The ABI freeze is scheduled for the v1.0 cycle; until then, `src/mino.h` continues to carry evolving-API language.

## License

MIT
