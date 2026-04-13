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

# v0.2 — conditional, sequencing, truthiness
run "if true"        '(if true 1 2)'           '1'
run "if false"       '(if false 1 2)'          '2'
run "if nil"         '(if nil 1 2)'            '2'
run "if zero truthy" '(if 0 "t" "f")'          '"t"'
run "if empty str"   '(if "" "t" "f")'         '"t"'
run "if no-else"     '(if false 1)'            'nil'
run "do last"        '(do 1 2 3)'              '3'
run "do side"        '(do (def d 7) d)'        '7'

# v0.2 — let and lexical scope
run "let single"     '(let (x 5) x)'           '5'
run "let multi"      '(let (x 1 y 2) (+ x y))' '3'
run "let sequential" '(let (x 1 y (+ x 10)) y)' '11'
run "let shadow"     '(def a 1)
(let (a 99) a)
a' '1
99
1'

# v0.2 — extended comparisons
run "<= chain"  '(<= 1 2 2 3)' 'true'
run ">  chain"  '(> 3 2 1)'    'true'
run ">= chain"  '(>= 3 3 1)'   'true'
run ">  false"  '(> 1 2)'      'false'

# v0.2 — fn, closures, higher-order
run "fn lambda"   '((fn (x) (* x x)) 7)' '49'
run "fn no-arg"   '((fn () 42))'          '42'
run "fn def"      '(def sq (fn (x) (* x x)))
(sq 9)' '#<fn>
81'
run "closure"     '(def adder (fn (n) (fn (x) (+ x n))))
(def add5 (adder 5))
(add5 10)' '#<fn>
#<fn>
15'
run "higher order" '(def apply-twice (fn (f x) (f (f x))))
(apply-twice (fn (n) (+ n 1)) 10)' '#<fn>
12'

# v0.2 — loop and recur
run "loop countdown" '(loop (n 5 acc 1)
  (if (<= n 1) acc (recur (- n 1) (* acc n))))' '120'
run "factorial"      '(def fact (fn (n)
  (loop (i n acc 1) (if (<= i 1) acc (recur (- i 1) (* acc i))))))
(fact 10)' '#<fn>
3628800'
run "fib"            '(def fib (fn (n)
  (loop (i 0 a 0 b 1) (if (< i n) (recur (+ i 1) b (+ a b)) a))))
(fib 20)' '#<fn>
6765'
run "recur in fn"    '(def count-down (fn (n) (if (<= n 0) "done" (recur (- n 1)))))
(count-down 1000)' '#<fn>
"done"'

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = "0" ]
