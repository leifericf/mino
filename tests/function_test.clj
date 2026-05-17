(require "tests/test")

;; Functions, closures, higher-order, loop/recur.

(deftest fn-lambda
  (is (= 49 ((fn [x] (* x x)) 7))))

(deftest fn-no-arg
  (is (= 42 ((fn [] 42)))))

(deftest fn-def
  (def sq__ft (fn [x] (* x x)))
  (is (= 81 (sq__ft 9))))

(deftest closure
  (def adder__ft (fn [n] (fn [x] (+ x n))))
  (def add5__ft (adder__ft 5))
  (is (= 15 (add5__ft 10))))

(deftest higher-order
  (def apply-twice__ft (fn [f x] (f (f x))))
  (is (= 12 (apply-twice__ft (fn [n] (+ n 1)) 10))))

(deftest variadic-rest
  (is (= '(1 (2 3 4)) ((fn [a & b] (list a b)) 1 2 3 4))))

(deftest loop-countdown
  (is (= 120 (loop [n 5 acc 1]
                (if (<= n 1) acc (recur (- n 1) (* acc n)))))))

(deftest factorial
  (def fact__ft (fn [n]
    (loop [i n acc 1] (if (<= i 1) acc (recur (- i 1) (* acc i))))))
  (is (= 3628800 (fact__ft 10))))

(deftest fib
  (def fib__ft (fn [n]
    (loop [i 0 a 0 b 1] (if (< i n) (recur (+ i 1) b (+ a b)) a))))
  (is (= 6765 (fib__ft 20))))

(deftest recur-in-fn
  (def count-down__ft (fn [n] (if (<= n 0) "done" (recur (- n 1)))))
  (is (= "done" (count-down__ft 1000))))

(deftest defn-basic
  (defn inc1__ft [x] (+ x 1))
  (is (= 6 (inc1__ft 5))))

(deftest defn-multi-body
  (defn f__ft [x] (+ x 1) (+ x 2))
  (is (= 12 (f__ft 10))))

(deftest apply-fn
  (is (= 6 (apply + [1 2 3])))
  (is (= 10 (apply + 1 2 [3 4]))))

(deftest apply-lazy-args-to-rest-binding
  (is (= [0 1 2] (apply (fn [& xs] (vec (take 3 xs))) (range))))
  (is (= [:a [0 1 2]]
         (apply (fn [a & xs] [a (vec (take 3 xs))]) :a (range))))
  (is (= [0 1 2]
         (apply (fn [a b c & xs] (vec (take 3 xs))) 1 2 3 (range)))))

(deftest apply-cons-list-params
  (let [f (fn [a b & rest] [a b (count rest)])]
    (is (= [1 2 3] (apply f 1 2 [3 4 5])))
    (is (= [1 2 0] (apply f 1 2 [])))
    (is (= [1 2 3] (apply f 1 [2 3 4 5])))))

(deftest apply-vector-destructure-params
  (is (= [:a :b 3] (apply (fn [[a b & rest]] [a b (count rest)])
                          [[:a :b :c :d :e]])))
  (is (= [:a :b 0] (apply (fn [[a b & rest]] [a b (count rest)])
                          [[:a :b]]))))

(deftest higher-order-utils
  (is (= false ((comp not nil?) nil)))
  (is (= 15 ((partial + 10) 5)))
  (is (= true ((complement nil?) 42)))
  (is (= false ((complement nil?) nil)))
  (is (= 42 ((constantly 42) :a :b))))

(deftest bc-arity-mismatch-raises
  ;; A bc-compiled fn called with no matching clause must raise the
  ;; same diagnostic the tree-walker raises -- a silent NULL return
  ;; surfaces as "unhandled exception" with no message, hiding the
  ;; actual problem.
  (defn m__am ([a] :one) ([a b c] :three))
  (testing "call with arity 0 raises"
    (is (thrown? (m__am))))
  (testing "call with arity 2 raises with the arity message"
    (let [msg (try (m__am 1 2) (catch e (ex-message e)))]
      (is (some? (re-find #"no matching arity" msg)))
      (is (some? (re-find #"2 args" msg)))
      (is (some? (re-find #"m__am" msg)))))
  (testing "fixed-arity single clause raises on wrong count"
    (defn s__am [a b] (+ a b))
    (let [msg0 (try (s__am) (catch e (ex-message e)))
          msg3 (try (s__am 1 2 3) (catch e (ex-message e)))]
      (is (some? (re-find #"no matching arity" msg0)))
      (is (some? (re-find #"0 args" msg0)))
      (is (some? (re-find #"s__am" msg0)))
      (is (some? (re-find #"no matching arity" msg3)))
      (is (some? (re-find #"3 args" msg3)))
      (is (some? (re-find #"s__am" msg3))))))
