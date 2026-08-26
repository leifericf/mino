(require "tests/test")
(require '[mino.zip :as mzip])
(require '[clojure.zip :as czip])

;; mino.zip facade pins (ADR 29).
;;
;; The facade is THIN by design: entries, read, and write alias the
;; floor zip prims, docstrings are the contract, there is no second
;; entry shape, and nothing touches clojure.zip (the namespace is
;; mino.zip over the "archive" domain; lib/clojure/zip.clj stays
;; the clojure.zip everyone else knows).
;;
;; The docstring examples evaluated below are the design's literal
;; code blocks (ADR 29, "Namespace and entry maps"): the alias pairs
;; and the bb-idiom write vector
;; [{:name "report.csv" :data csv} {:name "plot.png" :data png}].

(def ^:private zipf-csv "a,b\n1,2\n")

(def ^:private zipf-png
  ;; deterministic pseudo-png bytes: a seeded LCG over 300 bytes
  (let [n 300]
    (loop [i 0 seed 20260826 acc (transient [])]
      (if (= i n)
        (byte-array (persistent! acc))
        (let [s (long (mod (+ (* seed 1103515245) 12345) 2147483648))]
          (recur (inc i) s (conj! acc (mod (quot s 65536) 256))))))))

(def ^:private zipf-archive
  ;; the design's literal bb-idiom vector, written through the facade
  (mzip/write [{:name "report.csv" :data zipf-csv}
              {:name "plot.png" :data zipf-png}]))

(defn- zipf-kind
  "Run thunk; return :ok on success or the thrown :mino/kind."
  [thunk]
  (try (do (thunk) :ok) (catch e (:mino/kind e))))

;;; the design's alias pairs, pinned as equalities on real data

(deftest zipf-entries-aliases-zip-entries
  (is (= (zip-entries zipf-archive) (mzip/entries zipf-archive))))

(deftest zipf-read-aliases-zip-read
  (is (= (zip-read zipf-archive "report.csv")
         (mzip/read zipf-archive "report.csv")))
  (is (= (zip-read zipf-archive "report.csv" {:max-bytes 4096})
         (mzip/read zipf-archive "report.csv" {:max-bytes 4096}))))

(deftest zipf-write-aliases-zip-write
  (let [entries [{:name "report.csv" :data zipf-csv}
                 {:name "plot.png" :data zipf-png}]]
    (is (= (zip-write entries) (mzip/write entries)))
    (is (= (zip-write entries {:zip64 true})
           (mzip/write entries {:zip64 true})))))

;;; the docstring examples, evaluated as written

(deftest zipf-write-example-round-trips
  ;; (mzip/write [{:name "report.csv" :data csv} {:name "plot.png"
  ;;  :data png}]) => the archive bytes; deterministic on re-call
  (is (bytes? zipf-archive))
  (is (= zipf-archive
         (mzip/write [{:name "report.csv" :data zipf-csv}
                     {:name "plot.png" :data zipf-png}]))))

(deftest zipf-entries-example-lists-archive-order
  ;; (mzip/entries archive) => the entry maps in archive order; the
  ;; read-side key set is exactly the eight documented keys
  (let [names (mapv :name (mzip/entries zipf-archive))]
    (is (= ["report.csv" "plot.png"] names))
    (is (= #{:name :size :compressed-size :crc32 :method :mtime
             :directory? :comment}
           (set (keys (first (mzip/entries zipf-archive))))))))

(deftest zipf-read-example-returns-the-written-bytes
  ;; (mzip/read archive "report.csv") => the exact bytes written;
  ;; string :data contributes UTF-8 bytes (the digest.c rule)
  (is (= (byte-array (map int zipf-csv))
         (mzip/read zipf-archive "report.csv")))
  (is (= zipf-png (mzip/read zipf-archive "plot.png"))))

(deftest zipf-read-throws-missing-on-an-absent-name
  (is (= :codec/missing
         (zipf-kind #(mzip/read zipf-archive "absent.txt")))))

;;; the deterministic defaults and the zip64 hatch, through the facade

(deftest zipf-write-defaults-are-deterministic
  ;; defaults {:method :deflate :level 6 :mtime 0} (mapped to the DOS
  ;; minimum 1980-01-01): entries read back :mtime nil
  (let [es [{:name "a.txt" :data (byte-array (map int "abc"))}]]
    (is (= (mzip/write es) (mzip/write es)))
    (is (nil? (:mtime (first (mzip/entries (mzip/write es))))))))

(deftest zipf-zip64-hatch-forces-locator-structures
  ;; {:zip64 true} forces always-zip64 output; asserted by scanning
  ;; for the zip64 EOCD and locator signatures, never by trusting
  ;; structure offsets (the A4 rule)
  (let [b (mzip/write [{:name "z.txt" :data (byte-array 8)}]
                     {:zip64 true})
        bs (vec (seq b))
        n (count bs)
        hit? (fn [sig]
               (loop [i 0]
                 (if (> i (- n 4))
                   false
                   (if (= sig (subvec bs i (+ i 4)))
                     true (recur (inc i))))))]
    (is (hit? [0x50 0x4b 0x06 0x06]))       ; zip64 EOCD
    (is (hit? [0x50 0x4b 0x06 0x07]))))     ; zip64 locator

;;; the surface cap: thin means exactly three documented publics

(deftest zipf-surface-is-exactly-entries-read-write
  (is (= #{'entries 'read 'write} (set (keys (ns-publics 'mino.zip))))))

(deftest zipf-every-public-carries-a-docstring
  (doseq [v (vals (ns-publics 'mino.zip))]
    (is (string? (:doc (meta v))) (str (:name (meta v)) " undocumented"))))

(deftest zipf-clojure-zip-is-untouched
  ;; mino.zip interns nothing from clojure.zip: the surface cap above
  ;; pins the three publics, and clojure.zip's own vars stay where
  ;; they were (lib/clojure/zip.clj, the clojure.zip namespace)
  (is (contains? (ns-publics 'clojure.zip) 'zipper))
  (is (fn? czip/zipper))
  (is (not (contains? (ns-publics 'mino.zip) 'zipper))))

(run-tests-and-exit)
