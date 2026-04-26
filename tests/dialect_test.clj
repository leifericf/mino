(require "tests/test")

;; Dialect alignment tests: verify behavior matches standard expectations.
;; Covers gaps found during the core.clj compatibility verification.

;; --- Integer division ---

(deftest integer-division
  (testing "exact integer division returns integer"
    (is (= 5 (/ 10 2)))
    (is (integer? (/ 10 2)))
    (is (= 10 (/ 100 2 5)))
    (is (integer? (/ 100 2 5))))
  (testing "inexact integer division returns a ratio (Clojure dialect)"
    (is (ratio? (/ 10 3)))
    (is (= 10/3 (/ 10 3)))
    (is (float? (/ 7.0 2.0)))
    (is (= 3.5 (/ 7.0 2.0))))
  (testing "reciprocal"
    (is (= 1/2 (/ 2)))
    (is (= 0.5 (/ 2.0)))))

;; --- Variadic max / min ---

(deftest variadic-max-min
  (testing "max with multiple args"
    (is (= 5 (max 1 5 3)))
    (is (= -1 (max -10 -5 -1)))
    (is (= 9 (max 3 1 4 1 5 9 2 6)))
    (is (= 42 (max 42))))
  (testing "min with multiple args"
    (is (= 1 (min 1 5 3)))
    (is (= -10 (min -10 -5 -1)))
    (is (= 1 (min 3 1 4 1 5 9 2 6)))
    (is (= 42 (min 42)))))

;; --- first / rest on maps and sets ---

(deftest first-on-map
  (testing "first on map returns key-value pair"
    (is (= [:a 1] (first {:a 1 :b 2})))
    (is (nil? (first {}))))
  (testing "key and val on map entry"
    (is (= :a (key (first {:a 1}))))
    (is (= 1 (val (first {:a 1}))))))

