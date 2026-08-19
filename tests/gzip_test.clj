(require "tests/test")
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; gzip-decompress / deflate-decompress decode response-body-shaped
;; bytes through the vendored miniz inflate core. mino has no
;; compressor, so every round trip starts from a fixture generated
;; with gzip(1) (see tests/fixtures/gzip/README.md).
;;
;; Error kinds live in one family: :codec/truncated (input ends
;; mid-structure), :codec/magic (not a gzip container), :codec/crc
;; (CRC32 or ISIZE mismatch), :codec/corrupt (malformed stream,
;; reserved header bits, trailing bytes after the single member),
;; :codec/limit (output passed the :max-bytes cap). deflate-decompress
;; is raw-deflate only: a zlib-wrapped stream is rejected as data, not
;; decoded.

(def ^:private fx-dir "tests/fixtures/gzip/")

(defn- fixture [name]
  ;; slurp reads raw bytes into a string; the base64 pair round-trips
  ;; them back out as a bytes value losslessly.
  (base64-decode (base64-encode (slurp (str fx-dir name)))))

(def ^:private family
  #{:codec/truncated :codec/magic :codec/crc :codec/corrupt :codec/limit})

(defn- decode-kind
  "Run thunk; return :ok on success or the thrown :mino/kind."
  [thunk]
  (try (do (thunk) :ok) (catch e (:mino/kind e))))

(defn- flip-byte [b i]
  (byte-array (map-indexed (fn [j v] (if (= j i) (bit-xor v 1) v))
                           (seq b))))

(defn- prefix [b n]
  (byte-array (take n (seq b))))

(def ^:private hello-plaintext
  (byte-array (map int "hello, gzip\n")))

(defn- hello-parts
  "Split hello.gz into [header body trailer] at its structural
  boundaries (gzip -n writes a 10-byte no-flag header)."
  []
  (let [b (fixture "hello.gz")
        s (seq b)
        n (count b)]
    [(byte-array (take 10 s))
     (byte-array (take (- n 18) (drop 10 s)))
     (byte-array (drop (- n 8) s))]))

;;; fixed vectors

(deftest gzip-of-empty-and-single-byte
  (is (= (byte-array 0) (gzip-decompress (fixture "empty.gz"))))
  (is (= (byte-array [97]) (gzip-decompress (fixture "a.gz")))))

(deftest gzip-of-hello-round-trips
  (is (= hello-plaintext (gzip-decompress (fixture "hello.gz")))))

(deftest gzip-name-header-flag-is-skipped
  (is (= hello-plaintext (gzip-decompress (fixture "hello-fname.gz")))))

(deftest gzip-extra-comment-hcrc-flags-are-skipped
  ;; A hand-built container exercising the FEXTRA / FNAME / FCOMMENT /
  ;; FHCRC header walks. The 16-bit header CRC is consumed, not
  ;; verified: only the payload CRC32 gate is contractual.
  (let [[hdr body trailer] (hello-parts)
        flg 0x1e                  ; FHCRC | FEXTRA | FNAME | FCOMMENT
        container (byte-array
                    (concat [0x1f 0x8b 8 flg 0 0 0 0 0 255]
                            [5 0 1 2 3 4 5]      ; XLEN=5 + extra field
                            (map int "n") [0]    ; FNAME
                            (map int "c") [0]    ; FCOMMENT
                            [0 0]                ; FHCRC (unverified)
                            (seq body) (seq trailer)))]
    (is (= hello-plaintext (gzip-decompress container)))))

(deftest gzip-incompressible-10k-round-trips
  (is (= (fixture "random10k.bin")
         (gzip-decompress (fixture "random10k.gz")))))

(deftest gzip-1mb-of-zeros-round-trips
  (let [out (gzip-decompress (fixture "zeros1m.gz"))]
    (is (= 1048576 (count out)))
    (is (= (byte-array 1048576) out))))

;;; the output cap

(deftest decompression-bomb-hits-the-default-cap
  (is (= :codec/limit
         (decode-kind #(gzip-decompress (fixture "zeros200m.gz"))))))

(deftest decompression-bomb-passes-with-a-raised-cap
  (is (= 200000000
         (count (gzip-decompress (fixture "zeros200m.gz")
                                 {:max-bytes 300000000})))))

(deftest cap-is-inclusive-at-the-boundary
  (let [gz (fixture "zeros1m.gz")]
    (is (= 1048576 (count (gzip-decompress gz {:max-bytes 1048576}))))
    (is (= :codec/limit
           (decode-kind #(gzip-decompress gz {:max-bytes 1048575}))))))

;;; container integrity

(deftest trailer-crc-corruption-is-classified
  (let [b (fixture "hello.gz") n (count b)]
    ;; stored CRC32 occupies the first four trailer bytes
    (doseq [i (range (- n 8) (- n 4))]
      (is (= :codec/crc (decode-kind #(gzip-decompress (flip-byte b i))))))))

(deftest trailer-isize-corruption-is-classified
  (let [b (fixture "hello.gz") n (count b)]
    ;; stored ISIZE occupies the last four trailer bytes
    (doseq [i (range (- n 4) n)]
      (is (= :codec/crc (decode-kind #(gzip-decompress (flip-byte b i))))))))

(deftest body-bit-flips-never-succeed-quietly
  ;; A flipped body bit either breaks the deflate stream or decodes
  ;; bytes that no longer match the CRC; both are classified.
  (let [b (fixture "hello.gz") n (count b)]
    (doseq [i (range 10 (- n 8))]
      (is (contains? #{:codec/corrupt :codec/crc}
                     (decode-kind #(gzip-decompress (flip-byte b i))))))))

(deftest bad-magic-and-method-are-classified
  (let [b (fixture "hello.gz")]
    (is (= :codec/magic (decode-kind #(gzip-decompress (flip-byte b 0)))))
    (is (= :codec/magic (decode-kind #(gzip-decompress (flip-byte b 1)))))
    ;; byte 2 is the method; deflate is 8, so a flipped low bit is 9
    (is (= :codec/magic (decode-kind #(gzip-decompress (flip-byte b 2)))))))

(deftest reserved-header-flag-bits-are-rejected
  (let [b (fixture "hello.gz")
        bad (byte-array (map-indexed (fn [j v]
                                       (if (= j 3) (bit-or v 0x20) v))
                                     (seq b)))]
    (is (= :codec/corrupt (decode-kind #(gzip-decompress bad))))))

(deftest gzip-all-truncations-are-classified
  (let [b (fixture "hello.gz") n (count b)]
    (doseq [i (range 0 n)]
      (is (contains? family (decode-kind #(gzip-decompress (prefix b i))))))))

(deftest trailing-bytes-after-the-member-are-rejected
  (let [b (fixture "hello.gz")
        padded (byte-array (concat (seq b) [0]))
        doubled (byte-array (concat (seq b) (seq b)))]
    (is (= :codec/corrupt (decode-kind #(gzip-decompress padded))))
    (is (= :codec/corrupt (decode-kind #(gzip-decompress doubled))))))

;;; raw deflate

(deftest deflate-raw-round-trips
  (is (= hello-plaintext (deflate-decompress (fixture "hello.raw")))))

(deftest deflate-zlib-wrapper-is-rejected-as-data
  ;; HTTP "deflate" is decoded raw only; the zlib container
  ;; (0x78 ...) is a classified error, never a silent misdecode.
  (is (contains? family
                 (decode-kind #(deflate-decompress (fixture "hello.zlib"))))))

(deftest deflate-all-truncations-are-classified
  (let [b (fixture "hello.raw") n (count b)]
    (doseq [i (range 0 n)]
      (is (contains? family (decode-kind #(deflate-decompress (prefix b i))))))))

(deftest deflate-trailing-bytes-are-rejected
  (let [b (fixture "hello.raw")
        padded (byte-array (concat (seq b) [0]))]
    (is (= :codec/corrupt (decode-kind #(deflate-decompress padded))))))

;;; argument surface

(deftest input-must-be-bytes
  (doseq [bad ["x" 42 nil [1 2 3]]]
    (is (= :eval/type (decode-kind #(gzip-decompress bad))))
    (is (= :eval/type (decode-kind #(deflate-decompress bad))))))

(deftest arity-is-one-or-two-args
  (is (= :eval/arity (decode-kind #(gzip-decompress))))
  (is (= :eval/arity (decode-kind #(gzip-decompress (fixture "a.gz") {} 1))))
  (is (= :eval/arity (decode-kind #(deflate-decompress)))))

(deftest max-bytes-must-be-a-non-negative-integer
  (let [gz (fixture "a.gz")]
    (is (= :eval/contract (decode-kind #(gzip-decompress gz {:max-bytes -1}))))
    (is (= :eval/contract (decode-kind #(gzip-decompress gz {:max-bytes "x"}))))
    (is (= :eval/contract
           (decode-kind #(deflate-decompress gz {:max-bytes "x"}))))
    (is (= :eval/type (decode-kind #(gzip-decompress gz 5))))))

;;; generative and seeded fuzz

(def trials 60)

(defn- gz-qc [p]
  (:result (tc/quick-check trials p)))

(defn- decode-pair
  "Run f on v; [:ok result] or [:err kind]."
  [f v]
  (try [:ok (f v)] (catch e [:err (:mino/kind e)])))

(deftest random-bytes-never-crash-the-decoder
  (is (gz-qc (prop/for-all [v (gen/fmap byte-array
                                         (gen/vector (gen/choose 0 255) 0 80))]
                (let [[tag val] (decode-pair gzip-decompress v)]
                  (or (and (= :ok tag) (bytes? val))
                      (and (= :err tag) (contains? family val))))))))

(deftest seeded-mutations-of-hello-gz-stay-classified
  ;; Bit flips, byte clobbers, and truncations of a valid container,
  ;; driven by a deterministic LCG: every outcome is a classified
  ;; :codec/* error or the exact original plaintext. Mutations that
  ;; land in MTIME / XFL / OS or the filename decode fine; that arm
  ;; has teeth because the equality is against the known plaintext.
  (let [b (fixture "hello.gz")
        n (count b)
        iterations 300]
    (loop [seed 20260819 iter 0]
      (when (< iter iterations)
        (let [;; long coercion keeps the LCG in fixnums: the multiply
              ;; can pass through bigint, which byte-array rejects.
              seed' (long (mod (+ (* seed 1103515245) 12345) 2147483648))
              op    (mod (quot seed' 65536) 3)
              pos   (mod (quot seed' 128) n)
              mutated (cond
                        (= op 0) (flip-byte b pos)
                        (= op 1) (byte-array
                                   (map-indexed
                                     (fn [j v]
                                       (if (= j pos)
                                         (mod (quot seed' 1024) 256)
                                         v))
                                     (seq b)))
                        :else (prefix b pos))
              [tag val] (decode-pair gzip-decompress mutated)]
          (is (or (and (= :ok tag) (= hello-plaintext val))
                  (and (= :err tag) (contains? family val)))
              (str "iteration " iter " op " op))
          (recur seed' (inc iter)))))))

(run-tests-and-exit)
