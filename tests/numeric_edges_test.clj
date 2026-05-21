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

(deftest long-stays-fixnum-across-tag-boundary
  ;; long must always produce a fixnum (:int), even when the value
  ;; falls outside the inline-tagged 61-bit range. Promoting to bigint
  ;; here would silently disable checked-arithmetic overflow on the
  ;; result.
  (is (= :int (type (long 5))))
  (is (= :int (type (long 4611686018427387904))))
  (is (= :int (type (long -4611686018427387904))))
  (is (= :int (type (long 9223372036854775807))))
  (is (= :int (type (long -9223372036854775808))))
  ;; Bigint-shaped input that fits the long range coerces back to :int.
  (is (= :int (type (long 4611686018427387904N))))
  ;; Once coerced, downstream checked arithmetic must observe overflow
  ;; rather than auto-promote.
  (is (thrown? (* (long (/ -9223372036854775808 2)) 3)))
  (is (thrown? (* 3 (long (/ -9223372036854775808 2))))))

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

(deftest with-precision-rounding-down
  ;; Truncate toward zero.
  (is (= 0.33333M (with-precision 5 :rounding :down (/ 1M 3M))))
  ;; 1/6 = 0.16666... truncates to 0.1666 at precision 4.
  (is (= 0.1666M  (with-precision 4 :rounding :down (/ 1M 6M))))
  ;; Negative: also truncates toward zero (magnitude decreases).
  (is (= -0.33333M (with-precision 5 :rounding :down (/ -1M 3M)))))

(deftest with-precision-rounding-up
  ;; Always round away from zero when there is any non-zero excess.
  ;; 1/3 = 0.33333... → 0.34 at precision 2, since there's residual.
  (is (= 0.34M     (with-precision 2 :rounding :up (/ 1M 3M))))
  (is (= 0.334M    (with-precision 3 :rounding :up (/ 1M 3M))))
  ;; Negative: magnitude rounds up.
  (is (= -0.34M    (with-precision 2 :rounding :up (/ -1M 3M)))))

(deftest with-precision-rounding-floor
  ;; Toward -infinity: positives truncate, negatives round magnitude up.
  (is (= 0.33333M  (with-precision 5 :rounding :floor (/ 1M 3M))))
  (is (= -0.34M    (with-precision 2 :rounding :floor (/ -1M 3M)))))

(deftest with-precision-rounding-ceiling
  ;; Toward +infinity: positives round magnitude up, negatives truncate.
  (is (= 0.34M     (with-precision 2 :rounding :ceiling (/ 1M 3M))))
  (is (= -0.33333M (with-precision 5 :rounding :ceiling (/ -1M 3M)))))

(deftest with-precision-rounding-half-down
  ;; Ties (exactly half) go toward zero; non-ties round to nearest.
  ;; 0.25 at precision 1: candidates 0.2 / 0.3, half-point 0.25 (tie)
  ;; → 0.2 under half-down. Under half-up it would be 0.3.
  (is (= 0.2M      (with-precision 1 :rounding :half-down (/ 25M 100M))))
  (is (= 0.3M      (with-precision 1 :rounding :half-up   (/ 25M 100M))))
  ;; Strictly more than half rounds up under both half-modes.
  (is (= 0.3M      (with-precision 1 :rounding :half-down (/ 26M 100M)))))

(deftest with-precision-rounding-half-even
  ;; Banker's rounding. 0.125 at precision 2 has a tie (0.12 vs 0.13);
  ;; tie goes to even → 0.12.
  (is (= 0.12M     (with-precision 2 :rounding :half-even (/ 125M 1000M))))
  ;; 0.135 at precision 2 → tie between 0.13 and 0.14; even → 0.14.
  (is (= 0.14M     (with-precision 2 :rounding :half-even (/ 135M 1000M))))
  ;; Non-tie cases still round to nearest.
  (is (= 0.13M     (with-precision 2 :rounding :half-even (/ 126M 1000M)))))

(deftest with-precision-rounding-unnecessary
  ;; Exact division: no rounding needed → returns exact value.
  (is (= 0.5M (with-precision 5 :rounding :unnecessary (/ 1M 2M))))
  ;; Non-exact division: throws because rounding would change the value.
  (is (thrown? (with-precision 5 :rounding :unnecessary (/ 1M 3M))))
  (is (thrown? (with-precision 3 :rounding :unnecessary (/ 22M 7M)))))

(deftest with-precision-unknown-rounding-mode
  ;; An unrecognised :rounding-mode keyword still throws (clearly
  ;; classified) so user typos surface immediately.
  (is (thrown? (with-precision 5 :rounding :bogus-mode (/ 1M 3M)))))

(deftest with-precision-jvm-symbol-modes
  ;; ClojureDocs / canonical JVM examples pass the rounding mode as a
  ;; bare java.math.RoundingMode enum symbol (UP, HALF_UP, ...). mino
  ;; accepts these in addition to its native keyword surface so canon
  ;; examples paste through unchanged. Verification goes through /
  ;; since bigdec division is the path that consumes *math-context*.
  (is (= 0.3M (with-precision 1 :rounding HALF_UP   (/ 25M 100M))))
  (is (= 0.2M (with-precision 1 :rounding HALF_DOWN (/ 25M 100M))))
  (is (= 0.2M (with-precision 1 :rounding DOWN      (/ 25M 100M))))
  (is (= 0.3M (with-precision 1 :rounding UP        (/ 25M 100M))))
  (is (= 0.2M (with-precision 1 :rounding FLOOR     (/ 25M 100M))))
  (is (= 0.3M (with-precision 1 :rounding CEILING   (/ 25M 100M))))
  (is (= 0.12M (with-precision 2 :rounding HALF_EVEN (/ 125M 1000M))))
  (is (thrown? (with-precision 5 :rounding UNNECESSARY (/ 1M 3M))))
  (is (= 0.5M  (with-precision 5 :rounding UNNECESSARY (/ 1M 2M))))
  ;; A symbol that isn't a RoundingMode enum constant is rejected at
  ;; macroexpansion, not deferred to a runtime "unbound symbol".
  (is (thrown? (eval '(with-precision 5 :rounding NOT_A_MODE (/ 1M 3M))))))

(deftest no-math-context-still-exact-or-throws
  ;; Outside with-precision, the historical exact-or-throw behavior is
  ;; preserved. (/ 1M 3M) is non-terminating → throws.
  (is (thrown? (/ 1M 3M)))
  ;; Exact divisions are unchanged.
  (is (= 3M (/ 6M 2M))))

(deftest with-precision-negative
  (is (= -0.333M (with-precision 3 (/ -1M 3M))))
  (is (= -0.67M  (with-precision 2 (/ -2M 3M)))))
