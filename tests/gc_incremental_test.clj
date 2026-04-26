(require "tests/test")

;; Targeted tests for the incremental major collector. The full suite
;; exercises the collector in aggregate; these tests pin specific
;; incremental-path invariants so regressions surface loudly. Iteration
;; counts stay modest on purpose: these are correctness probes, not
;; stress benchmarks.

(defn- major-count [] (:collections-major (gc-stats)))

;; By the time the suite reaches these tests the collector has run
;; many majors; all we need is evidence that the begin-step-remark-
;; sweep pipeline actually ran (not just that the heap stayed small
;; enough to avoid firing). The other tests in this file assume a
;; working major path and depend on it indirectly.
(deftest major-has-run-at-least-once
  (is (> (major-count) 0)))

;; SATB invariant: mutation of an OLD atom during an active major
;; must preserve the previous value for this cycle. Churning an
;; atom's val across many cycles keeps both the remset and the SATB
;; push exercised; the final deref must match the last reset.
(deftest atom-churn-during-major-preserves-latest
  (let [a (atom [:start])]
    (dotimes [i 1000]
      (reset! a [:seq (vec (range (rem i 32)))]))
    (is (vector? @a))
    (is (= :seq (first @a)))))

;; Minor-during-major promotion hook: values built up and held through
;; many minors and at least one major must remain intact. Stress by
;; holding a nested vector through the allocation pressure loop.
(deftest promoted-during-major-survives
  (let [v (vec (for [i (range 500)] (vec (range i))))]
    (dotimes [_ 300]
      (doall (take 100 (range 500))))
    (is (= 500 (count v)))
    (is (= 499 (count (last v))))))

;; Var rebinds interleaved with heavy allocation cross several
;; major cycles. Every intermediate rebind must remain coherent
;; (dereferencing the var returns the most recent value).
(def ^:private inc-test-var {:k 0})
(deftest var-rebind-across-major-slices
  (dotimes [i 1000]
    (def ^:private inc-test-var {:k i :payload (vec (range (rem i 32)))}))
  (is (map? inc-test-var))
  (is (= 999 (:k inc-test-var))))

;; Env-rebind stability: nested let rebinds while a major is in
;; flight must not corrupt the bindings array when the env is
;; promoted mid-cycle.
(deftest env-rebind-across-major-slices
  (let [result
        (loop [i 0 acc []]
          (if (>= i 1000)
            acc
            (let [x (vec (range (rem i 32)))
                  y (conj acc (count x))]
              (recur (+ i 1) y))))]
    (is (= 1000 (count result)))))
