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

(deftest string-construction-under-nursery-pressure
  ;; mino_string_n allocates the raw data buffer (dup_n) before the
  ;; MINO_STRING val cell (alloc_val). If alloc_val triggers a minor GC
  ;; and the data pointer is kept only in a register (not spilled to
  ;; the C stack), the conservative scanner misses it and frees the
  ;; buffer -- the string cell then holds a dangling pointer.
  ;;
  ;; Without gc_depth++ protection this is reliably caught by ASAN and
  ;; by MINO_GC_STRESS=1 (which collects on every allocation). The loop
  ;; below creates enough strings to overflow the nursery many times,
  ;; verifying that content is preserved across all the GC cycles.
  (let [n 8000
        result (loop [i 0 acc []]
                 (if (= i n)
                   acc
                   (recur (inc i) (conj acc (str "gc-str-" i)))))]
    (is (= n (count result)))
    (dotimes [i n]
      (is (= (str "gc-str-" i) (nth result i))))))

;; Regression: the float/double fill value created by mino_float(S, 0.0)
;; inside mino_host_array_new was not protected across the subsequent
;; alloc_val(S, MINO_HOST_ARRAY) call.  vals[] is malloc-owned so the GC
;; does not trace it; without gc_depth protection the conservative scanner
;; could miss `fill` in a register and collect it mid-alloc, leaving
;; dangling pointers in every slot.
;;
;; MINO_GC_STRESS=1 triggers this reliably before the fix.
(deftest host-array-float-fill-gc-safe
  ;; Allocate a double-array under allocation pressure; under GC stress
  ;; the alloc_val inside mino_host_array_new triggers a collection.
  (let [n 500
        arr (double-array n)]
    (is (= n (alength arr)))
    ;; Every slot must hold the correct 0.0 fill -- not a dangling ptr.
    (dotimes [i n]
      (is (= 0.0 (aget arr i)))))
  ;; Same for float-array.
  (let [n 500
        arr (float-array n)]
    (is (= n (alength arr)))
    (dotimes [i n]
      (is (= (float 0.0) (aget arr i))))))

;; Regression: fn.wraps_prim (a GC-owned MINO_PRIM pointer) was not
;; pushed in the MINO_FN/MINO_MACRO GC walker.  A wrapper closure
;; surviving into OLD generation could have its target primitive freed
;; by a major sweep, causing a use-after-free in the fast-lane dispatch.
;;
;; MINO_GC_STRESS=1 triggers this reliably before the fix.
(deftest wraps-prim-gc-traced
  ;; A single-arg wrapper closure -- compile recognises these and sets
  ;; wraps_prim to the underlying primitive for the fast lane.
  (let [my-inc (fn [x] (inc x))
        my-neg (fn [x] (- x))]
    ;; Warm up both closures so wraps_prim is stamped.
    (is (= 1 (my-inc 0)))
    (is (= -1 (my-neg 1)))
    ;; Allocate aggressively to force GC cycles; under GC stress every
    ;; alloc collects.  The target primitives must survive.
    (dotimes [_ 2000]
      (vec (range 50)))
    (is (= 43 (my-inc 42)))
    (is (= -7 (my-neg 7)))))

;; Regression: gc_oom_throw stored NULL in the catch-frame exception slot,
;; so a (catch e ...) handler received nil instead of a proper OOM error
;; map.  The pre-allocated oom_exception singleton must survive GC and
;; carry :mino/kind :internal and :mino/code "MIN001".
(deftest oom-exception-identity
  ;; Trigger a simulated OOM on the very next allocation and verify the
  ;; catch handler receives a recognisable MIN001 exception map, not nil.
  (let [result
        (try
          (do (set-fail-alloc-at! 1)
              ;; Force an allocation so the countdown fires.
              (vec [])
              :no-throw)
          (catch e e))]
    (is (map? result)
        "OOM catch handler receives a map, not nil")
    (is (= :internal (:mino/kind result))
        "OOM exception carries :mino/kind :internal")
    (is (= "MIN001" (:mino/code result))
        "OOM exception carries :mino/code MIN001")))

;; Regression (ki-21): mino_sorted_set_by / mino_sorted_map_by raise
;; gc_depth around rb_assoc, which invokes the user comparator; a
;; comparator that throws longjmps past the matching decrement. Before
;; the try-frame fix nothing restored ctx->gc_depth on the catch
;; landing, so one caught cross-type compare left gc_depth elevated
;; forever: every later collection (including explicit gc!) became a
;; no-op and young bytes grew monotonically for the rest of the run.
;; The catch landing pads now rewind gc_depth to the frame-entry value.
(deftest gc-depth-restored-after-thrown-comparator
  (gc!)
  (let [baseline (long (:bytes-young (gc-stats)))]
    ;; Trigger both leak shapes: comparator throw inside sorted-set-by
    ;; and inside sorted-map-by construction.
    (is (thrown? (sorted-set-by compare 1 "a" :k)))
    (is (thrown? (sorted-map-by compare 1 "a" :k)))
    ;; Dead young garbage. With gc_depth stuck at 1 the final gc! is a
    ;; no-op and bytes-young stays far above baseline; with the fix the
    ;; minor reclaims it.
    (dotimes [_ 64] (vec (range 20000)))
    (gc!)
    (let [after (long (:bytes-young (gc-stats)))]
      (is (< after (+ baseline (* 2 1024 1024)))
          (str "gc! failed to reclaim young bytes after a caught "
               "comparator throw: baseline " baseline ", after " after
               " -- gc_depth left elevated?")))))

;; Range-index instrumentation. :range-walk-entries counts index
;; entries the per-collection merge and the post-minor compaction
;; have examined, cumulatively; :ranges-len is the entry count
;; currently in the index across all buffers. A nil or stale read
;; here means the gc-stats wiring is broken.
(deftest gc-stats-range-walk-entries-ticks
  (gc!)
  (let [before (long (:range-walk-entries (gc-stats)))]
    (dotimes [_ 64] (vec (range 20000)))
    (gc!)
    (let [after (long (:range-walk-entries (gc-stats)))]
      (is (pos? after) "range-walk-entries is exposed and nonzero")
      (is (> after before)
          (str "churn plus gc! must advance the walk counter: before "
               before ", after " after)))))

(deftest gc-stats-ranges-len-exposed-and-stable
  (gc!)
  (dotimes [_ 8] (vec (range 1000)))
  ;; Consecutive full collections keep sweeping deferred dead entries;
  ;; repeat to the fixed point, discard one read cycle of settle
  ;; drift, then compare.
  (dotimes [_ 6] (gc!))
  (long (:ranges-len (gc-stats)))
  (gc!)
  (let [len1 (long (:ranges-len (gc-stats)))]
    (is (pos? len1) "ranges-len is exposed and nonzero after a collection")
    (gc!)
    (let [len2 (long (:ranges-len (gc-stats)))]
      ;; A repeat collection at a fixed live set leaves the index
      ;; unchanged within the reads' own allocation noise; drift past
      ;; it means entries leak or drop incorrectly.
      (is (<= (- len1 128) len2 (+ len1 128))
          (str "ranges-len drifted across a repeat gc!: "
               len1 " then " len2)))))

