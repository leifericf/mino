(require "tests/test")
(require '[clojure.string :as str])

;; Compress perf budgets (compression-zip campaign p2, ADR 29).
;;
;; The corpus is a deterministic ~1 MB mixed byte set built from
;; seeded arithmetic (LCG word-stream text, LCG noise, repetitive
;; runs); no committed blobs. Budgets are absolute wall-clock
;; asserts, never ratios (the CI-runner lesson), with headroom for
;; resident-set GC pressure at the ADR 25/28 multiplier band: native
;; tdefl level 6 runs 30-80 MB/s standalone, so 1 MB costs 15-35 ms
;; against the 1500 ms budget; inflate at 150-400 MB/s puts 1 MB at
;; 3-7 ms against 500 ms.
;;
;; Land-time numbers (this host, arm64 darwin, cc -O2, 1,003,554
;; byte corpus):
;;   gzip-compress    level 6:  29 ms (463,760 bytes out)
;;   deflate-compress level 6:  27 ms (463,742 bytes out)
;;   zlib-compress    level 6:  27 ms (463,748 bytes out)
;;   gzip-decompress  of the gzip member:   3 ms
;;   zlib-decompress  of the zlib member:   1 ms
;;
;; Every timing test also verifies the round trip, so a
;; fast-but-wrong compressor cannot pass. This file joins the
;; nightly MINO_TEST_EXCLUDE seam (the gc-fuzz lane) in the same
;; change it lands.

(def ^:private perf-vocab
  ["time" "person" "year" "way" "day" "thing" "world" "life" "hand"
   "part" "child" "eye" "woman" "place" "work" "week" "case" "point"
   "government" "company" "number" "group" "problem" "fact" "water"
   "money" "month" "book" "school" "word" "business" "issue" "side"
   "kind" "head" "house" "service" "friend" "father" "power" "hour"
   "game" "line" "member" "city" "community" "name" "team" "minute"
   "idea" "level" "office" "health" "history" "party" "result"
   "change" "morning" "reason" "research" "moment" "teacher" "music"
   "market" "sense" "nation" "plan" "college" "interest" "experience"
   "effect" "class" "control" "care" "field" "development" "heart"])

(defn- perf-lcg
  "Deterministic LCG; long coercion keeps the multiply in fixnums
  (a bigint seed would make byte-array reject the index)."
  [s]
  (long (mod (+ (* s 1103515245) 12345) 2147483648)))

(defn- perf-text-section
  "Word-stream text, the natural-language shape (~9 KiB per 1000
  words)."
  [words seed]
  (let [n (count perf-vocab)]
    (loop [i 0, s seed, acc (transient [])]
      (if (= i words)
        (str/join " " (persistent! acc))
        (let [s1 (perf-lcg s)]
          (recur (inc i) s1
                 (conj! acc (nth perf-vocab
                                (long (mod (quot s1 6553) n))))))))))

(defn- perf-noise-section
  "LCG byte noise, the incompressible shape."
  [n seed]
  (byte-array (loop [i 0, s seed, acc (transient [])]
                (if (= i n)
                  (persistent! acc)
                  (let [s1 (perf-lcg s)]
                    (recur (inc i) s1
                           (conj! acc (mod (quot s1 7919) 256))))))))

(def ^:private perf-corpus
  ;; Transient int accumulation; no lazy map/concat/repeat chains at
  ;; this size (the 6000-entry test-writing rule).
  (let [acc (transient [])]
    (doseq [b (map int (perf-text-section 90000 7))] (conj! acc b))
    (doseq [b (perf-noise-section 262144 13)] (conj! acc b))
    (dotimes [_ 98304] (conj! acc 65))
    (doseq [b (perf-noise-section 65536 29)] (conj! acc b))
    (byte-array (persistent! acc))))

(def ^:private perf-corpus-size (count perf-corpus))

(defn- perf-ms
  "Run (f), return elapsed milliseconds."
  [f]
  (let [t0 (nano-time)
        r  (f)]
    [(quot (- (nano-time) t0) 1000000) r]))

(deftest perf-corpus-is-megabyte-scale
  (is (> perf-corpus-size 1000000) (str "corpus is " perf-corpus-size " bytes")))

(deftest perf-gzip-compress-within-budget
  (let [[ms gz] (perf-ms #(gzip-compress perf-corpus {:level 6}))]
    (is (= perf-corpus (gzip-decompress gz)) "round trip stays correct")
    (is (< ms 1500) (str "gzip-compress 1 MiB took " ms "ms"))))

(deftest perf-deflate-compress-within-budget
  (let [[ms raw] (perf-ms #(deflate-compress perf-corpus {:level 6}))]
    (is (= perf-corpus (deflate-decompress raw)) "round trip stays correct")
    (is (< ms 1500) (str "deflate-compress 1 MiB took " ms "ms"))))

(deftest perf-zlib-compress-within-budget
  (let [[ms z] (perf-ms #(zlib-compress perf-corpus {:level 6}))]
    (is (= perf-corpus (zlib-decompress z)) "round trip stays correct")
    (is (< ms 1500) (str "zlib-compress 1 MiB took " ms "ms"))))

(deftest perf-gzip-decompress-within-budget
  (let [gz (gzip-compress perf-corpus {:level 6})
        [ms out] (perf-ms #(gzip-decompress gz))]
    (is (= perf-corpus out) "round trip stays correct")
    (is (< ms 500) (str "gzip-decompress 1 MiB took " ms "ms"))))

(deftest perf-zlib-decompress-within-budget
  (let [z (zlib-compress perf-corpus {:level 6})
        [ms out] (perf-ms #(zlib-decompress z))]
    (is (= perf-corpus out) "round trip stays correct")
    (is (< ms 500) (str "zlib-decompress 1 MiB took " ms "ms"))))

(run-tests-and-exit)
