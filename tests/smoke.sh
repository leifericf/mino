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

# v0.3 — keywords
run "keyword print" ':foo'            ':foo'
run "keyword eq"    '(= :a :a)'       'true'
run "keyword ne"    '(= :a :b)'       'false'
run "keyword list"  '(list :a :b :c)' '(:a :b :c)'

# v0.3 — vector literal and self-eval
run "vec empty"    '[]'                        '[]'
run "vec literal"  '[1 2 3]'                   '[1 2 3]'
run "vec evals"    '(def x 7)
[x (+ x 1) (* x x)]' '7
[7 8 49]'
run "vec nested"   '[[1 2] [3 4]]'             '[[1 2] [3 4]]'
run "vec eq"       '(= [1 2 3] [1 2 3])'       'true'
run "vec ne len"   '(= [1 2] [1 2 3])'         'false'

# v0.3 — map literal and self-eval
run "map empty"    '{}'                          '{}'
run "map literal"  '{:a 1 :b 2}'                 '{:a 1, :b 2}'
run "map commas"   '{:a 1, :b 2}'                '{:a 1, :b 2}'
run "map evals"    '(def x 7)
{:n x :sq (* x x)}' '7
{:n 7, :sq 49}'
run "map eq order" '(= {:a 1 :b 2} {:b 2 :a 1})' 'true'
run "map ne val"   '(= {:a 1} {:a 2})'           'false'

# v0.3 — collection primitives
run "count vec"   '(count [1 2 3])'          '3'
run "count map"   '(count {:a 1 :b 2})'      '2'
run "count list"  '(count (list 1 2 3))'     '3'
run "count nil"   '(count nil)'              '0'
run "count str"   '(count "hello")'          '5'
run "first vec"   '(first [10 20 30])'       '10'
run "rest vec"    '(rest [10 20 30])'        '(20 30)'
run "first nil"   '(first nil)'              'nil'
run "rest nil"    '(rest nil)'               'nil'
run "nth vec"     '(nth [10 20 30] 1)'       '20'
run "nth default" '(nth [10 20] 99 :none)'   ':none'
run "vector fn"   '(vector 1 2 3)'           '[1 2 3]'
run "hash-map fn" '(hash-map :a 1 :b 2)'     '{:a 1, :b 2}'
run "assoc map"   '(assoc {:a 1} :b 2)'      '{:a 1, :b 2}'
run "assoc over"  '(assoc {:a 1} :a 99)'     '{:a 99}'
run "assoc vec"   '(assoc [10 20 30] 1 99)'  '[10 99 30]'
run "assoc append" '(assoc [10 20] 2 30)'    '[10 20 30]'
run "get map"     '(get {:a 1 :b 2} :a)'     '1'
run "get default" '(get {:a 1} :no :def)'    ':def'
run "get vec"     '(get [10 20 30] 1)'       '20'
run "conj list"   "(conj '(1 2) 3 4)"        '(4 3 1 2)'
run "conj vec"    '(conj [1 2] 3 4)'         '[1 2 3 4]'
run "conj map"    '(conj {:a 1} [:b 2])'     '{:a 1, :b 2}'
run "update map"  '(update {:a 5} :a (fn (n) (* n 10)))' '{:a 50}'
run "keys"        '(keys {:a 1 :b 2})'       '(:a :b)'
run "vals"        '(vals {:a 1 :b 2})'       '(1 2)'

# v0.3 — collections compose with closures
run "build map"  '(def m (hash-map :x 1 :y 2))
(get (update m :x (fn (n) (+ n 100))) :x)' '{:x 1, :y 2}
101'

