# mino

A tiny, embeddable, REPL-friendly Lisp implemented in pure ANSI C.

mino is designed first as a runtime library for native hosts: drop it into a C or C++ application and gain a programmable extension layer with immutable values, persistent collections, structural sharing, code-as-data, and interactive development. The standalone executable is a convenience for development and shell use; the embedding API is the product.

## Status

v0.19.0. The language has a reader, evaluator, REPL, closures, proper tail calls, persistent vectors and hash maps (HAMT), sets, lazy sequences, macros with quasiquote, mark-and-sweep GC, try/catch, source locations with stack traces, modules, atoms, actors, regex, and a host C API with sandboxing, resource limits, and multi-instance runtime support. The public C API is unstable.

## Building

```
make
./mino
```

Requires only an ANSI C compiler. No external dependencies.

## License

MIT — see [LICENSE](LICENSE).
