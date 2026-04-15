(require "tests/test")

;; Control flow: if, do, when, cond, and, or, threading.

(deftest if-form
  (is (= 1 (if true 1 2)))
  (is (= 2 (if false 1 2)))
  (is (= 2 (if nil 1 2)))
  (testing "truthiness"
    (is (= "t" (if 0 "t" "f")))
    (is (= "t" (if "" "t" "f"))))
  (testing "no else"
    (is (= nil (if false 1)))))

(deftest do-form
  (is (= 3 (do 1 2 3)))
  (is (= 7 (do (def d__ct 7) d__ct))))

(deftest when-macro
  (is (= 3 (when true 1 2 3)))
  (is (= nil (when false :no))))

(deftest cond-macro
  (is (= :b (cond false :a true :b)))
  (is (= nil (cond false :a false :b))))

(deftest and-macro
  (is (= 3 (and 1 2 3)))
  (is (= false (and 1 false 3))))

(deftest or-macro
  (is (= 42 (or nil nil 42)))
  (is (= false (or false false false))))

(deftest threading
  (is (= 5 (-> 10 (- 3) (- 2))))
  (is (= 9 (->> 10 (- 3) (- 2)))))
