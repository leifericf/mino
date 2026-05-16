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

;; Self-tail-call must not let later iterations clobber closures
;; captured in earlier iterations. Before this regression test was
;; added, the apply_callable trampoline reused `local` on self-tail-
;; call and mutated param slots in place via bind_params, so a
;; closure built in iteration N silently observed iteration N+1's
;; param values after the next recur landed -- a silent Clojure-
;; semantics divergence. The fix allocates a fresh env_child per
;; iteration; this suite pins that behavior down across recur,
;; named self-call, loop+recur, dotimes, and the macro-introduced
;; closure shapes (future / delay / for) that the source-form
;; scanner can't see directly.

(deftest closure-capture-named-self-tail-call
  (testing "self-tail-call via fn name captures per-iteration param"
    (let [cls (atom [])]
      (defn G-named [i]
        (swap! cls conj (fn [] i))
        (when (< i 4) (G-named (inc i))))
      (G-named 0)
      (is (= [0 1 2 3 4] (mapv (fn [f] (f)) @cls))))))

(deftest closure-capture-loop-recur
  (testing "loop+recur with closures over loop locals"
    (let [fs (loop [i 0 acc []]
               (if (>= i 5) acc
                   (recur (inc i) (conj acc (fn [] i)))))]
      (is (= [0 1 2 3 4] (mapv (fn [f] (f)) fs)))))
  (testing "loop+recur side-effect closure capture"
    (let [cls (atom [])]
      (loop [i 0]
        (when (< i 3)
          (swap! cls conj (fn [] i))
          (recur (inc i))))
      (is (= [0 1 2] (mapv (fn [f] (f)) @cls))))))

(deftest closure-capture-dotimes
  (testing "dotimes index captured per iteration"
    (let [fs (atom [])]
      (dotimes [i 5] (swap! fs conj (fn [] i)))
      (is (= [0 1 2 3 4] (mapv (fn [f] (f)) @fs))))))

(deftest closure-capture-for-comprehension
  (testing "for yields closures that captured each binding"
    (let [fs (for [i (range 5)] (fn [] i))]
      (is (= [0 1 2 3 4] (mapv (fn [f] (f)) fs))))))

(deftest closure-capture-macro-introduced
  ;; future / delay expand to a `(fn [] ...)` that the source-form
  ;; scanner can't see directly. The fix has to be unconditional on
  ;; recur / self-tail-call, not gated on a source-level "has
  ;; closures?" probe, or these regress silently.
  (testing "future inside loop+recur captures per-iteration i"
    (let [p (promise)]
      (loop [i 0]
        (when (< i 1)
          (future (deliver p (* i i)))
          (recur (inc i))))
      (is (= 0 @p))))
  (testing "10 futures x 10 promises via dotimes deliver to distinct slots"
    (let [ps (vec (repeatedly 10 promise))]
      (dotimes [i 10]
        (future (deliver (nth ps i) (* i i))))
      (is (= [0 1 4 9 16 25 36 49 64 81] (mapv deref ps)))))
  (testing "delay inside self-tail-call captures per-iteration param"
    (let [cls (atom [])]
      (defn G-delay [i]
        (swap! cls conj (delay i))
        (when (< i 2) (G-delay (inc i))))
      (G-delay 0)
      (is (= [0 1 2] (mapv (fn [d] @d) @cls))))))

(deftest closure-capture-multi-arity-self-call
  (testing "multi-arity self-recursion captures the right arity's param"
    (let [cls (atom [])]
      (defn G-multi
        ([] (G-multi 0))
        ([i]
         (swap! cls conj (fn [] i))
         (when (< i 2) (G-multi (inc i)))))
      (G-multi)
      (is (= [0 1 2] (mapv (fn [f] (f)) @cls))))))
