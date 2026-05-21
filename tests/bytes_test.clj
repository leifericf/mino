(require "tests/test")

;; MINO_BYTES core surface: bytes? / bitstring? predicates, byte-array
;; constructor, count / aget / alength, seq integration, equality,
;; print form, and the #bytes "..." reader literal.

(deftest bytes-question-on-byte-aligned
  (is (true?  (bytes? (byte-array 4))))
  (is (true?  (bytes? (byte-array [1 2 3]))))
  (is (true?  (bytes? (byte-array 0)))))

(deftest bytes-question-rejects-non-bytes
  (is (false? (bytes? nil)))
  (is (false? (bytes? "abc")))
  (is (false? (bytes? [1 2 3])))
  (is (false? (bytes? 42))))

(deftest bitstring-question-on-bytes
  (is (true?  (bitstring? (byte-array 0))))
  (is (true?  (bitstring? (byte-array 8))))
  (is (false? (bitstring? "abc")))
  (is (false? (bitstring? nil))))

(deftest byte-array-from-size
  (let [b (byte-array 5)]
    (is (= 5 (count b)))
    (is (= 5 (alength b)))
    ;; Zero-filled.
    (dotimes [i 5]
      (is (= 0 (aget b i))))))

(deftest byte-array-from-collection
  (let [b (byte-array [65 66 67 68])]
    (is (= 4 (count b)))
    (is (= 65 (aget b 0)))
    (is (= 66 (aget b 1)))
    (is (= 67 (aget b 2)))
    (is (= 68 (aget b 3)))))

(deftest byte-array-signed-bytes
  ;; JVM byte-array accepts signed (-128..127) and unsigned (0..255)
  ;; bytes. mino does the same; storage is unsigned and aget returns
  ;; 0..255.
  (let [b (byte-array [-1 -128 127 0])]
    (is (= 255 (aget b 0)))   ;; -1 -> 0xff
    (is (= 128 (aget b 1)))   ;; -128 -> 0x80
    (is (= 127 (aget b 2)))
    (is (= 0   (aget b 3)))))

(deftest byte-array-rejects-out-of-range
  (is (thrown? (byte-array [256])))
  (is (thrown? (byte-array [-129])))
  (is (thrown? (byte-array [:not-an-int]))))

(deftest byte-array-rejects-negative-size
  (is (thrown? (byte-array -1))))

(deftest byte-array-from-empty-collection
  (let [b (byte-array [])]
    (is (true? (bytes? b)))
    (is (zero? (count b)))))

(deftest bytes-equality
  (is (= (byte-array [1 2 3]) (byte-array [1 2 3])))
  (is (not= (byte-array [1 2 3]) (byte-array [1 2 4])))
  (is (not= (byte-array [1 2 3]) (byte-array [1 2 3 4])))
  ;; Bytes equality with a vector is false even when contents match
  ;; (matching JVM Clojure's byte[] which is identity-equal there).
  (is (not= (byte-array [1 2 3]) [1 2 3])))

(deftest bytes-hash-consistent-with-equality
  (is (= (hash (byte-array [1 2 3]))
         (hash (byte-array [1 2 3])))))

(deftest bytes-seq-yields-unsigned-ints
  (is (= [65 66 67] (seq (byte-array [65 66 67]))))
  (is (= [255 0 128] (seq (byte-array [-1 0 -128])))))

(deftest bytes-seq-empty
  (is (nil? (seq (byte-array 0)))))

(deftest bytes-empty?
  (is (true?  (empty? (byte-array 0))))
  (is (false? (empty? (byte-array 1)))))

(deftest aget-out-of-range
  (is (thrown? (aget (byte-array 4) 4)))
  (is (thrown? (aget (byte-array 4) -1))))

(deftest aset-throws-on-bytes
  ;; The immutable contract: aset on a bytes value throws rather than
  ;; silently mutating.
  (is (thrown? (aset (byte-array 4) 0 1))))

(deftest bytes-print-form
  (is (= "#bytes \"414243\"" (pr-str (byte-array [65 66 67]))))
  (is (= "#bytes \"\""        (pr-str (byte-array 0))))
  (is (= "#bytes \"ff\""      (pr-str (byte-array [255])))))

(deftest bytes-reader-literal
  (let [b (read-string "#bytes \"414243\"")]
    (is (true? (bytes? b)))
    (is (= 3 (count b)))
    (is (= 65 (aget b 0)))
    (is (= 67 (aget b 2)))))

(deftest bytes-reader-literal-whitespace-tolerant
  (let [b (read-string "#bytes \"AB CD EF\"")]
    (is (= 3 (count b)))
    (is (= 0xab (aget b 0)))
    (is (= 0xcd (aget b 1)))
    (is (= 0xef (aget b 2)))))

(deftest bytes-reader-literal-malformed-throws
  (is (thrown? (read-string "#bytes \"abc\"")))   ;; odd digits
  (is (thrown? (read-string "#bytes \"xyz\""))))  ;; not hex

(deftest bytes-roundtrip-through-read-print
  (let [b1 (byte-array [0 1 2 0xff 0x80 0x7f])
        s  (pr-str b1)
        b2 (read-string s)]
    (is (= b1 b2))
    (is (true? (bytes? b2)))))

(deftest bytes-type-of
  (is (= :bytes (type (byte-array 4)))))

(deftest bytes-conj-not-supported
  ;; conj on a bytes value is not part of the v0.415 surface; should
  ;; either throw or simply fail to dispatch. We don't pin a specific
  ;; error here, just that it doesn't silently succeed.
  (is (thrown? (conj (byte-array [1 2]) 3))))
