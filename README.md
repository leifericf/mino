# mino

A tiny, embeddable Lisp in pure ANSI C.

Drop it into a C or C++ application and gain a programmable extension layer. The standalone executable is a convenience for development; the embedding API is the product.

Requires only an ANSI C compiler. No external dependencies.

```
cc -std=c99 -O2 -Isrc -o mino src/*.c main.c -lm
./mino
```

Documentation: [mino-lang.org](https://mino-lang.org)

## Versioning

Pre-1.0.0: semantic versioning applies informally. Any minor version bump (0.X) may contain breaking changes to the embedding API, the language, or the standalone binary. Every break is called out under the corresponding version heading in `CHANGELOG.md` so embedders can audit the delta before upgrading. Patch versions (0.X.Y) are reserved for bug fixes and non-breaking additions.

Post-1.0.0: strict [SemVer 2.0.0](https://semver.org/spec/v2.0.0.html). Breaking changes happen only at major bumps; minor bumps add API; patch bumps fix bugs.

The ABI freeze is scheduled for the v1.0 cycle; until then, `src/mino.h` continues to carry evolving-API language.

## License

MIT
