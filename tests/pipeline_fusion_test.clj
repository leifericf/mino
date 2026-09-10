(require "tests/test")

;; Terminal (non-reduce) seq consumers over a lazy map/filter/take
;; pipeline must produce identical results whether the runtime fuses
;; the pipeline through the shared walker or falls back to the slow
;; per-element lazy-cell path. These tests pin the observable
;; semantics fusion must preserve: value equality, encounter order,
;; short-circuit, empty/nil handling, and correctness across the
;; 32-element chunk boundary of a lazy range.
;;
;; A lazy pipeline head is `(->> (range N) (map f) (filter p) ...)`;
;; the fused fast path recognises the map/filter/take lazy cells by
;; their exact thunk identity. A helper builds fresh pipelines so the
;; tests exercise unrealised heads (a realised head caches its cells).

(defn- pipe [n]
  (->> (range n) (map inc) (filter odd?)))

;; --- count -----------------------------------------------------------

(deftest count-pipeline-equals-realised
  ;; count over a lazy pipeline must equal counting the fully realised
  ;; sequence. Crosses several 32-element chunk boundaries at n=1000.
  (is (= (count (doall (pipe 1000)))
         (count (pipe 1000))))
  (is (= 500 (count (pipe 1000)))))

(deftest count-pipeline-edges
  (is (= 0 (count (pipe 0))))
  (is (= 0 (count (->> (range 10) (filter (fn [_] false))))))
  ;; single surviving element
  (is (= 1 (count (->> (range 1) (map inc) (filter odd?))))))

(deftest count-pipeline-with-take
  ;; take must terminate the walk; count of a take is bounded by it.
  (is (= 7 (count (->> (range 1000) (map inc) (filter odd?) (take 7)))))
  ;; take crossing a chunk boundary (>32)
  (is (= 40 (count (->> (range 1000) (map inc) (filter odd?) (take 40)))))
  ;; take larger than the surviving count clamps to what survives
  (is (= 500 (count (->> (range 1000) (map inc) (filter odd?)
                         (take 100000))))))

;; --- some ------------------------------------------------------------

