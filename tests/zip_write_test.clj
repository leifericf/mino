(require "tests/test")
(require '[clojure.edn :as edn])
(require '[clojure.string :as str])

;; Zip write-side contracts (compression-zip campaign p4, ADR 29).
;;
;; Golden split (R4): these tests pin mino's OWN writer bytes. The
;; golden archives under tests/fixtures/zip/ (write_golden.zip,
;; write_golden64.zip plus write_golden.edn) are self-frozen
;; determinism bytes: frozen once at the impl commit, diffed
;; thereafter, NEVER compared against gzip(1) or python output.
;; Compressed bodies are mino's own; cross-tool checks are decode
;; interop only (python3 zipfile open and read, unzip -t),
;; self-skipping when a binary is absent.
;;
;; D5 determinism: every entry carries an explicit or clamped
;; :mtime, so the frozen bytes are stable across runs, timezones,
;; and wall-clock time. The :mtime 1 entry is the canary: a plain
;; add_mem anywhere (vendor NULL last_modified stamps NOW) shifts
;; the DOS words and flips the frozen sha. Golden epochs sit
;; mid-day, away from DST transitions (the accepted D5 edge).
;;
;; D6 names: non-ASCII names set bit 11 (read back through the same
;; decoding); ASCII names leave it clear; backslashes rewrite to
;; forward slashes; a leading slash throws :eval/contract
;; (APPNOTE 4.4.17.1).

(def ^:private zw-fx-dir "tests/fixtures/zip/")

(def ^:private zw-payload (byte-array (map int "zip write payload\n")))

(defn- zw-fixture
  "Read a binary fixture as bytes (the base64 pair round-trips the
  raw slurp bytes losslessly)."
  [name]
  (base64-decode (base64-encode (slurp (str zw-fx-dir name)))))

(defn- zw-golden
  "The self-frozen write-golden manifest (lands with the impl
  commit; the sha pins regeneration diffs to empty)."
  [k]
  (k (edn/read-string (slurp (str zw-fx-dir "write_golden.edn")))))

(defn- zw-kind
  "Run thunk; return :ok on success or the thrown :mino/kind."
  [thunk]
  (try (do (thunk) :ok) (catch e (:mino/kind e))))

(defn- zw-sha-hex [b]
  (hex-encode (sha256 b)))

(defn- zw-scan
  "True when the 4-byte signature (a vector of ints) occurs anywhere
  in b. Absence and presence are both asserted BY SCAN, never
  trusted from structure offsets (A4)."
  [b sig]
  (let [bs (vec (seq b))]
    (loop [i 0]
      (if (> i (- (count bs) 4))
        false
        (if (= sig (subvec bs i (+ i 4)))
          true
          (recur (inc i)))))))

(def ^:private zw-zip64-eocd-sig [0x50 0x4b 0x06 0x06])
(def ^:private zw-zip64-locator-sig [0x50 0x4b 0x06 0x07])

;;; the frozen vector: every write-side shape in one archive

(def ^:private zw-golden-entries
  [{:name "hello.txt" :data (byte-array (map int "hello zip write side\n"))}
   {:name "dir/nested.bin" :data (byte-array (range 256)) :level 9}
   {:name "stored.txt" :data (byte-array (repeat 300 65)) :method :store}
   {:name "empty.txt" :data (byte-array 0)}
   {:name "caf\u00e9.txt" :data (byte-array (map int "unicode name"))
    :mtime 1718454896 :comment " Caf\u00e9 comment"}
   {:name "dir/" :data (byte-array 0)}
   {:name "canary.txt" :data (byte-array (map int "mtime one")) :mtime 1}])

(def ^:private zw-golden-names
  ["hello.txt" "dir/nested.bin" "stored.txt" "empty.txt" "caf\u00e9.txt"
   "dir/" "canary.txt"])