(deftest first-on-set
  (testing "first on set returns an element"
    (is (some? (first #{:a :b :c})))
    (is (nil? (first #{})))))

(deftest rest-on-map
  (testing "rest on map returns remaining entries"
    (is (= 1 (count (rest {:a 1 :b 2}))))
    (is (nil? (rest {:a 1})))))

;; --- get-in with not-found ---

(deftest get-in-not-found
  (testing "3-arity get-in returns not-found for missing path"
    (is (= :nope (get-in {} [:a] :nope)))
    (is (= :missing (get-in {:a 1} [:b :c] :missing))))
  (testing "2-arity get-in returns nil for missing path"
    (is (nil? (get-in {:a 1} [:b :c]))))
  (testing "get-in succeeds on valid path"
    (is (= 42 (get-in {:a {:b 42}} [:a :b])))))

;; --- :or destructuring ---

(deftest or-destructuring
  (testing ":or provides defaults for missing keys"
    (let [{:keys [a b] :or {a 10 b 20}} {:a 42}]
      (is (= 42 a))
      (is (= 20 b))))
  (testing ":or with all missing"
    (let [{:keys [x y] :or {x 1 y 2}} {}]
      (is (= 1 x))
      (is (= 2 y))))
  (testing ":or not used when key present"
    (let [{:keys [a] :or {a 99}} {:a nil}]
      (is (nil? a)))))

;; --- New predicates ---

(deftest integer-predicate
  (is (integer? 42))
  (is (not (integer? 3.14)))
  (is (not (integer? "x"))))

(deftest coll-predicate
  (is (coll? [1 2]))
  (is (coll? '(1 2)))
  (is (coll? {:a 1}))
  (is (coll? #{1}))
  (is (not (coll? 42)))
  (is (not (coll? "hello")))
  (is (not (coll? nil))))

(deftest numeric-equality
  (testing "== compares across int/float"
    (is (== 1 1.0))
    (is (== 0 0.0))
    (is (not (== 1 2)))
    (is (== 1 1 1.0 1))))

(deftest empty-fn
  (testing "empty returns empty collection of same type"
    (is (= [] (empty [1 2 3])))
    (is (= {} (empty {:a 1})))
    (is (= #{} (empty #{1 2})))
    (is (nil? (empty nil)))))

(deftest re-pattern-fn
  (testing "re-pattern is identity for strings"
    (is (= "\\d+" (re-pattern "\\d+")))
    (is (= "abc" (re-find (re-pattern "abc") "xabcy")))))

;; --- juxt returns vector ---

(deftest juxt-returns-vector
  (is (= [6 4] ((juxt inc dec) 5)))
  (is (= [1 4 4] ((juxt first last count) [1 2 3 4])))
  (is (vector? ((juxt inc dec) 0))))

;; --- partition with step ---

(deftest partition-with-step
  (testing "partition without step"
    (is (= '((1 2) (3 4)) (partition 2 [1 2 3 4])))
    (is (= '((1 2)) (partition 2 [1 2 3]))))
  (testing "partition with step"
    (is (= '((1 2) (2 3) (3 4)) (partition 2 1 [1 2 3 4])))
    (is (= '((1 2 3) (3 4 5)) (partition 3 2 [1 2 3 4 5 6])))))

(deftest partition-all-returns-lists
  (testing "partition-all groups are lists"
    (is (= '((1 2) (3)) (partition-all 2 [1 2 3])))
    (is (seq? (first (partition-all 2 [1 2 3]))))))

;; --- try without catch ---

(deftest try-bare
  (testing "try without catch/finally just evaluates body"
    (is (= 42 (try 42)))
    (is (= 3 (try (+ 1 2))))))

;; --- Reader metadata ---

(deftest reader-metadata-symbol
  (testing "^Type attaches {:tag Type} metadata"
    (let [v (read-string "^String x")]
      (is (= {:tag 'String} (meta v)))))
  (testing "^:key attaches {key true} metadata"
    (let [v (read-string "^:dynamic x")]
      (is (= {:dynamic true} (meta v))))))

(deftest reader-metadata-on-vector
  (testing "^{...} attaches metadata to vector at read time"
    (let [v ^{:doc "hi"} [1 2 3]]
      (is (= {:doc "hi"} (meta v)))
      (is (= [1 2 3] v)))))

;; --- def without value ---

(deftest def-declaration
  (testing "(def name) creates an unbound var"
    (def decl-test-var__)
    (is (false? (bound? #'decl-test-var__)))
    (def decl-test-var__ 42)
    (is (true? (bound? #'decl-test-var__)))
    (is (= 42 decl-test-var__))))

;; --- Star variants ---

(deftest star-variants
  (testing "fn* works like fn"
    (is (= 42 ((fn* [x] x) 42))))
  (testing "let* works like let"
    (is (= 3 (let* [x 1 y 2] (+ x y)))))
  (testing "loop* works like loop"
    (is (= 5 (loop* [i 0] (if (< i 5) (recur (inc i)) i))))))

;; --- Defn / defmacro with attr-map ---

(deftest defn-attr-map
  (testing "defn with docstring and attr-map"
    (defn test-fn-attr__ "docstring" {:added "1.0"} [x] (+ x 1))
    (is (= 6 (test-fn-attr__ 5)))))

(deftest defmacro-attr-map
  (testing "defmacro with docstring and attr-map"
    (defmacro test-mac-attr__ "docstring" {:added "1.0"} [x]
      (list '+ x 1))
    (is (= 6 (test-mac-attr__ 5)))))

;; --- alter-meta! ---

(deftest alter-meta-bang
  (testing "alter-meta! mutates metadata in place"
    (let [v (with-meta [] {:a 1})]
      (alter-meta! v assoc :b 2)
      (is (= {:a 1 :b 2} (meta v))))))

;; --- Single quote in symbols ---

(deftest quote-in-symbols
  (testing "symbols can contain single quotes"
    (let [my-fn' (fn [a b] (+ a b))]
      (is (= 3 (my-fn' 1 2))))
    ;; trailing-quote in a binding name
    (let [foo' 42]
      (is (= 42 foo')))))
