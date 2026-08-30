(require "tests/test")
(require '[clojure.edn :as edn])
(require '[clojure.string :as str])

;; Compress-side tests (compression-zip campaign p2, ADR 29):
;; gzip-compress, deflate-compress, zlib-compress write and
;; zlib-decompress reads, over the widened vendored miniz tdefl
;; core.
;;
;; Golden split (R4): the python3 oracle streams under
;; tests/fixtures/compress/ pin the DECODE side (zlib-decompress
;; decodes python's bytes; gzip-decompress already exists and reads
;; our members back). The writers are pinned by the header vectors
;; (RFC 1952 10-byte header, RFC 1950 CMF/FLG pair), the round-trip
;; property, and the two-call determinism check -- never by
;; comparing compressed bodies against python or gzip(1).
;;
;; One known header divergence is pinned deliberately: python's
;; gzip.compress writes XFL 4 at BOTH levels 0 and 1 (zlib's
;; "level < 2" rule), while mino's contract is XFL 2 at level 9,
;; 4 at level 1, else 0. XFL is advisory; mino follows its own
;; contract and agrees with python at levels 1, 6, and 9.

(def ^:private cmp-fx-dir "tests/fixtures/compress/")

(defn- cmp-fixture
  "Read a binary fixture as bytes (the base64 pair round-trips the
  raw slurp bytes losslessly)."
  [name]
  (base64-decode (base64-encode (slurp (str cmp-fx-dir name)))))

(def ^:private cmp-manifest
  (edn/read-string (slurp (str cmp-fx-dir "manifest.edn"))))

(def ^:private cmp-corpus (cmp-fixture "corpus.txt"))

(def ^:private cmp-levels [0 1 6 9])

(defn- cmp-level-entry [l]
  (first (filter #(= l (:level %)) (:levels cmp-manifest))))

(defn- cmp-kind
  "Run thunk; return :ok on success or the thrown :mino/kind."
  [thunk]
  (try (do (thunk) :ok) (catch e (:mino/kind e))))

(defn- cmp-level->xfl [l]
  (cond (= l 9) 2 (= l 1) 4 :else 0))

(defn- cmp-expected-gzip-header
  "The contract header vector at mtime 0 with no name and OS 255."
  [l]
  [31 139 8 0 0 0 0 0 (cmp-level->xfl l) 255])

(defn- cmp-head [b n]
  (vec (take n (seq b))))

;;; oracle decode pins (read side)

(deftest zlib-decompress-decodes-the-python-oracle
  (doseq [l cmp-levels]
    (is (= cmp-corpus
           (zlib-decompress (cmp-fixture (str "zlib_l" l ".bin")))
           (str "level " l " oracle stream")))))

(deftest zlib-decompress-decodes-python-at-every-level-0-9
  ;; Beyond the fixture levels: python streams for one payload at
  ;; each level 0-9, computed here via the manifest corpus so no
  ;; more fixtures land. This pins the FLEVEL-bucket spread on the
  ;; decode side; the payload stays under a kilobyte.
  (if (zero? (:exit (sh "sh" "-c" "command -v python3")))
    (doseq [l (range 10)]
      (let [payload (str/join "" (vec (repeat 700 "abcdeFGHIJ")))
            enc (base64-encode (byte-array (map int payload)))
            {:keys [exit out]}
            (sh "sh" "-c"
                (str "printf '%s' '" enc
                     "' | base64 -d | python3 -c 'import sys,zlib;"
                     "sys.stdout.buffer.write(zlib.compress(sys.stdin.buffer.read(),"
                     l "))' | base64"))]
        (is (zero? exit) (str "python3 level " l " encode failed: " out))
        ;; GNU `base64` wraps at 76 columns; BSD `base64` (macOS) emits
        ;; one line. Strip all whitespace so the strict base64-decode
        ;; (no embedded whitespace) accepts either platform's output.
        (is (= (byte-array (map int payload))
               (zlib-decompress (base64-decode (str/replace out #"\s" "")))))))
    (println "compress: python3 absent -- decode spread skipped")))

;;; writer header pins (byte-for-byte, headers only)

(deftest gzip-compress-header-matches-the-contract
  (doseq [l cmp-levels]
    (is (= (cmp-expected-gzip-header l)
           (cmp-head (gzip-compress cmp-corpus {:level l}) 10))
        (str "level " l " header"))))

(deftest gzip-compress-header-agrees-with-python-where-pinned
  ;; python writes the same 10 header bytes at levels 1, 6, 9
  ;; (mtime 0, no name). Level 0 is the recorded XFL divergence;
  ;; see the file header.
  (doseq [l [1 6 9]]
    (is (= (:gzip-header (cmp-level-entry l))
           (cmp-head (gzip-compress cmp-corpus {:level l}) 10))
        (str "level " l " python agreement"))))

(deftest gzip-compress-fname-is-the-basename
  (let [gz (gzip-compress cmp-corpus {:name "some/dir/file.txt"})
        [flg] (seq gz)]
    (is (pos? (bit-and flg 0x08)) "FNAME flag set")
    (is (= (vec (concat (map int "file.txt") [0]))
           (cmp-head (drop 10 (seq gz)) 9)))))

(deftest gzip-compress-mtime-and-os-fill-the-header
  (let [hdr (cmp-head (gzip-compress cmp-corpus) 10)]
    (is (= [0 0 0 0] (subvec hdr 4 8)) "default header pins mtime 0")
    (is (= 255 (nth hdr 9)) "default OS is 255"))
  (let [gz (gzip-compress cmp-corpus {:mtime 1234567890 :os 3})]
    (is (= [0xd2 0x02 0x96 0x49] (subvec (cmp-head gz 10) 4 8))
        ":mtime 1234567890 little-endian")
    (is (= 3 (nth (cmp-head gz 10) 9)) ":os 3")))

(deftest zlib-compress-cmf-flg-match-python
  (doseq [l cmp-levels]
    (is (= (:zlib-cmf-flg (cmp-level-entry l))
           (cmp-head (zlib-compress cmp-corpus {:level l}) 2))
        (str "level " l " CMF/FLG"))))

(deftest zlib-compress-fcheck-divides-and-flevel-buckets
  (let [probe (byte-array (map int "bucket probe"))
        flevel-at (fn [l]
                    (let [flg (second (seq (zlib-compress probe {:level l})))]
                      (bit-shift-right (bit-and flg 0xc0) 6)))
        expected {0 0 1 0 2 1 3 1 4 1 5 1 6 2 7 3 8 3 9 3}]
    (doseq [l (range 10)]
      (let [[cmf flg] (seq (zlib-compress probe {:level l}))]
        (is (zero? (rem (+ (* cmf 256) flg) 31))
            (str "level " l " FCHECK divisibility"))
        (is (= (expected l) (flevel-at l))
            (str "level " l " FLEVEL bucket"))))))

;;; size ordering

(def ^:private cmp-order-vocab
  ["time" "person" "year" "way" "day" "thing" "man" "world" "life" "hand"
   "part" "child" "eye" "woman" "place" "work" "week" "case" "point"
   "government" "company" "number" "group" "problem" "fact" "water"
   "money" "month" "lot" "book" "school" "word" "business" "issue"
   "side" "kind" "head" "house" "service" "friend" "father" "power"
   "hour" "game" "line" "end" "member" "law" "car" "city" "community"
   "name" "president" "team" "minute" "idea" "kid" "body" "back"
   "parent" "face" "level" "office" "door" "health" "art" "war"
   "history" "party" "result" "change" "morning" "reason" "research"
   "moment" "teacher" "guide" "music" "market" "sense" "nation" "plan"
   "college" "interest" "death" "experience" "effect" "class"
   "control" "care" "field" "development" "role" "rate" "heart" "drug"])

(defn- cmp-order-corpus
  "Deterministic word-stream corpus (~75 KB). Natural-text match
  structure at distance is what separates the levels; on perfectly
  repetitive templates tdefl's levels 6 and 9 tie and greedy level 1
  can even win, so the ordering assertion needs this shape. Built by
  transient int accumulation (no lazy chains at this size)."
  []
  (let [n (count cmp-order-vocab)]
    (loop [i 0, seed 42, first? true, acc (transient [])]
      (if (= i 12000)
        (byte-array (persistent! acc))
        (let [;; long coercion keeps the LCG in fixnums (the bigint
              ;; multiply would make byte-array reject the index).
              seed1 (long (mod (+ (* seed 1103515245) 12345) 2147483648))
              seed2 (long (mod (+ (* seed1 1103515245) 12345) 2147483648))
              w (nth cmp-order-vocab
                     (long (mod (quot seed1 6553) n)))
              word (if (zero? (mod i 13)) (str/capitalize w) w)]
          (when-not first? (conj! acc 32)) ; inter-word space
          (doseq [b (map int word)] (conj! acc b))
          (recur (inc i) seed2 false acc))))))

(def ^:private cmp-ordering-corpus (cmp-order-corpus))

(deftest compression-size-orders-by-level
  (doseq [[f who] [[gzip-compress "gzip-compress"]
                   [zlib-compress "zlib-compress"]
                   [deflate-compress "deflate-compress"]]]
    (let [size (fn [l] (count (f cmp-ordering-corpus {:level l})))]
      (is (> (size 1) (size 6)) (str who " size(1) > size(6)"))
      (is (> (size 6) (size 9)) (str who " size(6) > size(9)")))))

;;; round-trip property over the corpus variants

(def ^:private cmp-variants
  [["empty" (byte-array 0)]
   ["one byte" (byte-array [97])]
   ["all 256 byte values" (byte-array (range 256))]
   ["repetitive" (byte-array (repeat 4096 122))]
   ["pseudo-random" (byte-array (map (fn [i] (mod (+ (* i 31) (quot i 7)) 256))
                                     (range 4096)))]
   ["oracle corpus" cmp-corpus]])

(deftest compress-then-decompress-returns-the-input
  (doseq [[label input] cmp-variants
          l cmp-levels]
    (is (= input (deflate-decompress (deflate-compress input {:level l})))
        (str label " level " l " raw deflate"))
    (is (= input (gzip-decompress (gzip-compress input {:level l})))
        (str label " level " l " gzip"))
    (is (= input (zlib-decompress (zlib-compress input {:level l})))
        (str label " level " l " zlib"))))

(deftest compression-is-deterministic-two-calls
  (doseq [[f who] [[gzip-compress "gzip-compress"]
                   [zlib-compress "zlib-compress"]
                   [deflate-compress "deflate-compress"]]]
    (is (= (f cmp-corpus) (f cmp-corpus)) (str who " two-call ="))))

;;; zlib-decompress strictness (RFC 1950)

(def ^:private cmp-zlib-stream (cmp-fixture "zlib_l6.bin"))

(deftest zlib-bad-headers-are-magic
  (is (= :codec/magic
         (cmp-kind #(zlib-decompress
                      (byte-array (concat [0x79 0xbb] (rest (rest (seq cmp-zlib-stream)))))))))
  (is (= :codec/magic
         (cmp-kind #(zlib-decompress
                      ;; CMF 0x88: CINFO 8 (window 64 KiB) is out of
                      ;; spec; FLG 0x1c keeps FCHECK divisible by 31
                      (byte-array (concat [0x88 0x1c] (rest (rest (seq cmp-zlib-stream)))))))))
  (is (= :codec/magic
         (cmp-kind #(zlib-decompress
                      ;; FCHECK broken: FLG 0x9d makes the pair % 31 != 0
                      (byte-array (concat [0x78 0x9d]
                                          (rest (rest (seq cmp-zlib-stream))))))))))

(deftest zlib-fdict-is-unsupported
  (is (= :codec/unsupported
         (cmp-kind #(zlib-decompress
                      ;; FDICT set (FLG bit 5), rest a valid stream body
                      (byte-array (concat [0x78 0xbb]
                                          (rest (rest (seq cmp-zlib-stream))))))))))

(deftest zlib-truncations-are-classified
  (let [n (count cmp-zlib-stream)]
    (doseq [i (range 0 n)]
      (is (contains? #{:codec/truncated :codec/corrupt :codec/magic
                       :codec/crc :codec/limit}
                     (cmp-kind #(zlib-decompress
                                  (byte-array (take i (seq cmp-zlib-stream))))))
          (str "truncation at " i)))))

(deftest zlib-adler-corruption-is-crc
  (let [n (count cmp-zlib-stream)]
    (doseq [i (range (- n 4) n)]
      (is (= :codec/crc
             (cmp-kind #(zlib-decompress
                          (byte-array (map-indexed
                                        (fn [j v] (if (= j i) (bit-xor v 1) v))
                                        (seq cmp-zlib-stream))))))))))

(deftest zlib-trailing-bytes-are-corrupt
  (is (= :codec/corrupt
         (cmp-kind #(zlib-decompress
                      (byte-array (concat (seq cmp-zlib-stream) [0])))))))

(deftest zlib-decompress-cap-fires-at-the-boundary
  (is (= cmp-corpus (zlib-decompress cmp-zlib-stream {:max-bytes (count cmp-corpus)})))
  (is (= :codec/limit
         (cmp-kind #(zlib-decompress cmp-zlib-stream
                                     {:max-bytes (dec (count cmp-corpus))})))))

;;; argument surface

(deftest compress-input-must-be-bytes
  ;; Stream prims are bytes-strict (the gzip.c symmetry): strings
  ;; are rejected, not silently encoded.
  (doseq [bad ["x" 42 nil [1 2 3]]]
    (is (= :eval/type (cmp-kind #(gzip-compress bad))))
    (is (= :eval/type (cmp-kind #(deflate-compress bad))))
    (is (= :eval/type (cmp-kind #(zlib-compress bad))))
    (is (= :eval/type (cmp-kind #(zlib-decompress bad))))))

(deftest compress-opts-validation-matrix
  (doseq [f [gzip-compress deflate-compress zlib-compress]]
    (is (= :eval/contract (cmp-kind #(f cmp-corpus {:level 10}))))
    (is (= :eval/contract (cmp-kind #(f cmp-corpus {:level -1}))))
    (is (= :eval/contract (cmp-kind #(f cmp-corpus {:level "6"}))))
    (is (= :eval/contract (cmp-kind #(f cmp-corpus {:level 1.5}))))
    (is (= :eval/type (cmp-kind #(f cmp-corpus 5)))))
  (is (= :eval/contract (cmp-kind #(gzip-compress cmp-corpus {:mtime -1}))))
  (is (= :eval/contract (cmp-kind #(gzip-compress cmp-corpus {:mtime "x"}))))
  (is (= :eval/contract (cmp-kind #(gzip-compress cmp-corpus {:name 7}))))
  (is (= :eval/contract (cmp-kind #(gzip-compress cmp-corpus {:name "all/slashes/"}))))
  (is (= :eval/contract (cmp-kind #(gzip-compress cmp-corpus {:os 256}))))
  (is (= :eval/contract (cmp-kind #(gzip-compress cmp-corpus {:os "3"}))))
  (is (= :eval/contract (cmp-kind #(zlib-decompress cmp-zlib-stream {:max-bytes -1}))))
  (is (= :eval/type (cmp-kind #(zlib-decompress cmp-zlib-stream 5)))))

(deftest compress-arity-is-one-or-two-args
  (is (= :eval/arity (cmp-kind #(gzip-compress))))
  (is (= :eval/arity (cmp-kind #(gzip-compress cmp-corpus {} 1))))
  (is (= :eval/arity (cmp-kind #(zlib-decompress))))
  (is (= :eval/arity (cmp-kind #(deflate-compress)))))

;;; cross-tool decode interop (self-skipping, R4: interop not bytes)

(deftest gzip-t-accepts-our-member
  ;; gzip(1) decodes a mino-written member. The member goes to disk
  ;; through base64 (spit writes strings, not bytes). Self-skips
  ;; when gzip(1) or base64 is absent.
  (if (and (zero? (:exit (sh "sh" "-c" "command -v gzip")))
           (zero? (:exit (sh "sh" "-c" "command -v base64"))))
    (let [path "/tmp/mino_compress_gzip_t.gz"
          {:keys [exit out]}
          (sh "sh" "-c"
              (str "printf '%s' '" (base64-encode
                                     (gzip-compress cmp-corpus {:level 6}))
                   "' | base64 -d > " path " && gzip -t " path))]
      (is (zero? exit) (str "gzip -t rejected the mino member: " out))
      (sh "sh" "-c" (str "rm -f " path)))
    (println "compress: gzip(1) or base64 absent -- cross-check skipped")))

(run-tests-and-exit)