# v0.6 — defmacro + quasiquote
run "defmacro basic" '(defmacro twice (x) `(do ~x ~x))
(def n 0)
(def inc (fn () (def n (+ n 1)) n))
(twice (inc))' '#<macro>
0
#<fn>
2'
run "quasiquote splice" '(defmacro my-list (& xs) `(list ~@xs))
(my-list 1 2 3)' '#<macro>
(1 2 3)'
run "variadic rest"      '((fn (a & b) (list a b)) 1 2 3 4)' '(1 (2 3 4))'
run "macroexpand-1"      "(defmacro unless (c t f) (list 'if c f t))
(macroexpand-1 '(unless x 1 2))" '#<macro>
(if x 2 1)'
run "gensym fresh"       '(= (gensym) (gensym))' 'false'

# v0.6 — stdlib macros
run "when true"  '(when true 1 2 3)'       '3'
run "when false" '(when false :no)'        'nil'
run "cond"       '(cond false :a true :b)' ':b'
run "cond none"  '(cond false :a false :b)' 'nil'
run "and values" '(and 1 2 3)'             '3'
run "and short"  '(and 1 false 3)'         'false'
run "or values"  '(or nil nil 42)'         '42'
run "or none"    '(or false false false)'  'false'
run "thread ->"  '(-> 10 (- 3) (- 2))'     '5'
run "thread ->>" '(->> 10 (- 3) (- 2))'    '9'

# v0.5 — map iteration follows insertion order, not hash layout
run "map order preserved"    '(keys {:z 1 :a 2 :m 3 :b 4})' '(:z :a :m :b)'
run "map rebind keeps order" '(keys (assoc {:z 1 :a 2 :m 3} :a 99))' '(:z :a :m)'
run "map new key appends"    '(keys (assoc {:z 1 :a 2} :b 3 :c 4))' '(:z :a :b :c)'
run "map print order"        '{:c 3 :a 1 :b 2}' '{:c 3, :a 1, :b 2}'

# v0.5 — HAMT scales; exercises tree depth beyond one bitmap node
run "map 200 entries" '(let (m (loop (i 0 m {})
                                (if (< i 200) (recur (+ i 1) (assoc m i (* i 10))) m)))
  (list (count m) (get m 0) (get m 100) (get m 199) (get m 300 :miss)))' '(200 0 1000 1990 :miss)'

# v0.7 — GC stability: each case allocates far more than a single generation's
# worth of transient values, so the collector must be invoked and must leave
# the live set intact. Also run under `make test-gc-stress` which collects on
# every allocation.
run "gc long tail"    '(loop (i 0) (if (< i 50000) (recur (+ i 1)) i))' '50000'
run "gc vec churn"    '(count (loop (i 0 acc [])
                        (if (< i 2000) (recur (+ i 1) (conj acc i)) acc)))' '2000'
run "gc map churn"    '(get (loop (i 0 m {})
                        (if (< i 300) (recur (+ i 1) (assoc m i (* i 3))) m))
                       150)' '450'
run "gc closure churn" '(def make-inc (fn (n) (fn (x) (+ x n))))
(loop (i 0 acc 0)
  (if (< i 1000)
      (recur (+ i 1) ((make-inc i) acc))
      acc))' '#<fn>
499500'

# v0.4 — vectors scale beyond one leaf (crosses trie level boundaries)
run "vec 2000 build+nth" '(let (big (loop (i 0 v [])
  (if (< i 2000) (recur (+ i 1) (conj v i)) v)))
  (list (count big)
        (+ (nth big 0) (nth big 31) (nth big 32) (nth big 1023)
           (nth big 1024) (nth big 1999))))' '(2000 4109)'
run "vec assoc shares source" '(let (big (loop (i 0 v [])
                                    (if (< i 2000) (recur (+ i 1) (conj v i)) v))
                               big2 (assoc big 500 :x))
  (list (nth big 500) (nth big2 500) (= (count big) (count big2))))' '(500 :x true)'

# v0.8 — type predicates
run "string? yes"  '(string? "hi")'        'true'
run "string? no"   '(string? 42)'          'false'
run "number? int"  '(number? 42)'          'true'
run "number? flt"  '(number? 3.14)'        'true'
run "number? no"   '(number? "x")'         'false'
run "keyword? yes" '(keyword? :k)'         'true'
run "keyword? no"  '(keyword? "k")'        'false'
run "symbol? yes"  "(symbol? 'x)"          'true'
run "symbol? no"   '(symbol? :x)'          'false'
run "vector? yes"  '(vector? [1 2])'       'true'
run "vector? no"   '(vector? 1)'           'false'
run "map? yes"     '(map? {:a 1})'         'true'
run "map? no"      '(map? [1])'            'false'
run "fn? prim"     '(fn? +)'               'true'
run "fn? closure"  '(fn? (fn (x) x))'      'true'
run "fn? no"       '(fn? 42)'              'false'

# v0.8 — type reflection
run "type int"     '(type 42)'             ':int'
run "type string"  '(type "hi")'           ':string'
run "type nil"     '(type nil)'            ':nil'
run "type list"    "(type '(1 2))"         ':list'
run "type fn"      '(type +)'              ':fn'
run "type vec"     '(type [1 2])'          ':vector'
run "type map"     '(type {:a 1})'         ':map'
run "type kw"      '(type :foo)'           ':keyword'

