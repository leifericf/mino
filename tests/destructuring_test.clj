(require "tests/test")

;; Destructuring, named fn, multi-arity, vector bindings.

;; --- Vector binding forms ---

(deftest let-vector-bindings
  (testing "single binding"
    (is (= 5 (let [x 5] x))))
  (testing "multiple bindings"
    (is (= 3 (let [x 1 y 2] (+ x y)))))
  (testing "sequential reference"
    (is (= 11 (let [x 1 y (+ x 10)] y)))))

(deftest fn-vector-params
  (is (= 49 ((fn [x] (* x x)) 7)))
  (is (= 42 ((fn [] 42))))
  (is (= 7 ((fn [x y] (+ x y)) 3 4))))

(deftest loop-vector-bindings
  (is (= 120 (loop [n 5 acc 1]
               (if (<= n 1) acc (recur (- n 1) (* acc n)))))))

(deftest defn-vector-params
  (defn sq__dt [x] (* x x))
  (is (= 25 (sq__dt 5))))

;; --- Named anonymous functions ---

(deftest named-fn-recursion
  (def fib__dt (fn fib [n]
    (if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2))))))
  (is (= 55 (fib__dt 10))))

(deftest named-fn-no-leak
  (def outer__dt (fn [] (fn inner [x] (+ x 1))))
  (is (= 6 ((outer__dt) 5))))

(deftest named-fn-with-list-params
  (def fact__dt (fn fact (n) (if (<= n 1) 1 (* n (fact (- n 1))))))
  (is (= 120 (fact__dt 5))))

;; --- Multi-arity functions ---

(deftest multi-arity-basic
  (def greet__dt (fn
    ([name] (str "hello " name))
    ([greeting name] (str greeting " " name))))
  (is (= "hello world" (greet__dt "world")))
  (is (= "hey alice" (greet__dt "hey" "alice"))))

(deftest multi-arity-with-zero
  (def counter__dt (fn
    ([] 0)
    ([x] x)
    ([x y] (+ x y))))
  (is (= 0 (counter__dt)))
  (is (= 5 (counter__dt 5)))
  (is (= 7 (counter__dt 3 4))))

(deftest multi-arity-variadic
  (def my-add__dt (fn
    ([] 0)
    ([x] x)
    ([x y] (+ x y))
    ([x y & rest] (apply + x y rest))))
  (is (= 0 (my-add__dt)))
  (is (= 5 (my-add__dt 5)))
  (is (= 7 (my-add__dt 3 4)))
  (is (= 10 (my-add__dt 1 2 3 4))))

(deftest multi-arity-named
  (def fib2__dt (fn fib2
    ([n] (fib2 n 0 1))
    ([n a b] (if (<= n 0) a (fib2 (- n 1) b (+ a b))))))
  (is (= 55 (fib2__dt 10))))

(deftest defn-multi-arity
  (defn greeter__dt
    ([name] (greeter__dt "hello" name))
    ([greeting name] (str greeting " " name)))
  (is (= "hello world" (greeter__dt "world")))
  (is (= "hey bob" (greeter__dt "hey" "bob"))))

;; --- Vector destructuring ---

(deftest vec-destructure-let
  (let [[a b c] [1 2 3]]
    (is (= 1 a))
    (is (= 2 b))
    (is (= 3 c))))

