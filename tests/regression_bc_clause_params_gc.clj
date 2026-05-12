(require "tests/test")

;; Regression: the BC compiler rewrites a destructured param like
;; [[a b]] into a gensym placeholder plus a wrapping `let`, and stashes
;; the gensym symbol in `clauses[i].params_vec`. The `clauses` buffer
;; is allocated as GC_T_RAW (POD), so the GC tag-walk cannot see the
;; embedded value pointers. Without an explicit push for each clause's
;; params_vec, the gensym vector becomes unreachable as soon as the
;; original (pre-rewrite) params vector on fn.params has been observed
;; by the marker — and the next major cycle collects it. The runtime
;; then sees a NULL slot when binding params and returns NULL silently;
;; that NULL propagates upward until it reaches a prim arity check and
;; surfaces as a misleading "seq requires one argument" error.
;;
;; The original user-visible repro was `extend-type` with six or more
;; protocol groups: the macro expansion runs `mapcat` over a vector
;; whose inner lambda destructures `[[proto methods]]`, and the
;; gensym vector for that lambda got reclaimed mid-iteration. The
;; smaller direct-call test below isolates the same path without the
;; macro context so the failure mode is obvious.

(defprotocol RegrP1 (regr-m1 [_]))
(defprotocol RegrP2 (regr-m2 [_]))
(defprotocol RegrP3 (regr-m3 [_]))
(defprotocol RegrP4 (regr-m4 [_]))
(defprotocol RegrP5 (regr-m5 [_]))
(defprotocol RegrP6 (regr-m6 [_]))
(defrecord RegrR [])

(deftest extend-type-six-protocol-groups
  (is (do
        (extend-type RegrR
          RegrP1 (regr-m1 [_] 1)
          RegrP2 (regr-m2 [_] 2)
          RegrP3 (regr-m3 [_] 3)
          RegrP4 (regr-m4 [_] 4)
          RegrP5 (regr-m5 [_] 5)
          RegrP6 (regr-m6 [_] 6))
        true))
  (let [r (->RegrR)]
    (is (= 1 (regr-m1 r)))
    (is (= 6 (regr-m6 r)))))

(deftest destructured-fn-survives-allocation-churn
  (let [f (fn [[a b]] [b a])]
    ;; Force the bc compile and exercise the destructure path; then
    ;; allocate aggressively to push a major cycle (under GC stress,
    ;; every allocation collects). The fn's clause params_vec must
    ;; survive across the churn.
    (f [:warm :up])
    (dotimes [_ 2000]
      (vec (range 50)))
    (is (= [:b :a] (f [:a :b])))
    (is (= [2 1] (f [1 2])))))

(run-tests-and-exit)
