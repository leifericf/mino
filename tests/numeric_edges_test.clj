(require "tests/test")

;; Cycle 5 numeric / bigdec edges:
;;   - hash-combine matches the Boost-style 32-bit mix.
;;   - unchecked-long takes the low 64 bits of a bigint outside long range.
;;   - *math-context* + with-precision wires HALF_UP bigdec rounding.

(deftest hash-combine-shape
  ;; Deterministic mix.
  (is (= (hash-combine 0 (hash :a))
         (hash-combine 0 (hash :a))))
  ;; Result is a 32-bit signed int (in [-2^31, 2^31 - 1]).
  (let [h (hash-combine (hash :a) (hash :b))]
    (is (>= h -2147483648))
    (is (<= h  2147483647)))
  ;; The mixer is order-sensitive — swapping seed and hash arguments
  ;; gives a different result for non-symmetric inputs.
  (is (not= (hash-combine 1 2) (hash-combine 2 1))))

(deftest hash-combine-nils
  ;; nil seed / nil hash both treat as 0; this matches Clojure's
  ;; tolerance for hash-of-nil. The result must still be deterministic.
  (is (= (hash-combine nil nil) (hash-combine 0 0)))
  (is (= (hash-combine nil (hash :a)) (hash-combine 0 (hash :a)))))

(deftest unchecked-long-wraps
  ;; -(Long/MIN_VALUE) - 1 is just outside the signed long range. JVM
  ;; canon: wraps modulo 2^64 to Long/MAX_VALUE.
  (is (= 9223372036854775807 (unchecked-long -9223372036854775809N)))
  ;; +(Long/MAX_VALUE + 1) is also just outside. Wraps to Long/MIN_VALUE.
  (is (= -9223372036854775808 (unchecked-long 9223372036854775808N)))
  ;; 2^64 wraps to 0.
  (is (= 0 (unchecked-long 18446744073709551616N)))
  ;; 2^64 + 1 wraps to 1.
  (is (= 1 (unchecked-long 18446744073709551617N))))

(deftest unchecked-long-passthrough
  ;; Values within signed long range pass through unchanged.
  (is (= 42 (unchecked-long 42N)))
  (is (= -42 (unchecked-long -42N)))
  (is (= 9223372036854775807 (unchecked-long 9223372036854775807N)))
  (is (= -9223372036854775808 (unchecked-long -9223372036854775808N))))

(deftest with-precision-basic
  (is (= 0.33333M (with-precision 5 (/ 1M 3M))))
  (is (= 0.667M (with-precision 3 (/ 2M 3M))))
  (is (= 0.76M (with-precision 2 (/ 25M 33M)))))

(deftest with-precision-22-over-7
  (is (= 3.14M       (with-precision 3 (/ 22M 7M))))
  (is (= 3.1429M     (with-precision 5 (/ 22M 7M))))
  (is (= 3.142857143M (with-precision 10 (/ 22M 7M)))))

(deftest with-precision-explicit-half-up
  (is (= 0.33333M (with-precision 5 :rounding :half-up (/ 1M 3M)))))

(deftest with-precision-unsupported-mode
  ;; Other rounding modes throw :mino/unsupported with the host error
  ;; class. The error message names the mode so callers can decide
  ;; whether to retry under :half-up or fail loudly.
  (is (thrown? (with-precision 5 :rounding :half-down (/ 1M 3M))))
  (is (thrown? (with-precision 5 :rounding :half-even (/ 1M 3M)))))

(deftest no-math-context-still-exact-or-throws
  ;; Outside with-precision, the historical exact-or-throw behavior is
  ;; preserved. (/ 1M 3M) is non-terminating → throws.
  (is (thrown? (/ 1M 3M)))
  ;; Exact divisions are unchanged.
  (is (= 3M (/ 6M 2M))))

(deftest with-precision-negative
  (is (= -0.333M (with-precision 3 (/ -1M 3M))))
  (is (= -0.67M  (with-precision 2 (/ -2M 3M)))))
