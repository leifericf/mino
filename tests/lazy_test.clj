(require "tests/test")

;; Lazy sequences and lazy combinators.

(deftest lazy-seq-basic
  (is (= 1 (first (lazy-seq (cons 1 nil)))))
  (is (= nil (first (lazy-seq nil))))
  (is (= '(2) (rest (lazy-seq (cons 1 (cons 2 nil)))))))

(deftest seq-fn
  (is (= '(1 2 3) (seq '(1 2 3))))
  (is (= '(1 2 3) (seq [1 2 3])))
  (is (= nil (seq nil)))
  (is (= nil (seq [])))
  (is (= '([:a 1]) (seq {:a 1})))
  (is (= '(\a \b) (seq "ab"))))

(deftest realized-fn
  (let [s (lazy-seq (cons 1 nil))]
    (is (not (realized? s)))
    (first s)
    (is (realized? s))))

(deftest lazy-seq-count
  (is (= 2 (count (lazy-seq (cons 1 (cons 2 nil)))))))

(deftest lazy-seq-chain
  (defn nats__lt [n] (lazy-seq (cons n (nats__lt (+ n 1)))))
  (is (= 2 (first (rest (rest (nats__lt 0)))))))

(deftest lazy-seq-take
  (is (= '(1 2 3) (take 5 (lazy-seq (cons 1 (cons 2 (cons 3 nil))))))))

(deftest lazy-seq-type
  (is (= :lazy-seq (type (lazy-seq nil)))))

;; --- Lazy combinators ---

(deftest take-while-fn
  (is (= '(1 2) (take-while (fn [x] (< x 3)) [1 2 3 4]))))

(deftest drop-while-fn
  (is (= '(3 4) (drop-while (fn [x] (< x 3)) [1 2 3 4]))))

(deftest iterate-fn
  (is (= '(0 1 2 3 4) (take 5 (iterate inc 0)))))

(deftest repeatedly-fn
  (is (= '(42 42 42) (take 3 (repeatedly (fn [] 42))))))

(deftest interleave-fn
  (is (= '(1 :a 2 :b 3 :c) (interleave [1 2 3] [:a :b :c]))))

(deftest interpose-fn
  (is (= '(1 :sep 2 :sep 3) (interpose :sep [1 2 3]))))

(deftest distinct-fn
  (is (= '(1 2 3) (distinct [1 2 1 3 2]))))

(deftest cycle-fn
  (is (= '(1 2 1 2 1) (take 5 (cycle [1 2])))))

(deftest partition-fn
  (is (= '((1 2) (3 4) (5 6)) (partition 2 [1 2 3 4 5 6]))))

(deftest doall-fn
  (is (= 3 (count (doall (map inc [1 2 3]))))))

(deftest doall-bounded-arity
  ;; (doall n coll) / (dorun n coll) force at most n steps of the
  ;; spine and leave the rest lazy.
  (is (= '(2 3 4) (doall 2 (map inc [1 2 3]))))
  (is (= nil (dorun 1 (map inc [1 2 3]))))
  (let [hits (atom 0)
        s (map (fn [x] (swap! hits inc) x) (list 1 2 3 4 5 6))]
    (dorun 2 s)
    (is (<= @hits 3))
    (is (>= @hits 2))))

(deftest doall-realizes-every-chunk
  ;; A chunked source spans multiple chunks; doall must force the
  ;; whole spine, not just the head chunk.
  (let [hits (atom 0)]
    (doall (map (fn [x] (swap! hits inc) x) (vec (range 70))))
    (is (= 70 @hits)))
  (let [s (map inc (vec (range 70)))]
    (doall s)
    (is (= 70 (count s)))))

(deftest eq-chunked-cons-with-lazy-tail
  ;; Regression: a chunked-cons spine can have an unrealized lazy seq
  ;; in its `more` field. Equality must force that tail rather than
  ;; treating it as end-of-seq. (filter pred (range N)) produces this
  ;; shape; comparing it to (range N) was returning false even when
  ;; pred kept every element.
  (let [coll (doall (range 1000))]
    (is (= coll (filter (fn [_] true) coll)))
    (is (= coll (filter (fn [_] true) coll)))
    (is (not (= coll (filter (fn [_] false) coll))))))

(deftest failed-lazy-seq-rethrows-on-each-force
  ;; A thunk that throws leaves the seq unrealized; every subsequent
  ;; force re-runs the thunk and surfaces the same throw.
  (let [s (lazy-seq (throw (ex-info "boom" {:n 1})))]
    (is (= :c1 (try (first s) (catch e :c1))))
    (is (= :c2 (try (first s) (catch e :c2))))
    (is (not (realized? s)))))

(deftest lazy-seq-thunk-retries-after-throw
  (let [calls (atom 0)
        s (lazy-seq
            (swap! calls inc)
            (when (= 1 @calls) (throw (ex-info "transient" {})))
            (list :ok))]
    (is (= :caught (try (first s) (catch e :caught))))
    (is (= :ok (first s)))
    (is (= 2 @calls))))

;; --- Non-seq thunk results ---
;; Forcing a lazy-seq must call seq on the thunk's result when it is
;; not already a seq, so any seqable body (vector, string, map, set,
;; another lazy-seq) behaves like its seq.

(deftest lazy-seq-vector-body
  (is (= '(1 2) (seq (lazy-seq [1 2]))))
  (is (seq? (seq (lazy-seq [1 2]))))
  (is (= 1 (first (lazy-seq [1 2]))))
  (is (= '(2) (rest (lazy-seq [1 2])))))

(deftest lazy-seq-empty-body
  (is (= nil (seq (lazy-seq nil))))
  (is (= nil (first (lazy-seq nil))))
  (is (= nil (seq (lazy-seq []))))
  (is (= nil (first (lazy-seq [])))))

(deftest lazy-seq-string-body
  (is (= \a (first (lazy-seq "ab"))))
  (is (= '(\a \b) (seq (lazy-seq "ab")))))

(deftest lazy-seq-map-body
  (is (= [:a 1] (first (lazy-seq {:a 1}))))
  (is (= '([:a 1]) (seq (lazy-seq {:a 1})))))

(deftest lazy-seq-set-body
  (is (= #{1 2} (set (lazy-seq #{1 2}))))
  (is (contains? #{1 2} (first (lazy-seq #{1 2})))))

(deftest lazy-seq-nested-body
  (is (= 1 (first (lazy-seq (lazy-seq [1]))))))

(deftest lazy-seq-seq-body-still-works
  (is (= '(2 3 4) (take 3 (lazy-seq (map inc [1 2 3])))))
  (is (= 1 (first (lazy-seq (cons 1 nil))))))

(run-tests-and-exit)
