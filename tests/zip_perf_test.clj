(require "tests/test")
(require '[clojure.string :as str])

;; Zip read perf budgets (compression-zip campaign p3, ADR 29).
;;
;; Both archives are built in-test from stored members (byte-known
;; LOC/CDH/EOCD framing, transient accumulation, no committed blobs,
;; no python at test time); the timing sections measure only the
;; prims, never the build. Budgets are absolute wall-clock asserts
;; with the ADR 25/28 multiplier band: native zip read of a stored
;; member is memcpy plus CRC-32 (hundreds of MB/s), and a 10k-entry
;; central directory walk is bounded linear work, so 500 ms holds
;; order-of-magnitude headroom over the 2-8 ms land-time numbers.
;;
;; Land-time numbers (this host, arm64 darwin, cc -O2):
;;   zip-read   1,033,645 byte stored member:   3 ms
;;   zip-entries 10,000-entry central dir:     73 ms
;;
;; Every timing test also verifies the round trip, so a fast-but-
;; wrong reader cannot pass. This file joins the nightly
;; MINO_TEST_EXCLUDE seam (the gc-fuzz lane) in the same change it
;; lands.

(def ^:private zperf-vocab
  ["alpha" "bravo" "charlie" "delta" "echo" "foxtrot" "golf" "hotel"
   "india" "juliet" "kilo" "lima" "mike" "november" "oscar" "papa"
   "quebec" "romeo" "sierra" "tango" "uniform" "victor" "whiskey"
   "xray" "yankee" "zulu"])

(defn- zperf-lcg
  "Deterministic LCG; long coercion keeps the multiply in fixnums."
  [s]
  (long (mod (+ (* s 1103515245) 12345) 2147483648)))

(defn- zperf-conj-all!
  "Doseq-conjoin bytes onto a transient vector (no lazy chains)."
  [acc bs]
  (doseq [b bs] (conj! acc b))
  acc)

(defn- zperf-text
  "Word-stream text section (~8 bytes per word)."
  [words seed]
  (let [n (count zperf-vocab)]
    (loop [i 0, s seed, acc (transient [])]
      (if (= i words)
        (byte-array (persistent! acc))
        (let [s1 (zperf-lcg s)
              word (nth zperf-vocab (long (mod (quot s1 6553) n)))
              acc (if (pos? i) (conj! acc 32) acc)]
          (recur (inc i) s1 (zperf-conj-all! acc (map int word))))))))

(defn- zperf-noise [n seed]
  (byte-array (loop [i 0, s seed, acc (transient [])]
                (if (= i n)
                  (persistent! acc)
                  (let [s1 (zperf-lcg s)]
                    (recur (inc i) s1
                           (conj! acc (mod (quot s1 7919) 256))))))))

(def ^:private zperf-member
  ;; ~1 MB mixed corpus: text, noise, a long run, noise.
  (let [acc (transient [])]
    (zperf-conj-all! acc (zperf-text 60000 3))
    (zperf-conj-all! acc (zperf-noise 262144 11))
    (dotimes [_ 262144] (conj! acc 66))
    (zperf-conj-all! acc (zperf-noise 131072 17))
    (byte-array (persistent! acc))))

;;; stored-zip framing (byte-known, unique per this suite file)

(defn- zperf-w16 [v] [(bit-and v 0xff) (bit-shift-right v 8)])

(defn- zperf-w32 [v] (mapv #(bit-and (bit-shift-right v (* % 8)) 0xff)
                           (range 4)))

(defn- zperf-name-of [k]
  (byte-array (map int (format "f%05d.txt" k))))

(defn- zperf-loc [name data]
  (byte-array
   (concat (zperf-w32 0x04034b50) (zperf-w16 20) (zperf-w16 0)
           (zperf-w16 0) (zperf-w16 0) (zperf-w16 0x21)
           (zperf-w32 (crc32 data)) (zperf-w32 (count data))
           (zperf-w32 (count data)) (zperf-w16 (count name))
           (zperf-w16 0) (seq name) (seq data))))

(defn- zperf-cdh [name data ofs]
  (byte-array
   (concat (zperf-w32 0x02014b50) (zperf-w16 20) (zperf-w16 20)
           (zperf-w16 0) (zperf-w16 0) (zperf-w16 0) (zperf-w16 0x21)
           (zperf-w32 (crc32 data)) (zperf-w32 (count data))
           (zperf-w32 (count data)) (zperf-w16 (count name))
           (zperf-w16 0) (zperf-w16 0) (zperf-w16 0) (zperf-w16 0)
           (zperf-w32 0) (zperf-w32 ofs) (seq name))))

(defn- zperf-zip-of
  "Assemble a stored-member archive from (index, payload) pairs.
  Eager throughout: vectors and loops, never lazy chains (the
  6000-entry stack rule)."
  [payloads]
  (let [n (count payloads)
        names (mapv (fn [p] (zperf-name-of (first p))) payloads)
        datas (mapv second payloads)
        locs (mapv zperf-loc names datas)
        offsets (loop [i 0, o 0, acc (transient [])]
                  (if (= i n)
                    (persistent! acc)
                    (recur (inc i) (+ o (count (nth locs i)))
                           (conj! acc o))))
        cdhs (mapv zperf-cdh names datas offsets)
        cd-ofs (loop [i 0, o 0] (if (= i n) o (recur (inc i) (+ o (count (nth locs i))))))
        cd-size (loop [i 0, o 0] (if (= i n) o (recur (inc i) (+ o (count (nth cdhs i))))))
        acc (transient [])]
    (loop [k 0]
      (when (< k n)
        (zperf-conj-all! acc (seq (nth locs k)))
        (recur (inc k))))
    (loop [k 0]
      (when (< k n)
        (zperf-conj-all! acc (seq (nth cdhs k)))
        (recur (inc k))))
    (zperf-conj-all!
     acc (concat (zperf-w32 0x06054b50) (zperf-w16 0) (zperf-w16 0)
                 (zperf-w16 n) (zperf-w16 n) (zperf-w32 cd-size)
                 (zperf-w32 cd-ofs) (zperf-w16 0)))
    (byte-array (persistent! acc))))

(def ^:private zperf-one-mb-archive
  (zperf-zip-of [[0 zperf-member]]))

(def ^:private zperf-ten-k-entry-count 10000)

(def ^:private zperf-ten-k-archive
  ;; 10,000 empty stored entries: the central-directory walk bound.
  (zperf-zip-of (mapv (fn [k] [k (byte-array 0)])
                      (range zperf-ten-k-entry-count))))

(defn- zperf-ms
  "Run (f), return elapsed milliseconds and the value."
  [f]
  (let [t0 (nano-time)
        r (f)]
    [(quot (- (nano-time) t0) 1000000) r]))

(deftest zperf-member-is-megabyte-scale
  (is (> (count zperf-member) 1000000)
      (str "member is " (count zperf-member) " bytes")))

(deftest zperf-zip-read-within-budget
  (let [[ms out] (zperf-ms #(zip-read zperf-one-mb-archive "f00000.txt"))]
    (is (= zperf-member out) "round trip stays correct")
    (is (< ms 500) (str "zip-read 1 MiB member took " ms "ms"))))

(deftest zperf-zip-entries-within-budget
  (let [[ms entries] (zperf-ms #(zip-entries zperf-ten-k-archive))]
    (is (= zperf-ten-k-entry-count (count entries))
        "every entry listed")
    (is (= "f00000.txt" (:name (first entries))))
    (is (= "f09999.txt" (:name (last entries))))
    (is (< ms 500) (str "zip-entries 10k-entry directory took " ms "ms"))))

(run-tests-and-exit)
