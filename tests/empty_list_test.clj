;; Empty-list canon parity probes. The canonical empty list `()` is a
;; distinct value from nil: seq?-true, nil?-false, equal to other empty
;; sequential collections, but not equal to nil.

(deftest empty-list-equality-self
  (is (= '() ()))
  (is (= (list) ()))
  (is (= '() (list))))

(deftest empty-list-cross-type-equality
  (is (= '() []))
  (is (= '() (take 0 [1 2 3])))
  (is (= '() (filter (fn [_] false) [1 2 3])))
  (is (= '() (map inc nil))))

(deftest empty-list-not-equal-nil
  (is (not= '() nil))
  (is (not= nil ()))
  (is (not= [] nil))
  (is (not= nil [])))

(deftest empty-list-predicates
  (is (seq? '()))
  (is (not (nil? '())))
  (is (empty? '()))
  (is (= 0 (count '())))
  (is (= :list (type '()))))

(deftest empty-list-accessors
  (is (= nil (first '())))
  (is (= nil (next '())))
  (is (= '() (rest '())))
  (is (= '() (rest '(1)))))

(deftest empty-list-print-form
  (is (= "()" (pr-str '())))
  (is (= "()" (pr-str (list))))
  (is (= "()" (pr-str (rest '(1))))))

(deftest empty-list-cons-yields-singleton-list
  (is (= '(1) (cons 1 '())))
  (is (= '(1) (cons 1 (list))))
  (is (seq? (cons 1 '()))))
