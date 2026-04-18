# mino

A tiny, embeddable, REPL-friendly Lisp implemented in pure ANSI C.

mino is designed first as a runtime library for native hosts: drop it into a C or C++ application and gain a programmable extension layer with immutable values, persistent collections, structural sharing, code-as-data, and interactive development. The standalone executable is a convenience for development and shell use; the embedding API is the product.

## Status

v0.34.0. The language has a reader, evaluator, REPL, closures, proper tail calls, persistent vectors, hash maps (HAMT), sorted maps and sets (LLRB), lazy sequences, macros with quasiquote, mark-and-sweep GC, try/catch/throw, source locations with stack traces, namespaces, vars, modules, protocols, transducers, destructuring, multi-arity functions, metadata, atoms with watches and validators, host interop via capability registry, actors with mailboxes, regex, and a host C API with sandboxing, execution limits, and multi-instance runtime support. The public C API is unstable.

## Building

```
make
./mino
```

Requires only an ANSI C compiler. No external dependencies.

## License

MIT — see [LICENSE](LICENSE).
