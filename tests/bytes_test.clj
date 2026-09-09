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

(deftest bytes-reader-literal-bit-tail-suffix
  ;; The #bytes literal accepts an optional /N trailing-bit-count suffix
  ;; naming how many bits of the final byte are significant (1..7). A
  ;; value with a non-zero tail is a bitstring, not a byte-aligned bytes
  ;; value, and it prints back with its /N suffix.
  (let [b (read-string "#bytes \"ff/7\"")]
    (is (true?  (bitstring? b)))
    (is (false? (bytes? b)))
    (is (= "#bytes \"ff/7\"" (pr-str b))))
  ;; /7 is the largest tail; /8 and above name a whole byte and are
  ;; rejected.
  (is (true? (bitstring? (read-string "#bytes \"ff/1\""))))
  (is (thrown? (read-string "#bytes \"ff/8\"")))
  (is (thrown? (read-string "#bytes \"ff/9\"")))
  ;; A tail with no payload byte is meaningless and rejected.
  (is (thrown? (read-string "#bytes \"/3\""))))

(deftest bytes-reader-literal-suffix-payload-intact
  ;; The /N suffix names only trailing bits; the hex payload before the
  ;; slash decodes to exactly the same bytes it would without a suffix.
  (let [b (read-string "#bytes \"ff/3\"")]
    (is (= 1 (count b)))
    (is (= 0xff (aget b 0))))
  (let [b (read-string "#bytes \"4142/5\"")]
    (is (= 2 (count b)))
    (is (= 0x41 (aget b 0)))
    (is (= 0x42 (aget b 1)))))

(deftest bytes-reader-literal-zero-tail-is-byte-aligned
  ;; A byte-aligned value (no suffix, or an explicit /0) is plain bytes,
  ;; not a bit-tailed bitstring, and prints without any /N.
  (let [b (read-string "#bytes \"ff\"")]
    (is (true? (bytes? b)))
    (is (= "#bytes \"ff\"" (pr-str b))))
  (let [b (read-string "#bytes \"ff/0\"")]
    (is (true? (bytes? b)))
    (is (= "#bytes \"ff\"" (pr-str b))))
  ;; An empty payload is a valid empty byte-aligned value.
  (let [b (read-string "#bytes \"\"")]
    (is (true? (bytes? b)))
    (is (zero? (count b)))))

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

(deftest bytes-first-rest-next-last
  (is (= 9 (first (byte-array [9 8 7]))))
  (is (= [8 7] (vec (rest (byte-array [9 8 7])))))
  (is (= 7 (last (byte-array [9 8 7])))))

(deftest bytes-nth-and-get
  (is (= 8 (nth (byte-array [9 8 7]) 1)))
  (is (= 8 (get (byte-array [9 8 7]) 1)))
  (is (= :missing (get (byte-array [9 8 7]) 99 :missing)))
  (is (= :missing (nth (byte-array [9 8 7]) 99 :missing))))

(deftest bytes-reduce-fold
  (is (= 10 (reduce + (byte-array [1 2 3 4]))))
  (is (= 24 (reduce * 1 (byte-array [1 2 3 4]))))
  (is (= 0  (reduce + (byte-array 0)))))

(deftest bytes-into-vector
  (is (= [10 20 30] (into [] (byte-array [10 20 30]))))
  (is (= #{1 2 3}   (into #{} (byte-array [1 2 3 1 2])))))

(deftest bytes-map-filter
  (is (= [2 3 4]   (map inc (byte-array [1 2 3]))))
  (is (= [2 4]     (filter even? (byte-array [1 2 3 4 5])))))

(deftest bytes-empty-returns-empty-bytes
  (let [e (empty (byte-array [1 2 3]))]
    (is (true? (bytes? e)))
    (is (zero? (count e)))))
