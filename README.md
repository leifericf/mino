# mino

A tiny, embeddable, REPL-friendly Lisp implemented in pure ANSI C.

mino is designed first as a runtime library for native hosts: drop it into a C or C++ application and gain a programmable extension layer with immutable values, persistent collections, structural sharing, code-as-data, and interactive development. The standalone executable is a convenience for development and shell use; the embedding API is the product.

## Status

Pre-release. The roadmap runs from `v0.1.0` (walking skeleton) to `v1.0.0` (frozen ABI and language semantics). The public C API is marked unstable until 1.0.

## Building

```
make
./mino
```

Requires only an ANSI C compiler. No external dependencies.

## License

MIT — see [LICENSE](LICENSE).
