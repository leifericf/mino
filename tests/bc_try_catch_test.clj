(require "tests/test")

;; Try/catch/throw exercised inside a defn body so the bytecode compiler
;; takes the form (top-level try forms run on the tree-walker and miss
;; the bc lane regardless). The point is to verify PUSHCATCH /
;; POPCATCH / THROW handle every reasonable nesting and unwind shape;
;; tree-walker parity is the contract, not new semantics.

(deftest bc-try-catch-simple
  (testing "throw in body, caught in same fn"
    (defn t1 [] (try (throw "boom") (catch e (ex-data e))))
    (is (= "boom" (t1))))
  (testing "throw of a map, catch reads :type"
    (defn t2 [] (try (throw {:type :err :v 42}) (catch e (:type (ex-data e)))))
    (is (= :err (t2))))
  (testing "no-throw path returns body value"
    (defn t3 [] (try 99 (catch e :no)))
    (is (= 99 (t3))))
  (testing "body computes through several ops, no throw"
    (defn t4 [] (try (let [a 10 b 20] (+ a b)) (catch e :no)))
    (is (= 30 (t4)))))

(deftest bc-try-catch-deep-throw
  (testing "throw from a callee unwinds into caller's catch"
    (defn deep []  (throw "deep"))
    (defn outer [] (try (deep) (catch e (ex-data e))))
    (is (= "deep" (outer))))
  (testing "throw from nested fn call chain"
    (defn lvl3 [] (throw {:level 3}))
    (defn lvl2 [] (lvl3))
    (defn lvl1 [] (lvl2))
    (defn top  [] (try (lvl1) (catch e (:level (ex-data e)))))
    (is (= 3 (top)))))

(deftest bc-try-catch-nested
  (testing "inner catch and re-throw to outer"
    (defn t1 [] (try (try (throw :inner)
                          (catch e (throw {:from :inner})))
                     (catch e (:from (ex-data e)))))
    (is (= :inner (t1))))
  (testing "inner handles, outer doesn't run"
    (defn t2 [] (try (try (throw :x)
                          (catch e :inner-caught))
                     (catch e :outer-caught)))
    (is (= :inner-caught (t2)))))

(deftest bc-try-finally
  (testing "finally runs on normal completion"
    (let [a (atom 0)]
      (defn t [] (try 7 (finally (reset! a 42))))
      (is (= 7 (t)))
      (is (= 42 @a))))
  (testing "finally runs on uncaught throw"
    (let [a (atom 0)]
      (defn t [] (try (throw "boom")
                      (finally (reset! a 99))))
      (try (t) (catch _ nil))
      (is (= 99 @a))))
  (testing "finally runs on caught throw"
    (let [a (atom 0)]
      (defn t [] (try (throw "x")
                      (catch e :caught)
                      (finally (reset! a 1))))
      (is (= :caught (t)))
      (is (= 1 @a))))
  (testing "finally runs on re-throw from handler"
    (let [a (atom 0)]
      (defn t [] (try (try (throw "inner")
                           (catch e (throw "rethrown"))
                           (finally (swap! a inc)))
                      (catch e (ex-data e))))
      (is (= "rethrown" (t)))
      (is (= 1 @a)))))

(deftest bc-try-error-path
  (testing "thrown? macro works on BC-compiled throws"
    (defn boom-int []  (inc 9223372036854775807))
    (is (thrown? (boom-int))))
  (testing "catch sees normalized diagnostic"
    (defn t [] (try (throw :raw) (catch e (error? e))))
    (is (true? (t))))
  (testing "try value escapes through let/do without losing exception data"
    (defn t [] (let [r (try (do (throw {:k :v}) :unreached)
                            (catch e (ex-data e)))]
                 r))
    (is (= {:k :v} (t)))))
