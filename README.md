# mino

A tiny, embeddable Lisp in pure ANSI C.

Drop it into a C or C++ application and gain a programmable extension layer. The standalone executable is a convenience for development; the embedding API is the product.

Requires only an ANSI C compiler. No external dependencies.

```
gen_header() {
  src="$1"; sym="$2"; out="src/$sym.h"
  printf 'static const char *%s_src =\n' "$sym" > "$out"
  sed 's/\\/\\\\/g; s/"/\\"/g; s/^/    "/; s/$/\\n"/' "$src" >> "$out"
  printf '    ;\n' >> "$out"
}
gen_header src/core.clj                     core_mino
gen_header lib/clojure/string.clj           lib_clojure_string
gen_header lib/clojure/set.clj              lib_clojure_set
gen_header lib/clojure/walk.clj             lib_clojure_walk
gen_header lib/clojure/edn.clj              lib_clojure_edn
gen_header lib/clojure/pprint.clj           lib_clojure_pprint
gen_header lib/clojure/zip.clj              lib_clojure_zip
gen_header lib/clojure/data.clj             lib_clojure_data
gen_header lib/clojure/test.clj             lib_clojure_test
gen_header lib/clojure/template.clj         lib_clojure_template
gen_header lib/clojure/repl.clj             lib_clojure_repl
gen_header lib/clojure/stacktrace.clj       lib_clojure_stacktrace
gen_header lib/clojure/datafy.clj           lib_clojure_datafy
gen_header lib/clojure/core/protocols.clj   lib_clojure_core_protocols
gen_header lib/clojure/instant.clj          lib_clojure_instant
gen_header lib/clojure/spec/alpha.clj       lib_clojure_spec_alpha
gen_header lib/clojure/core/specs/alpha.clj lib_clojure_core_specs_alpha
cc -std=c99 -O2 \
  -Isrc -Isrc/public -Isrc/runtime -Isrc/gc -Isrc/eval \
  -Isrc/collections -Isrc/prim -Isrc/async -Isrc/interop \
  -Isrc/diag -Isrc/vendor/imath \
  -o mino \
  src/public/*.c src/runtime/*.c src/gc/*.c src/eval/*.c \
  src/collections/*.c src/prim/*.c src/async/*.c src/interop/*.c \
  src/regex/*.c src/diag/*.c src/vendor/imath/*.c \
  main.c -lm
./mino
```

Documentation: [mino-lang.org](https://mino-lang.org)

## Versioning

Pre-1.0.0: semantic versioning applies informally. Any minor version bump (0.X) may contain breaking changes to the embedding API, the language, or the standalone binary. Every break is called out under the corresponding version heading in `CHANGELOG.md` so embedders can audit the delta before upgrading. Patch versions (0.X.Y) are reserved for bug fixes and non-breaking additions.

Post-1.0.0: strict [SemVer 2.0.0](https://semver.org/spec/v2.0.0.html). Breaking changes happen only at major bumps; minor bumps add API; patch bumps fix bugs.

The ABI freeze is scheduled for the v1.0 cycle; until then, `src/mino.h` continues to carry evolving-API language.

## License

MIT
