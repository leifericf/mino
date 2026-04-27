(require "tests/test")

;; Numeric tower:
;;   MINO_BIGINT type, `1N` literal reader, `bigint` / `biginteger` /
;;   `bigint?` constructors, cross-tier `=` and `hash` for int/bigint,
;;   readable printer via `print-method` default; MINO_RATIO and
;;   MINO_BIGDEC types, `1/2` / `1M` literal readers, full tower
;;   dispatch across the five tiers in `+/-/*/`, `<`/`<=`/`>`/`>=`, and
;;   a `==` numeric-equality primitive. Strict `=` (no cross-tier
;;   int/float equality) follows.
;;
;;   Plain `+` / `-` / `*` / `inc` / `dec` auto-promote on long
;;   overflow; the `unchecked-+/-/*\inc\dec` family is the named
;;   opt-in for two's-complement-wraparound int64 semantics.

;; --- reader: 1N literals produce real bigints ---

(deftest nt-literal-reader
  (is (= :bigint (type 0N)))
  (is (= :bigint (type 1N)))
  (is (= :bigint (type -1N)))
  (is (= :bigint (type 99999999999999999999999N)))
  (is (= :bigint (type -99999999999999999999999N))))

;; --- constructors ---

(deftest nt-constructors
  (is (bigint? (bigint 0)))
  (is (bigint? (bigint 42)))
  (is (bigint? (bigint -42)))
  (is (bigint? (bigint 1N)))
  (is (bigint? (bigint "12345")))
  (is (bigint? (bigint "99999999999999999999999")))
  ;; biginteger is an alias for bigint
  (is (= (bigint 7) (biginteger 7)))
  ;; float truncation
  (is (= 1N (bigint 1.7)))
  (is (= -1N (bigint -1.7))))

;; --- predicates ---

(deftest nt-predicates
  (is (bigint? 1N))
  (is (not (bigint? 1)))
  (is (not (bigint? 1.0)))
  (is (not (bigint? "1")))
  (is (not (bigint? nil))))

;; --- equality: cross-tier int/bigint ---

(deftest nt-equality-cross-tier
  (is (= 1 1N))
  (is (= 1N 1))
  (is (= 0 0N))
  (is (= -1 -1N))
  (is (not (= 1 2N)))
  (is (not (= 2N 1)))
  ;; distinct bigint values
  (is (= 1N (bigint 1)))
  (is (not (= 1N 2N)))
  ;; large bigints beyond long long
  (is (= 99999999999999999999999N
         (bigint "99999999999999999999999")))
  (is (not (= 99999999999999999999999N
              99999999999999999999998N))))

;; --- hash: int and bigint of the same value share a bucket ---

