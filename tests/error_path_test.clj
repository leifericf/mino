(require "tests/test")

;; Error path regression tests: verify that exception-producing error
;; conditions are catchable and produce meaningful messages.

(deftest throw-catch-roundtrip
  (testing "thrown string is catchable"
    (is (= "oops" (try (throw "oops") (catch e (ex-data e))))))
  (testing "thrown map is catchable"
    (is (= :err (try (throw {:type :err}) (catch e (:type (ex-data e)))))))
  (testing "nested throw propagates"
    (is (= "inner"
           (try (try (throw "inner")
                     (catch e (throw e)))
                (catch e (ex-data e))))))
  (testing "catch always receives diagnostic map"
    (is (error? (try (throw "x") (catch e e))))))

(deftest finally-runs-on-error
  (testing "finally runs after catch"
    (let [a (atom 0)]
      (try (throw "x")
           (catch e nil)
           (finally (reset! a 1)))
      (is (= 1 @a))))
  (testing "finally runs on success"
    (let [a (atom 0)]
      (try :ok
           (finally (reset! a 1)))
      (is (= 1 @a)))))

(deftest binding-dynamic-scope
  (testing "binding establishes dynamic scope"
    (def ^:dynamic *ept-x* 0)
    (is (= 42 (binding [*ept-x* 42] *ept-x*)))
    (is (= 0 *ept-x*)))
  (testing "binding with vector form"
    (def ^:dynamic *ept-y* 0)
    (is (= 99 (binding [*ept-y* 99] *ept-y*))))
  (testing "binding restores on exception"
    (def ^:dynamic *ept-z* 0)
    (try (binding [*ept-z* 42] (throw "boom"))
         (catch e nil))
    (is (= 0 *ept-z*))))

(deftest serialization-roundtrip
  (testing "nested value survives serialize/deserialize"
    (let [v {:a [1 2 3] :b "hello" :c true}
          s (pr-str v)
          r (read-string s)]
      (is (= v r))))
  (testing "deeply nested structure"
    (let [v [[[[1]]] {:a {:b {:c 42}}}]
          s (pr-str v)
          r (read-string s)]
      (is (= v r)))))

(deftest arity-error-names-the-callee-and-counts
  ;; A fn or macro arity mismatch must name the callee and the
  ;; received-vs-expected counts so the call site is obvious from
  ;; the diagnostic. Without this, a typo or reader-conditional
  ;; elision (e.g. `(defonce x #?(:clj v))` collapsing to
  ;; `(defonce x)` on a dialect that doesn't match the conditional)
  ;; surfaces as the bare "macro arity mismatch" with no indication
  ;; which macro mismatched.
  (let [err (try (eval '(defonce only-name)) nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"arity" err)))
    (is (some? (re-find #"defonce" err))))
  ;; Same for fns.
  (let [_   (eval '(defn arity-test-fn [a b] (+ a b)))
        err (try (eval '(arity-test-fn 1)) nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"arity" err)))
    (is (some? (re-find #"arity-test-fn" err)))))

(deftest constructor-sugar-diagnostic-names-the-form
  ;; `(ClassName. args...)` is JVM Clojure's `(new ClassName args...)`
  ;; shorthand. mino has no JVM class layer, but the diagnostic must
  ;; name the constructor sugar specifically instead of surfacing the
  ;; misleading "unbound symbol: ClassName." -- the user wrote a
  ;; constructor call, not a symbol reference, and the message
  ;; should reflect that and point at the supported alternative
  ;; (defrecord + ->Name positional ctor).
  (let [err (try (eval '(FooClass. 1 2 3)) nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"FooClass\." err)))
    (is (some? (re-find #"constructor" err)))
    (is (some? (re-find #"defrecord" err))))
  ;; Same for dotted-path constructor names.
  (let [err (try (eval '(some.pkg.PersistentThing. 1)) nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"constructor" err)))))

(deftest constructor-sugar-symbol-value-still-constructible
  ;; The diagnostic only fires when the trailing-dot name is being
  ;; looked up. Constructing the symbol as a value -- e.g. via
  ;; `(symbol "Foo.")` or `'Foo.` -- must still work.
  (is (= "Foo." (str (symbol "Foo."))))
  (is (= "Foo." (str (quote Foo.)))))