(deftest zip-write-golden-bytes-are-frozen
  (is (= (zw-fixture "write_golden.zip") (zip-write zw-golden-entries))
      "byte equality against the self-frozen golden")
  (is (= (zw-golden :sha256)
         (zw-sha-hex (zip-write zw-golden-entries)))
      "the frozen sha pins regeneration to an empty diff"))

(deftest zip-write-forced-zip64-golden-is-frozen
  (is (= (zw-fixture "write_golden64.zip")
         (zip-write zw-golden-entries {:zip64 true})))
  (is (= (zw-golden :zip64-sha256)
         (zw-sha-hex (zip-write zw-golden-entries {:zip64 true})))))

(deftest zip-write-zip64-signatures-by-scan
  ;; Forced zip64 carries the 64-bit EOCD and its locator; the
  ;; default sub-threshold output and the empty archive carry
  ;; neither locator nor 64-bit EOCD (A4).
  (let [forced (zip-write zw-golden-entries {:zip64 true})
        plain (zip-write zw-golden-entries)
        empty (zip-write [])]
    (is (zw-scan forced zw-zip64-eocd-sig))
    (is (zw-scan forced zw-zip64-locator-sig))
    (is (not (zw-scan plain zw-zip64-locator-sig)))
    (is (not (zw-scan plain zw-zip64-eocd-sig)))
    (is (not (zw-scan empty zw-zip64-locator-sig)))
    (is (= 22 (count empty)) "the empty archive is the bare EOCD")))

;;; determinism (D5, A2)

(deftest zip-write-is-deterministic-two-calls
  (is (= (zip-write zw-golden-entries) (zip-write zw-golden-entries)))
  (is (= (zip-write zw-golden-entries {:zip64 true})
         (zip-write zw-golden-entries {:zip64 true})))
  (doseq [l [0 1 6 9]]
    (is (= (zip-write zw-golden-entries {:level l})
           (zip-write zw-golden-entries {:level l}))
        (str "level " l " two-call ="))))

(deftest zip-write-mtime-one-canary-is-time-independent
  ;; :mtime 1 is pre-1980 and clamps to the DOS minimum, so nothing
  ;; in these bytes can vary with the wall clock: two calls equal,
  ;; and equal to the frozen default-write shape. A plain add_mem
  ;; (NULL last_modified, vendor NOW stamp) flips both.
  (let [canary [{:name "canary.txt" :data zw-payload :mtime 1}]]
    (is (= (zip-write canary) (zip-write canary)))
    (is (= (zip-write [{:name "c.txt" :data zw-payload}])
           (zip-write [{:name "c.txt" :data zw-payload :mtime 0}])
           (zip-write [{:name "c.txt" :data zw-payload :mtime nil}])))))

;;; round trip through mino's own reader

(deftest zip-write-round-trips-through-zip-entries-and-read
  (let [ar (zip-write zw-golden-entries)
        entries (zip-entries ar)]
    (is (= zw-golden-names (mapv :name entries)) "archive order preserved")
    (is (= [:deflate :deflate :store :store :deflate :store :deflate]
           (mapv :method entries)))
    (is (= [false false false false false true false]
           (mapv :directory? entries)))
    (is (= [nil nil nil nil 1718454896 nil nil] (mapv :mtime entries))
        "default and clamped mtimes read nil at the DOS minimum")
    (is (= " Caf\u00e9 comment" (:comment (nth entries 4))))
    (is (= "" (:comment (first entries))))
    (doseq [e zw-golden-entries]
      (is (= (:data e) (zip-read ar (:name e)))
          (str (:name e) " bytes survive the round trip")))))

(deftest zip-write-string-data-is-utf-8
  ;; A string :data contributes its UTF-8 bytes (the digest.c rule).
  ;; map over a string walks codepoints, so the expected bytes are
  ;; spelled as a literal vector: h 195 169(é) l l o space z i p.
  (let [ar (zip-write [{:name "s.txt" :data "h\u00e9llo zip"}])]
    (is (= (byte-array [104 195 169 108 108 111 32 122 105 112])
           (zip-read ar "s.txt")))))