(deftest some-pipeline-first-truthy
  ;; some returns the first truthy (pred x), short-circuiting.
  (is (= true (some #(> % 5) (pipe 1000))))
  (is (= :hit (some #(when (= % 101) :hit) (pipe 1000)))))

(deftest some-pipeline-never
  (is (= nil (some #(> % 100000) (pipe 1000))))
  (is (= nil (some odd? (->> (range 10) (map inc) (filter even?)))))
  (is (= nil (some (fn [_] true) (pipe 0)))))

(deftest some-pipeline-short-circuits
  ;; Once a truthy is found the pred must not run on later elements.
  ;; A pred that throws on the sentinel proves we stopped before it.
  (let [seen (atom [])]
    (is (= true
           (some (fn [x]
                   (swap! seen conj x)
                   (>= x 4))
                 (pipe 1000))))
    ;; survivors of (map inc)(filter odd?) are 1 3 5 ...; first >=4 is 5.
    (is (= [1 3 5] @seen))))

;; --- every? ----------------------------------------------------------

(deftest every-pipeline-all-true
  (is (= true (every? odd? (pipe 1000))))
  (is (= true (every? #(< % 100000) (pipe 1000))))
  ;; vacuously true on empty
  (is (= true (every? (fn [_] false) (pipe 0)))))

(deftest every-pipeline-early-false
  (is (= false (every? #(< % 5) (pipe 1000))))
  (is (= false (every? even? (pipe 1000)))))

(deftest every-pipeline-short-circuits
  ;; every? must stop at the first falsy element.
  (let [seen (atom [])]
    (is (= false
           (every? (fn [x]
                     (swap! seen conj x)
                     (< x 4))
                   (pipe 1000))))
    ;; survivors 1 3 5 ...; first not <4 is 5.
    (is (= [1 3 5] @seen))))

;; --- frequencies -----------------------------------------------------

(deftest frequencies-pipeline-equals-realised
  (is (= (frequencies (doall (pipe 1000)))
         (frequencies (pipe 1000))))
  ;; every survivor is distinct and odd, so each count is 1
  (is (= 500 (count (frequencies (pipe 1000)))))
  (is (= #{1} (set (vals (frequencies (pipe 1000)))))))

(deftest frequencies-pipeline-repeats
  ;; a stage that collapses values must count multiplicities correctly
  (is (= {0 500, 1 500}
         (frequencies (->> (range 1000) (map #(mod % 2))))))
  (is (= {} (frequencies (pipe 0)))))

;; --- group-by --------------------------------------------------------

(deftest group-by-pipeline-equals-realised
  (is (= (group-by #(mod % 3) (doall (pipe 100)))
         (group-by #(mod % 3) (pipe 100)))))

(deftest group-by-pipeline-encounter-order
  ;; buckets preserve encounter order.
  (is (= {true [1 3 5 7 9]}
         (group-by odd? (->> (range 10) (map inc) (filter odd?)))))
  (is (= {0 [0 3 6 9] 1 [1 4 7] 2 [2 5 8]}
         (group-by #(mod % 3) (->> (range 10)))))
  (is (= {} (group-by odd? (pipe 0)))))

;; --- concrete-source drains for mapv / filterv / into ----------------
;; mapv / filterv / into over a CONCRETE vector or range (not a lazy
;; pipeline head) route through the shared walker's inline
;; vector-drain / int-range fast path instead of seq-iter +
;; per-element apply_callable. The fused result must be
;; indistinguishable from the slow path: same values, same order,
;; same identity for pass-through elements, same empty / single-element
;; behaviour, and the callable must run once per element in order.

(deftest mapv-concrete-vector-equals-slow
  ;; a plain vector source; crosses several 32-element chunk boundaries.
  (let [v (vec (range 1000))]
    (is (= (mapv inc v) (vec (map inc v))))
    (is (= 1000 (count (mapv inc v))))
    (is (= 1 (first (mapv inc v))))
    (is (= 1000 (last (mapv inc v)))))
  ;; empty and single element
  (is (= [] (mapv inc [])))
  (is (= [42] (mapv inc [41])))
  ;; identity function returns the same element objects
  (let [v [{:a 1} {:b 2}]]
    (is (= v (mapv identity v)))
    (is (identical? (first v) (first (mapv identity v))))))

(deftest mapv-concrete-range-equals-slow
  (is (= (mapv inc (range 1000)) (vec (map inc (range 1000)))))
  (is (= [] (mapv inc (range 0))))
  (is (= [1] (mapv inc (range 1))))
  ;; stepped and descending ranges
  (is (= (vec (map inc (range 0 100 3)))
         (mapv inc (range 0 100 3))))
  (is (= (vec (map inc (range 10 0 -1)))
         (mapv inc (range 10 0 -1)))))

(deftest mapv-concrete-visits-once-in-order
  ;; the fast path must apply the fn exactly once per element, in order.
  (let [seen (atom [])
        out  (mapv (fn [x] (swap! seen conj x) (* x 10)) (vec (range 5)))]
    (is (= [0 10 20 30 40] out))
    (is (= [0 1 2 3 4] @seen))))

(deftest filterv-concrete-vector-equals-slow
  (let [v (vec (range 1000))]
    (is (= (filterv even? v) (vec (filter even? v))))
    (is (= 500 (count (filterv even? v)))))
  (is (= [] (filterv even? [])))
  (is (= [] (filterv even? [1 3 5])))
  (is (= [2] (filterv even? [1 2 3])))
  ;; survivors are the identical source objects, not copies
  (let [a {:k 1} b {:k 2}
        v [a b]]
    (is (identical? a (first (filterv (fn [_] true) v))))))

(deftest filterv-concrete-range-equals-slow
  (is (= (filterv even? (range 1000)) (vec (filter even? (range 1000)))))
  (is (= [] (filterv even? (range 0))))
  (is (= [] (filterv (fn [_] false) (range 100))))
  (is (= (vec (range 100)) (filterv (fn [_] true) (range 100)))))

(deftest filterv-concrete-visits-once-in-order
  (let [seen (atom [])
        out  (filterv (fn [x] (swap! seen conj x) (odd? x)) (vec (range 5)))]
    (is (= [1 3] out))
    (is (= [0 1 2 3 4] @seen))))

(deftest into-concrete-vector-equals-slow
  (let [v (vec (range 1000))]
    (is (= (into [] v) v))
    (is (= (into [:x] v) (vec (concat [:x] v))))
    (is (= 1000 (count (into [] v)))))
  (is (= [] (into [] [])))
  (is (= [:a] (into [] [:a])))
  ;; source objects pass through by identity
  (let [a {:k 1}]
    (is (identical? a (first (into [] [a]))))))

(deftest into-concrete-range-equals-slow
  (is (= (into [] (range 1000)) (vec (range 1000))))
  (is (= [] (into [] (range 0))))
  (is (= [:x 0 1 2] (into [:x] (range 3))))
  (is (= (vec (range 10 0 -1)) (into [] (range 10 0 -1)))))

;; --- widened fusible stages: remove / keep / map-indexed -------------
;; The 2-arg lazy forms of remove / keep / map-indexed produce their own
;; C lazy thunks, recognised by the pipeline unwinder as inverted-filter
;; (remove), map+nil-drop (keep), and map-with-index (map-indexed)
;; stages. The fused walk must be indistinguishable from the slow
;; per-element path: same values, same order, same short-circuit under a
;; downstream take, correct behaviour across the 32-element chunk
;; boundary, when a stage empties a whole chunk, and for the stateful
;; map-indexed counter crossing chunk boundaries. `doall` forces the
;; reference (slow, fully-realised) sequence; the bare pipeline exercises
;; the fused head. A fresh pipeline is built each time so heads are
;; unrealised.

;; --- remove ----------------------------------------------------------

(deftest remove-pipeline-equals-realised
  ;; remove over a lazy source, crossing chunk boundaries at n=1000.
  (is (= (doall (remove even? (range 1000)))
         (remove even? (range 1000))))
  (is (= (reduce + 0 (doall (remove even? (range 1000))))
         (reduce + 0 (remove even? (range 1000)))))
  ;; remove is the complement of filter
  (is (= (filter odd? (range 1000))
         (remove even? (range 1000)))))

(deftest remove-pipeline-edges
  (is (= '() (remove even? (range 0))))
  ;; remove nothing: every element survives
  (is (= (range 100) (remove (fn [_] false) (range 100))))
  ;; remove everything: whole seq empties (and each chunk empties)
  (is (= '() (remove (fn [_] true) (range 1000))))
  ;; single survivor
  (is (= '(0) (remove pos? (range 5)))))

(deftest remove-pipeline-mixed-and-take
  ;; remove composes with map / filter / take in one fused chain.
  (is (= (doall (->> (range 1000) (map inc) (remove even?)))
         (->> (range 1000) (map inc) (remove even?))))
  ;; take terminates the walk after remove; crosses a chunk boundary
  (is (= 40 (count (->> (range 1000) (remove even?) (take 40)))))
  (is (= (->> (range 1000) (remove even?) (take 40))
         (doall (->> (range 1000) (remove even?) (take 40))))))

(deftest remove-pipeline-short-circuits
  ;; under a downstream take, the fused remove must invoke its predicate
  ;; on exactly the elements the slow chunked path does. A chunked range
  ;; realises a whole 32-element chunk at a time, so both paths run the
  ;; pred on the full first chunk (0..31) even though take wants only 3.
  ;; The invariant pinned here is fused-equals-unfused side effects.
  (let [seen-fused (atom []) seen-slow (atom [])]
    (is (= [1 3 5]
           (->> (range 1000)
                (remove (fn [x] (swap! seen-fused conj x) (even? x)))
                (take 3))))
    (doall (->> (range 1000)
                (remove (fn [x] (swap! seen-slow conj x) (even? x)))
                (take 3)))
    (is (= @seen-slow @seen-fused))))

;; --- keep ------------------------------------------------------------

(deftest keep-pipeline-equals-realised
  (let [f (fn [x] (when (odd? x) (* x 10)))]
    (is (= (doall (keep f (range 1000)))
           (keep f (range 1000))))
    (is (= (reduce + 0 (doall (keep f (range 1000))))
           (reduce + 0 (keep f (range 1000)))))))

(deftest keep-pipeline-edges
  (is (= '() (keep identity (range 0))))
  ;; keep everything (never nil)
  (is (= (range 100) (keep identity (range 100))))
  ;; keep nothing (always nil): whole seq and every chunk empties
  (is (= '() (keep (fn [_] nil) (range 1000))))
  ;; keep must NOT drop false, only nil (false is a kept value)
  (is (= '(false false false)
         (keep (fn [x] (when (< x 3) false)) (range 3))))
  (is (= 3 (count (keep (fn [x] (when (< x 3) false)) (range 100))))))

(deftest keep-pipeline-mixed-and-take
  (let [f (fn [x] (when (odd? x) x))]
    (is (= (doall (->> (range 1000) (map inc) (keep f)))
           (->> (range 1000) (map inc) (keep f))))
    (is (= 20 (count (->> (range 1000) (keep f) (take 20)))))
    (is (= (->> (range 1000) (keep f) (take 20))
           (doall (->> (range 1000) (keep f) (take 20)))))))

(deftest keep-pipeline-short-circuits
  ;; keep's fn must run on exactly the elements the slow chunked path
  ;; touches; a chunked range realises the whole first chunk.
  (let [seen-fused (atom []) seen-slow (atom [])]
    (is (= [1 3 5]
           (->> (range 1000)
                (keep (fn [x] (swap! seen-fused conj x) (when (odd? x) x)))
                (take 3))))
    (doall (->> (range 1000)
                (keep (fn [x] (swap! seen-slow conj x) (when (odd? x) x)))
                (take 3)))
    (is (= @seen-slow @seen-fused))))

;; --- map-indexed -----------------------------------------------------

(deftest map-indexed-pipeline-equals-realised
  ;; the stateful index must be correct across chunk boundaries (n>32).
  (let [f (fn [i x] [i x])]
    (is (= (doall (map-indexed f (range 1000)))
           (map-indexed f (range 1000))))
    ;; index tracks position: element k maps to [k (+ 100 k)]
    (is (= [[0 100] [1 101] [2 102]]
           (take 3 (map-indexed f (map #(+ 100 %) (range 1000))))))
    ;; sum of (+ i x) is a cheap scalar witness the counter is right
    (is (= (reduce + 0 (doall (map-indexed + (range 1000))))
           (reduce + 0 (map-indexed + (range 1000)))))))

(deftest map-indexed-counter-crosses-chunk-boundary
  ;; the 33rd element (index 32) sits just past the first 32-elem chunk;
  ;; its index must be 32, not reset to 0.
  (is (= [32 1032]
         (nth (map-indexed (fn [i x] [i x]) (map #(+ 1000 %) (range 1000)))
              32)))
  ;; last element of a 1000-seq has index 999
  (is (= [999 999]
         (last (map-indexed (fn [i x] [i x]) (range 1000))))))

(deftest map-indexed-pipeline-edges
  (is (= '() (map-indexed (fn [i x] [i x]) (range 0))))
  (is (= [[0 :a]] (map-indexed (fn [i x] [i x]) [:a])))
  ;; map-indexed composed under filter and take (fused chain)
  (is (= (doall (->> (range 1000)
                     (map-indexed (fn [i x] (+ i x)))
                     (filter even?)
                     (take 10)))
         (->> (range 1000)
              (map-indexed (fn [i x] (+ i x)))
              (filter even?)
              (take 10)))))

;; --- transducer fusion ----------------------------------------------
;; A comp of standard stage transducers routed through transduce /
;; into-with-xform / sequence-with-xform must fuse through the same
;; walker yet stay observably identical to the slow closure path. The
;; reference in every case is the equivalent lazy-seq pipeline reduced
;; the ordinary way, so these pass whether or not the fast path fires:
;; they pin the semantics fusion must never change. An xform the walker
;; cannot model (stateful take, take-while, halt-when, or any user
;; closure) MUST take the slow path and still be correct.

(deftest transduce-map-filter-equals-lazy
  ;; The headline case: (comp (map inc) (filter odd?)) over 1000 elems.
  ;; transduce applies xform stages left-to-right per element, so inc
  ;; runs before odd?. The lazy reference nests the other way:
  ;; (filter odd? (map inc coll)) also runs inc then tests odd?.
  (is (= (reduce + 0 (filter odd? (map inc (range 1000))))
         (transduce (comp (map inc) (filter odd?)) + 0 (range 1000))))
  ;; three stages
  (is (= (reduce + 0 (filter even? (map #(* 2 %) (map inc (range 1000)))))
         (transduce (comp (map inc) (map #(* 2 %)) (filter even?))
                    + 0 (range 1000)))))

(deftest transduce-order-is-left-to-right
  ;; comp order matters when stages are not commutative: (map inc) then
  ;; (filter odd?) keeps the odd results of x+1; swapping filters first.
  (is (= [2 4 6]
         (transduce (comp (map inc) (filter even?)) conj [] (range 6))))
  ;; (filter even?) then (map inc): keep 0 2 4, then inc -> 1 3 5
  (is (= [1 3 5]
         (transduce (comp (filter even?) (map inc)) conj [] (range 6)))))

(deftest transduce-single-stage
  ;; A lone standard transducer (no comp) must fuse or slow-path equal.
  (is (= (reduce + 0 (map inc (range 1000)))
         (transduce (map inc) + 0 (range 1000))))
  (is (= (reduce + 0 (filter odd? (range 1000)))
         (transduce (filter odd?) + 0 (range 1000)))))

(deftest transduce-remove-keep-map-indexed
  ;; remove / keep / map-indexed transducers fuse as their stages.
  (is (= (reduce + 0 (remove even? (range 1000)))
         (transduce (remove even?) + 0 (range 1000))))
  ;; keep drops nil, keeps false; (keep #(when (odd? %) %)) keeps odds.
  (is (= (reduce + 0 (keep #(when (odd? %) %) (range 1000)))
         (transduce (keep #(when (odd? %) %)) + 0 (range 1000))))
  ;; map-indexed index must be seeded at 0 and cross chunk boundaries.
  (is (= (reduce + 0 (map-indexed + (range 1000)))
         (transduce (map-indexed +) + 0 (range 1000))))
  ;; a comp mixing the new stages
  (is (= (reduce + 0 (keep #(when (pos? %) %)
                           (remove #(zero? (mod % 3))
                                   (map-indexed + (range 1000)))))
         (transduce (comp (map-indexed +)
                          (remove #(zero? (mod % 3)))
                          (keep #(when (pos? %) %)))
                    + 0 (range 1000)))))

(deftest transduce-edges
  ;; empty source
  (is (= 0 (transduce (comp (map inc) (filter odd?)) + 0 (range 0))))
  (is (= 0 (transduce (comp (map inc) (filter odd?)) + 0 nil)))
  ;; single element
  (is (= 2 (transduce (map inc) + 0 [1])))
  ;; filter empties a chunk: no element in [0,32) survives, but later
  ;; chunks do; the walk must not stop when a whole chunk is rejected.
  (is (= (reduce + 0 (filter #(>= % 100) (range 1000)))
         (transduce (filter #(>= % 100)) + 0 (range 1000))))
  ;; a stage over a concrete vector source (not a range)
  (is (= (reduce + 0 (filter odd? (map inc (vec (range 1000)))))
         (transduce (comp (map inc) (filter odd?)) + 0 (vec (range 1000))))))

(deftest transduce-completion-arity-runs
  ;; transduce's contract is (xrf (unreduced result)): the completion
  ;; arity of the composed xform must fire exactly once, after the
  ;; reduce, on both the fused and the slow path. mino wraps the
  ;; reducing fn in (completing f), so the observable completion is the
  ;; sum threaded through each stage's ([result] (rf result)) pass. A
  ;; scalar fused result must equal the lazy reference and the same
  ;; result must survive the completion pass unchanged (identity cf).
  (is (= (reduce + 0 (filter odd? (map inc (range 100))))
         (transduce (comp (map inc) (filter odd?)) + 0 (range 100))))
  ;; the fused path and the (forced) slow path agree element-for-element
  ;; into a vector, which threads conj's completion identically.
  (is (= (vec (filter odd? (map inc (range 100))))
         (transduce (comp (map inc) (filter odd?)) conj [] (range 100)))))

(deftest transduce-two-arg-uses-rf-init
  ;; (transduce xform f coll) with no init seeds from (f); for + that
  ;; is 0. Must equal the 4-arg form with the identity init.
  (is (= (transduce (comp (map inc) (filter odd?)) + 0 (range 100))
         (transduce (comp (map inc) (filter odd?)) + (range 100)))))

(deftest transduce-reduced-short-circuit-take
  ;; take is stateful and produces `reduced`; it MUST stay slow-path and
  ;; still short-circuit exactly. Result equals taking from the lazy seq.
  (is (= (reduce + 0 (take 7 (filter odd? (map inc (range 1000)))))
         (transduce (comp (map inc) (filter odd?) (take 7))
                    + 0 (range 1000))))
  ;; take crossing a chunk boundary (n>32)
  (is (= (reduce + 0 (take 40 (map inc (range 1000))))
         (transduce (comp (map inc) (take 40)) + 0 (range 1000))))
  ;; take-while short-circuit
  (is (= (reduce + 0 (take-while #(< % 50) (map inc (range 1000))))
         (transduce (comp (map inc) (take-while #(< % 50)))
                    + 0 (range 1000)))))

(deftest transduce-take-halts-before-later-elements
  ;; With take fronted, elements past the count must never be seen.
  (let [seen (atom [])]
    (transduce (comp (map (fn [x] (swap! seen conj x) x)) (take 3))
               + 0 (range 1000))
    (is (= [0 1 2] @seen))))

(deftest transduce-opaque-user-xform-slow-path
  ;; A user-written transducer the walker cannot recognise must run
  ;; correctly on the slow closure path. This doubles every element.
  (let [dup (fn [rf]
              (fn ([] (rf))
                  ([r] (rf r))
                  ([r x] (rf (rf r x) x))))]
    (is (= (* 2 (reduce + 0 (range 100)))
           (transduce dup + 0 (range 100))))
    ;; opaque xform composed with a standard stage: the whole comp must
    ;; fall back to the slow path and stay correct.
    (is (= (reduce + 0 (mapcat (fn [x] [x x]) (map inc (range 100))))
           (transduce (comp (map inc) dup) + 0 (range 100))))))

(deftest into-with-xform-fuses-equal
  ;; into with an xform routes through transduce; must equal the lazy
  ;; build and preserve conj order into a vector.
  (is (= (vec (filter odd? (map inc (range 1000))))
         (into [] (comp (map inc) (filter odd?)) (range 1000))))
  ;; into a non-empty target keeps existing elements first
  (is (= (into [:a] (comp (map inc) (filter odd?)) (range 6))
         (vec (concat [:a] (filter odd? (map inc (range 6)))))))
  ;; into with take (slow path) still bounded
  (is (= (into [] (comp (map inc) (take 5)) (range 1000))
         (vec (take 5 (map inc (range 1000)))))))

(deftest sequence-with-xform-fuses-equal
  ;; sequence with an xform yields a lazy seq of the transformed items.
  (is (= (filter odd? (map inc (range 1000)))
         (sequence (comp (map inc) (filter odd?)) (range 1000))))
  ;; realised equality across chunk boundaries
  (is (= (doall (sequence (comp (map inc) (filter odd?)) (range 1000)))
         (filter odd? (map inc (range 1000)))))
  ;; sequence stays lazy: taking a prefix does not force the whole source
  (is (= [2 4 6]
         (take 3 (sequence (comp (map inc) (filter odd?)
                                 (map inc))
                           (range 1000))))))

(deftest transduce-long-comp-exceeds-stage-limit
  ;; A comp of more standard stages than the walker's fixed stage array
  ;; (8) must fall back to the slow closure path and stay correct, not
  ;; leak the fusion no-fuse sentinel as a result.
  (let [xf (apply comp (repeat 9 (map inc)))
        expected (reduce (fn [c _] (map inc c)) (range 5) (range 9))]
    (is (= (reduce + 0 expected)
           (transduce xf + 0 (range 5))))
    ;; ten stages, mixed kinds, still correct
    (is (= (reduce + 0 (filter odd? (map inc (map inc (map inc (map inc
             (map inc (map inc (map inc (map inc (map inc (range 100))))))))))))
           (transduce (apply comp (concat (repeat 9 (map inc)) [(filter odd?)]))
                      + 0 (range 100))))))

;; Property: for random comps of standard stateless stages over random
;; sources, transduce-into-a-scalar must equal reducing the equivalent
;; lazy-seq composition. Covers empty, single, chunk boundaries at 32,
;; and filters that empty a chunk.
(deftest transduce-random-comps-equal-lazy
  (let [stages [{:x (map inc)        :seq #(map inc %)}
                {:x (filter odd?)    :seq #(filter odd? %)}
                {:x (map #(* 2 %))   :seq #(map (fn [e] (* 2 e)) %)}
                {:x (remove #(zero? (mod % 3)))
                 :seq #(remove (fn [e] (zero? (mod e 3))) %)}
                {:x (keep #(when (pos? %) %))
                 :seq #(keep (fn [e] (when (pos? e) e)) %)}
                {:x (map-indexed +) :seq #(map-indexed + %)}]
        sources [(range 0) (range 1) (range 5) (range 32) (range 33)
                 (range 64) (range 200) (vec (range 33)) (vec (range 200))]
        rng (atom 12345)
        nextr (fn [] (swap! rng (fn [s] (mod (+ (* s 1103515245) 12345)
                                             2147483648))))]
    (dotimes [_ 60]
      (let [k    (inc (mod (nextr) 3))
            picks (repeatedly k #(nth stages (mod (nextr) (count stages))))
            xform (apply comp (map :x picks))
            src   (nth sources (mod (nextr) (count sources)))
            lazy-fn (reduce (fn [acc st] (fn [s] ((:seq st) (acc s))))
                            identity picks)
            expected (reduce + 0 (lazy-fn src))
            actual   (transduce xform + 0 src)]
        (is (= expected actual)
            (str "mismatch k=" k " src-count=" (count src)))))))
