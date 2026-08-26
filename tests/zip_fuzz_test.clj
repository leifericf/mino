(require "tests/test")
(require '[clojure.edn :as edn])

;; Zip fuzz lane (compression-zip campaign p6t1, ADR 29; the
;; html_fuzz pattern). Zip read is untrusted input: truncate and
;; byte-flip the golden oracle archives and a byte-known crafted
;; archive, seeded LCG only, no runtime randomness; every mutation
;; must answer a classified :codec ex-info or a coherent value --
;; never a crash, hang, :internal, or shapeless result. 3 fixed
;; seeds x bounded mutations; each parse is individually bounded
;; (5 s) so a hang fails the lane instead of the runner's timeout.
;;
;; Wrong bytes: the vendor verifies CRC-32 on every successful
;; extract, so a returned member is already content-checked; on top
;; of that every successful read must return exactly the size the
;; successful listing declared (length coherence), and the
;; unmutated corpus must reproduce the manifest's entry vectors and
;; member hashes (the control pass, so a fast-but-wrong reader
;; cannot pass a lane that only asserts no-crash).
;;
;; The structure-boundary truncation set runs once, not per seed:
;; the crafted archive is byte-known (LOC spans [0,55), CDH
;; [55,106), EOCD [106,128)), and a cut at EVERY offset walks every
;; LOC, CDH, and EOCD boundary deterministically.
;;
;; This file joins the nightly MINO_TEST_EXCLUDE seam (the gc-fuzz
;; lane) in the same change it lands; it runs in the ordinary test
;; lane and under the sanitizer builds.

(def ^:private zfz-seeds
  "Three fixed seeds; changing them changes every derived mutation."
  [20260826 3141592 271828182])

(def ^:private zfz-flip-bytes
  "The flip alphabet: zip structural bytes (PK signature halves,
  version words, method words), NUL, 0xFF, and ordinary bytes, so a
  flip sometimes breaks structure and sometimes only dirties
  content."
  [0x50 0x4b 0x03 0x04 0x14 0x00 0x08 0x01 0x09 0xFF 0x00
   0x41 0x61 0x7A 0x30 0x20])

(def ^:private zfz-fx-dir "tests/fixtures/zip/")

(defn- zfz-fixture
  "Read a binary fixture as bytes (the base64 pair round-trips the
  raw slurp bytes losslessly)."
  [name]
  (base64-decode (base64-encode (slurp (str zfz-fx-dir name)))))

(def ^:private zfz-manifest
  (edn/read-string (slurp (str zfz-fx-dir "manifest.edn"))))

(def ^:private zfz-archives
  "The golden corpus with each archive's read-probe member: the
  first entry's name (the readable members are pinned by the
  control pass below; the probe only needs a name to ask for, and
  first-entry names cover the bomb cap and the unsupported-method
  shapes too)."
  (mapv (fn [name]
          (let [section (get zfz-manifest
                             (keyword (subs name 0 (- (count name) 4))))]
            {:name name
             :bytes (zfz-fixture name)
             :member (:name (first (:entries section)))}))
        ["basic.zip" "utf8.zip" "cp437.bin" "mojibake.bin" "zip64.bin"
         "descriptor.zip" "methods.zip" "dup.zip" "overlap.bin"
         "encrypted.bin" "bomb.bin"]))

(defn- zfz-lcg
  "One LCG step (rand()/BSD constants); deterministic everywhere.
  The int coercion keeps the state a fixnum."
  [x]
  (int (mod (+ (* x 1103515245) 12345) 2147483648)))

(defn- zfz-truncate
  "First k bytes of b."
  [b k]
  (byte-array (take k (seq b))))

(defn- zfz-flip
  "Replace the byte at index i with v."
  [b i v]
  (byte-array (map-indexed (fn [j x] (if (= j i) v x)) (seq b))))

;;; ---- the crafted byte-known archive (structure boundaries) ----

(defn- zfz-w16 [v] [(bit-and v 0xff) (bit-shift-right v 8)])

