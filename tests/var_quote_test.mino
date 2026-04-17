(require "tests/test")

;; var-quote reader macro and var special form tests

(def my-var 42)

(deftest var-returns-var-object
  (is (= :var (type (var my-var)))))

(deftest var-quote-returns-var-object
  (is (= :var (type #'my-var))))

(deftest var-same-identity
  (is (= (var my-var) #'my-var)))

(deftest var-on-function
  (is (= :var (type #'inc))))

(deftest var-quote-round-trip
  (is (= #'map #'map)))
