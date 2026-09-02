(require "tests/test")
(require '[clojure.edn :as edn])

;; Tar read-side and extract-hardening goldens.
;;
;; The archives under tests/fixtures/tar/ are python3 tarfile-made;
;; the EDN manifest is DERIVED from python's own getmembers read
;; (the oracle), so the fixtures were realism-checked before any
;; reader existed. Entry maps mirror zip-entries where sensible:
;; the fixed key set is {:name :size :mode :mtime :type :linkname},
;; :mode the 07777-masked permission int, :mtime the stored epoch
;; SECONDS (nil when the stored field is zero, the zip DOS-minimum
;; rule's sibling), :type one of :file :dir :symlink :hardlink,
;; :linkname the link target and nil off link entries.
;;
;; tar-entries and tar-read never touch a filesystem, so hostile
;; names are inert data there (the zip R8 rule). tar-extract is the
;; hardened fs path: an absolute name, a ".." traversal component,
;; a link entry whose target escapes the destination, or a write
;; through an already-materialized symlink all throw :codec/unsafe
;; BEFORE anything lands outside the destination.

(def ^:private windows? (some? (getenv "OS")))

(def ^:private tar-fx-dir "tests/fixtures/tar/")

(defn- tar-fixture
  "Read a binary fixture as bytes (the base64 pair round-trips the
  raw slurp bytes losslessly)."
  [name]
  (base64-decode (base64-encode (slurp (str tar-fx-dir name)))))

(def ^:private tar-manifest
  (edn/read-string (slurp (str tar-fx-dir "manifest.edn"))))

(def ^:private tar-listable-archives
  ["basic.tar" "longname.tar" "traversal.tar" "absolute.tar"
   "linktarget.tar" "linkwrite.tar" "hardlink.tar"])

(defn- tar-section [name]
  (get tar-manifest (keyword (subs name 0 (- (count name) 4)))))

(defn- tar-kind
  "Run thunk; return :ok on success or the thrown :mino/kind."
  [thunk]
  (try (do (thunk) :ok) (catch e (:mino/kind e))))

(defn- tar-sha-hex [b]
  (hex-encode (sha256 b)))

;;; the entry vectors, pinned to the oracle manifest

(deftest tar-entries-matches-the-oracle-manifests
  (doseq [name tar-listable-archives]
    (is (= (vec (:entries (tar-section name)))
           (tar-entries (tar-fixture name)))
        (str name " entry vector in archive order"))))

(deftest tar-entry-maps-freeze-their-keys
  ;; The fixed six-key read shape: :linkname is present-and-nil off
  ;; link entries, never absent (the manifest equality above depends
  ;; on it; this pins the key set on its own).
  (doseq [e (tar-entries (tar-fixture "basic.tar"))]
    (is (= #{:name :size :mode :mtime :type :linkname} (set (keys e)))
        (str (:name e) " carries exactly the six documented keys"))))

(deftest tar-entries-empty-archive-lists-nothing
  ;; Two zero blocks are a complete empty archive.
  (is (= [] (tar-entries (byte-array 1024)))))

(deftest tar-pax-long-name-surfaces-the-logical-member
  ;; The 124-char basename travels in a pax path record; the listing
  ;; is the ONE logical member under its full name, never the
  ;; PaxHeader pseudo-entry.
  (let [es (tar-entries (tar-fixture "longname.tar"))]
    (is (= 1 (count es)))
    (is (= 137 (count (:name (first es)))))
    (is (= (:name (first (:entries (tar-section "longname.tar"))))
           (:name (first es))))))

(deftest tar-zero-mtime-field-reads-as-nil
  (let [by-name (into {} (map (juxt :name identity)
                              (tar-entries (tar-fixture "basic.tar"))))]
    (is (nil? (:mtime (get by-name "epoch.txt"))))
    (is (= 1700000000 (:mtime (get by-name "hello.txt"))))))

;;; tar-read against the pinned content hashes

(deftest tar-read-returns-the-pinned-bytes
  (doseq [name tar-listable-archives
          [member sha] (:contents (tar-section name))]
    (is (= sha (tar-sha-hex (tar-read (tar-fixture name) member)))
        (str name " member " member))))

(deftest tar-read-absent-name-is-missing
  (is (= :codec/missing
         (tar-kind #(tar-read (tar-fixture "basic.tar") "no-such.txt"))))
  (is (= :codec/missing
         (tar-kind #(tar-read (tar-fixture "basic.tar") "HELLO.TXT")))))

(deftest tar-hostile-names-are-inert-data-on-the-read-side
  ;; "../" and absolute names come back exactly as stored: reading
  ;; touches no filesystem, so traversal is inert BY CONSTRUCTION
  ;; here and re-litigated by tar-extract below.
  (is (= ["../escaped.txt"]
         (mapv :name (tar-entries (tar-fixture "traversal.tar")))))
  (is (= ["/tmp/mino-tar-abs-evil.txt"]
         (mapv :name (tar-entries (tar-fixture "absolute.tar"))))))

;;; classified parse failures (never :internal, never a crash)

(deftest tar-garbage-is-magic
  (is (= :codec/magic
         (tar-kind #(tar-entries
                     (byte-array (map int "definitely not a tar archive")))))))

(deftest tar-declared-size-past-eof-is-truncated
  ;; sizebomb.tar's header is valid (python parses it) but declares
  ;; ~8 GiB with no payload: walking to the next header runs off the
  ;; end of the data.
  (is (= :codec/truncated
         (tar-kind #(tar-entries (tar-fixture "sizebomb.tar"))))))

(deftest tar-base256-size-overflow-is-corrupt
  ;; size256.tar's GNU base-256 size field carries int64 max; the
  ;; 512-block round-up overflows a signed 64-bit offset and must
  ;; classify, never wrap.
  (is (= :codec/corrupt
         (tar-kind #(tar-entries (tar-fixture "size256.tar"))))))

;;; tar-extract, the golden path

(deftest tar-extract-materializes-the-archive
  ;; POSIX-only: the fixture carries symlink and hardlink entries
  ;; (Windows link creation is privileged; the Windows contract is a
  ;; classified not-supported on link entries, pinned with the impl).
  (when-not windows?
    (with-temp-dir [d]
      (let [names (tar-extract (tar-fixture "basic.tar") d)]
        (is (= ["hello.txt" "epoch.txt" "dir/" "dir/nested.txt"
                "link" "hard.txt"]
               names)
            "extract returns the materialized names in archive order")
        (is (= "hello tar contents\n" (slurp (str d "/hello.txt"))))
        (is (= "nested payload\n" (slurp (str d "/dir/nested.txt"))))
        (let [st (stat (str d "/hello.txt"))]
          (is (= 0644 (:mode st)) "archive mode lands on disk")
          (is (= 1700000000 (quot (:mtime st) 1000))
              "tar's stored seconds are stat's floored milliseconds"))
        (is (= 0600 (:mode (stat (str d "/dir/nested.txt")))))
        (is (= :dir (:type (stat (str d "/dir")))))
        (is (= 0755 (:mode (stat (str d "/dir")))))
        (let [lst (stat (str d "/link"))]
          (is (= :symlink (:type lst)) "symlink entry lands as a link")
          (is (= "hello.txt" (read-symlink (str d "/link")))))
        (is (= "hello tar contents\n" (slurp (str d "/hard.txt")))
            "hardlink to an earlier member carries its bytes")))))

;;; tar-extract, the hardened path: every hostile fixture rejects
;;; with :codec/unsafe and nothing lands outside the destination

(deftest tar-extract-rejects-traversal-names
  (with-temp-dir [d]
    (let [dest (str d "/inner")]
      (mkdir-p dest)
      (is (= :codec/unsafe
             (tar-kind #(tar-extract (tar-fixture "traversal.tar") dest))))
      (is (not (file-exists? (str d "/escaped.txt")))
          "the ../ member never lands in the parent"))))

(deftest tar-extract-rejects-absolute-names
  (try (rm-rf "/tmp/mino-tar-abs-evil.txt") (catch _ nil))
  (with-temp-dir [d]
    (is (= :codec/unsafe
           (tar-kind #(tar-extract (tar-fixture "absolute.tar") d))))
    (is (not (file-exists? "/tmp/mino-tar-abs-evil.txt"))
        "the absolute member never lands at its own path")))

(deftest tar-extract-rejects-an-escaping-symlink-target
  (with-temp-dir [d]
    (let [dest (str d "/inner")]
      (mkdir-p dest)
      (is (= :codec/unsafe
             (tar-kind #(tar-extract (tar-fixture "linktarget.tar") dest)))
          "a linkname that escapes the destination is refused"))))

(deftest tar-extract-never-writes-through-a-materialized-symlink
  ;; sub -> .. then sub/evil.txt: following the link would write
  ;; evil.txt into the destination's PARENT (the copy-tree link
  ;; policy: store the link, never follow it).
  (with-temp-dir [d]
    (let [dest (str d "/inner")]
      (mkdir-p dest)
      (is (= :codec/unsafe
             (tar-kind #(tar-extract (tar-fixture "linkwrite.tar") dest))))
      (is (not (file-exists? (str d "/evil.txt")))
          "nothing lands outside the destination"))))

(deftest tar-extract-rejects-an-escaping-hardlink
  ;; The guard must fire as classification, not as a downstream io
  ;; error from linking a nonexistent outside path.
  (with-temp-dir [d]
    (let [dest (str d "/inner")]
      (mkdir-p dest)
      (is (= :codec/unsafe
             (tar-kind #(tar-extract (tar-fixture "hardlink.tar") dest)))))))

(run-tests-and-exit)
