# mino

A tiny, embeddable Lisp in pure ANSI C.

Drop it into a C or C++ application and gain a programmable extension layer. The standalone executable is a convenience for development; the embedding API is the product.

Requires only an ANSI C compiler. No external dependencies.

```
cc -std=c99 -O2 -Isrc -o mino src/*.c main.c -lm
./mino
```

Documentation: [mino-lang.org](https://mino-lang.org)

## License

MIT
