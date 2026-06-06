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

(deftest recur-arity-must-match-loop-bindings
  (is (thrown? (eval '(loop [x 1] (if (= x 1) (recur 1 2) x)))))
  (is (thrown? (eval '(loop [x 1 y 2] (if (= x 1) (recur 9) x)))))
  (is (= 3 (loop [x 1 y 2] (if (= x 1) (recur 9 3) y)))))

(deftest recur-across-try-rejected
  (is (thrown? (eval '(loop [x 1] (try (recur 2) (catch e nil))))))
  (is (thrown? (eval '(loop [x 1]
                        (try (throw (ex-info "t" {}))
                             (catch e (recur 2)))))))
  ;; recur in a fn inside try is fine: the fn is its own recur target
  (is (= 0 (try ((fn f [n] (if (pos? n) (recur (dec n)) n)) 3)
                (catch e :wrong)))))

(deftest loop-recur-with-destructured-bindings
  (is (= [1 2 3] (loop [[x & more] [1 2 3]
                        acc []]
                   (if x (recur more (conj acc x)) acc))))
  (is (= [false #{1 3}]
         (loop [[[f _] & rest-pairs] [[1 2] [3 4]]
                first? true
                seen #{}]
           (if f (recur rest-pairs false (conj seen f)) [first? seen])))))
