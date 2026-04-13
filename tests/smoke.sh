#!/bin/sh
# Smoke test: pipe a few expressions through the mino binary and compare
# stdout to expected output. Exits non-zero on any mismatch.

set -u

cd "$(dirname "$0")/.."

if [ ! -x ./mino ]; then
    echo "smoke: ./mino missing; run 'make' first" >&2
    exit 1
fi

pass=0
fail=0

run() {
    desc=$1
    input=$2
    expected=$3
    actual=$(printf '%s\n' "$input" | ./mino 2>/dev/null)
    if [ "$actual" = "$expected" ]; then
        pass=$((pass + 1))
        printf '  ok   %s\n' "$desc"
    else
        fail=$((fail + 1))
        printf '  FAIL %s\n' "$desc"
        printf '    expected:\n'
        printf '%s\n' "$expected" | sed 's/^/      /'
        printf '    got:\n'
        printf '%s\n' "$actual" | sed 's/^/      /'
    fi
}

run "self-eval int"     '42'                                 '42'
run "self-eval string"  '"hi"'                               '"hi"'
run "addition"          '(+ 1 2)'                            '3'
run "subtraction"       '(- 10 3 2)'                         '5'
run "negate"            '(- 7)'                              '-7'
run "multiply"          '(* 2 3 4)'                          '24'
run "float division"    '(/ 10 4)'                           '2.5'
run "mixed arith"       '(+ 1 2.5)'                          '3.5'
run "less than"         '(< 1 2 3)'                          'true'
run "less than false"   '(< 3 2 1)'                          'false'
run "equal int float"   '(= 1 1.0)'                          'true'
run "cons quote"        "(cons 1 '(2 3))"                    '(1 2 3)'
run "list primitive"    "(list 1 2 'a)"                      '(1 2 a)'
run "car"               "(car '(1 2 3))"                     '1'
run "cdr"               "(cdr '(1 2 3))"                     '(2 3)'
run "nil printing"      'nil'                                'nil'
run "true/false"        '(list true false)'                  '(true false)'

# Redefinable global env
run "def then read" "(def x 41)
(+ x 1)" "41
42"

run "def redefine" "(def x 1)
(def x 2)
x" "1
2
2"

# Multi-line form support
run "multi-line list" "(cons 1
  (cons 2
    (cons 3 nil)))" "(1 2 3)"

# Quote shorthand expansion
run "quote symbol" "'foo" "foo"
run "quote nested" "'(a (b c) d)" "(a (b c) d)"

# Comments
run "line comment" "; ignore me
(+ 1 1)" "2"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = "0" ]
