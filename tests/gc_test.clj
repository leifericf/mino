(require "tests/test")
(require '[clojure.string :as str])
(require '[clojure.data.json :as json])

;; GC stability: heavy allocation tests.
;; Also run under `make test-gc-stress` which collects on every allocation.

(defn- corpus-desc-cell
  "One 512-char description cell with a non-ASCII codepoint every 64
  characters, the json-corpus document shape."
  [seed]
  (apply str
         (map (fn [i]
                (if (zero? (rem i 64))
                  (if (odd? (+ seed i)) "\u00e9" "\u4e2d")
                  "x"))
              (range 512))))

(defn- corpus-entry
  "One corpus entry: nested map, array, escapes, unicode, numbers."
  [i]
  (str "{\"name\": \"item " i "\", \"desc\": \"" (corpus-desc-cell i) "\","
       " \"tags\": [\"alpha\", \"beta\\\"q\"],"
       " \"score\": " i ".25,"
       " \"active\": "
       (if (odd? i) "true" "false")
       "}"))

(defn- corpus-doc
  "A json document of n entries whose parse tree holds a large multiple
  of n headers (10k entries parse to roughly 290k)."
  [n]
  (str "{\"entries\": [" (str/join "," (map corpus-entry (range n))) "]}"))

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

;; prim_disj rebuilds the result set by allocating the new MINO_SET
;; first and filling root/key_order only after a rebuild loop that can
;; allocate more than a nursery's worth of headers. A minor firing
;; mid-loop promotes the still-unfilled container to OLD (gc_pin roots
;; it but does not defer aging); the one-cycle promote-then-add remset
;; entry is dropped at the next walk while the slots are still empty,
;; and the late fills then install YOUNG trie nodes into an OLD
;; container with no write barrier. The next minor frees those nodes
;; as unreachable and membership reads walk recycled memory (observed
;; as the dev-host 64 KiB-nursery SIGSEGV in eq_step via
;; set/difference). The rebuild now raises gc_depth from allocation to
;; first fill, the mino_set/mino_map construction precedent, so no
;; minor can promote the container in between. Sizes are chosen so
;; each rebuild crosses several minors at the default nursery.
(deftest disj-rebuild-keeps-membership-across-minors
  (let [n 12000
        keep (set (map #(str "keep-" %) (range n)))
        drop (set (map #(str "keep-" %) (range 96)))]
    (gc!)
    (let [out (reduce disj keep drop)]
      (gc!)
      ;; Recycle the freed young nodes before reading: an uncovered
      ;; OLD->YOUNG edge only shows once the freed child's memory is
      ;; reused, which is why the dev-host crash needed the suite's
      ;; allocation storm around the reads.
      (dotimes [_ 64] (vec (range 20000)))
      (is (= (- n 96) (count out)))
      (dotimes [i 96]
        (is (not (contains? out (str "keep-" i)))
            (str "kept dropped keep-" i)))
      (dotimes [i (- n 96)]
        (is (contains? out (str "keep-" (+ i 96))) (str "lost keep-" i))))))

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

;; Minor collections must examine a young-bounded slice of the range
;; index, never a slice proportional to the whole heap. Under a big
;; live OLD tree (the corpus parse, hundreds of thousands of entries)
;; with only a young window between collections, the per-collection
;; :range-walk-entries delta tracks the young census. A regression to
;; walking every entry, young and old alike, shows up as the delta
;; tracking :ranges-len instead (measured at 2.36x ranges-len per
;; minor on the unified index).
(deftest gc-range-walk-stays-young-bounded-under-big-old-tree
  (let [doc  (corpus-doc 10000)
        tree (json/read-str doc)]
    (is (= 10000 (count (get tree "entries"))))
    ;; Age the parse tree to OLD and settle the index before sampling;
    ;; the tree stays live via this binding for the whole test.
    (gc!)
    (dotimes [_ 3] (gc!))
    (let [len   (long (:ranges-len (gc-stats)))
          walk1 (long (:range-walk-entries (gc-stats)))
          coll1 (long (:collections-minor (gc-stats)))]
      ;; Eight windows of big young allocations (64 KiB string slices
      ;; of the document, a few hundred headers per window), each
      ;; closed by an explicit gc! so every window's cost lands in one
      ;; measured cycle.
      (dotimes [_ 8]
        (dotimes [_ 100] (subs doc 0 65536))
        (gc!))
      (let [walk2  (long (:range-walk-entries (gc-stats)))
            coll2  (long (:collections-minor (gc-stats)))
            minors (- coll2 coll1)
            walked (- walk2 walk1)]
        (is (>= minors 2)
            (str "windows must collect: only " minors " minors"))
        (let [per-minor (quot walked (max minors 1))]
          (is (< per-minor (quot len 2))
              (str "per-minor walk tracks ranges-len (" per-minor
                   " vs len " len "), not the young census"))
          ;; Census budget: each window holds a few hundred young
          ;; headers (100 string slices plus survivors), so a
          ;; young-bounded walk stays far under 8192 entries per
          ;; collection; an old-proportional walk clears it by an
          ;; order of magnitude.
          (is (< per-minor 8192)
              (str "per-minor walk " per-minor
                   " exceeds the young-window budget 8192")))))))

;; The per-generation lens of the range index: young entries (the
;; nursery-sized slice minors maintain), old entries (the folded
;; majority), and the old-pending buffer that collects entries of
;; headers promoted since the last fold. Under a big live OLD tree the
;; young slice stays far below the old slice, and the phase-one
;; :ranges-len key keeps its meaning: the sum across all buffers.
(deftest gc-stats-exposes-per-generation-range-lens
  (let [doc (corpus-doc 2500)
        tree (json/read-str doc)]
    (is (= 2500 (count (get tree "entries"))))
    (gc!)
    (let [st   (gc-stats)
          len  (long (:ranges-len st))
          young (long (:ranges-young-len st))
          old   (long (:ranges-old-len st))
          pend  (long (:ranges-old-pending-len st))]
      (is (pos? young) "ranges-young-len is exposed and nonzero")
      (is (> old (* 4 young))
          (str "old slice must dominate under a big old tree: old "
               old ", young " young))
      (is (>= pend 0) "ranges-old-pending-len is exposed")
      (is (= len (+ young old pend))
          (str "ranges-len must sum the generation slices: len " len
               " vs young+old+pending " (+ young old pend))))))

;; Promotion must not drop index coverage: a header promoted at a
;; minor sweep keeps resolvable through every buffer transition, so a
;; collection aged to OLD and then churned past further minors (each
;; one re-routing promoted entries) still answers exact membership and
;; equality reads. Freed-then-recycled nodes are what make a coverage
;; gap observable, so the churn precedes every read sweep.
(deftest aged-set-and-vector-keep-exact-membership
  (let [n 4000
        s (set (map #(str "pk" %) (range n)))
        v (vec (map #(str "pv" %) (range 2048)))]
    (dotimes [_ 64] (vec (range 20000)))
    (let [fresh-s (set (map #(str "pk" %) (range n)))
          fresh-v (vec (map #(str "pv" %) (range 2048)))]
      (is (= s fresh-s) "aged set equals a fresh copy")
      (is (= v fresh-v) "aged vector equals a fresh copy")
      (let [out (disj s "pk7")]
        (is (= (dec n) (count out)))
        (dotimes [_ 64] (vec (range 20000)))
        (dotimes [i n]
          (is (if (= i 7)
                (not (contains? out "pk7"))
                (contains? out (str "pk" i)))
              (str "membership broke at pk" i)))
        (is (= n (count s)) "disj leaves the source untouched")
        (dotimes [i 2048]
          (is (= (str "pv" i) (nth v i)) (str "vector slot broke at " i)))))))

