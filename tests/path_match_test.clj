(require "tests/test")
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; path-glob-match: the pure matcher under the walker (ADR 22).
;; Golden vectors pin every syntax element and the cross-language
;; disagreement rules: * does not cross /, ** is only a whole
;; segment (zero or more directories, trailing ** eats everything),
;; dotfile visibility is NOT matcher policy (the walker filters).
;; The fuzz tier pins the untrusted-input contract: any pattern
;; and string either match or not, never crash; the escape law
;; holds for every string.

(defn pq [p seed n]
  (:result (tc/quick-check n p :seed seed)))

(defn m [pat s] (path-glob-match pat s))

(def pat-alphabet ["*" "?" "[" "]" "{" "}" "," "\\" "!" "a" "b" "c"
                   "/" "." "-" "x"])
(def garbage-gen
  (gen/fmap (fn [cs] (apply str cs))
            (gen/vector (gen/elements
                          (mapv char (filter #(not= % 92)
                                             (range 33 127))))
                        0 24)))
(def pat-garbage-gen
  (gen/fmap (fn [cs] (apply str cs))
            (gen/vector (gen/elements pat-alphabet) 0 32)))

;;; star

(deftest star-goldens
  (are [expected pat s] (= expected (m pat s))
    true  "*"         "a"
    true  "*"         ".foo"          ; visibility is walker policy
    true  "*"         "abc"
    true  "*"         ""              ; zero chars, fnmatch parity
    false "*"         "a/b"           ; star does not cross /
    true  "*.clj"     "a.clj"
    false "*.clj"     "a/b.clj"
    true  "a*b"       "ab"
    true  "a*b"       "axxb"
    false "a*b"       "a/b"
    true  "a*b*c"     "abc"
    true  "a*b*c"     "axxbyyc"))

;;; question mark

(deftest qmark-goldens
  (are [expected pat s] (= expected (m pat s))
    true  "?"    "a"
    false "?"    "ab"
    false "?"    ""
    false "?"    "/"                ; / never matches a wildcard
    false "a?b"  "a/b"
    true  "a?b"  "axb"
    true  "???"  "abc"))

;;; classes

(deftest class-goldens
  (are [expected pat s] (= expected (m pat s))
    true  "[abc]x"    "bx"
    false "[abc]x"    "dx"
    true  "[a-c]x"    "ax"
    true  "[a-c]x"    "cx"
    false "[a-c]x"    "dx"
    true  "[!a-c]x"   "dx"
    false "[!a-c]x"   "ax"
    true  "[0-9][0-9]" "42"
    false "[0-9][0-9]" "4a"
    true  "[]a]x"     "]x"           ; ] first in the class is a member
    true  "[-x]a"     "-a"           ; leading dash is literal
    false "[ab"       "a"            ; unterminated: no match, no crash
    true  "[a\\]]b"   "]b"           ; escaped ] inside a class
    false "[/]"       "/"            ; / never matches even when listed
    false "[!a]"      "/"))

;;; braces

(deftest brace-goldens
  (are [expected pat s] (= expected (m pat s))
    true  "{foo,bar}.txt"  "foo.txt"
    true  "{foo,bar}.txt"  "bar.txt"
    false "{foo,bar}.txt"  "baz.txt"
    true  "{a,{b,c}}.txt"  "b.txt"   ; nested
    true  "{a,{b,c}}.txt"  "c.txt"
    true  "{a,{b,c}}.txt"  "a.txt"
    false "{a,{b,c}}.txt"  "d.txt"
    true  "{,x}.txt"       ".txt"    ; empty alternative
    true  "{,x}.txt"       "x.txt"
    true  "{foo,bar},c"    "bar,c"   ; comma inside braces only
    false "{foo,bar},c"    "foo"
    true  "a{b?c,[0-9]}d"  "ab7cd"   ; wildcards inside braces
    true  "a{b?c,[0-9]}d"  "a5d"
    false "a{b?c,[0-9]}d"  "ab7d"
    false "a{b?c,[0-9]}d"  "abcd"
    true  "{ab"             "{ab"))   ; unterminated: literal brace

;;; escapes

(deftest escape-goldens
  (are [expected pat s] (= expected (m pat s))
    true  "a\\*b"   "a*b"           ; escaped star is literal
    false "a\\*b"   "axb"
    true  "a\\?b"   "a?b"
    true  "a\\[b"   "a[b"
    true  "a\\\\b"  "a\\b"          ; escaped backslash
    true  "a\\"     "a\\"           ; trailing backslash is literal
    false "a\\"     "ab"))

;;; doublestar (whole segment only)

