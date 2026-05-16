(require "tests/test")

;; Regression: the BC compiler's BINOP_SHL / BAND / BOR / BXOR /
;; SHR / USHR fast paths used to route their results through
;; tag_or_box_int, which calls mino_int that promotes overflowing
;; values to MINO_BIGINT when the bignum capability is installed.
;; The corresponding prims used mino_int_wrap which always boxes as
;; MINO_INT. The surface symptom: a downstream bit-xor against the
;; promoted bigint raised MTY001 "bit-xor expects integers" --
;; misleading, since the bigint IS an integer, just not the type the
;; bit-xor fast path was looking for. Fix routes the bitwise BC fast
;; paths through mino_int_wrap to match the prim's semantics.

(deftest bc-bit-shift-left-stays-int
  (testing "bit-shift-left of a large i64 stays :int in a BC-compiled fn"
    (defn step [x] (bit-shift-left x 25))
    (let [r (step 142452317671654462)]
      (is (= :int (type r)))
      (is (= 4728920382667489280 r))))
  (testing "chained bit-shift-left + bit-xor in a fn body"
    (defn xorshift [x]
      (let [a (bit-xor x (unsigned-bit-shift-right x 12))
            b (bit-xor a (bit-shift-left a 25))
            c (bit-xor b (unsigned-bit-shift-right b 27))]
        c))
    (is (= :int (type (xorshift 142435135655313518))))))

(deftest bc-bitwise-stays-int
  (testing "bit-and, bit-or, bit-xor in BC-compiled fn stay :int"
    (defn bw [a b]
      [(bit-and a b) (bit-or a b) (bit-xor a b)])
    (let [r (bw 142452317671654462 9223372036854775000)]
      (is (every? #(= :int (type %)) r))))
  (testing "unsigned-bit-shift-right in BC fn"
    (defn ushr [x] (unsigned-bit-shift-right x 1))
    (is (= :int (type (ushr 142452317671654462))))))
