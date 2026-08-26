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

(run-tests-and-exit)
