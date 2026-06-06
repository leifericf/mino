(require "tests/test")

;; Math functions: wrappers around math.h

(deftest math-floor-fn
  (is (= 3.0 (math-floor 3.7)))
  (is (= 3.0 (math-floor 3.0)))
  (is (= -4.0 (math-floor -3.2))))

(deftest math-ceil-fn
  (is (= 4.0 (math-ceil 3.2)))
  (is (= 3.0 (math-ceil 3.0)))
  (is (= -3.0 (math-ceil -3.2))))

(deftest math-round-fn
  (is (= 4.0 (math-round 3.5)))
  (is (= 3.0 (math-round 3.4)))
  (is (= -4.0 (math-round -3.5))))

(deftest math-sqrt-fn
  (is (= 4.0 (math-sqrt 16)))
  (is (= 0.0 (math-sqrt 0)))
  (is (= 1.0 (math-sqrt 1))))

(deftest math-pow-fn
  (is (= 1024.0 (math-pow 2 10)))
  (is (= 1.0 (math-pow 5 0)))
  (is (= 8.0 (math-pow 2 3))))

(deftest math-log-exp
  (is (= 0.0 (math-log 1)))
  (is (= 1.0 (math-exp 0)))
  ;; log and exp are inverses
  (is (< (abs (- 5.0 (math-exp (math-log 5)))) 0.0001)))

(deftest math-trig
  (is (= 0.0 (math-sin 0)))
  (is (= 1.0 (math-cos 0)))
  (is (= 0.0 (math-tan 0)))
  ;; sin(pi/2) ~ 1
  (is (< (abs (- 1.0 (math-sin (/ math-pi 2)))) 0.0001)))

(deftest math-atan2-fn
  ;; atan2(1, 1) = pi/4
  (is (< (abs (- (/ math-pi 4) (math-atan2 1 1))) 0.0001))
  ;; atan2(0, 1) = 0
  (is (= 0.0 (math-atan2 0 1))))

(deftest rand-fn
  (testing "returns a number"
    (is (number? (rand))))
  (testing "in range [0, 1)"
    (let [r (rand)]
      (is (>= r 0.0))
      (is (< r 1.0))))
  (testing "produces different values"
    (let [values (map (fn [_] (rand)) (range 10))]
      (is (> (count (distinct values)) 1)))))

(deftest time-ms-fn
  (is (number? (time-ms)))
  (is (> (time-ms) 0))
  (testing "elapsed time is positive"
    (let [start (time-ms)]
      (loop [i 0] (if (< i 100000) (recur (+ i 1)) nil))
      (is (>= (- (time-ms) start) 0))))
  (testing "measures wall-clock, not CPU time"
    ;; Regression: time-ms used to be clock()/CLOCKS_PER_SEC (process
    ;; CPU time). (time (thread-sleep 200)) reported "0.194 ms" because
    ;; the sleeping thread spent no CPU. Now backed by monotonic
    ;; wall-clock (nano-time / 1e6) so it reports >= the sleep duration.
    (let [start (time-ms)
          _     (thread-sleep 50)
          elapsed (- (time-ms) start)]
      (is (>= elapsed 40))
      (is (<= elapsed 5000)))))

(deftest math-pi-constant
  (is (> math-pi 3.14159))
  (is (< math-pi 3.14160)))

;; --- clojure.math: integer division, rounding, exact ops, double bits ---

(require '[clojure.math :as cmath])

(deftest clojure-math-floor-div-mod
  (is (= -4 (cmath/floor-div 7 -2)))
  (is (= -1 (cmath/floor-mod 7 -2)))
  (is (= 3 (cmath/floor-div 7 2)))
  (is (= 1 (cmath/floor-mod 7 2)))
  (is (= -4 (cmath/floor-div -7 2)))
  (is (= 1 (cmath/floor-mod -7 2)))
  (is (thrown? (cmath/floor-div 1 0)))
  (is (thrown? (cmath/floor-div 1.5 2))))

(deftest clojure-math-rint
  ;; Half-even rounding, result is a double.
  (is (= 2.0 (cmath/rint 2.5)))
  (is (= 4.0 (cmath/rint 3.5)))
  (is (= -2.0 (cmath/rint -2.5)))
  (is (= 2.0 (cmath/rint 1.7))))

(deftest clojure-math-exact-ops
  (is (= 5 (cmath/add-exact 2 3)))
  (is (thrown? (cmath/add-exact 9223372036854775807 1)))
  (is (= -1 (cmath/subtract-exact 2 3)))
  (is (thrown? (cmath/subtract-exact -9223372036854775808 1)))
  (is (= 6 (cmath/multiply-exact 2 3)))
  (is (thrown? (cmath/multiply-exact 9223372036854775807 2)))
  (is (= 3 (cmath/increment-exact 2)))
  (is (thrown? (cmath/increment-exact 9223372036854775807)))
  (is (= 1 (cmath/decrement-exact 2)))
  (is (thrown? (cmath/decrement-exact -9223372036854775808)))
  (is (= -2 (cmath/negate-exact 2)))
  (is (thrown? (cmath/negate-exact -9223372036854775808)))
  ;; Longs only, like the canonical signatures.
  (is (thrown? (cmath/add-exact 1.5 1))))

(deftest clojure-math-double-bits
  (is (= 2.220446049250313E-16 (cmath/ulp 1.0)))
  (is (= 4.9E-324 (cmath/ulp 0.0)))
  (is (= ##Inf (cmath/ulp ##Inf)))
  (is (NaN? (cmath/ulp ##NaN)))
  (is (= 8.0 (cmath/scalb 1.0 3)))
  (is (= 0.125 (cmath/scalb 1.0 -3)))
  (is (= 0 (cmath/get-exponent 1.5)))
  (is (= 1 (cmath/get-exponent 2.0)))
  (is (= -1023 (cmath/get-exponent 0.0)))
  (is (= 1024 (cmath/get-exponent ##Inf)))
  (is (= 1024 (cmath/get-exponent ##NaN)))
  (is (= 1.0 (cmath/next-after 1.0 1.0)))
  (is (< 1.0 (cmath/next-after 1.0 2.0)))
  (is (> 1.0 (cmath/next-after 1.0 0.0))))
