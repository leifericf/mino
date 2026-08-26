(require "tests/test")
(require '[clojure.edn :as edn])
(require '[clojure.string :as str])

;; Zip read-side goldens (compression-zip campaign p3, ADR 29).
;;
;; The oracle archives under tests/fixtures/zip/ are zipfile-made or
;; hand-crafted bytes whose expected entry vectors live in the EDN
;; manifest, DERIVED from python's own infolist read: names decode
;; exactly as python decodes them (UTF-8 on bit 11, CP437 otherwise,
;; the D6 rule), sizes and CRCs come from the central directory, and
;; :mtime is the decoded DOS time/date word, nil at the DOS minimum
;; (D5). The manifest is the golden; regeneration diffs empty.
;;
;; Golden split (R4): the archives pin the DECODE side only. mino's
;; own writer bytes are pinned in p4's self-frozen goldens, never
;; against these archives.
;;
;; D8 is pinned here deliberately: duplicate names resolve to the
;; FIRST central-directory match in archive order, the recorded
;; divergence from python's getinfo (which yields the last).

(def ^:private zip-fx-dir "tests/fixtures/zip/")

(defn- zip-fixture
  "Read a binary fixture as bytes (the base64 pair round-trips the
  raw slurp bytes losslessly)."
  [name]
  (base64-decode (base64-encode (slurp (str zip-fx-dir name)))))

(def ^:private zip-manifest
  (edn/read-string (slurp (str zip-fx-dir "manifest.edn"))))

(def ^:private zip-golden-archives
  ["basic.zip" "utf8.zip" "cp437.bin" "mojibake.bin" "zip64.bin"
   "descriptor.zip" "methods.zip" "dup.zip" "overlap.bin"
   "encrypted.bin" "bomb.bin"])

(defn- zip-section [name]
  (get zip-manifest (keyword (subs name 0 (- (count name) 4)))))

(defn- zip-kind
  "Run thunk; return :ok on success or the thrown :mino/kind."
  [thunk]
  (try (do (thunk) :ok) (catch e (:mino/kind e))))

(defn- zip-sha-hex [b]
  (hex-encode (sha256 b)))

;;; the entry vectors, pinned to the oracle manifest

(deftest zip-entries-matches-the-oracle-manifests
  (doseq [name zip-golden-archives]
    (is (= (vec (:entries (zip-section name)))
           (zip-entries (zip-fixture name)))
        (str name " entry vector in archive order"))))

(deftest zip-entries-empty-archive-lists-nothing
  ;; An EOCD with zero entries is a valid empty archive; built by
  ;; hand here (22 bytes, all counts zero).
  (let [eocd (byte-array [0x50 0x4b 0x05 0x06
                          0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0])]
    (is (= [] (zip-entries eocd)))))

;;; zip-read against the pinned content hashes

(deftest zip-read-returns-the-pinned-bytes
  (doseq [name zip-golden-archives
          [member sha] (:contents (zip-section name))]
    (is (= sha (zip-sha-hex (zip-read (zip-fixture name) member)))
        (str name " member " member))))

(deftest zip-read-resolves-duplicate-names-to-the-first-entry
  ;; D8: the first central-directory match wins. The manifest's
  ;; :contents for dup.zip pins the FIRST payload's hash; python's
  ;; getinfo would yield the second (the recorded divergence).
  (let [dup (zip-fixture "dup.zip")
        entries (:entries (zip-section "dup.zip"))]
    (is (= (get (:contents (zip-section "dup.zip")) "same.txt")
           (zip-sha-hex (zip-read dup "same.txt"))))
    (is (not= (:size (second entries)) (:size (first entries)))
        "the two duplicate entries carry different sizes")))

(deftest zip-read-directory-entry-returns-empty-bytes
  (is (= (byte-array 0) (zip-read (zip-fixture "basic.zip") "dir/"))))

;;; cross-tool decode interop (self-skipping, R4: interop not bytes)

(deftest python3-zipfile-agrees-on-every-readable-member
  ;; python3 reads the zipfile-made archives' members back and both
  ;; sides must agree on the decoded bytes. The hand-crafted specials
  ;; stay out (the bomb would allocate 4 GiB under python, the
  ;; encrypted entry raises, and methods 12/14 are readable by python
  ;; but not by mino -- the interop claim is only over what both
  ;; sides read).
  (if (zero? (:exit (sh "sh" "-c" "command -v python3")))
    (let [{:keys [exit out]}
          (sh "sh" "-c"
              (str "cd tests/fixtures/zip && python3 -c '"
                   "import hashlib,sys,zipfile"
                   ";names=[\"basic.zip\",\"utf8.zip\",\"descriptor.zip\"]"
                   ";[print(n,z.filename,hashlib.sha256(zf.read(z)).hexdigest())"
                   " for n in names"
                   " for zf in [zipfile.ZipFile(n)]"
                   " for z in zf.infolist()]'"))]
      (is (zero? exit) (str "python3 cross-check failed: " out))
      (doseq [line (str/split-lines out)]
        (when-not (or (nil? line) (= "" line))
          (let [[archive member sha] (str/split line #" ")]
            (is (= sha (zip-sha-hex
                        (zip-read (zip-fixture archive) member)))
                (str archive " member " member " agrees with python"))))))
    (println "zip: python3 absent -- cross-check skipped")))

;;; ---- the adversarial matrix (p3t4) ----

;; Zip read is untrusted input (ADR 29): every mutation below must
;; classify into the :codec family or return a correct value; never
;; :internal, never a crash, never a hang. Archives are crafted in
;; test code (bytes are immutable; every mutation rebuilds the
;; array), so the layouts are byte-known: one stored entry puts the
;; LOC at [0,30), the CDH at [30+5+12, ...), the EOCD last.

(def ^:private zip-kinds
  "The full zip-side family; every member is hit by this matrix."
  #{:codec/truncated :codec/magic :codec/crc :codec/corrupt
    :codec/limit :codec/missing :codec/unsupported})

(defn- zip-w16 [v] [(bit-and v 0xff) (bit-shift-right v 8)])

(defn- zip-w32 [v] (mapv #(bit-and (bit-shift-right v (* % 8)) 0xff) (range 4)))

(defn- zip-mk-loc
  "One stored local header plus payload. dos-time/dos-date default to
  the DOS minimum."
  ([name data] (zip-mk-loc name data 0 0 0 0x21))
  ([name data flags method dos-time dos-date]
   (byte-array
    (concat (zip-w32 0x04034b50) (zip-w16 20) (zip-w16 flags)
            (zip-w16 method) (zip-w16 dos-time) (zip-w16 dos-date)
            (zip-w32 (crc32 data)) (zip-w32 (count data))
            (zip-w32 (count data)) (zip-w16 (count name)) (zip-w16 0)
            (map int name) (seq data)))))

(defn- zip-mk-cdh
  "One central directory header; size-overrides build the adversarial
  shapes (lying sizes, wrong CRC)."
  ([name data loc-ofs] (zip-mk-cdh name data loc-ofs 0 0 0 0x21 nil nil))
  ([name data loc-ofs flags method dos-time dos-date uncomp crc]
   (let [u (if (nil? uncomp) (count data) uncomp)
         c (if (nil? crc) (crc32 data) crc)]
     (byte-array
      (concat (zip-w32 0x02014b50) (zip-w16 20) (zip-w16 20)
              (zip-w16 flags) (zip-w16 method) (zip-w16 dos-time)
              (zip-w16 dos-date) (zip-w32 c) (zip-w32 (count data))
              (zip-w32 u) (zip-w16 (count name)) (zip-w16 0) (zip-w16 0)
              (zip-w16 0) (zip-w16 0) (zip-w32 0) (zip-w32 loc-ofs)
              (map int name))))))

(defn- zip-mk-zip
  "Assemble LOCs, CDHs (archive order), and the EOCD."
  [entries]
  (let [locs (map (fn [{:keys [name data flags method]}]
                    (zip-mk-loc name data (or flags 0) (or method 0) 0 0x21))
                  entries)
        offsets (reductions + 0 (map count locs))
        cdhs (map (fn [{:keys [name data flags method uncomp crc]} ofs]
                    (zip-mk-cdh name data ofs (or flags 0) (or method 0)
                                0 0x21 uncomp crc))
                  entries (butlast offsets))
        cd (apply concat (map seq cdhs))
        cd-size (count cd)
        cd-ofs (apply + (map count locs))]
    (byte-array
     (concat (apply concat (map seq locs)) cd
             (zip-w32 0x06054b50) (zip-w16 0) (zip-w16 0)
             (zip-w16 (count entries)) (zip-w16 (count entries))
             (zip-w32 cd-size) (zip-w32 cd-ofs) (zip-w16 0)))))

(def ^:private zip-adv-payload (byte-array (map int "adversarial payload\n")))

(def ^:private zip-adv-archive
  ;; One stored entry, byte-known layout: the LOC spans [0,55), the
  ;; CDH [55,106), the EOCD [106,128).
  (zip-mk-zip [{:name "a.txt" :data zip-adv-payload}]))

(defn- zip-flip-at [b i]
  (byte-array (map-indexed (fn [j v] (if (= j i) (bit-xor v 1) v)) (seq b))))

(deftest zip-mutations-never-escape-the-family
  ;; Every single-byte mutation of the crafted archive -- across every
  ;; LOC, CDH, and EOCD boundary, payload included -- classifies or
  ;; returns a value. Nothing :internal, ever.
  (doseq [i (range (count zip-adv-archive))]
    (let [m (zip-flip-at zip-adv-archive i)]
      (is (contains? (conj zip-kinds :ok) (zip-kind #(zip-entries m)))
          (str "zip-entries mutation at " i))
      (is (contains? (conj zip-kinds :ok) (zip-kind #(zip-read m "a.txt")))
          (str "zip-read mutation at " i)))))

(deftest zip-truncations-are-classified
  ;; Cutting the tail anywhere before the EOCD completes loses the
  ;; record: a zip-shaped stream truncates (the D9 truncated/magic
  ;; split resolved by the EOCD scan plus the leading LOC signature;
  ;; see decisions.edn).
  (doseq [cut [55 70 105 112]]
    (is (contains? #{:codec/truncated :codec/corrupt :codec/magic}
                     (zip-kind #(zip-entries (byte-array (take cut (seq zip-adv-archive))))))
          (str "truncation at " cut)))
  ;; Cutting inside the EOCD's own 22 bytes leaves a partial record
  ;; with its signature: truncated.
  (is (= :codec/truncated
         (zip-kind #(zip-entries (byte-array (take 112 (seq zip-adv-archive)))))))
  ;; Flattening the EOCD signature (at 106) of an otherwise-intact zip
  ;; also reports truncation, not "not a zip": the stream starts with
  ;; a local header, so the tail was cut or damaged.
  (is (= :codec/truncated
         (zip-kind #(zip-entries (zip-flip-at zip-adv-archive 106))))))

(deftest zip-garbage-is-magic
  (is (= :codec/magic (zip-kind #(zip-entries (byte-array (map int "definitely not a zip")))))))

(deftest zip-crc-corruption-is-crc
  ;; A lying CRC in the central directory survives listing (the CD is
  ;; the listing's source) and fails the read with the CRC kind: the
  ;; vendor verifies CRC-32 on every extract.
  (let [bad-crc (zip-mk-zip [{:name "a.txt" :data zip-adv-payload
                              :crc 0xDEADBEEF}])]
    (is (= :ok (zip-kind #(zip-entries bad-crc))))
    (is (= :codec/crc (zip-kind #(zip-read bad-crc "a.txt"))))))

(deftest zip-loc-corruption-is-corrupt
  ;; Damaging the local header's signature leaves the listing intact
  ;; (it reads only the central directory) and fails the read at the
  ;; LOC/CDH coherence check.
  (let [bad-loc (zip-flip-at zip-adv-archive 0)]
    (is (= :ok (zip-kind #(zip-entries bad-loc))))
    (is (= :codec/corrupt (zip-kind #(zip-read bad-loc "a.txt"))))))

(deftest zip-encrypted-and-exotic-methods-are-unsupported
  (is (= :codec/unsupported
         (zip-kind #(zip-read (zip-fixture "encrypted.bin") "secret.txt"))))
  (is (= :codec/unsupported
         (zip-kind #(zip-read (zip-fixture "methods.zip") "bzip2.txt"))))
  (is (= :codec/unsupported
         (zip-kind #(zip-read (zip-fixture "methods.zip") "lzma.txt")))))

(deftest zip-absent-name-is-missing
  (is (= :codec/missing
         (zip-kind #(zip-read (zip-fixture "basic.zip") "no-such-entry.txt"))))
  (is (= :codec/missing
         (zip-kind #(zip-read zip-adv-archive "A.TXT")))))

(deftest zip-bomb-throws-limit-without-allocating
  ;; The declared-size cap fires before any inflation: the throw's own
  ;; diagnostics are the only allocation. The bound is relative to a
  ;; control throw (same ex-info machinery, tiny archive): a capless
  ;; read would allocate the ~4 GiB declared output and blow far past
  ;; it; 8 KiB of slack covers the longer limit message.
  (let [alloc-around (fn [thunk]
                       (let [b (:bytes-alloc (gc-stats))]
                         (zip-kind thunk)
                         (- (:bytes-alloc (gc-stats)) b)))
        control (alloc-around #(zip-read zip-adv-archive "absent.txt"))
        bomb (alloc-around #(zip-read (zip-fixture "bomb.bin") "bomb.bin"))]
    (is (= :codec/limit
           (zip-kind #(zip-read (zip-fixture "bomb.bin") "bomb.bin"))))
    (is (< bomb (+ control 8192))
        (str "bomb read allocated " bomb " bytes, control " control))))

(deftest zip-traversal-names-round-trip-verbatim
  ;; "../" and absolute names come back exactly as stored: zip-read
  ;; returns bytes and touches no filesystem, so traversal is inert
  ;; BY CONSTRUCTION. A future fs-layer prim MUST re-litigate this
  ;; before ever materializing entry names as paths (R8).
  (let [up (byte-array (map int "up and out"))
        abs (byte-array (map int "absolute path"))
        trav (zip-mk-zip [{:name "../evil.txt" :data up}
                          {:name "/abs/root.txt" :data abs}
                          {:name "C:\\windows\\style.txt" :data abs}])
        names (mapv :name (zip-entries trav))]
    (is (= ["../evil.txt" "/abs/root.txt" "C:\\windows\\style.txt"] names))
    (is (= up (zip-read trav "../evil.txt")))
    (is (= abs (zip-read trav "/abs/root.txt")))))

(deftest zip-zero-date-word-reads-as-nil-mtime
  (let [loc (zip-mk-loc "z.txt" zip-adv-payload 0 0 0 0)
        cdh (zip-mk-cdh "z.txt" zip-adv-payload 0 0 0 0 0 nil nil)
        z (byte-array
           (concat (seq loc) (seq cdh)
                   (zip-w32 0x06054b50) (zip-w16 0) (zip-w16 0)
                   (zip-w16 1) (zip-w16 1) (zip-w32 (count cdh))
                   (zip-w32 (count loc)) (zip-w16 0)))]
    (is (nil? (:mtime (first (zip-entries z)))))))

(deftest zip-mojibake-name-matches-python
  ;; Raw UTF-8 without bit 11 decodes CP437 on both sides; the
  ;; expectation IS the manifest value, python's own decode of those
  ;; bytes (0xC3 maps to the box-drawing character, not Latin-1).
  (let [moji (first (:entries (zip-section "mojibake.bin")))]
    (is (= [(:name moji)] (mapv :name (zip-entries (zip-fixture "mojibake.bin")))))
    (is (= (:contents (zip-section "mojibake.bin"))
           {(str (:name moji)) (zip-sha-hex
                                (zip-read (zip-fixture "mojibake.bin")
                                          (:name moji)))}))))

(deftest zip-matrix-covers-every-distinct-kind
  ;; The named adversarial cases hit all seven members of the family;
  ;; each kind is a distinct value (nothing collapses, nothing
  ;; escapes to :internal).
  (let [garbage (zip-kind #(zip-entries (byte-array (map int "not a zip"))))
        cut (zip-kind #(zip-entries
                        (byte-array (take 110 (seq zip-adv-archive)))))
        lying-crc (zip-kind #(zip-read
                              (zip-mk-zip [{:name "a.txt"
                                            :data zip-adv-payload
                                            :crc 0xDEADBEEF}])
                              "a.txt"))
        bad-loc (zip-kind #(zip-read (zip-flip-at zip-adv-archive 0) "a.txt"))
        bomb (zip-kind #(zip-read (zip-fixture "bomb.bin") "bomb.bin"))
        absent (zip-kind #(zip-read (zip-fixture "basic.zip") "absent"))
        enc (zip-kind #(zip-read (zip-fixture "encrypted.bin") "secret.txt"))
        hit-kinds #{garbage cut lying-crc bad-loc bomb absent enc}]
    (is (= zip-kinds hit-kinds))
    (is (= 7 (count hit-kinds)) "every kind distinct")))

(run-tests-and-exit)
