# Conformance Status

Measured against the external conformance test suite (222 test files
covering `clojure.core` functions).

## Current Metrics

- **114 / 222** test files pass cleanly (51%)
- **181 / 222** test files run to completion (82%)
- **2602 / 2850** assertions pass on runnable files (91%)
- Curated regression runner covers all cleanly passing files (100% pass)

Run `make test-external` to execute the curated regression runner.

## Intentional Divergences

These are design decisions, not bugs. They follow from mino's goals as
a lightweight embeddable interpreter.

### No distinct empty list

mino's `rest` has `next` semantics: it returns nil when exhausted,
not an empty list. `(list)` returns nil. `(when (list) :foo)` evaluates
to nil rather than `:foo`. This avoids the need for a separate empty
list type distinct from nil.

### No big number types

Ratios parse but convert to int/float. `1N` and `1M` suffixes are
consumed by the reader but produce int/float values. `decimal?` and
`ratio?` always return false. `int?` returns false for `0N`.
Arithmetic does not auto-promote to big numbers on overflow.

### No transients

`transient`, `persistent!`, and bang variants (`assoc!`, `conj!`, etc.)
are not implemented. `volatile!` is provided as an alias for `atom`.

### No multimethods or records

`defmulti`, `defmethod`, `defrecord`, `deftype` are not implemented.
Protocol-based dispatch covers current use cases.

### No JVM interop

`object-array`, `to-array`, `clojure.lang.MapEntry/create`, and JVM
class names like `Object` and `String` are not available. Host interop
uses mino's capability registry instead.

### No delay primitive

`delay` is implemented as a macro using atoms. It works for typical
use cases but is not a primitive type, so `(type (delay 1))` returns
`:map` rather than a dedicated type keyword.

### Integer overflow wraps silently

`(+ Long/MAX_VALUE 1)` wraps to `Long/MIN_VALUE` instead of throwing
or auto-promoting. This matches C semantics.

### Float precision follows IEEE 754

No exact rational arithmetic. `(+ 1 -0.666667)` may not exactly equal
`0.333333` due to floating point representation.

### No binary integer literals via `0b` prefix

The `0b1111` prefix form is not supported. Use radix literals instead:
`2r1111` (binary), `8r17` (octal), `16rF` (hex), or any base 2-36.
