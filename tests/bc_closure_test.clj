(require "tests/test")

;; Closures over fn params and lexical lets, exercised on the bytecode
;; compiler. The bc record is shared across every closure built from
;; one fn template, so the OP_GETGLOBAL_CACHED inline cache cannot
;; commit env-resolved values without poisoning sibling closures. A
;; regression here means two closures of `(fn [i] (fn [] i))` return
;; the same captured value -- previously [100 100] instead of [100 200].

(deftest bc-closure-captures-distinct-fn-params
  (testing "two closures over fn param resolve to distinct values"
    (defn mk-counter [i] (fn [] i))
    (is (= 100 ((mk-counter 100))))
    (is (= 200 ((mk-counter 200))))
    (let [c1 (mk-counter 1)
          c2 (mk-counter 2)]
      (is (= 1 (c1)))
      (is (= 2 (c2)))
      (is (= 1 (c1)))))
  (testing "many closures across a single mapv resolve independently"
    (defn outer [i] (fn [] i))
    (is (= [0 1 2 3 4 5 6 7 8 9]
           (mapv (fn [f] (f)) (mapv outer (range 10)))))))

(deftest bc-closure-captures-let-bindings
  (testing "closure over let binding is per-call"
    (defn mk [x] (let [a (* x 2)] (fn [] a)))
    (is (= 6 ((mk 3))))
    (is (= 10 ((mk 5)))))
  (testing "nested let with closure between"
    (defn mk [x]
      (let [a x b (+ x 1) c (+ x 2)]
        (fn [] [a b c])))
    (is (= [10 11 12] ((mk 10))))
    (is (= [20 21 22] ((mk 20))))))

(deftest bc-closure-captures-and-globals-coexist
  ;; True globals must still cache (the IC is the whole point); closure
  ;; free vars must not. Verifies the env-first probe didn't kill the
  ;; cache hit path for globals.
  (testing "closure references a global plus a captured param"
    (def k-mul 10)
    (defn mk [a] (fn [] (* a k-mul)))
    (is (= 30 ((mk 3))))
    (is (= 50 ((mk 5))))))

(deftest bc-closure-respects-dyn-over-env
  ;; Dyn lookup order: dyn -> env -> ns. With my IC fix, env-first
  ;; cannot be allowed to win over an active dyn binding; otherwise a
  ;; closure that happens to have a sibling-name lexically bound at
  ;; ns-install time (e.g. *agent* lives in env with root nil) would
  ;; mask the worker-thread dyn binding for the same name.
  (def ^:dynamic *bc-cls-x* :root)
  (testing "dyn binding shadows env binding"
    (defn read-x [] *bc-cls-x*)
    (is (= :root (read-x)))
    (is (= :bound (binding [*bc-cls-x* :bound] (read-x))))
    (is (= :root (read-x)))))