(deftest zip-write-non-ascii-name-keeps-store-method
  ;; A name byte >= 0x80 must not leak into the compression-level
  ;; nibble: :method :store stays stored (not silently deflated) and
  ;; the bytes round-trip, whatever the name's encoding.
  (let [payload (byte-array (repeat 300 65))
        ar (zip-write [{:name "café.txt" :data payload
                        :method :store}])
        entries (zip-entries ar)]
    (is (= [:store] (mapv :method entries)))
    (is (= "café.txt" (:name (first entries))))
    (is (= payload (zip-read ar "café.txt"))))
  ;; A requested level survives untouched for a non-ASCII name too.
  (let [payload (byte-array (repeat 300 66))
        a (zip-write [{:name "ü.txt" :data payload :level 4}])
        b (zip-write [{:name "ü.txt" :data payload :level 4}])]
    (is (= a b) "non-ASCII name still encodes deterministically")
    (is (= payload (zip-read a "ü.txt")))))

(deftest zip-write-empty-archive-lists-nothing
  (is (= [] (zip-entries (zip-write [])))))

(deftest zip-write-mtime-clamping
  ;; Pre-1980 clamps up to the DOS minimum (reads nil); the explicit
  ;; DOS-minimum epoch reads nil too (the D5 nil rule is
  ;; byte-symmetric); post-2107 clamps down to the last encodable
  ;; DOS second, 2107-12-31 23:59:58.
  (let [ar (zip-write [{:name "up.txt" :data zw-payload :mtime -1234}
                       {:name "min.txt" :data zw-payload :mtime 315532800}
                       {:name "down.txt" :data zw-payload :mtime 4354819199}
                       {:name "way-down.txt" :data zw-payload
                        :mtime 9999999999}])
        mt (mapv :mtime (zip-entries ar))]
    (is (= [nil nil 4354819198 4354819198] mt))))

;;; name rules (D6, APPNOTE 4.4.17.1)

(deftest zip-write-backslashes-rewrite-to-forward-slashes
  (let [ar (zip-write [{:name "a\\b.txt" :data zw-payload}
                       {:name "C:\\dir\\f.txt" :data zw-payload}])]
    (is (= ["a/b.txt" "C:/dir/f.txt"] (mapv :name (zip-entries ar))))
    (is (= zw-payload (zip-read ar "a/b.txt")))))

(deftest zip-write-leading-slash-is-rejected
  (is (= :eval/contract (zw-kind #(zip-write [{:name "/abs.txt"
                                               :data zw-payload}]))))
  ;; A leading backslash rewrites to a leading slash and rejects too.
  (is (= :eval/contract (zw-kind #(zip-write [{:name "\\abs.txt"
                                               :data zw-payload}])))))

(deftest zip-write-traversal-names-round-trip-verbatim
  ;; "../" names write and read verbatim: the writer emits bytes,
  ;; nothing touches a filesystem, traversal is inert BY
  ;; CONSTRUCTION. A future fs layer MUST re-litigate before ever
  ;; materializing entry names as paths (R8).
  (let [ar (zip-write [{:name "../up.txt" :data zw-payload}])]
    (is (= zw-payload (zip-read ar "../up.txt")))))

;;; the invalid-map matrix

(deftest zip-write-unsupported-methods-throw-unsupported
  (doseq [m [:bzip2 :lzma :weird]]
    (is (= :codec/unsupported
           (zw-kind #(zip-write [{:name "x.txt" :data zw-payload
                                  :method m}])))
        (str ":method " m))))

