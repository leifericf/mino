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
