(require "tests/test")

;; Reader conditional tests

(deftest reader-cond-mino-branch
  (is (= 1 #?(:mino 1 :clj 2))))

(deftest reader-cond-clj-matches
  ;; Mino matches both :mino and :clj as active reader-conditional
  ;; dialects, mirroring how Babashka treats :clj as the active feature.
  (is (= 42 #?(:clj 42 :default 99))))

(deftest reader-cond-default-when-no-active-key
  (is (= 99 #?(:cljs 42 :default 99))))

(deftest reader-cond-skipped-when-no-match
  (is (= 7 #?(:cljs 100 :default 7))))

(deftest reader-cond-mino-over-default
  (is (= :mino #?(:default :fallback :mino :mino))))

(deftest reader-cond-in-vector
  (is (= [1 2 3] [1 #?(:mino 2 :clj 99) 3])))

(deftest reader-cond-in-list
  (is (= 6 (+ #?(:mino 1 :clj 0) 2 3))))

(deftest reader-cond-splice-in-vector
  (is (= [1 2 3 4] [1 #?@(:mino [2 3] :clj [99]) 4])))

(deftest reader-cond-splice-in-list
  (is (= 10 (+ 1 #?@(:mino (2 3) :clj (0 0)) 4))))

(deftest reader-cond-splice-clj-matches
  (is (= [1 2 3 4] [1 #?@(:clj [2 3]) 4])))

(deftest reader-cond-splice-skipped-when-no-match
  (is (= [1 4] [1 #?@(:cljs [2 3]) 4])))

(deftest reader-cond-nested
  (is (= :yes #?(:mino #?(:mino :yes :default :no) :default :nope))))