(deftest zip-write-invalid-entries-throw-contract
  (let [bad (fn [entry] (zw-kind #(zip-write [entry])))]
    ;; :level violations, per-entry and archive-level
    (doseq [l [10 -1 "6" 1.5]]
      (is (= :eval/contract
             (bad {:name "x.txt" :data zw-payload :level l})))
      (is (= :eval/contract
             (zw-kind #(zip-write [{:name "x.txt" :data zw-payload}]
                                  {:level l})))
          (str "opts :level " l)))
    ;; :name violations
    (is (= :eval/contract (bad {:data zw-payload})) "missing :name")
    (is (= :eval/contract (bad {:name 7 :data zw-payload})))
    (is (= :eval/contract (bad {:name "" :data zw-payload})))
    (is (= :eval/contract
           (bad {:name (str "a\u0000b") :data zw-payload}))
        "embedded NUL truncates in the vendor strlen")
    (is (= :eval/contract
           (bad {:name (apply str (repeat 65536 "n")) :data zw-payload}))
        "names past the 16-bit field width")
    ;; :data violations
    (is (= :eval/contract (bad {:name "x.txt"})) "missing :data")
    (is (= :eval/contract (bad {:name "x.txt" :data nil})))
    (is (= :eval/contract (bad {:name "x.txt" :data 5})))
    ;; directory entries cannot carry data
    (is (= :eval/contract (bad {:name "d/" :data zw-payload})))
    ;; :mtime / :comment type violations and field-width ceilings
    (is (= :eval/contract (bad {:name "x.txt" :data zw-payload
                                :mtime "x"})))
    (is (= :eval/contract (bad {:name "x.txt" :data zw-payload
                                :comment 9})))
    (is (= :eval/contract
           (bad {:name "x.txt" :data zw-payload
                 :comment (apply str (repeat 65536 "c"))}))
        "comments past the 16-bit field width")
    ;; :method type violations (the vocabulary is keywords)
    (is (= :eval/contract (bad {:name "x.txt" :data zw-payload
                                :method 8})))
    (is (= :eval/contract (bad {:name "x.txt" :data zw-payload
                                :method "deflate"})))))

(deftest zip-write-argument-surface
  (is (= :eval/arity (zw-kind #(zip-write))))
  (is (= :eval/arity (zw-kind #(zip-write [] {} 1))))
  (is (= :eval/type (zw-kind #(zip-write "not a vector"))))
  (is (= :eval/type (zw-kind #(zip-write '({:name "a" :data (byte-array 0)}))))
      "entries must be a vector")
  (is (= :eval/contract (zw-kind #(zip-write [1 2])))
      "each entry must be a map")
  (is (= :eval/type (zw-kind #(zip-write [] 5))))
  (is (= :eval/contract (zw-kind #(zip-write [] {:zip64 "yes"}))))
  (is (= :eval/contract (zw-kind #(zip-write [] {:level 10})))))

;;; defaults and per-entry overrides

(deftest zip-write-default-level-is-six
  (let [entry {:name "e.txt" :data (byte-array (map int "default level text"))}]
    (is (= (zip-write [entry]) (zip-write [entry] {:level 6}))
        "the archive default is level 6")))

(def ^:private zw-order-vocab
  ["time" "person" "year" "way" "day" "thing" "world" "life" "hand"
   "part" "child" "eye" "woman" "place" "work" "week" "case" "point"
   "government" "company" "number" "group" "problem" "fact" "water"
   "money" "month" "book" "school" "word" "business" "issue"])

(defn- zw-level-corpus
  "Deterministic word-stream corpus (~6 KB). Natural-text match
  structure at distance is what separates the levels; on repetitive
  templates tdefl's levels 1 and 9 tie exactly (the p2 lesson)."
  []
  (let [n (count zw-order-vocab)]
    (loop [i 0, seed 7, first? true, acc (transient [])]
      (if (= i 900)
        (byte-array (persistent! acc))
        (let [seed1 (long (mod (+ (* seed 1103515245) 12345) 2147483648))
              seed2 (long (mod (+ (* seed1 1103515245) 12345) 2147483648))
              w (nth zw-order-vocab (long (mod (quot seed1 6553) n)))
              word (if (zero? (mod i 11)) (str/capitalize w) w)]
          (when-not first? (conj! acc 32))
          (doseq [b (map int word)] (conj! acc b))
          (recur (inc i) seed2 false acc))))))

(deftest zip-write-per-entry-level-overrides-the-archive
  ;; Level changes the deflate stream on a natural-text corpus, and
  ;; the per-entry override wins over the archive default.
  (let [e {:name "e.txt" :data (zw-level-corpus)}]
    (is (not= (count (zip-write [(assoc e :level 1)] {:level 9}))
              (count (zip-write [(assoc e :level 9)] {:level 1}))))
    (is (= (zip-write [(assoc e :level 1)] {:level 9})
           (zip-write [(assoc e :level 1)])))))

;;; cross-tool decode interop (self-skipping, R4: interop not bytes)

(defn- zw-spill
  "Write the archive bytes to path through base64 (spit writes
  strings, not bytes)."
  [ar path]
  (sh "sh" "-c" (str "printf '%s' '" (base64-encode ar) "' | base64 -d > " path)))

(deftest python3-zipfile-reads-mino-written-archives
  ;; The oracle opens fresh mino-written archives -- default AND
  ;; forced-zip64 -- lists names and dates, reads members; both
  ;; sides must agree on bytes, and the caf\u00e9 entry's date_time
  ;; must be the exact UTC civil fields D5 pinned (2024-06-15
  ;; 12:34:56), the real proof the timestamp compensation lands
  ;; regardless of the runner's timezone.
  (if (zero? (:exit (sh "sh" "-c" "command -v python3")))
    (doseq [[ar label] [[(zip-write zw-golden-entries) "default"]
                        [(zip-write zw-golden-entries {:zip64 true})
                         "forced-zip64"]] ]
      (let [path "/tmp/mino_zip_write_py.zip"
            _ (zw-spill ar path)
            {:keys [exit out]}
            (sh "sh" "-c"
                (str "python3 -c '"
                     "import hashlib,zipfile;"
                     "zf=zipfile.ZipFile(\"" path "\");"
                     "[print(z.filename, hashlib.sha256(zf.read(z)).hexdigest(), z.date_time)"
                     " for z in zf.infolist()]"
                     "' 2>&1"))]
        (is (zero? exit) (str "python3 read of " label " failed: " out))
        (doseq [line (str/split-lines out)]
          (when-not (or (nil? line) (= "" line))
            (let [[name sha date] (str/split line #" " 3)]
              (is (= (zw-sha-hex (zip-read ar name)) sha)
                  (str label " " name " agrees with python3"))
              (when (= name "caf\u00e9.txt")
                (is (= "(2024, 6, 15, 12, 34, 56)" (str/trim date))
                    (str label " D5 date_time, got " date))))))
        (sh "sh" "-c" (str "rm -f " path))))
    (println "zip-write: python3 absent -- cross-check skipped")))

(deftest unzip-t-accepts-mino-written-archives
  (if (zero? (:exit (sh "sh" "-c" "command -v unzip")))
    (let [path "/tmp/mino_zip_write_t.zip"]
      (zw-spill (zip-write zw-golden-entries) path)
      (let [{:keys [exit out]}
            (sh "sh" "-c" (str "unzip -t " path))]
        (is (zero? exit) (str "unzip -t rejected the mino archive: " out)))
      (zw-spill (zip-write zw-golden-entries {:zip64 true}) path)
      (let [{:keys [exit out]}
            (sh "sh" "-c" (str "unzip -t " path))]
        (is (zero? exit) (str "unzip -t rejected the forced-zip64 archive: "
                              out)))
      (sh "sh" "-c" (str "rm -f " path)))
    (println "zip-write: unzip absent -- cross-check skipped")))

(run-tests-and-exit)
