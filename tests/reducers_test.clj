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
