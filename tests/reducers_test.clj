(require "tests/test")
(require '[clojure.core.reducers :as r])

;; Sequential transducer-layer wrapper. Parallel fork/join is deferred
;; until mino ships the multi-state OS-thread cycle; until then fold
;; reduces left-to-right through reducef.

(deftest reduce-passes-through-clojure-reduce
  (is (= 15 (r/reduce + 0 [1 2 3 4 5])))
  (is (= 15 (r/reduce + [1 2 3 4 5])))
  (is (= [1 2 3] (r/reduce conj [] [1 2 3]))))

(deftest map-produces-mapped-reducer
  (is (= [2 3 4] (r/reduce conj [] (r/map inc [1 2 3]))))
  (is (= 6 (r/reduce + (r/map inc [0 1 2])))))

(deftest filter-and-remove
  (is (= [2 4] (r/reduce conj [] (r/filter even? [1 2 3 4]))))
  (is (= [1 3] (r/reduce conj [] (r/remove even? [1 2 3 4])))))

(deftest mapcat-flattens-one-level
  (is (= [1 1 2 4 3 9]
         (r/reduce conj [] (r/mapcat (fn [x] [x (* x x)]) [1 2 3])))))

(deftest take-take-while
  (is (= [0 1 2] (r/reduce conj [] (r/take 3 (range 100)))))
  (is (= [0 1 2] (r/reduce conj [] (r/take-while #(< % 3) (range 100))))))

(deftest drop-drop-while
  (is (= [3 4 5] (r/reduce conj [] (r/drop 3 [0 1 2 3 4 5]))))
  (is (= [3 4 5] (r/reduce conj [] (r/drop-while #(< % 3) [0 1 2 3 4 5])))))

(deftest flatten-cat
  (is (= [1 2 3 4] (r/reduce conj [] (r/flatten [[1 2] [3 4]]))))
  (is (= [1 2 3 4] (r/reduce conj [] (r/cat [1 2] [3 4])))))

(deftest foldcat-returns-vector
  (is (= [1 2 3] (r/foldcat [1 2 3])))
  (is (= [2 3 4] (r/foldcat (r/map inc [1 2 3])))))

(deftest fold-sequential
  (is (= 45 (r/fold + (range 10))))
  ;; Three- and four-arg variants delegate to the sequential reduce.
  (is (= 45 (r/fold + + (range 10))))
  (is (= 45 (r/fold 100 + + (range 10)))))

(deftest monoid-builds-init-and-op
  (let [m (r/monoid + (constantly 0))]
    (is (= 0 (m)))
    (is (= 3 (m 1 2)))))

;; ---------------------------------------------------------------------------
;; Foldable layer: the CollFold protocol, the reducer / folder wrappers,
;; and the cat / append! / ->Cat combining machinery. Sequential execution
;; is an accepted fold strategy, so everything below asserts results,
;; never scheduling.
;; ---------------------------------------------------------------------------

(defn- reducers-xf-inc
  "Reducing-fn transformer in the shape reducer/folder expect: takes a
  reducing fn and returns a reducing fn that increments each value
  before handing it on."
  [f1]
  (fn
    ([] (f1))
    ([ret v] (f1 ret (inc v)))))

(deftest fold-vector-result-variants
  (let [v (vec (range 100))]
    (is (= 4950 (r/fold + v)))
    (is (= 4950 (r/fold + + v)))
    (is (= 4950 (r/fold 16 + + v)))))

(deftest fold-map-with-three-arity-reducef
  ;; Folding a map feeds key and value separately to a 3-arity reducef
  ;; (ret k v); assoc round-trips the entries.
  (is (= {:a 1 :b 2}
         (r/fold (fn ([] {}) ([a b] (merge a b))) assoc {:a 1 :b 2})))
  (is (= {} (r/fold (fn ([] {}) ([a b] (merge a b))) assoc {}))))

(deftest fold-over-reducer-chains
  (is (= 55 (r/fold + (r/map inc (vec (range 10))))))
  (is (= 20 (r/fold + (r/filter even? (vec (range 9))))))
  (is (= 5050 (r/fold 16 + + (r/map inc (vec (range 100))))))
  (is (= 2550 (r/fold + (r/filter even? (r/map inc (vec (range 100))))))))

(deftest collfold-protocol-and-method
  ;; CollFold is the protocol behind fold; coll-fold is its method:
  ;; (coll-fold coll n combinef reducef).
  (is (satisfies? r/CollFold []))
  (is (satisfies? r/CollFold (vec (range 5))))
  (is (= 6 (r/coll-fold [1 2 3] 2 + +)))
  (is (= 0 (r/coll-fold [] 512 + +))))

(deftest reducer-returns-reducible
  ;; (reducer coll xf) wraps coll so any reducing fn handed to reduce
  ;; is first transformed by xf.
  (let [rdc (r/reducer [1 2 3] reducers-xf-inc)]
    (is (= [2 3 4] (r/reduce conj [] rdc)))
    (is (= 9 (r/reduce + 0 rdc)))
    (is (= 9 (r/reduce + rdc)))))

(deftest folder-supports-reduce-and-fold
  ;; (folder coll xf) is the foldable analogue of reducer: the wrapped
  ;; collection honors both reduce and fold.
  (let [fld (r/folder (vec (range 10)) reducers-xf-inc)]
    (is (= [1 2 3 4 5 6 7 8 9 10] (r/reduce conj [] fld)))
    (is (= 55 (r/reduce + 0 fld)))
    (is (= 55 (r/fold + fld)))
    (is (= 55 (r/fold 4 + + fld)))))

(deftest append-builds-accumulator
  ;; (cat) with no args yields an empty accumulator; append! adds an
  ;; element and returns the accumulator, fold's reducef counterpart
  ;; to the cat combinef.
  (let [acc (r/append! (r/append! (r/cat) 1) 2)]
    (is (= 2 (count acc)))
    (is (= [1 2] (vec (seq acc))))))

(deftest cat-positional-constructor
  ;; ->Cat builds the counted, seqable, reducible, foldable catenation
  ;; node from a count and two halves.
  (let [c (r/->Cat 4 [1 2] [3 4])]
    (is (= 4 (count c)))
    (is (= [1 2 3 4] (vec (seq c))))
    (is (= 10 (r/reduce + 0 c)))
    (is (= 10 (r/fold + c)))))

(deftest foldcat-counted-and-seqable
  (let [res (r/foldcat (r/map inc (vec (range 10))))]
    (is (= 10 (count res)))
    (is (= [1 2 3 4 5 6 7 8 9 10] (vec (seq res))))
    (is (= 55 (r/reduce + 0 res))))
  ;; Large enough to span several fold partitions.
  (let [res (r/foldcat (r/map inc (vec (range 2000))))]
    (is (= 2000 (count res)))
    (is (= 1 (first res)))
    (is (= 2001000 (r/reduce + 0 res)))))

(run-tests-and-exit)