(deftest nt-hash-cross-tier
  ;; A set keyed by int contains the equivalent bigint.
  (is (contains? #{1} 1N))
  (is (contains? #{1N} 1))
  ;; A map keyed by int looks up under the equivalent bigint.
  (is (= :one (get {1 :one} 1N)))
  (is (= :one (get {1N :one} 1)))
  ;; Two distinct bigint instances with the same value hit the same bucket.
  (let [a 1N b (bigint 1)]
    (is (= a b))
    (is (contains? #{a} b))))

;; --- printer: readable round-trip via print-method default ---

(deftest nt-printer-roundtrip
  (doseq [x [0N 1N -1N 42N -42N
             99999999999999999999999N
             -99999999999999999999999N]]
    (is (= x (read-string (pr-str x))))))

;; --- bigint in collections round-trips cleanly ---

(deftest nt-collections-roundtrip
  (is (= [1N 2N 3N] (read-string (pr-str [1N 2N 3N]))))
  (is (= #{1N 2N 3N} (read-string (pr-str #{1N 2N 3N}))))
  (is (= {:a 1N :b 2N} (read-string (pr-str {:a 1N :b 2N})))))

;; --- self-evaluation ---

(deftest nt-self-eval
  ;; bigints evaluate to themselves, like ints and floats.
  (is (= 42N (eval '42N))))

;; --- type reporting ---

(deftest nt-type-reporting
  ;; type returns :bigint; type-name / type-tag reporting paths all agree.
  (is (= :bigint (type 1N)))
  (is (= :bigint (type (bigint "12345678901234567890")))))

;; --- plain arithmetic: tower-dispatched, with auto-promote on overflow ---

(deftest nt-arithmetic-plain-tower
  ;; Plain +/-/* tier-dispatch when given non-int operands. Mixed-type
  ;; expressions promote to the highest tier present (bigint, ratio,
  ;; bigdec, or float).
  (is (= 2N (+ 1 1N)))
  (is (= 6N (* 2N 3N)))
  (is (= -1N (- 0 1N)))
  (is (= 5/6 (+ 1/2 1/3)))
  ;; Bigdec scale follows the operands: 1M and 2M are both scale 0, so
  ;; their sum is `3M`. `1.0M + 2.0M` would be `3.0M`.
  (is (= 3M (+ 1M 2M)))
  (is (= 3.0M (+ 1.0M 2.0M)))
  ;; Long overflow on homogeneous-int arithmetic auto-promotes to
  ;; bigint instead of throwing.
  (is (= 9223372036854775808N (+ 9223372036854775807 1)))
  (is (= :bigint (type (+ 9223372036854775807 1))))
  (is (= 1000000000000000000000000000N
         (* 1000000000 1000000000 1000000000)))
  (is (= 9223372036854775808N (inc 9223372036854775807)))
  (is (= -9223372036854775809N (dec -9223372036854775808)))
  ;; Unary - on LLONG_MIN promotes.
  (is (= 9223372036854775808N (- -9223372036854775808)))
  ;; Binary - that crosses LLONG_MIN promotes.
  (is (= -9223372036854775809N (- -9223372036854775808 1))))

;; --- ratio / bigdec / float in plain arithmetic ---

(deftest nt-arithmetic-mixed-tower
  (is (= 0 (+)))
  (is (= 5 (+ 5)))
  (is (= 3 (+ 1 2)))
  (is (= 10 (+ 1 2 3 4)))
  (is (= 6N (+ 1N 2N 3N)))
  (is (= 3N (+ 1 2N)))
  (is (= 3N (+ 1N 2)))
  (is (= 2.5 (+ 1 1.5)))
  (is (= 2.5 (+ 1.5 1)))
  (is (= :float (type (+ 1N 2.0))))
  (is (= 1 (*)))
  (is (= 5 (* 5)))
  (is (= 12 (* 3 4)))
  (is (= 24 (* 1 2 3 4)))
  (is (= 6N (* 1N 2N 3N)))
  (is (= 10000000000000000000000N (* 100000000000 100000000000)))
  (is (= :bigint (type (* 100000000000 100000000000))))
  (is (= 6.0 (* 2 3.0))))

;; --- inc / dec on the full tower ---

(deftest nt-inc-dec-tower
  (is (= 2 (inc 1)))
  (is (= 0 (dec 1)))
  (is (= 2N (inc 1N)))
  (is (= -1N (dec 0N)))
  (is (= 100000000000000000000N (inc 99999999999999999999N)))
  (is (= 2.5 (inc 1.5)))
  (is (= 0.5 (dec 1.5)))
  (is (thrown? (inc)))
  (is (thrown? (dec)))
  (is (thrown? (inc 1 2)))
  (is (thrown? (inc "x")))
  (is (thrown? (dec nil))))

;; --- promotion boundary: iterated inc crosses long/bigint seam cleanly ---

(deftest nt-iterated-promotion
  (let [max-long 9223372036854775807
        over (inc max-long)]
    (is (= :bigint (type over)))
    (is (= over 9223372036854775808N))
    (is (= (inc over) 9223372036854775809N)))
  (let [min-long -9223372036854775808
        under (dec min-long)]
    (is (= :bigint (type under)))
    (is (= under -9223372036854775809N))
    (is (= (dec under) -9223372036854775810N))))

;; --- unchecked-* family: int64 wraparound, no promotion ---

(deftest nt-unchecked-arithmetic
  ;; Wraparound semantics. LLONG_MAX + 1 wraps to LLONG_MIN.
  (is (= -9223372036854775808 (unchecked-add 9223372036854775807 1)))
  (is (= -9223372036854775808 (unchecked-inc 9223372036854775807)))
  ;; LLONG_MIN - 1 wraps to LLONG_MAX.
  (is (= 9223372036854775807 (unchecked-subtract -9223372036854775808 1)))
  (is (= 9223372036854775807 (unchecked-dec -9223372036854775808)))
  ;; Multiply wraparound.
  (is (= -9223372036854775808 (unchecked-multiply 2 4611686018427387904)))
  ;; Negate at LLONG_MIN wraps back to itself.
  (is (= -9223372036854775808 (unchecked-negate -9223372036854775808)))
  (is (= -5 (unchecked-negate 5)))
  ;; Normal arithmetic in-range.
  (is (= 3 (unchecked-add 1 2)))
  (is (= 2 (unchecked-subtract 5 3)))
  (is (= 6 (unchecked-multiply 2 3)))
  (is (= 6 (unchecked-inc 5)))
  (is (= 4 (unchecked-dec 5)))
  ;; Arity errors: each unchecked op is fixed-arity.
  (is (thrown? (unchecked-add 1)))
  (is (thrown? (unchecked-add 1 2 3)))
  (is (thrown? (unchecked-inc 1 2)))
  ;; Type errors: non-int operands.
  (is (thrown? (unchecked-add 1.0 2)))
  (is (thrown? (unchecked-multiply 1N 2)))
  (is (thrown? (unchecked-inc "x")))
  (is (thrown? (unchecked-dec nil)))
  (is (thrown? (unchecked-negate nil))))

;; --- constructor edge cases ---

(deftest nt-constructor-errors
  (is (thrown? (bigint)))              ;; arity
  (is (thrown? (bigint 1 2)))          ;; arity
  (is (thrown? (bigint nil)))          ;; type
  (is (thrown? (bigint "not-a-num")))  ;; parse
  (is (thrown? (bigint [1 2])))        ;; type
  (is (thrown? (bigint? 1 2))))        ;; arity

;; --- Phase C.3: ratio reader, predicates, accessors ---

(deftest nt-ratio-literal
  (is (= :ratio (type 1/2)))
  (is (= :ratio (type -3/4)))
  ;; Reduced literals: 4/2 narrows to 2 (int).
  (is (= 2 6/3))
  (is (= :int (type 6/3)))
  ;; Sign normalisation: denom always positive after construction.
  (is (= -1/2 (rationalize -0.5)))
  ;; Numerator / denominator narrow when they fit in long.
  (is (= 1 (numerator 1/2)))
  (is (= 2 (denominator 1/2)))
  (is (= -3 (numerator -3/4)))
  (is (= 4 (denominator -3/4)))
  ;; Arbitrary-magnitude ratio.
  (is (= 99999999999999999999999/3 33333333333333333333333N))
  (is (= 1 (numerator 1/3)))
  (is (= 3 (denominator 1/3))))

(deftest nt-ratio-predicates
  (is (ratio? 1/2))
  (is (not (ratio? 1)))
  (is (not (ratio? 1.5)))
  (is (not (ratio? 1N)))
  (is (rational? 1))
  (is (rational? 1N))
  (is (rational? 1/2))
  (is (not (rational? 1.5)))
  (is (not (rational? 1.5M))))

;; --- Phase C.3: bigdec reader, predicates, accessors ---

(deftest nt-bigdec-literal
  (is (= :bigdec (type 1.5M)))
  (is (= :bigdec (type 1M)))
  (is (= :bigdec (type 0.0M)))
  (is (decimal? 1.5M))
  (is (not (decimal? 1.5)))
  (is (not (decimal? 1)))
  (is (not (rational? 1.5M)))
  ;; Same-rep equality under `=` (decimal scale-aware).
  (is (= 1.5M 1.5M))
  (is (not (= 1.0M 1.00M)))
  ;; Numeric equality across scales under `==`.
  (is (== 1.0M 1.00M))
  (is (== 1.0M 1M))
  (is (== 1.5 1.5M)))

;; --- Phase C.3: tower-dispatched arithmetic ---

(deftest nt-tower-add
  (is (= 3 (+ 1 2)))
  (is (= 2N (+ 1 1N)))
  (is (= 5/6 (+ 1/2 1/3)))
  (is (= 3/2 (+ 1 1/2)))
  (is (= :ratio (type (+ 1 1/2))))
  ;; Ratio result that simplifies back to int.
  (is (= 1 (+ 1/2 1/2)))
  (is (= :int (type (+ 1/2 1/2))))
  ;; Float collapses everything.
  (is (= 2.5 (+ 1 1.5)))
  (is (= 2.5 (+ 1/2 2.0)))
  ;; Bigdec same-tier.
  (is (= 3M (+ 1M 2M)))
  (is (= :bigdec (type (+ 1M 2M)))))

(deftest nt-tower-sub
  (is (= -1 (- 1 2)))
  (is (= -1N (- 0 1N)))
  (is (= 1/6 (- 1/2 1/3)))
  (is (= 1/2 (- 1 1/2)))
  (is (= 0.5 (- 2 1.5)))
  (is (= 0.5M (- 1.5M 1.0M))))

(deftest nt-tower-mul
  (is (= 6 (* 2 3)))
  (is (= 1 (* 1/2 2)))
  (is (= :int (type (* 1/2 2))))
  (is (= 1/6 (* 1/2 1/3)))
  ;; Bigdec scale follows operand scales: 1.5M (scale 1) * 2M (scale 0) = 3.0M.
  (is (= 3.0M (* 1.5M 2M))))

(deftest nt-tower-div
  ;; Integer division returns a ratio when not exact.
  (is (= 1/2 (/ 1 2)))
  (is (= 5 (/ 10 2)))
  (is (= 2/3 (/ 2 3)))
  ;; Float anywhere goes float.
  (is (= 0.5 (/ 1.0 2)))
  ;; Mixed int/bigint.
  (is (= 2N (/ 4N 2)))
  ;; Reciprocal of int is ratio.
  (is (= 1/2 (/ 2)))
  (is (= 1/3 (/ 3)))
  ;; Reciprocal of negative.
  (is (= -1/2 (/ -2))))

;; --- Phase C.3: tower comparison and == ---

(deftest nt-tower-compare
  (is (< 1 1.5 2 3))
  (is (< 1/4 1/2 3/4))
  (is (< 1N 2N 3N))
  (is (< 1 2N 3))
  (is (< 1.0M 2.0M))
  (is (<= 1 1N 2))            ;; int and bigint of same value are <=
  (is (<= 1 1 2))
  (is (>= 3 3 1))
  (is (> 3.5 3 2))
  (is (< 0.99 1)))

(deftest nt-num-eq
  ;; == is numeric equality across the tower.
  (is (== 1 1.0))
  (is (== 1 1N))
  (is (== 1/2 0.5))
  (is (== 1.5M 1.5))
  (is (== 1 1.0M))
  (is (== 1N 1.0))
  (is (not (== 1 2)))
  (is (not (== 1 1.5)))
  ;; = is type-strict on numeric tier (Clojure dialect).
  (is (not (= 1 1.0)))
  (is (not (= 1 1.5M)))
  (is (not (= 1/2 0.5)))
  ;; int and bigint are the same kind under =.
  (is (= 1 1N))
  (is (= 1N 1)))

;; --- Phase C.3: rationalize ---

(deftest nt-rationalize
  ;; Half representations are exact in IEEE-754.
  (is (= 1/2 (rationalize 0.5)))
  (is (= 1/4 (rationalize 0.25)))
  (is (= 0 (rationalize 0.0)))
  ;; Already-rational values pass through.
  (is (= 1 (rationalize 1)))
  (is (= 1/3 (rationalize 1/3)))
  (is (= 1N (rationalize 1N)))
  ;; ratio? on the result.
  (is (ratio? (rationalize 0.5))))

;; --- Plain ops on the full tower (ratio / bigdec / inc / dec) ---

(deftest nt-tower-ops-mixed
  (is (= 5/6 (+ 1/2 1/3)))
  (is (= 1/6 (- 1/2 1/3)))
  (is (= 1/6 (* 1/2 1/3)))
  (is (= 3M (+ 1M 2M)))
  (is (= -1M (- 1M 2M)))
  ;; inc/dec on ratio.
  (is (= 3/2 (inc 1/2)))
  (is (= -1/2 (dec 1/2))))
