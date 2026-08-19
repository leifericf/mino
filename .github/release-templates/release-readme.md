# mino __VERSION__ (__OS__/__ARCH__)

A tiny, embeddable Lisp in pure ANSI C.

This archive contains a single self-contained `mino` binary built for
__OS__/__ARCH__, plus this README and the project LICENSE. No runtime
files are required alongside the binary.

## Quickstart

    ./mino --version
    ./mino --help
    ./mino -e '(+ 1 2)'
    ./mino           # interactive REPL

## Documentation

Full docs: <https://mino-lang.org>

Source and issue tracker: <https://github.com/leifericf/mino>

## License

MIT. See `LICENSE` for the full text.

The binary embeds three MIT-licensed libraries (imath, BearSSL,
miniz) and Mozilla CA root certificate data (MPL-2.0). Every notice,
including the full MPL-2.0 text, is in `THIRD_PARTY_LICENSES.md`.
