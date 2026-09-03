(require "tests/test")

;; Core macros the bytecode compiler has no special handler for
;; (cond, case, reify, ...) must macroexpand at compile time --
;; including the clojure.core/-qualified spelling their own
;; expansions nest -- so the enclosing fn stays on the bytecode fast
;; path. Declining sent the whole fn to the tree-walker, which
;; re-expands every macro on every call: the if-let + cond repro
;; below measured ~435us/call against ~1.2us after the fix
;; (dotimes 5000, arm64 darwin, 2026-09-03).

(defn- bmf-decline-total []
  (reduce + 0 (vals (:bc-declines (gc-stats)))))

;; The first call of a fn triggers its bytecode compile; a decline
;; ticks the histogram. Snapshot around that first call. The helper
;; warms its own compile with a throwaway read first so it never
;; lands inside the measured window.

(defn- bmf-compiles? [f arg]
  (bmf-decline-total)
  (let [before (bmf-decline-total)]
    (f arg)
    (= before (bmf-decline-total))))

(deftest if-let-with-cond-stays-compiled
  (defn bmf-k [x] (if-let [s x] [:a s] (cond (nil? x) :b :else :c)))
  (is (bmf-compiles? bmf-k 42))
  (is (= [:a 42] (bmf-k 42)))
  (is (= :b (bmf-k nil)))
  (is (= :c (bmf-k false))))

(deftest bare-cond-body-stays-compiled
  (defn bmf-cond [x] (cond (string? x) :s (int? x) :i :else :other))
  (is (bmf-compiles? bmf-cond 1))
  (is (= :s (bmf-cond "x")))
  (is (= :i (bmf-cond 1)))
  (is (= :other (bmf-cond :k))))

(deftest case-body-stays-compiled
  (defn bmf-case [x] (case x 1 :one 2 :two :other))
  (is (bmf-compiles? bmf-case 1))
  (is (= :one (bmf-case 1)))
  (is (= :two (bmf-case 2)))
  (is (= :other (bmf-case 9))))

(defprotocol BmfProto (bmf-pm [this]))

(deftest reify-body-stays-compiled
  (defn bmf-reify [x] (reify BmfProto (bmf-pm [_] x)))
  (is (bmf-compiles? bmf-reify 7))
  (is (= 7 (bmf-pm (bmf-reify 7)))))

(deftest if-let-with-cond-perf-regression
  ;; The exact BUGS.md repro shape: only the if-let's taken branch
  ;; executes, so a regression here means the fn itself fell back to
  ;; per-call tree-walk expansion, not that cond got slower.
  (defn bmf-perf [x] (if-let [s x] [:a s] (cond (string? x) :b :else :c)))
  (dotimes [_ 200] (bmf-perf 42))
  (let [t0 (System/nanoTime)]
    (dotimes [_ 5000] (bmf-perf 42))
    (let [per-call (/ (- (System/nanoTime) t0) 5000.0)]
      ;; ~1.2us/call on the land host; the bug measured ~435us. The
      ;; budget is absolute wall clock (never a ratio), ~40x headroom
      ;; for slow hosts and still ~9x under the bug's floor.
      (is (< per-call 50000.0)))))

(run-tests-and-exit)
