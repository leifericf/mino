(require "tests/test")

;; Arithmetic operations, comparisons, bitwise, and numeric utilities.

(deftest self-eval
  (is (= 42 42))
  (is (= "hi" "hi"))
  (is (= nil nil))
  (is (= true true))
  (is (= false false)))

(deftest addition
  (is (= 3 (+ 1 2)))
  (is (= 3.5 (+ 1 2.5))))

(deftest subtraction
  (is (= 5 (- 10 3 2)))
  (is (= -7 (- 7))))

(deftest multiplication
  (is (= 24 (* 2 3 4))))

(deftest division
  ;; `/` on integers returns a ratio (exact) — use float coercion for a double.
  (is (= 5/2 (/ 10 4)))
  (is (= 2.5 (double (/ 10 4)))))

(deftest comparisons
  (testing "less than"
    (is (< 1 2 3))
    (is (not (< 3 2 1))))
  (testing "equal"
    ;; = is type-strict on numeric tier (Clojure dialect): int and float
    ;; aren't `=` even when numerically equal. Use `==` for cross-tier
    ;; numeric equality.
    (is (not (= 1 1.0)))
    (is (== 1 1.0)))
  (testing "less or equal"
    (is (<= 1 2 2 3)))
  (testing "greater than"
    (is (> 3 2 1))
    (is (not (> 1 2))))
  (testing "greater or equal"
    (is (>= 3 3 1))))

(deftest mod-rem-quot
  (testing "mod"
    (is (= 1 (mod 10 3)))
    (is (= 2 (mod -10 3)))
    (is (= 1.5 (mod 5.5 2.0))))
  (testing "rem"
    (is (= 1 (rem 10 3)))
    (is (= -1 (rem -10 3))))
  (testing "quot"
    (is (= 3 (quot 10 3)))
    (is (= -3 (quot -10 3)))))

(deftest bitwise
  (is (= 8 (bit-and 12 10)))
  (is (= 14 (bit-or 12 10)))
  (is (= 6 (bit-xor 12 10)))
  (is (= -1 (bit-not 0)))
  (is (= 16 (bit-shift-left 1 4)))
  (is (= 4 (bit-shift-right 16 2))))

(deftest bit-shift-boundary
  ;; In-range shifts at both ends of [0, 63] are defined and produce the
  ;; usual two's-complement results.
  (is (= 1 (bit-shift-left 1 0)))
  (is (= -9223372036854775808 (bit-shift-left 1 63)))
  (is (= 1 (bit-shift-right 1 0)))
  (is (= 0 (bit-shift-right 1 63)))
  (is (= 9223372036854775807 (unsigned-bit-shift-right -1 1)))
  (is (= 1 (unsigned-bit-shift-right -1 63)))
  ;; Out-of-range shift amounts (negative or >= 64) must throw; C99 leaves
  ;; such shifts undefined, so surface them as a bounds error rather than
  ;; letting the underlying shift instruction silently mask the count.
  (is (thrown? (bit-shift-left 1 64)))
  (is (thrown? (bit-shift-left 1 -1)))
  (is (thrown? (bit-shift-right 1 64)))
  (is (thrown? (bit-shift-right 1 -1)))
  (is (thrown? (unsigned-bit-shift-right 1 64)))
  (is (thrown? (unsigned-bit-shift-right 1 -1))))

(deftest trivial-compositions
  (is (= 2 (second [1 2 3])))
  (is (= 1 (ffirst '((1 2) (3 4)))))
  (is (= 6 (inc 5)))
  (is (= 4 (dec 5)))
  (is (zero? 0))
  (is (pos? 5))
  (is (neg? -1))
  (is (even? 4))
  (is (odd? 3))
  (is (= 5 (abs -5)))
  (is (= 7 (max 3 7)))
  (is (= 3 (min 3 7))))

(deftest numeric-coercion
  (is (= 3 (int 3.7)))
  (is (= 5 (int 5)))
  (is (= 5.0 (float 5)))
  (is (= 3.14 (float 3.14))))

(deftest integer-overflow-promotes
  ;; Plain +/-/*/inc/dec auto-promote to bigint on long overflow rather
  ;; than wrap silently or throw. The unchecked-* family is the named
  ;; opt-in for two's-complement-wraparound int64 semantics.
  (let [max-int 9223372036854775807
        min-int -9223372036854775808]
    (is (= 9223372036854775808N (+ max-int 1)))
    (is (= 9223372036854775808N (+ 1 max-int)))
    (is (= -9223372036854775809N (+ min-int -1)))
    (is (= 9223372036854775810N (+ 1 2 max-int)))
    (is (= -9223372036854775809N (- min-int 1)))
    (is (= 9223372036854775808N (- max-int -1)))
    (is (= 9223372036854775808N (- min-int)))
    (is (= 18446744073709551614N (* max-int 2)))
    (is (= 9223372036854775808N (* -1 min-int)))
    (is (= 9223372036854775808N (* min-int -1)))
    (is (= 9223372036854775808N (inc max-int)))
    (is (= -9223372036854775809N (dec min-int)))
    ;; Each promoted result is a bigint.
    (is (= :bigint (type (+ max-int 1))))
    (is (= :bigint (type (* max-int 2))))
    ;; In-range arithmetic still works and stays in long.
    (is (= max-int (+ (dec max-int) 1)))
    (is (= (- max-int 1) (+ max-int -1)))
    (is (= -1 (+ max-int min-int)))
    (is (= max-int (* max-int 1)))
    (is (= :int (type (+ 1 2))))
    ;; Float arithmetic is not range-checked; IEEE 754 handles it.
    (is (float? (+ max-int 1.0)))
    ;; unchecked-* wraps without promoting.
    (is (= min-int (unchecked-add max-int 1)))
    (is (= max-int (unchecked-subtract min-int 1)))
    (is (= min-int (unchecked-inc max-int)))
    (is (= max-int (unchecked-dec min-int)))
    (is (= min-int (unchecked-multiply 2 4611686018427387904)))))

(deftest scientific-notation-signed-exponents
  (is (float? 1e10))
  (is (float? 1e-10))
  (is (float? 1e+10))
  (is (float? 1.5e-3))
  (is (float? -2.5e+4))
  (is (float? 3E-2))
  (is (= 1e10 1e+10))
  (is (< 1e-10 1))
  (is (< 1.5e-3 1))
  (is (< -2.5e+4 0)))
