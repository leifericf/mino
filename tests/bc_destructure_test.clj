(require "tests/test")

;; Destructure inside a defn body (so the bytecode compiler takes the
;; form) -- both let-bindings and fn-params, vector + map shapes.
;; Tree-walker parity is the contract; the BC compiler flattens via
;; prim_destructure at compile time for lets, and rewrites params
;; into a wrapping let for fn shapes.

(deftest bc-let-vec-destructure
  (testing "positional binding"
    (defn t1 [v] (let [[a b c] v] (+ a b c)))
    (is (= 6 (t1 [1 2 3]))))
  (testing "rest binding"
    (defn t2 [v] (let [[a & r] v] [a (count r)]))
    (is (= [1 3] (t2 [1 2 3 4]))))
  (testing "with :as"
    (defn t3 [v] (let [[a b :as all] v] [a b all]))
    (is (= [1 2 [1 2 3]] (t3 [1 2 3]))))
  (testing "nested vector destructure"
    (defn t4 [v] (let [[[a b] c] v] (+ a b c)))
    (is (= 6 (t4 [[1 2] 3])))))

(deftest bc-let-map-destructure
  (testing ":keys binding"
    (defn t1 [m] (let [{:keys [x y]} m] (+ x y)))
    (is (= 30 (t1 {:x 10 :y 20}))))
  (testing "explicit pair binding"
    (defn t3 [m] (let [{name :n age :a} m] [name age]))
    (is (= ["Alice" 30] (t3 {:n "Alice" :a 30}))))
  (testing ":or defaults"
    (defn t4 [m] (let [{:keys [x y] :or {y 99}} m] [x y]))
    (is (= [1 99] (t4 {:x 1}))))
  (testing ":as alias"
    (defn t5 [m] (let [{:keys [x] :as all} m] [x all]))
    (is (= [1 {:x 1 :y 2}] (t5 {:x 1 :y 2})))))

(deftest bc-fn-param-destructure
  (testing "vector destructure in param"
    (defn t1 [[a b]] (+ a b))
    (is (= 30 (t1 [10 20]))))
  (testing "map destructure in param"
    (defn t2 [{:keys [x y]}] (* x y))
    (is (= 12 (t2 {:x 3 :y 4}))))
  (testing "mixed destructures across params"
    (defn t3 [[a b] {:keys [k]}] (+ a b k))
    (is (= 10 (t3 [1 2] {:k 7}))))
  (testing "rest inside vector destructure"
    (defn t4 [[h & t]] {:h h :t t})
    (is (= {:h 1 :t '(2 3)} (t4 [1 2 3]))))
  (testing "destructure mixed with plain params"
    (defn t5 [x [a b]] [x a b])
    (is (= [99 1 2] (t5 99 [1 2])))))

(deftest bc-destructure-with-throw
  (testing "throw inside destructured-let body"
    (defn t [v] (try (let [[a b] v] (throw {:got [a b]}))
                     (catch e (ex-data e))))
    (is (= {:got [1 2]} (t [1 2]))))
  (testing "throw inside destructured-fn body"
    (defn deep [[a b]] (throw [a b]))
    (defn t [v] (try (deep v) (catch e (ex-data e))))
    (is (= [3 4] (t [3 4])))))
