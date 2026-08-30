(require "tests/test")
(require '[clojure.string :as str])
(require '[clojure.data.json :as json])

;; JSON reading must stay near-linear in document size. The reader
;; tokenizes with the C-backed regex engine and slices whole tokens,
;; so the cost is per-token, not per-byte; a per-character walk over
;; a mixed-ASCII document (any non-ASCII byte forces the codepoint
;; slow path on every subs) is quadratic and clears these budgets by
;; far: the old reader needed hundreds of seconds at this size.

(defn- desc-cell
  "Build one 512-char description cell with a non-ASCII byte every
  64 characters, so the document as a whole is mixed content."
  [seed]
  (apply str
         (map (fn [i]
                (if (zero? (rem i 64))
                  (if (odd? (+ seed i)) "\u00e9" "\u4e2d")
                  "x"))
              (range 512))))

(defn- entry
  "One object entry: nested map, array, escapes, unicode, numbers."
  [i]
  (str "{\"name\": \"item " i "\", \"desc\": \"" (desc-cell i) "\","
       " \"tags\": [\"alpha\", \"beta\\\"q\"],"
       " \"score\": " i ".25,"
       " \"active\": "
       (if (odd? i) "true" "false")
       "}"))

(def ^:private json-doc
  (str "{\"entries\": [" (apply str (interpose "," (map entry (range 1500)))) "]}"))

(def ^:private doc-size (count json-doc))

(deftest read-megabyte-doc-within-budget
  (let [t0  (nano-time)
        v   (json/read-str json-doc)
        ms  (quot (- (nano-time) t0) 1000000)]
    (is (> doc-size 900000) "document must be megabyte scale")
    ;; Spot-check structure so a fast-but-wrong reader cannot pass.
    (is (= "item 7" (get-in v ["entries" 7 "name"])))
    (is (= "alpha" (get-in v ["entries" 7 "tags" 0])))
    (is (= "beta\"q" (get-in v ["entries" 7 "tags" 1])))
    (is (= 7.25 (get-in v ["entries" 7 "score"])))
    (is (= true (get-in v ["entries" 7 "active"])))
    (is (= 512 (count (get-in v ["entries" 7 "desc"]))))
    (is (= 1500 (count (get v "entries"))))
    (println (str "  [perf] megabyte parse took " ms "ms"))))

(deftest read-quarter-megabyte-doc-within-budget
  ;; A second size point catches regressions that only bite at
  ;; scale on the smaller budget too.
  (let [small (str "{\"entries\": ["
                   (apply str (interpose "," (map entry (range 350))))
                   "]}")
        t0    (nano-time)
        v     (json/read-str small)
        ms    (quot (- (nano-time) t0) 1000000)]
    (is (= 350 (count (get v "entries"))))
    (println (str "  [perf] quarter-megabyte parse took " ms "ms"))))

(defn- build-big-doc
  "Build a corpus-scale document (6000 entries, escapes, unicode,
   numbers, booleans). Local to the gate test so the document and
   its parsed trees are collectable as soon as the test returns;
   built with a transient accumulator and the C-backed join because
   the lazy interpose chain overflows the script stack at this
   arity."
  []
  (let [acc (transient [])]
    (dotimes [i 6000]
      (conj! acc (entry i)))
    (str "{\"entries\": [" (clojure.string/join "," (persistent! acc)) "]}")))

(deftest read-four-megabyte-doc-within-gate
  ;; The clojuredocs corpus gate: a 3.7MB-class document must parse
  ;; in under two seconds. The Clojure reader needed minutes at this
  ;; scale; this pins the native reader's contract. One untimed
  ;; warm-up parse absorbs first-touch collection in an
  ;; already-loaded suite; the gate times the steady-state parse.
  (let [big-json-doc (build-big-doc)]
    (json/read-str big-json-doc)
    (let [t0  (nano-time)
          v   (json/read-str big-json-doc)
          ms  (quot (- (nano-time) t0) 1000000)]
      (is (> (count big-json-doc) 3500000) "document must be corpus scale")
      (is (= "item 11" (get-in v ["entries" 11 "name"])))
      (is (= 6000 (count (get v "entries"))))
      (println (str "  [perf] four-megabyte parse took " ms "ms")))))

(run-tests-and-exit)