(deftest doublestar-goldens
  (are [expected pat s] (= expected (m pat s))
    true  "**"         "a"          ; trailing ** eats everything
    true  "**"         "a/b/c"
    true  "**"         ""
    true  "**/*.clj"   "a.clj"      ; zero directories
    true  "**/*.clj"   "a/b.clj"
    true  "**/*.clj"   "a/b/c.clj"
    false "**/*.clj"   "a/b.clj.txt"
    true  "a/**/b"     "a/b"        ; the zero-dir case
    true  "a/**/b"     "a/x/b"
    true  "a/**/b"     "a/x/y/z/b"
    false "a/**/b"     "a/x/y/b/c"
    true  "src/**"     "src/a/b.clj"
    true  "src/**"     "src/a"
    false "src/**"     "src"
    true  "a**"        "abc"        ; not whole segment: plain star
    false "a**"        "a/b"
    true  "a/**b"      "a/xb"       ; ** mid-segment: plain star
    true  "a/**b"      "a/b"
    false "**a"        "x/ya"       ; leading ** not whole segment
    true  "**a"        "xxa"))

(deftest doublestar-retry-cursor-regression
  ;; review round: the ** retry scan once started from a cursor the
  ;; failed zero-directory attempt had already advanced, skipping
  ;; earlier / boundaries
  (are [expected pat s] (= expected (m pat s))
    true  "**/a/a"     "a/a/a"
    true  "**/ab/ab"   "ab/ab/ab"
    true  "**/x/y"     "x/x/y"
    false "**/a/b"     "a/a/c"))

;;; literals and edges

(deftest literal-goldens
  (are [expected pat s] (= expected (m pat s))
    true  "abc"   "abc"
    false "abc"   "abd"
    false "abc"   "ab"
    false "abc"   "abcd"
    true  ""      ""
    false ""      "a"
    true  "a/b/c" "a/b/c"
    true  "a.b"   "a.b"))

;;; errors

(deftest matcher-errors
  (is (thrown? (path-glob-match :kw "a")))
  (is (thrown? (path-glob-match "a" 42)))
  (is (thrown? (path-glob-match "a")))
  (is (thrown-with-msg? #"path-glob-match" (path-glob-match "*" 42)))
  (is (= :eval/bounds
         (try (path-glob-match (apply str (repeat 257 "a")) "a")
              (catch e (:mino/kind e))))
      "patterns over 256 bytes throw :eval/bounds"))

(deftest matcher-backtracking-budget
  ;; review round: adversarial star-heavy patterns against a long
  ;; subject once hung the matcher; the work budget turns that into
  ;; a classified :eval/bounds throw (the fuzz contract is
  ;; match-or-classified-throw, temporally as well). Proving a
  ;; non-match through 120 stars is exponential, so it throws;
  ;; honest patterns on the same subject stay boolean.
  (let [subject (apply str (repeat 8000 "a"))
        stars (str (apply str (interpose "a" (repeat 100 "*"))) "b")]
    (is (= :eval/bounds
           (try (path-glob-match stars subject)
                (catch e (:mino/kind e))))))
  (let [subject (apply str (repeat 8000 "a"))]
    (is (true? (path-glob-match "*a*" subject)))
    (is (false? (path-glob-match "*z*" subject)))))

;;; fuzz: untrusted patterns and strings never crash

(deftest matcher-total-over-garbage-prop
  (is (pq (prop/for-all [pat pat-garbage-gen
                         s garbage-gen]
                         (or (true? (m pat s))
                             (false? (m pat s))))
          20260827 500)))

(deftest matcher-mutation-prop
  ;; mutate valid patterns; the matcher still answers a boolean
  (is (pq (prop/for-all [base (gen/elements ["**/*.clj" "a/**/b"
                                             "{foo,bar}.txt" "[a-c]?x"
                                             "a\\*b" "*/*/*.md"])
                         i (gen/choose 0 20)
                         c (gen/elements pat-alphabet)]
                         (let [p (if (<= i (count base))
                                   (str (subs base 0 (min i (count base)))
                                        c
                                        (subs base (min i (count base))))
                                   base)]
                           (or (true? (m p "a/b.clj"))
                               (false? (m p "a/b.clj")))))
          20260828 500)))

(deftest escape-law-prop
  ;; every string matches its fully escaped pattern
  (let [special? #{\* \? \[ \] \{ \} \, \\}
        escape (fn [s]
                 (apply str (mapcat (fn [ch]
                                      (if (special? ch) [\\ ch] [ch]))
                                    s)))]
    (is (pq (prop/for-all [s garbage-gen]
                           (true? (m (escape s) s)))
            20260829 500))))

(run-tests-and-exit)
