(require "tests/test")

;; GC stability: heavy allocation tests.
;; Also run under `make test-gc-stress` which collects on every allocation.

(deftest gc-long-tail
  (is (= 50000 (loop [i 0] (if (< i 50000) (recur (+ i 1)) i)))))

(deftest gc-vec-churn
  (is (= 2000 (count (loop [i 0 acc []]
                       (if (< i 2000) (recur (+ i 1) (conj acc i)) acc))))))

(deftest gc-map-churn
  (is (= 450 (get (loop [i 0 m {}]
                    (if (< i 300) (recur (+ i 1) (assoc m i (* i 3))) m))
                  150))))

(deftest gc-closure-churn
  (def make-inc__gc (fn [n] (fn [x] (+ x n))))
  (is (= 499500
         (loop [i 0 acc 0]
           (if (< i 1000)
             (recur (+ i 1) ((make-inc__gc i) acc))
             acc)))))

(deftest deep-nest-safe
  (def build__gc (fn [n acc]
    (if (= n 0)
      acc
      (build__gc (- n 1) (list acc)))))
  (is (cons? (build__gc 200 42))))

;; Regression for gc! during mid-major-mark: mino_gc_collect(MINO_GC_FULL)
;; used to run the minor BEFORE finishing the in-flight major; the minor
;; would free a YOUNG header still on the major's mark stack, and the
;; subsequent major step would chase the freed pointer. Surfaces on CI
;; runners as a hang in tests/transient_test (test-suite ordering puts
;; transient-survives-gc-yield inside an in-flight major). Locally
;; reproduced via ASan as heap-use-after-free in gc_mark_child_push.
;;
;; The trigger is: heat enough OLD objects to start an incremental
;; major, then call gc! while the major's mark stack is still
;; non-empty. The fixed mino_gc_collect now finish-majors first, then
;; runs the minor, matching the auto-tick path's invariant.
(deftest gc-bang-during-incremental-major
  ;; Warm up: promote many objects so the next minor will start a
  ;; major. Each loop iteration grows a vec; the vec survives long
  ;; enough to be promoted.
  (let [warmup (loop [i 0 acc []]
                 (if (= i 5000)
                   acc
                   (recur (inc i) (conj acc i))))]
    (is (= 5000 (count warmup))))
  ;; Now run the pattern that used to corrupt: transient + gc!
  ;; sequence under an active major mark. If the bug regresses we
  ;; get a SIGSEGV in gc_sweep / gc_mark_child_push, or a stray
  ;; "inc expects a number" caught by the test framework.
  (let [t (transient [])]
    (conj! t 1)
    (gc!)
    (conj! t 2)
    (gc!)
    (conj! t 3)
    (gc!)
    (is (= [1 2 3] (persistent! t)))))

(deftest aset-keeps-young-value-alive-across-minor
  ;; aset writes a slot of a host array in place. If the array has
  ;; already been promoted to OLD when the slot is overwritten with a
  ;; freshly-allocated YOUNG value, the only path that keeps the
  ;; YOUNG value alive across a minor is the remset entry installed
  ;; by the GC write barrier. Without the barrier, the next minor
  ;; reclaims the YOUNG and the slot points at freed memory.
  (let [arr (to-array [0 0 0 0])]
    (dotimes [_ 4] (gc!))   ; age arr to OLD
    (aset arr 0 (assoc {} :marker 12345))
    (dotimes [_ 4] (gc!))   ; minor cycles after the OLD->YOUNG write
    (is (= 12345 (get (nth arr 0) :marker)))))
