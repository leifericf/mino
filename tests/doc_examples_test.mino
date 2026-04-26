(require "tests/test")
(require '[clojure.string :refer [blank?]])

;; Documentation examples: clean, self-contained assertions designed
;; to appear in the Language Reference on mino-lang.org. Each deftest
;; name matches the function it documents so the site's example matcher
;; picks it up.

;; --- Special forms ---

(deftest def
  (def x 42)
  (is (= 42 x)))

(deftest defmacro
  (defmacro unless [cond then else]
    (list 'if cond else then))
  (is (= "no" (unless true "yes" "no")))
  (is (= "yes" (unless false "yes" "no"))))

(deftest quasiquote
  (let [x 1 y 2]
    (is (= '(1 2 3) `(~x ~y 3)))))

(deftest unquote
  (let [x 10]
    ;; Bare `+` in a syntax-quoted template auto-qualifies to clojure.core/+
    ;; because backquote namespace-resolves any non-locally-bound symbol.
    (is (= '(clojure.core/+ 10 1) `(+ ~x 1)))))

(deftest unquote-splicing
  (def xs '(1 2 3))
  (is (= '(0 1 2 3 4) `(0 ~@xs 4))))

(deftest defn
  (defn square [x] (* x x))
  (is (= 25 (square 5)))
  (is (= 1 (square 1))))

;; --- Predicates ---

(deftest not=
  (is (= true (not= 1 2)))
  (is (= false (not= 1 1))))

(deftest empty?
  (is (= true (empty? [])))
  (is (= false (empty? [1 2 3])))
  (is (= true (empty? nil))))

(deftest >=
  (is (= true (>= 3 2)))
  (is (= true (>= 3 3)))
  (is (= false (>= 2 3))))

(deftest nil?
  (is (= true (nil? nil)))
  (is (= false (nil? 0)))
  (is (= false (nil? false))))

(deftest string?
  (is (= true (string? "hello")))
  (is (= false (string? 42))))

(deftest number?
  (is (= true (number? 42)))
  (is (= true (number? 3.14)))
  (is (= false (number? "5"))))

(deftest keyword?
  (is (= true (keyword? :foo)))
  (is (= false (keyword? "foo"))))

(deftest symbol?
  (is (= true (symbol? 'x)))
  (is (= false (symbol? :x))))

(deftest vector?
  (is (= true (vector? [1 2 3])))
  (is (= false (vector? '(1 2 3)))))

(deftest map?
  (is (= true (map? {:a 1})))
  (is (= false (map? [1 2]))))

(deftest fn?
  (is (= true (fn? +)))
  (is (= true (fn? (fn [x] x))))
  (is (= false (fn? 42))))

(deftest set?
  (is (= true (set? #{1 2 3})))
  (is (= false (set? [1 2 3]))))

(deftest seq?
  (is (= true (seq? '(1 2 3))))
  (is (= false (seq? [1 2 3]))))

(deftest cons?
  (is (= true (cons? (cons 1 nil))))
  (is (= false (cons? [1 2]))))

(deftest ifn?
  (is (= true (ifn? +)))
  (is (= true (ifn? :foo)))
  (is (= true (ifn? {:a 1})))
  (is (= false (ifn? 42))))

(deftest true?
  (is (= true (true? true)))
  (is (= false (true? 1))))

(deftest false?
  (is (= true (false? false)))
  (is (= false (false? nil))))

(deftest boolean?
  (is (= true (boolean? true)))
  (is (= true (boolean? false)))
  (is (= false (boolean? nil))))

(deftest int?
  (is (= true (int? 42)))
  (is (= false (int? 3.14))))

(deftest integer?
  (is (= true (integer? 42)))
  (is (= false (integer? 3.14))))

(deftest float?
  (is (= true (float? 3.14)))
  (is (= false (float? 42))))

(deftest double?
  (is (= true (double? 3.14)))
  (is (= false (double? 42))))

(deftest coll?
  (is (= true (coll? [1 2])))
  (is (= true (coll? {:a 1})))
  (is (= false (coll? 42))))

(deftest list?
  (is (= true (list? '(1 2 3))))
  (is (= false (list? [1 2 3]))))

(deftest some?
  (is (= true (some? 0)))
  (is (= true (some? false)))
  (is (= false (some? nil))))

(deftest any?
  (is (= true (any? nil)))
  (is (= true (any? 42))))

(deftest sequential?
  (is (= true (sequential? [1 2])))
  (is (= true (sequential? '(1 2))))
  (is (= false (sequential? {:a 1}))))

(deftest associative?
  (is (= true (associative? {:a 1})))
  (is (= true (associative? [1 2])))
  (is (= false (associative? '(1 2)))))

(deftest seqable?
  (is (= true (seqable? [1 2])))
  (is (= true (seqable? "hi")))
  (is (= false (seqable? 42))))

(deftest indexed?
  (is (= true (indexed? [1 2])))
  (is (= false (indexed? '(1 2)))))

(deftest counted?
  (is (= true (counted? [1 2])))
  (is (= true (counted? {:a 1})))
  (is (= false (counted? (range)))))

(deftest reversible?
  (is (= true (reversible? [1 2])))
  (is (= false (reversible? '(1 2)))))

(deftest sorted?
  (is (= true (sorted? (sorted-set 1 2 3))))
  (is (= false (sorted? #{1 2 3}))))

(deftest distinct?
  (is (= true (distinct? 1 2 3)))
  (is (= false (distinct? 1 2 1))))

(deftest qualified-symbol?
  (is (= true (qualified-symbol? 'foo/bar)))
  (is (= false (qualified-symbol? 'foo))))

(deftest simple-symbol?
  (is (= true (simple-symbol? 'foo)))
  (is (= false (simple-symbol? 'foo/bar))))

(deftest qualified-keyword?
  (is (= true (qualified-keyword? :foo/bar)))
  (is (= false (qualified-keyword? :foo))))

(deftest simple-keyword?
  (is (= true (simple-keyword? :foo)))
  (is (= false (simple-keyword? :foo/bar))))

;; --- Numeric predicates ---

(deftest zero?
  (is (= true (zero? 0)))
  (is (= false (zero? 1))))

(deftest pos?
  (is (= true (pos? 1)))
  (is (= false (pos? -1))))

(deftest neg?
  (is (= true (neg? -1)))
  (is (= false (neg? 1))))

(deftest pos-int?
  (is (= true (pos-int? 1)))
  (is (= false (pos-int? -1)))
  (is (= false (pos-int? 1.0))))

(deftest neg-int?
  (is (= true (neg-int? -1)))
  (is (= false (neg-int? 1))))

(deftest nat-int?
  (is (= true (nat-int? 0)))
  (is (= true (nat-int? 5)))
  (is (= false (nat-int? -1))))

(deftest ==
  (is (= true (== 1 1.0)))
  (is (= false (== 1 2))))

;; --- Arithmetic ---

(deftest dec
  (is (= 4 (dec 5)))
  (is (= -1 (dec 0))))

;; --- Sequences ---

(deftest take-nth
  (is (= '(0 3 6 9) (take-nth 3 (range 10)))))

(deftest lazy-cat
  (is (= '(1 2 3 4) (lazy-cat [1 2] [3 4]))))

(deftest dorun
  (is (= nil (dorun (map identity [1 2 3])))))

(deftest run!
  (is (= nil (run! identity [1 2 3]))))

;; --- Collections ---

(deftest array-map
  (is (= {:a 1 :b 2} (array-map :a 1 :b 2))))

;; --- Threading ---

(deftest ->>
  (is (= [2 3 4] (->> [1 2 3] (map inc) vec))))

;; --- String ---

(deftest blank?
  (is (= true (blank? "")))
  (is (= true (blank? "   ")))
  (is (= false (blank? "hi"))))

;; --- Bitwise ---

(deftest bit-test
  (is (= true (bit-test 5 0)))
  (is (= false (bit-test 5 1)))
  (is (= true (bit-test 5 2))))

;; --- Functional ---

(deftest some-fn
  (let [f (some-fn :a :b)]
    (is (= 1 (f {:a 1})))
    (is (= 2 (f {:b 2})))))

;; --- Stateful ---

(deftest volatile!
  (let [v (volatile! 0)]
    (vswap! v inc)
    (is (= 1 @v))))

(deftest volatile?
  (is (= true (volatile? (volatile! 0))))
  (is (= false (volatile? 42))))

(deftest vswap!
  (let [v (volatile! 10)]
    (is (= 11 (vswap! v inc)))))

(deftest vreset!
  (let [v (volatile! 0)]
    (vreset! v 42)
    (is (= 42 @v))))

;; --- Macros and reflection ---

(deftest gensym
  (is (= true (symbol? (gensym))))
  (is (= false (= (gensym) (gensym)))))

(deftest macroexpand-1
  (is (= '(if true (do 1 2)) (macroexpand-1 '(when true 1 2)))))

(deftest macroexpand
  (is (= '(if true (do 1 2)) (macroexpand '(when true 1 2)))))

(deftest quote
  (is (= 'foo 'foo))
  (is (= '(1 2 3) '(1 2 3))))

(deftest doc
  (is (= true (string? (doc 'map)))))

(deftest source
  (is (= true (cons? (source 'when)))))

;; --- Strings ---

(deftest starts-with?
  (is (= true (starts-with? "hello" "hel")))
  (is (= false (starts-with? "hello" "world"))))

(deftest ends-with?
  (is (= true (ends-with? "hello" "llo")))
  (is (= false (ends-with? "hello" "hel"))))

(deftest includes?
  (is (= true (includes? "hello world" "world")))
  (is (= false (includes? "hello" "xyz"))))

;; --- Delay ---

(deftest delay
  (let [d (delay (+ 1 2))]
    (is (= 3 (force d)))))

(deftest delay?
  (is (= true (delay? (delay 1))))
  (is (= false (delay? 42))))

(deftest force
  (is (= 42 (force (delay 42))))
  (is (= 42 (force 42))))

(deftest deref-delay
  (let [d (delay (+ 1 2))]
    (is (= 3 (deref-delay d)))))

;; --- Higher-order ---

(deftest memoize
  (let [f (memoize (fn [x] (* x x)))]
    (is (= 25 (f 5)))
    (is (= 25 (f 5)))))

(deftest doto
  (is (= [1 2 3] (doto [1 2 3] count))))

;; --- Iteration ---

(deftest shuffle
  (let [s (shuffle [1 2 3 4 5])]
    (is (= 5 (count s)))
    (is (= #{1 2 3 4 5} (set s)))))

;; --- Tree walking ---

(deftest walk
  (is (= [2 3 4] (walk inc vec [1 2 3]))))

(deftest prewalk
  (is (= [2 [3 [4]]] (prewalk #(if (number? %) (inc %) %) [1 [2 [3]]]))))

;; --- Transducers ---

(deftest cat
  (is (= [1 2 3 4] (into [] cat [[1 2] [3 4]]))))

(deftest unreduced
  (is (= 42 (unreduced (reduced 42))))
  (is (= 42 (unreduced 42))))

;; --- Type predicates ---

(deftest char?
  (is (= false (char? "a")))
  (is (= false (char? 65))))

(deftest ratio?
  (is (= false (ratio? 1)))
  (is (= false (ratio? 1.5))))

(deftest decimal?
  (is (= false (decimal? 1.0)))
  (is (= false (decimal? 1))))

(deftest rational?
  (is (= true (rational? 42)))
  (is (= false (rational? 3.14))))

;; --- Random ---

(deftest rand-int
  (let [n (rand-int 10)]
    (is (= true (and (>= n 0) (< n 10))))))

(deftest rand-nth
  (let [x (rand-nth [1 2 3])]
    (is (= true (contains? #{1 2 3} x)))))
