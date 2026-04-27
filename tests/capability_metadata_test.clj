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
  (is (= :proc (mino-capability 'sh!))))

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
