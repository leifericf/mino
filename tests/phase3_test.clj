;; Phase 3 tests: reduced, multi-collection map, format precision,
;; set constructor, declare, variadic comp.

;; --- reduced ---

(deftest reduced-early-termination
  (testing "reduce stops at reduced"
    (is (= 6 (reduce (fn [acc x] (if (> x 3) (reduced acc) (+ acc x))) 0 (range 10)))))
  (testing "reduced on first element"
    (is (= 0 (reduce (fn [acc x] (reduced acc)) 0 [1 2 3]))))
  (testing "reduce without reduced works normally"
    (is (= 6 (reduce + 0 [1 2 3]))))
  (testing "reduced? predicate"
    (is (reduced? (reduced 42)))
    (is (not (reduced? 42)))
    (is (not (reduced? nil)))))

;; --- multi-collection map ---

(deftest multi-collection-map
  (testing "single collection unchanged"
    (is (= [2 3 4] (into [] (map inc [1 2 3])))))
  (testing "two collections"
    (is (= [11 22 33] (into [] (map + [1 2 3] [10 20 30])))))
  (testing "three collections"
    (is (= [111 222 333] (into [] (map + [1 2 3] [10 20 30] [100 200 300])))))
  (testing "stops at shortest"
    (is (= [11 22] (into [] (map + [1 2] [10 20 30])))))
  (testing "with vector constructor"
    (is (= [[1 :a] [2 :b]] (into [] (map vector [1 2] [:a :b]))))))

;; --- format precision ---

(deftest format-precision
  (testing "basic specifiers still work"
    (is (= "42" (format "%d" 42)))
    (is (= "hello" (format "%s" "hello")))
    (is (= "%" (format "%%"))))
  (testing "float precision"
    (is (= "3.14" (format "%.2f" 3.14159)))
    (is (= "3.1" (format "%.1f" 3.14))))
  (testing "integer width"
    (is (= "0042" (format "%04d" 42))))
  (testing "float width and precision"
    (is (= "  3.14" (format "%6.2f" 3.14))))
  (testing "hex and octal"
    (is (= "ff" (format "%x" 255)))
    (is (= "10" (format "%o" 8))))
  (testing "scientific notation"
    (is (= "1.000000e+03" (format "%e" 1000.0))))
  (testing "sign flag"
    (is (= "+42" (format "%+d" 42)))))

;; --- set constructor ---

(deftest set-constructor
  (testing "set from vector"
    (is (= #{1 2 3} (set [1 2 3]))))
  (testing "set deduplicates"
    (is (= #{1 2 3} (set [1 2 3 2 1]))))
  (testing "set from list"
    (is (= #{1 2 3} (set '(1 2 3)))))
  (testing "set from nil"
    (is (= #{} (set nil))))
  (testing "set from set"
    (is (= #{1 2} (set #{1 2})))))

;; --- declare ---

(declare my-even?_ my-odd?_)
(defn my-even?_ [n] (if (= n 0) true (my-odd?_ (- n 1))))
(defn my-odd?_ [n] (if (= n 0) false (my-even?_ (- n 1))))

(deftest declare-forward-ref
  (testing "mutual recursion via declare"
    (is (my-even?_ 10))
    (is (my-odd?_ 7))
    (is (not (my-even?_ 3)))
    (is (not (my-odd?_ 4)))))

;; --- variadic comp ---

(deftest variadic-comp
  (testing "zero-arity returns identity"
    (is (= 42 ((comp) 42))))
  (testing "single-arity returns the function"
    (is (= 2 ((comp inc) 1))))
  (testing "two-arity composes"
    (is (= 3 ((comp inc inc) 1))))
  (testing "three-arity composes right-to-left"
    (is (= "3" ((comp str inc inc) 1))))
  (testing "multi-step chain"
    (is (= "11" ((comp str inc (fn [x] (* x 2))) 5)))))
