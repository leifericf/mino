(require "tests/test")

(require '[clojure.template :refer [apply-template do-template]])
(require '[clojure.instant])

;; --- clojure.template ---

(deftest template-apply-template-basic
  (is (= '(+ 2 2)    (apply-template '[x] '(+ x x) [2])))
  (is (= '(- 5 3)    (apply-template '[a b] '(- a b) [5 3])))
  (is (= '(vec 1 2 3) (apply-template '[a b c] '(vec a b c) [1 2 3]))))

(deftest template-apply-template-validates
  (is (thrown? (apply-template "not-a-vector" '(+ x x) [1])))
  (is (thrown? (apply-template '[1 2] '(+ x x) [1 2]))))

(deftest template-do-template-runs
  (is (= 8 (do-template [x y] (+ y x) 2 4 3 5)))   ; last form (+ 5 3) = 8
  (is (= 6 (do-template [x] (* x 2) 1 2 3))))      ; last form (* 3 2)

(deftest template-do-template-expands
  (is (= '(do (+ 4 2) (+ 5 3))
         (macroexpand '(do-template [x y] (+ y x) 2 4 3 5)))))

;; --- clojure.instant ---

(deftest instant-year-only
  (let [m (clojure.instant/read-instant-date "2026")]
    (is (= 2026 (:years m)))
    (is (= 1 (:months m)))
    (is (= 1 (:days m)))
    (is (= 0 (:hours m)))))

(deftest instant-full-date
  (let [m (clojure.instant/read-instant-date "2026-04-27")]
    (is (= 2026 (:years m)))
    (is (= 4 (:months m)))
    (is (= 27 (:days m)))))

(deftest instant-with-time
  (let [m (clojure.instant/read-instant-date "2026-04-27T10:30")]
    (is (= 10 (:hours m)))
    (is (= 30 (:minutes m)))
    (is (= 0 (:seconds m)))))

(deftest instant-with-seconds
  (let [m (clojure.instant/read-instant-date "2026-04-27T10:30:45")]
    (is (= 45 (:seconds m)))
    (is (= 0 (:nanoseconds m)))))

(deftest instant-with-fractional-seconds
  (let [m (clojure.instant/read-instant-date "2026-04-27T10:30:45.123Z")]
    (is (= 123000000 (:nanoseconds m)))
    (is (= 1 (:offset-sign m))))
  (let [m (clojure.instant/read-instant-date "2026-04-27T10:30:45.123456789Z")]
    (is (= 123456789 (:nanoseconds m)))))

(deftest instant-with-positive-offset
  (let [m (clojure.instant/read-instant-date "2026-04-27T10:30:45+02:30")]
    (is (= 1 (:offset-sign m)))
    (is (= 2 (:offset-hours m)))
    (is (= 30 (:offset-minutes m)))))

(deftest instant-with-negative-offset
  (let [m (clojure.instant/read-instant-date "2026-04-27T10:30:45-08:00")]
    (is (= -1 (:offset-sign m)))
    (is (= 8 (:offset-hours m)))
    (is (= 0 (:offset-minutes m)))))

(deftest instant-rejects-out-of-range
  (is (thrown? (clojure.instant/read-instant-date "2026-13-01")))   ; bad month
  (is (thrown? (clojure.instant/read-instant-date "2026-12-32")))   ; bad day
  (is (thrown? (clojure.instant/read-instant-date "2026-01-01T25:00")))) ; bad hour

(deftest instant-rejects-malformed
  (is (thrown? (clojure.instant/read-instant-date "abcd")))
  (is (thrown? (clojure.instant/read-instant-date "2026-1")))      ; truncated month
  (is (thrown? (clojure.instant/read-instant-date "2026-01-01T10:30:45.")))) ; empty fractional