(deftest vec-destructure-rest
  (let [[x & rest] [1 2 3 4]]
    (is (= 1 x))
    (is (= '(2 3 4) rest))))

(deftest vec-destructure-as
  (let [[a b :as all] [1 2 3]]
    (is (= 1 a))
    (is (= 2 b))
    (is (= [1 2 3] all))))

(deftest vec-destructure-rest-and-as
  (let [[a & more :as all] [1 2 3]]
    (is (= 1 a))
    (is (= '(2 3) more))
    (is (= [1 2 3] all))))

(deftest vec-destructure-nested
  (let [[a [b c]] [1 [2 3]]]
    (is (= 1 a))
    (is (= 2 b))
    (is (= 3 c))))

(deftest vec-destructure-from-list
  (let [[a b c] '(10 20 30)]
    (is (= 10 a))
    (is (= 20 b))
    (is (= 30 c))))

(deftest vec-destructure-fn-params
  (defn head-tail__dt [[h & t]] {:head h :tail t})
  (is (= {:head 1 :tail '(2 3)} (head-tail__dt [1 2 3]))))

;; --- Map destructuring ---

(deftest map-destructure-keys
  (let [{:keys [name age]} {:name "Alice" :age 30}]
    (is (= "Alice" name))
    (is (= 30 age))))

(deftest map-destructure-explicit
  (let [{n :name a :age} {:name "Bob" :age 25}]
    (is (= "Bob" n))
    (is (= 25 a))))

(deftest map-destructure-symbol-key-evaluates
  ;; In {sym k} the RHS is evaluated, not used as a literal, so a
  ;; bound symbol resolves to whatever it points to at that scope.
  (let [k :name]
    (let [{nm k} {:name "john"}]
      (is (= "john" nm))))
  (let [k (keyword "name")
        {person-name k} {:name "john"}]
    (is (= "john" person-name))))

(deftest map-destructure-nested-patterns
  ;; The binding position in {pattern :key} may itself be a vector
  ;; or map pattern; recursive destructure binds its elements.
  (let [{a :a, [lhs rhs] :c} {:a 1, :c [:foo :bar]}]
    (is (= 1     a))
    (is (= :foo  lhs))
    (is (= :bar  rhs)))
  (let [{a :a, {x :x y :y} :inner} {:a 1 :inner {:x 10 :y 20}}]
    (is (= 1  a))
    (is (= 10 x))
    (is (= 20 y))))

(deftest map-destructure-defaults
  (let [{:keys [x y] :or {x 10 y 20}} {:x 5}]
    (is (= 5 x))
    (is (= 20 y))))

(deftest map-destructure-as
  (let [{:keys [a] :as m} {:a 1 :b 2}]
    (is (= 1 a))
    (is (= {:a 1 :b 2} m))))

(deftest map-destructure-nested
  (let [[{:keys [x]} {:keys [y]}] [{:x 1} {:y 2}]]
    (is (= 1 x))
    (is (= 2 y))))

(deftest map-destructure-fn-params
  (defn extract__dt [{:keys [name age]}] (str name " is " age))
  (is (= "Alice is 30" (extract__dt {:name "Alice" :age 30}))))

;; --- Mixed ---

(deftest mixed-vec-map-destructure
  (let [[a {:keys [b c]}] [1 {:b 2 :c 3}]]
    (is (= 1 a))
    (is (= 2 b))
    (is (= 3 c))))

;; --- String- and symbol-keyed map destructure ---

(deftest map-destructure-strs
  (let [{:strs [a b]} {"a" 1 "b" 2}]
    (is (= 1 a))
    (is (= 2 b))))

(deftest map-destructure-syms
  (let [{:syms [x y]} {'x 10 'y 20}]
    (is (= 10 x))
    (is (= 20 y))))

(deftest map-destructure-strs-with-defaults
  (let [{:strs [a b c] :or {b 99 c 100}} {"a" 1 "c" 3}]
    (is (= 1 a))
    (is (= 99 b))
    (is (= 3 c))))

(deftest map-destructure-strs-and-keys
  (let [{:keys [k] :strs [s]} {:k 1 "s" 2}]
    (is (= 1 k))
    (is (= 2 s))))

(deftest map-destructure-strs-fn-params
  (defn pick__dt [{:strs [host port]}] [host port])
  (is (= ["x" 80] (pick__dt {"host" "x" "port" 80}))))

(deftest map-destructure-syms-fn-params
  (defn pick-syms__dt [{:syms [a b]}] [a b])
  (is (= [1 2] (pick-syms__dt {'a 1 'b 2}))))

(deftest vec-destructure-lazy-seq
  ;; Regression: vector destructuring failed on lazy seqs because the
  ;; downstream positional walk's mino_is_cons check returned false for
  ;; MINO_LAZY (and MINO_CHUNKED_CONS). Every pattern slot bound to
  ;; nil instead of the corresponding element. Common in code that
  ;; destructures (range), (map), (filter), or any chunked seq.
  (testing "(let [[a b c] (range 3)] ...) binds to 0 1 2"
    (let [[a b c] (range 3)]
      (is (= 0 a))
      (is (= 1 b))
      (is (= 2 c))))
  (testing "destructure (map f coll)"
    (let [[x y z] (map inc [10 20 30])]
      (is (= 11 x))
      (is (= 21 y))
      (is (= 31 z))))
  (testing "destructure (filter pred coll)"
    (let [[a b] (filter even? (range 10))]
      (is (= 0 a))
      (is (= 2 b))))
  (testing "shorter lazy seq pads with nil"
    (let [[a b c] (range 2)]
      (is (= 0 a))
      (is (= 1 b))
      (is (nil? c))))
  (testing "destructure inside fn arg"
    (defn first-three [[a b c]] [a b c])
    (is (= [0 1 2] (first-three (range 3)))))
  (testing "doall lazy seq still destructures"
    (let [[a b c] (doall (range 3))]
      (is (= [0 1 2] [a b c])))))
