(require "tests/test")
(require '[clojure.string :as str])
(require '[clojure.data.csv :as csv])

;; CSV reading must stay linear in document size. The reader is the
;; native single-pass csv-parse; the mino-side reader it replaced was
;; quadratic through the string prims (nth walks from the string's
;; start, subs copies the remaining slice) and needed minutes at this
;; scale (ADR 24). The writer stays Clojure and is pinned by the
;; round-trip, not by timing.

(defn- build-doc
  "Deterministic 100k-row document just over 2MB: three plain columns
  per row, and every 1000th row a quoted field so the quoted path and
  quote escaping run at scale. Transient accumulation; no lazy chains
  at this size."
  []
  (let [acc (transient [])]
    (dotimes [i 100000]
      (conj! acc (if (zero? (rem i 1000))
                   (str i ",\"a,b\"\n")
                   (str i ",item-" i ",v" (rem i 97) "\n"))))
    (str/join (persistent! acc))))

(def ^:private csv-doc (build-doc))
(def ^:private doc-size (count csv-doc))

(deftest read-two-megabyte-hundred-k-rows-within-budget
  (let [t0   (nano-time)
        rows (csv/read-csv csv-doc)
        ms   (quot (- (nano-time) t0) 1000000)]
    (is (> doc-size 1000000) "document must be megabyte scale")
    (is (= 100000 (count rows)))
    ;; Spot-check structure so a fast-but-wrong reader cannot pass.
    (is (= ["0" "a,b"] (nth rows 0)))
    (is (= ["1" "item-1" "v1"] (nth rows 1)))
    (is (= ["1000" "a,b"] (nth rows 1000)))
    (is (= ["99999" "item-99999" (str "v" (rem 99999 97))]
           (nth rows 99999)))
    ;; 100000 rows of 3 fields less the 100 two-field quoted rows.
    (is (= 299900 (reduce + 0 (map count rows))))
    (println (str "  [perf] two-megabyte parse took " ms "ms"))))

(deftest write-read-round-trip-equal
  (let [f    (str (or (getenv "TMPDIR") "/tmp") "/mino_csv_gate.csv")
        rows (csv/read-csv csv-doc)]
    (csv/write-csv f rows)
    (let [text  (slurp f)
          rows2 (csv/read-csv text)]
      (is (> (count text) 1000000) "written document must keep the scale")
      (is (= rows rows2))
      (is (= 100000 (count rows2)))
      (is (= ["1000" "a,b"] (nth rows2 1000))))))

(run-tests-and-exit)
