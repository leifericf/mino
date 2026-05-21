(require "tests/test")

;; Erlang-inspired bit-syntax surface: bits / bits-get / subbits.

(deftest bits-byte-aligned-be
  (let [bs (bits [0x12 :size 8] [0x34 :size 8])]
    (is (= 2 (count bs)))
    (is (= 0x12 (aget bs 0)))
    (is (= 0x34 (aget bs 1)))))

(deftest bits-16bit-big-endian
  (let [bs (bits [0x1234 :size 16])]
    (is (= 2 (count bs)))
    (is (= 0x12 (aget bs 0)))
    (is (= 0x34 (aget bs 1)))))

(deftest bits-16bit-little-endian
  (let [bs (bits [0x1234 :size 16 :endian :little])]
    (is (= 0x34 (aget bs 0)))
    (is (= 0x12 (aget bs 1)))))

(deftest bits-32bit-mixed
  (let [bs (bits [0xCAFEBABE :size 32])]
    (is (= 4 (count bs)))
    (is (= 0xCA (aget bs 0)))
    (is (= 0xFE (aget bs 1)))
    (is (= 0xBA (aget bs 2)))
    (is (= 0xBE (aget bs 3)))))

(deftest bits-uint-rejects-negative
  (is (thrown? (bits [-1 :size 8 :type :uint]))))

(deftest bits-int-default
  ;; :int is unsigned-equivalent at write time. Range check accepts
  ;; the natural two's-complement range or the unsigned range.
  (let [bs (bits [255 :size 8])]
    (is (= 0xff (aget bs 0)))))

(deftest bits-bit-aligned-tail
  ;; 5-bit segment -> 1 byte with 5 valid bits (bit_tail=5).
  ;; Value 1 in 5 bits = 0b00001, written MSB-first at offset 0
  ;; gives buf[0]=0b00001000 = 0x08.
  (let [bs (bits [1 :size 5])]
    (is (false? (bytes? bs)))
    (is (true?  (bitstring? bs)))))

(deftest bits-multi-segment-bit-aligned
  ;; 5+3 = 8 bits = byte-aligned.
  (let [bs (bits [1 :size 5] [7 :size 3])]
    (is (true? (bytes? bs)))
    (is (= 1 (count bs)))
    ;; bits 0-4 = 00001, bits 5-7 = 111 -> 0b00001111 = 0x0f
    (is (= 0x0f (aget bs 0)))))

(deftest bits-float-32-roundtrip
  (let [bs (bits [3.14 :size 32 :type :float])]
    (is (= 4 (count bs)))
    (let [v (bits-get bs :offset 0 :size 32 :type :float)]
      (is (< (- v 3.14) 0.001))
      (is (> (- v 3.14) -0.001)))))

(deftest bits-float-64-roundtrip
  (let [bs (bits [3.141592653589793 :size 64 :type :float])]
    (is (= 8 (count bs)))
    (let [v (bits-get bs :offset 0 :size 64 :type :float)]
      (is (= v 3.141592653589793)))))

(deftest bits-bytes-segment
  (let [src (byte-array [0xDE 0xAD 0xBE 0xEF])
        bs  (bits [0xCA :size 8] [src :type :bytes])]
    (is (= 5 (count bs)))
    (is (= 0xCA (aget bs 0)))
    (is (= 0xDE (aget bs 1)))
    (is (= 0xEF (aget bs 4)))))

(deftest bits-get-uint
  (let [bs (bits [0x1234 :size 16])]
    (is (= 0x1234 (bits-get bs :offset 0 :size 16)))))

(deftest bits-get-little-endian
  (let [bs (bits [0x1234 :size 16 :endian :little])]
    (is (= 0x1234 (bits-get bs :offset 0 :size 16 :endian :little)))))

(deftest bits-get-signed-int
  ;; -1 as 8-bit signed = 0xff. Read back as signed should be -1.
  (let [bs (bits [0xff :size 8])]
    (is (= 0xff (bits-get bs :offset 0 :size 8)))
    (is (= -1   (bits-get bs :offset 0 :size 8 :type :int :signed? true)))))

(deftest bits-get-at-non-byte-offset
  ;; Pack 4 bits then 4 bits; read them out individually.
  (let [bs (bits [0xA :size 4] [0xB :size 4])]
    (is (= 0xA (bits-get bs :offset 0 :size 4)))
    (is (= 0xB (bits-get bs :offset 4 :size 4)))
    (is (= 0xAB (bits-get bs :offset 0 :size 8)))))

