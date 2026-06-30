(require "tests/test")
(require '[clojure.string :as str])

;; Collections and keywords as callable functions (IFn).

;; --- Vars as functions ---
;; clojure.lang.Var implements IFn, so a var in function position
;; invokes its bound value. Covers both the tree-walk eval path
;; (top-level / unresolved heads) and the bytecode call path (a var
;; handed to and called by a compiled fn).

(deftest var-as-fn-via-resolve
  (is (= "HI" ((resolve 'clojure.string/upper-case) "hi")))
  (is (= "YO" ((var clojure.string/upper-case) "yo"))))

(deftest var-as-fn-through-compiled-fn
  (let [call-it (fn [f x] (f x))]
    (is (= 42 (call-it (resolve 'clojure.core/inc) 41)))
    (is (= "ABC" (call-it (resolve 'str/upper-case) "abc")))))

;; --- Maps as functions ---

(deftest map-as-fn-basic
  (is (= 1 ({:a 1 :b 2} :a)))
  (is (= 2 ({:a 1 :b 2} :b)))
  (is (nil? ({:a 1} :z))))

(deftest map-as-fn-default
  (is (= 42 ({:a 1} :z 42)))
  (is (= 1 ({:a 1} :a 42))))

(deftest map-as-fn-higher-order
  (is (= '(1 2 3) (map {:a 1 :b 2 :c 3} [:a :b :c])))
  (is (= '("a" "b") (map :name [{:name "a"} {:name "b"}]))))

;; --- Vectors as functions ---

(deftest vector-as-fn-basic
  (is (= :a ([:a :b :c] 0)))
  (is (= :b ([:a :b :c] 1)))
  (is (= :c ([:a :b :c] 2))))

(deftest vector-as-fn-higher-order
  (is (= '(:a :b) (map [10 :a :b] [1 2]))))

;; --- Sets as functions ---

(deftest set-as-fn-basic
  (is (= :a (#{:a :b :c} :a)))
  (is (nil? (#{:a :b :c} :z))))

(deftest set-as-fn-filter
  (is (= '(:a :b) (filter #{:a :b} [:a :c :b :d]))))

;; --- Keywords in higher-order contexts ---

(deftest keyword-higher-order
  (is (= '(1 2 3) (map :val [{:val 1} {:val 2} {:val 3}])))
  (is (= '(1 2) (map :a [{:a 1 :b 10} {:a 2 :b 20}]))))

;; --- apply with collections ---

(deftest apply-with-collections
  (is (= 1 (apply {:a 1 :b 2} [:a])))
  (is (= :b (apply [:a :b :c] [1])))
  (is (= :a (apply #{:a :b} [:a]))))

;; (run-tests) -- called by tests/run.clj
