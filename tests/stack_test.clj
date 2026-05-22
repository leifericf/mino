(require "tests/test")

;; peek and pop: stack abstraction for vectors and lists.

;; --- peek ---

(deftest peek-vector
  (is (= 3 (peek [1 2 3])))
  (is (= 1 (peek [1])))
  (is (nil? (peek []))))

(deftest peek-list
  (is (= 1 (peek '(1 2 3))))
  (is (= :a (peek '(:a)))))

(deftest peek-nil
  (is (nil? (peek nil))))

;; --- pop ---

(deftest pop-vector-basic
  (is (= [1 2] (pop [1 2 3])))
  (is (= [] (pop [1]))))

(deftest pop-vector-sizes
  (testing "size 32 boundary"
    (let [v (vec (range 32))]
      (is (= 31 (count (pop v))))
      (is (= 30 (peek (pop v))))))
  (testing "size 33 boundary"
    (let [v (vec (range 33))]
      (is (= 32 (count (pop v))))
      (is (= 31 (peek (pop v))))))
  (testing "size 1024 boundary"
    (let [v (vec (range 1024))]
      (is (= 1023 (count (pop v))))
      (is (= 1022 (peek (pop v))))))
  (testing "size 1025 boundary"
    (let [v (vec (range 1025))]
      (is (= 1024 (count (pop v))))
      (is (= 1023 (peek (pop v)))))))

(deftest pop-list
  (is (= '(2 3) (pop '(1 2 3))))
  ;; Popping the only element of a list returns the empty list, not nil.
  ;; JVM Clojure: (pop '(1)) -> () with `list?` true.
  (is (= '() (pop '(1))))
  (is (list? (pop '(1))))
  (is (not (nil? (pop '(1))))))

(deftest pop-empty-list-throws
  ;; JVM Clojure throws IllegalStateException "Can't pop empty list".
  ;; mino used to return a misleading "expected vector, list, or queue,
  ;; got list" -- it IS a list, just empty.
  (let [err (try (pop '()) nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"empty" err)))
    (is (some? (re-find #"pop" err)))))

(deftest pop-preserves-structure
  (testing "round-trip through repeated pop"
    (let [v (vec (range 100))]
      (loop [v v n 100]
        (if (= n 0)
          (is (= 0 (count v)))
          (do
            (is (= n (count v)))
            (is (= (- n 1) (peek v)))
            (recur (pop v) (- n 1))))))))

(deftest pop-empty-errors
  (is (thrown? "pop" (pop [])))
  (is (= nil (pop nil))))

;; (run-tests) -- called by tests/run.clj
