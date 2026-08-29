(require "tests/test")

;; ---------------------------------------------------------------------------
;; Capability metadata as documentation (cycle G0.5).
;;
;; Each install group tags its primitives with a per-state capability
;; label (NULL for the always-installed core). The label is reachable
;; from script side via (mino-capability 'sym) and surfaces in
;; (clojure.repl/doc sym) as a trailing "Capability: :group" line.
;;
;; Crucially this is descriptive, not prescriptive — the gate lives
;; at install time in C, not at call time. User code can't strip the
;; metadata to gain access because the fn either exists in the env or
;; doesn't.
;; ---------------------------------------------------------------------------

(deftest mino-capability-by-group
  (is (= :io   (mino-capability 'slurp)))
  (is (= :io   (mino-capability 'spit)))
  (is (= :io   (mino-capability 'exit)))
  (is (= :fs   (mino-capability 'mkdir-p)))
  (is (= :fs   (mino-capability 'file-exists?)))
  (is (= :proc (mino-capability 'sh)))
  (is (= :proc (mino-capability 'sh!)))
  ;; json / csv are gated data libraries, not floor prims: their reader
  ;; prims carry the capability tag like every other install group.
  (is (= :json (mino-capability 'json-parse)))
  (is (= :csv  (mino-capability 'csv-parse)))
  ;; The remaining pure-data libraries moved out of the floor too: each
  ;; prim reports its own install group.
  (is (= :time     (mino-capability 'now)))
  (is (= :time     (mino-capability 'parse-time)))
  (is (= :digest   (mino-capability 'sha256)))
  (is (= :digest   (mino-capability 'crc32)))
  (is (= :html     (mino-capability 'html-parse)))
  (is (= :xml      (mino-capability 'xml-parse)))
  (is (= :yaml     (mino-capability 'yaml-parse)))
  (is (= :toml     (mino-capability 'toml-parse)))
  (is (= :compress (mino-capability 'gzip-compress)))
  (is (= :archive  (mino-capability 'zip-entries)))
  (is (= :term     (mino-capability 'terminal-width)))
  (is (= :path     (mino-capability 'path-join)))
  (is (= :codec    (mino-capability 'percent-encode)))
  (is (= :codec    (mino-capability 'base64-encode))))

(deftest mino-capability-nil-for-core
  ;; Core primitives carry no capability label.
  (is (nil? (mino-capability 'inc)))
  (is (nil? (mino-capability '+)))
  (is (nil? (mino-capability 'println)))   ; println is io_core (not gated)
  (is (nil? (mino-capability 'prn)))
  (is (nil? (mino-capability 'conj))))

(deftest mino-capability-nil-for-unknown
  (is (nil? (mino-capability 'this-does-not-exist))))

(deftest mino-capability-throws-on-non-symbol
  (is (thrown? (mino-capability "slurp")))
  (is (thrown? (mino-capability :slurp)))
  (is (thrown? (mino-capability 1))))

(require '[clojure.repl :refer [doc-string]])

(deftest doc-includes-capability-line-for-gated
  (let [s (doc-string 'slurp)]
    (is (string? s))
    (is (clojure.string/includes? s "Capability: :io"))))

(deftest doc-omits-capability-line-for-core
  (let [s (doc-string 'inc)]
    (is (string? s))
    (is (not (clojure.string/includes? s "Capability:")))))

(deftest doc-string-handles-fs-group
  (let [s (doc-string 'mkdir-p)]
    (is (string? s))
    (is (clojure.string/includes? s "Capability: :fs"))))

(deftest doc-string-prefixes-with-newline-after-real-doc
  ;; Regression: a docstring + capability renders the capability on a
  ;; new line preceded by two spaces, NOT inline at the end of the
  ;; previous sentence. Catches future changes to the prefix shape.
  (let [s (doc-string 'slurp)]
    (is (clojure.string/includes? s "\n  Capability: :io"))))

(run-tests-and-exit)
