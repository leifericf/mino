;; Tests for multi-binding doseq and for.

;; --- doseq ---

(deftest doseq-multi-binding
  (testing "single binding unchanged"
    (let [log (atom [])]
      (doseq [x [1 2 3]]
        (swap! log conj x))
      (is (= [1 2 3] @log))))
  (testing "two bindings"
    (let [log (atom [])]
      (doseq [x [1 2] y [:a :b]]
        (swap! log conj [x y]))
      (is (= [[1 :a] [1 :b] [2 :a] [2 :b]] @log))))
  (testing "three bindings"
    (let [log (atom [])]
      (doseq [x [1] y [2] z [3 4]]
        (swap! log conj [x y z]))
      (is (= [[1 2 3] [1 2 4]] @log)))))

;; --- for ---

(deftest for-multi-binding
  (testing "single binding unchanged"
    (is (= [1 4 9] (into [] (for [x [1 2 3]] (* x x))))))
  (testing "two bindings"
    (is (= [[1 :a] [1 :b] [2 :a] [2 :b]]
           (into [] (for [x [1 2] y [:a :b]] [x y])))))
  (testing "three bindings"
    (is (= [[1 2 :x] [1 2 :y]]
           (into [] (for [a [1] b [2] c [:x :y]] [a b c]))))))

(deftest for-when-modifier
  (testing ":when filter"
    (is (= [0 2 4 6 8]
           (into [] (for [x (range 10) :when (even? x)] x)))))
  (testing ":when with multiple bindings"
    (is (= [[1 2] [1 4] [3 2] [3 4]]
           (into [] (for [x [1 2 3] :when (odd? x) y [2 4]] [x y]))))))

(deftest for-let-modifier
  (testing ":let local bindings"
    (is (= [1 4 9]
           (into [] (for [x [1 2 3] :let [y (* x x)]] y)))))
  (testing ":let with multiple bindings"
    (is (= [11 21 12 22]
           (into [] (for [x [1 2] y [10 20] :let [z (+ x y)]] z))))))
