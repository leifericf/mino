#!/bin/sh
# crash_test.sh — feed adversarial inputs through the reader and verify
# it never crashes. Exits non-zero on any segfault/abort.
#
# Build:  make fuzz-stdin
# Run:    ./fuzz/crash_test.sh

set -u

cd "$(dirname "$0")/.."

READER=./fuzz/fuzz_reader
if [ ! -x "$READER" ]; then
    echo "crash_test: $READER missing; run 'make fuzz-stdin' first" >&2
    exit 1
fi

pass=0
fail=0

feed() {
    desc=$1
    input=$2
    printf '%s' "$input" | "$READER" 2>/dev/null
    rc=$?
    if [ $rc -eq 0 ]; then
        pass=$((pass + 1))
        printf '  ok   %s\n' "$desc"
    else
        fail=$((fail + 1))
        printf '  CRASH %s (exit %d)\n' "$desc" "$rc"
    fi
}

# --- Unterminated forms ---
feed "unterminated list"          "(1 2 3"
feed "unterminated vector"        "[1 2 3"
feed "unterminated map"           "{:a 1"
feed "unterminated set"           "#{1 2"
feed "unterminated string"        '"hello'
feed "unterminated escape"        '"hello\'

# --- Mismatched delimiters ---
feed "close paren only"           ")"
feed "close bracket only"         "]"
feed "close brace only"           "}"
feed "paren-bracket mismatch"     "(1 2]"
feed "bracket-paren mismatch"     "[1 2)"
feed "brace-paren mismatch"       "{:a 1)"

# --- Empty / whitespace ---
feed "empty input"                ""
feed "only whitespace"            "   "
feed "only newlines"              "


"
feed "only comments"              "; just a comment"
feed "comment no newline"         ";; no newline"

# --- Edge-case atoms ---
feed "lone colon"                 ":"
feed "lone hash"                  "#"
feed "hash no brace"              "#x"
feed "lone tilde"                 "~"
feed "lone backtick"              "\`"
feed "lone quote"                 "'"
feed "lone at"                    "@"
feed "tilde at"                   "~@"
feed "double quote only"          '"'

# --- Deeply nested ---
feed "deep parens"                "((((((((((((((((((((((((((((((1))))))))))))))))))))))))))))))"
feed "deep vectors"               "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]"
feed "deep maps"                  "{:a {:b {:c {:d {:e {:f {:g {:h {:i {:j 1}}}}}}}}}}"

# --- Odd map forms ---
feed "map odd count"              "{:a 1 :b}"
feed "map empty"                  "{}"
feed "set empty"                  "#{}"

# --- Long strings ---
feed "long string"                "\"$(printf 'a%.0s' $(seq 1 10000))\""
feed "long symbol"                "$(printf 'x%.0s' $(seq 1 5000))"

# --- Binary junk ---
feed "null bytes"                 "$(printf '\x00\x00\x00')"
feed "high bytes"                 "$(printf '\xff\xfe\xfd')"
feed "mixed junk"                 "$(printf '(\x00\xff\"\\n)')"

# --- Quote edge cases ---
feed "quote eof"                  "'"
feed "quasiquote eof"             "\`"
feed "unquote eof"                "~"
feed "splice eof"                 "~@"
feed "quote in list"              "(')"
feed "double quote list"          "(''x)"

# --- Numbers ---
feed "just minus"                 "-"
feed "just dot"                   "."
feed "leading zeros"              "007"
feed "huge integer"               "999999999999999999999999999999999999999999"
feed "many decimals"              "3.14159265358979323846264338327950288"
feed "negative float"             "-0.0"
feed "exponent"                   "1e999"

# --- Repeated specials ---
feed "many quotes"                "''''''''x"
feed "many tildes"                "~~~~~~~~x"
feed "alternating delims"         "([{([{([{([{"

# --- Corpus files ---
for f in fuzz/corpus/*.mino; do
    if [ -f "$f" ]; then
        "$READER" < "$f" 2>/dev/null
        rc=$?
        if [ $rc -eq 0 ]; then
            pass=$((pass + 1))
            printf '  ok   corpus: %s\n' "$(basename "$f")"
        else
            fail=$((fail + 1))
            printf '  CRASH corpus: %s (exit %d)\n' "$(basename "$f")" "$rc"
        fi
    fi
done

echo ""
printf '%d passed, %d crashed\n' "$pass" "$fail"
[ $fail -eq 0 ]
