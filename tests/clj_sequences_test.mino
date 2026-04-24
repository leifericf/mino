(require "tests/test")

;; Tests from clojure/test_clojure/sequences.clj
;; Expected values are taken as-is from the Clojure test suite.

;; --- cons ---

(deftest clj-cons
  (is (= (cons 1 nil) '(1)))
  (is (= (cons nil nil) '(nil)))
  (is (= (cons 1 ()) '(1)))
  (is (= (cons 1 '(2 3)) '(1 2 3)))
  (is (= (cons 1 []) '(1)))
  (is (= (cons 1 [2 3]) '(1 2 3)))
  (is (= (cons 1 #{}) '(1))))

;; --- first ---

(deftest clj-first
  (is (= (first nil) nil))
  (is (= (first '(1)) 1))
  (is (= (first '(1 2 3)) 1))
  (is (= (first '(nil)) nil))
  (is (= (first '(1 nil)) 1))
  (is (= (first '(nil 2)) nil))
  (is (= (first '(())) ()))
  (is (= (first []) nil))
  (is (= (first [1]) 1))
  (is (= (first [1 2 3]) 1))
  (is (= (first [nil]) nil))
  (is (= (first [1 nil]) 1))
  (is (= (first [nil 2]) nil))
  (is (= (first [[]]) []))
  (is (= (first #{}) nil))
  (is (= (first {}) nil)))

;; --- next ---

(deftest clj-next
  (is (= (next nil) nil))
  (is (= (next '(1)) nil))
  (is (= (next '(1 2 3)) '(2 3)))
  (is (= (next '(nil)) nil))
  (is (= (next '(1 nil)) '(nil)))
  (is (= (next '(1 ())) '(())))
  (is (= (next '(nil 2)) '(2)))
  (is (= (next '(())) nil))
  (is (= (next '(() nil)) '(nil)))
  (is (= (next '(() 2 nil)) '(2 nil)))
  (is (= (next []) nil))
  (is (= (next [1]) nil))
  (is (= (next [1 2 3]) '(2 3)))
  (is (= (next [nil]) nil))
  (is (= (next [1 nil]) '(nil)))
  (is (= (next [1 []]) '([])))
  (is (= (next [nil 2]) '(2)))
  (is (= (next [[]]) nil))
  (is (= (next [[] nil]) '(nil)))
  (is (= (next [[] 2 nil]) '(2 nil)))
  (is (= (next #{}) nil))
  (is (= (next {}) nil)))

;; --- fnext / second ---

(deftest clj-fnext
  (is (= (fnext nil) nil))
  (is (= (fnext '(1)) nil))
  (is (= (fnext '(1 2 3 4)) 2))
  (is (= (fnext []) nil))
  (is (= (fnext [1]) nil))
  (is (= (fnext [1 2 3 4]) 2))
  (is (= (fnext {}) nil))
  (is (= (fnext #{}) nil)))

;; --- last ---

(deftest clj-last
  (is (= (last nil) nil))
  (is (= (last '(1)) 1))
  (is (= (last '(1 2 3)) 3))
  (is (= (last '(nil)) nil))
  (is (= (last '(1 nil)) nil))
  (is (= (last '(nil 2)) 2))
  (is (= (last '(())) ()))
  (is (= (last '(() nil)) nil))
  (is (= (last '(() 2 nil)) nil))
  (is (= (last []) nil))
  (is (= (last [1]) 1))
  (is (= (last [1 2 3]) 3))
  (is (= (last [nil]) nil))
  (is (= (last [1 nil]) nil))
  (is (= (last [nil 2]) 2))
  (is (= (last [[]]) []))
  (is (= (last [[] nil]) nil))
  (is (= (last [[] 2 nil]) nil))
  (is (= (last #{}) nil))
  (is (= (last {}) nil)))

;; --- ffirst ---

(deftest clj-ffirst
  (is (= (ffirst nil) nil))
  (is (= (ffirst '((1 2) (3 4))) 1))
  (is (= (ffirst []) nil))
  (is (= (ffirst [[1 2] [3 4]]) 1))
  (is (= (ffirst {}) nil))
  (is (= (ffirst {:a 1}) :a))
  (is (= (ffirst #{}) nil))
  (is (= (ffirst #{[1 2]}) 1)))

;; --- nfirst ---

(deftest clj-nfirst
  (is (= (nfirst nil) nil))
  (is (= (nfirst '((1 2 3) (4 5 6))) '(2 3)))
  (is (= (nfirst []) nil))
  (is (= (nfirst [[1 2 3] [4 5 6]]) '(2 3)))
  (is (= (nfirst {}) nil))
  (is (= (nfirst {:a 1}) '(1)))
  (is (= (nfirst #{}) nil))
  (is (= (nfirst #{[1 2]}) '(2))))

;; --- nnext ---

(deftest clj-nnext
  (is (= (nnext nil) nil))
  (is (= (nnext '(1)) nil))
  (is (= (nnext '(1 2)) nil))
  (is (= (nnext '(1 2 3 4)) '(3 4)))
  (is (= (nnext []) nil))
  (is (= (nnext [1]) nil))
  (is (= (nnext [1 2]) nil))
  (is (= (nnext [1 2 3 4]) '(3 4)))
  (is (= (nnext #{}) nil))
  (is (= (nnext {}) nil)))

;; --- nthnext ---

(deftest clj-nthnext
  (is (= (nthnext [1 2 3 4 5] 1) '(2 3 4 5)))
  (is (= (nthnext [1 2 3 4 5] 3) '(4 5)))
  (is (= (nthnext [1 2 3 4 5] 5) nil))
  (is (= (nthnext [1 2 3 4 5] 9) nil))
  (is (= (nthnext [1 2 3 4 5] 0) '(1 2 3 4 5)))
  (is (= (nthnext [1 2 3 4 5] -1) '(1 2 3 4 5)))
  (is (= (nthnext [1 2 3 4 5] -2) '(1 2 3 4 5))))

;; --- nthrest ---

(deftest clj-nthrest
  (is (= (nthrest [1 2 3 4 5] 1) '(2 3 4 5)))
  (is (= (nthrest [1 2 3 4 5] 3) '(4 5)))
  (is (= (nthrest [1 2 3 4 5] 5) ()))
  (is (= (nthrest [1 2 3 4 5] 9) ()))
  (is (= (nthrest [1 2 3 4 5] 0) '(1 2 3 4 5)))
  (is (= (nthrest [1 2 3 4 5] -1) '(1 2 3 4 5)))
  (is (= (nthrest [1 2 3 4 5] -2) '(1 2 3 4 5))))

;; --- butlast ---

(deftest clj-butlast
  (is (= (butlast []) nil))
  (is (= (butlast [1]) nil))
  (is (= (butlast [1 2 3]) '(1 2))))

;; --- take ---

(deftest clj-take
  (is (= (take 1 [1 2 3 4 5]) '(1)))
  (is (= (take 3 [1 2 3 4 5]) '(1 2 3)))
  (is (= (take 5 [1 2 3 4 5]) '(1 2 3 4 5)))
  (is (= (take 9 [1 2 3 4 5]) '(1 2 3 4 5)))
  (is (= (take 0 [1 2 3 4 5]) ()))
  (is (= (take -1 [1 2 3 4 5]) ()))
  (is (= (take -2 [1 2 3 4 5]) ())))

;; --- drop ---

(deftest clj-drop
  (is (= (drop 1 [1 2 3 4 5]) '(2 3 4 5)))
  (is (= (drop 3 [1 2 3 4 5]) '(4 5)))
  (is (= (drop 5 [1 2 3 4 5]) ()))
  (is (= (drop 9 [1 2 3 4 5]) ()))
  (is (= (drop 0 [1 2 3 4 5]) '(1 2 3 4 5)))
  (is (= (drop -1 [1 2 3 4 5]) '(1 2 3 4 5)))
  (is (= (drop -2 [1 2 3 4 5]) '(1 2 3 4 5))))

;; --- take-while ---

(deftest clj-take-while
  (is (= (take-while pos? []) ()))
  (is (= (take-while pos? [1 2 3 4]) '(1 2 3 4)))
  (is (= (take-while pos? [1 2 3 -1]) '(1 2 3)))
  (is (= (take-while pos? [1 -1 2 3]) '(1)))
  (is (= (take-while pos? [-1 1 2 3]) ()))
  (is (= (take-while pos? [-1 -2 -3]) ())))

;; --- drop-while ---

(deftest clj-drop-while
  (is (= (drop-while pos? []) ()))
  (is (= (drop-while pos? [1 2 3 4]) ()))
  (is (= (drop-while pos? [1 2 3 -1]) '(-1)))
  (is (= (drop-while pos? [1 -1 2 3]) '(-1 2 3)))
  (is (= (drop-while pos? [-1 1 2 3]) '(-1 1 2 3)))
  (is (= (drop-while pos? [-1 -2 -3]) '(-1 -2 -3))))

;; --- drop-last ---

(deftest clj-drop-last
  (is (= (drop-last []) ()))
  (is (= (drop-last [1]) ()))
  (is (= (drop-last [1 2 3]) '(1 2)))
  (is (= (drop-last 1 []) ()))
  (is (= (drop-last 1 [1]) ()))
  (is (= (drop-last 1 [1 2 3]) '(1 2)))
  (is (= (drop-last 2 []) ()))
  (is (= (drop-last 2 [1]) ()))
  (is (= (drop-last 2 [1 2 3]) '(1)))
  (is (= (drop-last 5 []) ()))
  (is (= (drop-last 5 [1]) ()))
  (is (= (drop-last 5 [1 2 3]) ()))
  (is (= (drop-last 0 []) ()))
  (is (= (drop-last 0 [1]) '(1)))
  (is (= (drop-last 0 [1 2 3]) '(1 2 3)))
  (is (= (drop-last -1 []) ()))
  (is (= (drop-last -1 [1]) '(1)))
  (is (= (drop-last -1 [1 2 3]) '(1 2 3)))
  (is (= (drop-last -2 []) ()))
  (is (= (drop-last -2 [1]) '(1)))
  (is (= (drop-last -2 [1 2 3]) '(1 2 3))))

;; --- split-at ---

(deftest clj-split-at
  (is (vector? (split-at 2 [])))
  (is (vector? (split-at 2 [1 2 3])))
  (is (= (split-at 2 []) [() ()]))
  (is (= (split-at 2 [1 2 3 4 5]) ['(1 2) '(3 4 5)]))
  (is (= (split-at 5 [1 2 3]) ['(1 2 3) ()]))
  (is (= (split-at 0 [1 2 3]) [() '(1 2 3)]))
  (is (= (split-at -1 [1 2 3]) [() '(1 2 3)]))
  (is (= (split-at -5 [1 2 3]) [() '(1 2 3)])))

;; --- split-with ---

(deftest clj-split-with
  (is (vector? (split-with pos? [])))
  (is (vector? (split-with pos? [1 2 -1 0 3 4])))
  (is (= (split-with pos? []) [() ()]))
  (is (= (split-with pos? [1 2 -1 0 3 4]) ['(1 2) '(-1 0 3 4)]))
  (is (= (split-with pos? [-1 2 3 4 5]) [() '(-1 2 3 4 5)]))
  (is (= (split-with number? [1 -2 "abc"]) ['(1 -2) '("abc")])))

;; --- concat ---

(deftest clj-concat
  (is (= (concat) ()))
  (is (= (concat []) ()))
  (is (= (concat [1 2]) '(1 2)))
  (is (= (concat [1 2] [3 4]) '(1 2 3 4)))
  (is (= (concat [] [3 4]) '(3 4)))
  (is (= (concat [1 2] []) '(1 2)))
  (is (= (concat [] []) ()))
  (is (= (concat [1 2] [3 4] [5 6]) '(1 2 3 4 5 6))))

;; --- distinct ---

(deftest clj-distinct
  (is (= (distinct []) ()))
  (is (= (distinct [1]) '(1)))
  (is (= (distinct [1 2 3]) '(1 2 3)))
  (is (= (distinct [1 2 3 1 2 2 1 1]) '(1 2 3)))
  (is (= (distinct [1 1 1 2]) '(1 2)))
  (is (= (distinct [1 2 1 2]) '(1 2)))
  (is (= (distinct [nil nil]) [nil]))
  (is (= (distinct [false false]) [false]))
  (is (= (distinct [true true]) [true]))
  (is (= (distinct [0 0]) [0]))
  (is (= (distinct [42 42]) [42]))
  (is (= (distinct ["" ""]) [""]))
  (is (= (distinct ["abc" "abc"]) ["abc"]))
  (is (= (distinct [:kw :kw]) [:kw]))
  (is (= (distinct [[] []]) [[]]))
  (is (= (distinct [[1 2] [1 2]]) [[1 2]]))
  (is (= (distinct [{} {}]) [{}]))
  (is (= (distinct [{:a 1 :b 2} {:a 1 :b 2}]) [{:a 1 :b 2}]))
  (is (= (distinct [#{} #{}]) [#{}]))
  (is (= (distinct [#{1 2} #{1 2}]) [#{1 2}])))

;; --- interpose ---

(deftest clj-interpose
  (is (= (interpose 0 []) ()))
  (is (= (interpose 0 [1]) '(1)))
  (is (= (interpose 0 [1 2]) '(1 0 2)))
  (is (= (interpose 0 [1 2 3]) '(1 0 2 0 3))))

;; --- interleave ---

(deftest clj-interleave
  (is (= (interleave [1 2] [3 4]) '(1 3 2 4)))
  (is (= (interleave [1] [3 4]) '(1 3)))
  (is (= (interleave [1 2] [3]) '(1 3)))
  (is (= (interleave [] [3 4]) ()))
  (is (= (interleave [1 2] []) ()))
  (is (= (interleave [] []) ()))
  (is (= (interleave [1]) '(1)))
  (is (= (interleave) ())))

;; --- partition ---

(deftest clj-partition
  (is (= (partition 2 [1 2 3]) '((1 2))))
  (is (= (partition 2 [1 2 3 4]) '((1 2) (3 4))))
  (is (= (partition 2 []) ()))
  (is (= (partition 2 3 [1 2 3 4 5 6 7]) '((1 2) (4 5))))
  (is (= (partition 2 3 [1 2 3 4 5 6 7 8]) '((1 2) (4 5) (7 8))))
  (is (= (partition 2 3 []) ()))
  (is (= (partition 1 []) ()))
  (is (= (partition 1 [1 2 3]) '((1) (2) (3))))
  (is (= (partition 5 [1 2 3]) ()))
  (is (= (partition -1 [1 2 3]) ()))
  (is (= (partition -2 [1 2 3]) ())))

;; --- partition-all ---

(deftest clj-partition-all
  (is (= (partition-all 4 [1 2 3 4 5 6 7 8 9])
         '((1 2 3 4) (5 6 7 8) (9))))
  (is (= (partition-all 4 2 [1 2 3 4 5 6 7 8 9])
         '((1 2 3 4) (3 4 5 6) (5 6 7 8) (7 8 9) (9)))))

;; --- partition-by ---

(deftest clj-partition-by
  (is (= (partition-by (fn [s] (even? (count s))) ["a" "bb" "cccc" "dd" "eee" "f" "" "hh"])
         '(("a") ("bb" "cccc" "dd") ("eee" "f") ("" "hh")))))

;; --- frequencies ---

(deftest clj-frequencies
  (is (= (frequencies [1 1 1 1 2 2 3]) {1 4, 2 2, 3 1})))

;; --- group-by ---

(deftest clj-group-by
  (is (= (group-by even? [1 2 3 4 5])
         {false [1 3 5], true [2 4]})))

;; --- flatten ---

(deftest clj-flatten
  (is (= (flatten nil) []))
  (is (= (flatten 1) []))
  (is (= (flatten :keyword) []))
  (is (= (flatten true) []))
  (is (= (flatten false) []))
  (is (= (flatten [[1 2] [3 4 [5]]]) [1 2 3 4 5]))
  (is (= (flatten [1 2 3 4 5]) [1 2 3 4 5]))
  (is (= (flatten [#{1 2} 3 4 5]) [#{1 2} 3 4 5]))
  (is (= (flatten {:a 1 :b 2}) [])))

;; --- repeat ---

(deftest clj-repeat
  (is (= (take 0 (repeat 7)) ()))
  (is (= (take 1 (repeat 7)) '(7)))
  (is (= (take 2 (repeat 7)) '(7 7)))
  (is (= (take 5 (repeat 7)) '(7 7 7 7 7)))
  (is (= (repeat 0 7) ()))
  (is (= (repeat 1 7) '(7)))
  (is (= (repeat 2 7) '(7 7)))
  (is (= (repeat 5 7) '(7 7 7 7 7)))
  (is (= (repeat -1 7) ()))
  (is (= (repeat -3 7) ()))
  (is (= (repeat 3 nil) '(nil nil nil)))
  (is (= (repeat 3 false) '(false false false)))
  (is (= (repeat 3 true) '(true true true)))
  (is (= (repeat 3 0) '(0 0 0)))
  (is (= (repeat 3 42) '(42 42 42)))
  (is (= (repeat 3 "abc") '("abc" "abc" "abc")))
  (is (= (repeat 3 :kw) '(:kw :kw :kw)))
  (is (= (repeat 3 []) '([] [] [])))
  (is (= (repeat 3 [1 2]) '([1 2] [1 2] [1 2])))
  (is (= (repeat 3 {}) '({} {} {})))
  (is (= (repeat 3 {:a 1}) '({:a 1} {:a 1} {:a 1})))
  (is (= (repeat 3 #{}) '(#{} #{} #{})))
  (is (= (repeat 3 #{1 2}) '(#{1 2} #{1 2} #{1 2})))
  (is (= '(:a) (drop 1 (repeat 2 :a))))
  (is (= () (drop 2 (repeat 2 :a))))
  (is (= () (drop 3 (repeat 2 :a)))))

;; --- range ---

(deftest clj-range
  (is (= (take 100 (range)) (range 100)))
  (is (= (range 0) ()))
  (is (= (range 1) '(0)))
  (is (= (range 5) '(0 1 2 3 4)))
  (is (= (range -1) ()))
  (is (= (range -3) ()))
  (is (= (range 0 3) '(0 1 2)))
  (is (= (range 0 1) '(0)))
  (is (= (range 0 0) ()))
  (is (= (range 0 -3) ()))
  (is (= (range 3 6) '(3 4 5)))
  (is (= (range 3 4) '(3)))
  (is (= (range 3 3) ()))
  (is (= (range 3 1) ()))
  (is (= (range 3 0) ()))
  (is (= (range 3 -2) ()))
  (is (= (range -2 5) '(-2 -1 0 1 2 3 4)))
  (is (= (range -2 0) '(-2 -1)))
  (is (= (range -2 -1) '(-2)))
  (is (= (range -2 -2) ()))
  (is (= (range -2 -5) ()))
  (is (= (range 3 9 1) '(3 4 5 6 7 8)))
  (is (= (range 3 9 2) '(3 5 7)))
  (is (= (range 3 9 3) '(3 6)))
  (is (= (range 3 9 10) '(3)))
  (is (= (range 3 9 -1) ()))
  (is (= (range 10 9 -1) '(10)))
  (is (= (range 10 8 -1) '(10 9)))
  (is (= (range 10 7 -1) '(10 9 8)))
  (is (= (range 10 0 -2) '(10 8 6 4 2)))
  (is (= (reduce + (range 100)) 4950))
  (is (= (reduce + 0 (range 100)) 4950)))

;; --- iterate ---

(deftest clj-iterate
  (is (= (take 0 (iterate inc 0)) ()))
  (is (= (take 1 (iterate inc 0)) '(0)))
  (is (= (take 2 (iterate inc 0)) '(0 1)))
  (is (= (take 5 (iterate inc 0)) '(0 1 2 3 4)))
  (is (= '(:foo 42 :foo 42) (take 4 (iterate #(if (= % :foo) 42 :foo) :foo))))
  (is (= '(256 128 64 32 16 8 4 2 1 0) (take 10 (iterate #(quot % 2) 256))))
  (is (= 2 (first (next (next (iterate inc 0))))))
  (is (= [1 2 3] (into [] (take 3) (next (iterate inc 0))))))

;; --- cycle ---

(deftest clj-cycle
  (is (= (cycle []) ()))
  (is (= (take 3 (cycle [1])) '(1 1 1)))
  (is (= (take 5 (cycle [1 2 3])) '(1 2 3 1 2)))
  (is (= (take 3 (cycle [nil])) '(nil nil nil))))

;; --- reductions ---

(deftest clj-reductions
  (is (= (reductions + [1 2 3 4 5]) '(1 3 6 10 15)))
  (is (= (reductions + 10 [1 2 3 4 5]) '(10 11 13 16 20 25))))

;; --- every? ---

(deftest clj-every?
  (is (= (every? pos? nil) true))
  (is (= (every? pos? []) true))
  (is (= (every? pos? {}) true))
  (is (= (every? pos? #{}) true))
  (is (= true (every? pos? [1])))
  (is (= true (every? pos? [1 2])))
  (is (= true (every? pos? [1 2 3 4 5])))
  (is (= false (every? pos? [-1])))
  (is (= false (every? pos? [-1 -2])))
  (is (= false (every? pos? [-1 -2 3])))
  (is (= false (every? pos? [-1 2])))
  (is (= false (every? pos? [1 -2])))
  (is (= false (every? pos? [1 2 -3])))
  (is (= false (every? pos? [1 2 -3 4])))
  (is (= true (every? #{:a} [:a :a]))))

;; --- not-every? ---

(deftest clj-not-every?
  (is (= (not-every? pos? nil) false))
  (is (= (not-every? pos? []) false))
  (is (= false (not-every? pos? [1])))
  (is (= false (not-every? pos? [1 2])))
  (is (= false (not-every? pos? [1 2 3 4 5])))
  (is (= true (not-every? pos? [-1])))
  (is (= true (not-every? pos? [-1 -2])))
  (is (= true (not-every? pos? [-1 -2 3])))
  (is (= true (not-every? pos? [-1 2])))
  (is (= true (not-every? pos? [1 -2])))
  (is (= true (not-every? pos? [1 2 -3])))
  (is (= true (not-every? pos? [1 2 -3 4])))
  (is (= false (not-every? #{:a} [:a :a])))
  (is (= true (not-every? #{:a} [:a :b])))
  (is (= true (not-every? #{:a} [:b :b]))))

;; --- not-any? ---

(deftest clj-not-any?
  (is (= (not-any? pos? nil) true))
  (is (= (not-any? pos? []) true))
  (is (= false (not-any? pos? [1])))
  (is (= false (not-any? pos? [1 2])))
  (is (= false (not-any? pos? [1 2 3 4 5])))
  (is (= true (not-any? pos? [-1])))
  (is (= true (not-any? pos? [-1 -2])))
  (is (= false (not-any? pos? [-1 -2 3])))
  (is (= false (not-any? pos? [-1 2])))
  (is (= false (not-any? pos? [1 -2])))
  (is (= false (not-any? pos? [1 2 -3])))
  (is (= false (not-any? pos? [1 2 -3 4])))
  (is (= false (not-any? #{:a} [:a :a])))
  (is (= false (not-any? #{:a} [:a :b])))
  (is (= true (not-any? #{:a} [:b :b]))))

;; --- some ---

(deftest clj-some
  (is (= (some pos? nil) nil))
  (is (= (some pos? []) nil))
  (is (= (some pos? #{}) nil))
  (is (= (some nil nil) nil))
  (is (= true (some pos? [1])))
  (is (= true (some pos? [1 2])))
  (is (= nil (some pos? [-1])))
  (is (= nil (some pos? [-1 -2])))
  (is (= true (some pos? [-1 2])))
  (is (= true (some pos? [1 -2])))
  (is (= :a (some #{:a} [:a :a])))
  (is (= :a (some #{:a} [:b :a])))
  (is (= nil (some #{:a} [:b :b])))
  (is (= :a (some #{:a} '(:a :b))))
  (is (= :a (some #{:a} #{:a :b}))))

;; --- empty? ---

(deftest clj-empty?
  (is (empty? nil))
  (is (empty? []))
  (is (empty? {}))
  (is (empty? #{}))
  (is (not (empty? '(1 2))))
  (is (not (empty? [1 2])))
  (is (not (empty? {:a 1 :b 2})))
  (is (not (empty? #{1 2}))))

;; --- not-empty ---

(deftest clj-not-empty
  (is (= (not-empty []) nil))
  (is (= (not-empty {}) nil))
  (is (= (not-empty #{}) nil))
  (is (= (not-empty '(1 2)) '(1 2)))
  (is (= (not-empty [1 2]) [1 2]))
  (is (= (not-empty {:a 1}) {:a 1}))
  (is (= (not-empty #{1 2}) #{1 2})))

;; --- map equality ---

(deftest clj-map-equality
  (is (= (map inc []) ()))
  (is (= (map inc #{}) ()))
  (is (= (map inc {}) ()))
  (is (= (sequence (map inc) (range 10)) (range 1 11)))
  (is (= (range 1 11) (sequence (map inc) (range 10)))))

;; --- into via concat ---

(deftest clj-into-concat
  (is (= [1 2 "a" "b"] (into [] (concat [1 2] "ab")))))