(deftest bits-get-bytes-slice
  (let [bs (bits [0xAA :size 8] [0xBB :size 8] [0xCC :size 8])
        slice (bits-get bs :offset 8 :size 16 :type :bytes)]
    (is (true? (bytes? slice)))
    (is (= 2 (count slice)))
    (is (= 0xBB (aget slice 0)))
    (is (= 0xCC (aget slice 1)))))

(deftest bits-get-out-of-range
  (let [bs (bits [0x12 :size 8])]
    (is (thrown? (bits-get bs :offset 0 :size 16)))
    (is (thrown? (bits-get bs :offset 4 :size 8)))))

(deftest subbits-byte-aligned
  (let [bs (bits [0xFF :size 8] [0x00 :size 8])
        slice (subbits bs 4 12)]
    ;; bits 4..12 of FF 00 -> 1111 0000 -> 0xf0
    (is (true? (bytes? slice)))
    (is (= 1 (count slice)))
    (is (= 0xF0 (aget slice 0)))))

(deftest subbits-bit-aligned-result
  (let [bs (bits [0xAB :size 8] [0xCD :size 8])
        slice (subbits bs 0 11)]   ;; 11 bits is not byte-aligned
    (is (false? (bytes? slice)))
    (is (true?  (bitstring? slice)))))

(deftest subbits-full-range
  (let [bs (bits [0xAB :size 8] [0xCD :size 8])
        slice (subbits bs 0 16)]
    (is (= bs slice))))

(deftest subbits-empty-range
  (let [bs (bits [0xAB :size 8])
        slice (subbits bs 4 4)]
    (is (zero? (count slice)))
    (is (true? (bytes? slice)))))

(deftest subbits-rejects-out-of-range
  (let [bs (bits [0xAB :size 8])]
    (is (thrown? (subbits bs -1 4)))
    (is (thrown? (subbits bs 0 9)))
    (is (thrown? (subbits bs 4 2)))))

(deftest let-bits-two-field-extraction
  (let-bits [(bits [0xAB :size 8] [0xCD :size 8])
             [a :size 8]
             [b :size 8]]
    (is (= 0xAB a))
    (is (= 0xCD b))))

(deftest let-bits-mixed-sizes
  (let-bits [(bits [0xAB :size 8] [0xCD :size 8])
             [hi :size 4]
             [mi :size 8]
             [lo :size 4]]
    ;; bytes 0xAB 0xCD -> 10101011 11001101
    ;; hi = bits 0..3  = 1010 = 0xA
    ;; mi = bits 4..11 = 10111100 = 0xBC
    ;; lo = bits 12..15 = 1101 = 0xD
    (is (= 0xA hi))
    (is (= 0xBC mi))
    (is (= 0xD lo))))

(deftest let-bits-little-endian-segment
  (let-bits [(bits [0xCA :size 8] [0xFE :size 8])
             [v :size 16 :endian :little]]
    (is (= 0xFECA v))))

(deftest let-bits-rest-as-bytes
  (let-bits [(bits [0xCA :size 8] [0xFE :size 8] [0xBA :size 8] [0xBE :size 8])
             [head :size 16]
             [tail :type :bytes]]
    (is (= 0xCAFE head))
    (is (true? (bytes? tail)))
    (is (= 2 (count tail)))
    (is (= 0xBA (aget tail 0)))
    (is (= 0xBE (aget tail 1)))))

(deftest let-bits-signed-int
  (let-bits [(byte-array [0xFF])
             [v :size 8 :signed? true]]
    (is (= -1 v))))

(deftest let-bits-rejects-non-vector-segments
  ;; A segment must be a vector. The macro should throw at expand
  ;; time, surfacing as a runtime exception when the form is
  ;; evaluated.
  (is (thrown? (eval '(let-bits [bs :not-a-vector] :body)))))

(deftest bits-rejects-bad-options
  (is (thrown? (bits [1 :unknown :something])))
  (is (thrown? (bits [1.0 :size 16 :type :float])))   ;; 16-bit float not supported
  (is (thrown? (bits [1 :size 9 :endian :little]))))  ;; LE requires multiple-of-8

(deftest bits-no-segments-is-empty
  (let [bs (bits)]
    (is (zero? (count bs)))
    (is (true? (bytes? bs)))))