# v0.8 — str (string concatenation / coercion)
run "str cat"      '(str "hello" " " "world")'  '"hello world"'
run "str num"      '(str "n=" 42)'               '"n=42"'
run "str mixed"    '(str :hi " " 3.14)'          '":hi 3.14"'
run "str empty"    '(str)'                        '""'
run "str nil"      '(str "a" nil "b")'            '"ab"'

# v0.8 — I/O: println and prn return nil (output goes to stdout)
run "println ret"  '(println "test")'      'test
nil'
run "prn ret"      '(prn 1 2 3)'           '1 2 3
nil'

# v0.9 — try/catch/throw
run "try no throw"   '(try (+ 1 2) (catch e :fail))'    '3'
run "try catch str"  '(try (throw "oops") (catch e (str "caught: " e)))' '"caught: oops"'
run "try catch num"  '(try (throw 42) (catch e (* e 10)))' '420'
run "try catch nil"  '(try (throw nil) (catch e (nil? e)))' 'true'
run "try catch map"  '(try (throw {:k :v}) (catch e (get e :k)))' ':v'
run "try nested"     '(try (try (throw 1) (catch e (throw (+ e 1)))) (catch e e))' '2'
run "try fn throw"   '(def boom (fn () (throw "bang")))
(try (boom) (catch e e))' '#<fn>
"bang"'
run "throw unhandled" '(def x (try (throw "err") (catch e e)))
x' '"err"
"err"'

# v0.9 — source locations in errors
run "srcloc line 1"   '(+ z 1)' ''
run "srcloc line 3"   '(def a 1)
(def b 2)
(+ c 3)' '1
2'

# Capture eval error output for location tests
run_err() {
    desc=$1
    input=$2
    expected=$3
    actual=$(printf '%s\n' "$input" | ./mino 2>&1 1>/dev/null)
    if printf '%s\n' "$actual" | grep -qF "$expected"; then
        pass=$((pass + 1))
        printf '  ok   %s\n' "$desc"
    else
        fail=$((fail + 1))
        printf '  FAIL %s\n' "$desc"
        printf '    expected to contain:\n'
        printf '%s\n' "$expected" | sed 's/^/      /'
        printf '    got:\n'
        printf '%s\n' "$actual" | sed 's/^/      /'
    fi
}

run_err "srcloc in error"  '(+ z 1)' '<input>:1: unbound symbol: z'
run_err "srcloc multiline" '(def a 1)
(def b 2)
(+ c 3)' '<input>:3: unbound symbol: c'
run_err "trace in error"   '(def f (fn (x) (+ x z)))
(f 5)' 'in fn (<input>:2)'

# v0.9 — sandbox: default core has no I/O (tested by our REPL which
# explicitly opts in; the exit criterion is that an env without
# mino_install_io cannot access println/prn/slurp)
# These are verified at the C level by embed.c; the smoke test below
# just confirms throw propagates correctly without an enclosing try.
run_err "throw no try" '(throw "boom")' 'unhandled exception: boom'

# v0.10 — var redefinition with live reference update
run "var redef closure" '(def f (fn () x))
(def x 10)
(f)
(def x 20)
(f)' '#<fn>
10
10
20
20'

# v0.10 — doc / source / apropos
run "doc with docstring" '(def inc "increment by one" (fn (x) (+ x 1)))
(doc (quote inc))' '#<fn>
"increment by one"'

run "doc no docstring" '(def y 42)
(doc (quote y))' '42
nil'

run "source returns form" '(def sq "square" (fn (x) (* x x)))
(car (source (quote sq)))' '#<fn>
def'

run "defmacro docstring" '(defmacro my-id "identity macro" (x) x)
(doc (quote my-id))' '#<macro>
"identity macro"'

run "apropos finds" '(apropos "co")' '(cons count conj cons? cond)'

run "apropos empty" '(apropos "zzzznotfound")' 'nil'

# v0.10 — cycle-safe printing (depth guard)
# Build a deeply nested list that exceeds 128-depth guard; verify
# the REPL does not crash and the value is still a cons.
run "deep nest safe" '(def build (fn (n acc)
  (if (= n 0)
    acc
    (build (- n 1) (list acc)))))
(cons? (build 200 42))' '#<fn>
true'

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" = "0" ]