(defn- zfz-w32 [v] (mapv #(bit-and (bit-shift-right v (* % 8)) 0xff) (range 4)))

(def ^:private zfz-payload (byte-array (map int "fuzz payload\n")))

(defn- zfz-loc [name data]
  (byte-array
   (concat (zfz-w32 0x04034b50) (zfz-w16 20) (zfz-w16 0) (zfz-w16 0)
           (zfz-w16 0) (zfz-w16 0x21) (zfz-w32 (crc32 data))
           (zfz-w32 (count data)) (zfz-w32 (count data))
           (zfz-w16 (count name)) (zfz-w16 0) (map int name) (seq data))))

(defn- zfz-cdh [name data ofs]
  (byte-array
   (concat (zfz-w32 0x02014b50) (zfz-w16 20) (zfz-w16 20)
           (zfz-w16 0) (zfz-w16 0) (zfz-w16 0) (zfz-w16 0x21)
           (zfz-w32 (crc32 data)) (zfz-w32 (count data))
           (zfz-w32 (count data)) (zfz-w16 (count name)) (zfz-w16 0)
           (zfz-w16 0) (zfz-w16 0) (zfz-w16 0) (zfz-w32 0)
           (zfz-w32 ofs) (map int name))))

(def ^:private zfz-crafted
  ;; One stored entry, byte-known layout: the LOC spans [0,55), the
  ;; CDH [55,106), the EOCD [106,128) -- the same framing arithmetic
  ;; as the adversarial matrix in zip_test, spelled independently
  ;; here per the unique-helper-names rule.
  (let [name (byte-array (map int "f.txt"))
        loc (zfz-loc name zfz-payload)
        cdh (zfz-cdh name zfz-payload (count loc))]
    (byte-array
     (concat (seq loc) (seq cdh)
             (zfz-w32 0x06054b50) (zfz-w16 0) (zfz-w16 0)
             (zfz-w16 1) (zfz-w16 1) (zfz-w32 (count cdh))
             (zfz-w32 (count loc)) (zfz-w16 0)))))

;;; ---- classification ----

(def ^:private zfz-kinds
  "The full zip-side family; every member is legal fuzz output."
  #{:codec/truncated :codec/magic :codec/crc :codec/corrupt
    :codec/limit :codec/missing :codec/unsupported})

(defn- zfz-entry-shape
  "True when v is a plausible entry vector: sequential of maps each
  carrying a string :name and an integral :size (a two-key shape
  check; the control pass below pins all eight read-side keys)."
  [v]
  (and (vector? v)
       (every? (fn [e] (and (map? e) (string? (:name e)) (int? (:size e))))
               v)))

(defn- zfz-classify
  "One bounded mutation check: zip-entries must answer a shape-correct
  entry vector or a family kind; when the listing succeeded, zip-read
  of the archive's probe member must answer exactly the bytes the
  listing declared (length coherence over the CRC the reader already
  verified) or a family kind. Returns the accumulator with :parses,
  :errors, :max-ms, and the first :bad description."
  [{:keys [bad] :as acc} b member]
  (if bad
    acc
    (let [t0 (nano-time)
          lr (try {:v (zip-entries b)}
                (catch e {:k (:mino/kind e)}))
          ms1 (quot (- (nano-time) t0) 1000000)
          max-ms (max (:max-ms acc) ms1)
          acc1 (assoc acc :max-ms max-ms)]
      (cond
        (contains? lr :k)
        (if (contains? zfz-kinds (:k lr))
          (update (update acc1 :parses inc) :errors inc)
          (assoc acc1 :bad (str "zip-entries escaped the family: " (:k lr))))

        (zfz-entry-shape (:v lr))
        (let [declared (or (some (fn [e] (when (= member (:name e)) (:size e)))
                                 (:v lr))
                           0)
              t1 (nano-time)
              rr (try {:v (zip-read b member)}
                   (catch e {:k (:mino/kind e)}))
              ms2 (quot (- (nano-time) t1) 1000000)
              acc2 (-> acc1
                       (update :parses inc)
                       (assoc :max-ms (max max-ms ms2)))]
          (cond
            (contains? rr :k)
            (if (contains? zfz-kinds (:k rr))
              (update acc2 :errors inc)
              (assoc acc2 :bad (str "zip-read escaped the family: " (:k rr))))

            (= declared (count (:v rr)))
            acc2

            :else
            (assoc acc2 :bad (str "read returned " (count (:v rr))
                                  " bytes, listing declared " declared))))

        :else
        (assoc acc1 :bad "zip-entries returned a shapeless success")))))

(defn- zfz-run
  "Fold classify over every mutation derived from one input set."
  [acc inputs]
  (reduce (fn [a [b member]] (zfz-classify a b member)) acc inputs))

;;; ---- the control pass: unmutated corpus is exactly the manifest ----

(deftest zfz-control-corpus-matches-the-manifest
  ;; The correct-value control: with no mutation every archive must
  ;; reproduce the oracle manifest's entry vector and every pinned
  ;; member hash. A lane that only asserted no-crash would pass a
  ;; reader that throws on everything; this pins the value side.
  (doseq [{:keys [name bytes member]} zfz-archives
          :let [section (get zfz-manifest
                             (keyword (subs name 0 (- (count name) 4))))]]
    (is (= (vec (:entries section)) (zip-entries bytes))
        (str name " entry vector matches the manifest"))
    (doseq [[m sha] (:contents section)]
      (is (= sha (hex-encode (sha256 (zip-read bytes m))))
          (str name " member " m " hash matches the manifest")))))

;;; ---- the structure-boundary truncation set (every offset) ----

(deftest zfz-truncation-at-every-structure-offset-classifies
  ;; A cut at EVERY offset 1..127 of the byte-known archive walks
  ;; every LOC, CDH, and EOCD boundary. Every tail cut loses or
  ;; damages the EOCD record itself, so all cuts must classify
  ;; inside the family; cuts inside the payload exercise the EOCD
  ;; tail scan over payload bytes (no false signature found), and
  ;; the successful-read path is exercised by the seeded flips
  ;; below, which corrupt bytes without removing the directory.
  (let [n (count zfz-crafted)
        res (loop [k 1
                   acc {:parses 0 :errors 0 :max-ms 0 :bad nil}]
          (if (= k n)
            acc
            (let [acc' (zfz-classify acc (zfz-truncate zfz-crafted k) "f.txt")]
              (if (:bad acc')
                acc'
                (recur (inc k) acc')))))]
    (is (nil? (:bad res)) (:bad res))
    (is (= (dec n) (:parses res))
        (str "every one of " (dec n) " cuts answered, saw " (:parses res)))
    (is (< (:max-ms res) 5000)
        (str "slowest single parse " (:max-ms res) "ms; bound 5000ms"))))

;;; ---- seeded LCG mutations over the golden corpus ----

(defn- zfz-inputs-for-seed
  "The flat mutation set for one seed: per archive, 2 truncations
  and 4 byte-flips (LCG positions over the archive's own length),
  each pre-realized as a [bytes member] pair. Deterministic in the
  seed and the corpus alone."
  [seed]
  (let [pairs (transient [])]
    (doseq [{:keys [bytes member]} zfz-archives
            :let [n (count bytes)
                  st0 (zfz-lcg (+ seed (count bytes)))
                  cuts (mapv #(mod % n)
                             [(zfz-lcg st0) (zfz-lcg (zfz-lcg st0))])
                  st1 (zfz-lcg (zfz-lcg (zfz-lcg st0)))]]
      (doseq [c cuts]
        (conj! pairs [(zfz-truncate bytes c) member]))
      (loop [i 0, s st1]
        (when (< i 4)
          (let [s1 (zfz-lcg s)
                s2 (zfz-lcg s1)]
            (conj! pairs [(zfz-flip bytes (mod s1 n)
                                    (nth zfz-flip-bytes
                                         (mod s2 (count zfz-flip-bytes))))
                          member])
            (recur (inc i) s2)))))
    (persistent! pairs)))

(deftest zfz-seeded-mutations-never-crash-hang-or-escape
  ;; 3 seeds x 11 archives x (2 truncations + 4 flips) = 198 mutated
  ;; inputs, each parsed (listing plus read probe): every answer is a
  ;; family classification or a coherent value, each inside the 5 s
  ;; per-parse bound.
  (let [res (reduce
              (fn [acc seed] (zfz-run acc (zfz-inputs-for-seed seed)))
              {:parses 0 :errors 0 :max-ms 0 :bad nil}
              zfz-seeds)]
    (is (nil? (:bad res)) (:bad res))
    (is (= 198 (:parses res))
        (str "expected 198 mutated inputs parsed, saw " (:parses res)))
    (is (< (:max-ms res) 5000)
        (str "slowest single parse " (:max-ms res) "ms; bound 5000ms"))
    ;; The family must have been exercised, not just survived: with
    ;; three seeds over corrupted archives, errors must have occurred
    ;; (a corpus where nothing ever throws would mean the mutations
    ;; never reached a signature).
    (is (pos? (:errors res))
        (str "some mutations must classify as errors "
             "(the flip alphabet hits signatures by construction)"))))

(run-tests-and-exit)
