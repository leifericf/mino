(require "tests/test")
(require '[clojure.string :as str])
(require '[mino.tasks.builtin :as builtin])

;; Byte-identity guard for the two stdlib bundlers. The bootstrap
;; Makefile escapes each bundled source through src/bundle.awk; the
;; incremental `./mino task build` escapes it through
;; gen-stdlib-headers in mino.tasks.builtin. Both emit
;; src/<c-symbol>.h, and the runtime links whichever one built the
;; binary, so their output must agree byte for byte. bundle.awk's
;; header comment asserts that contract in prose; this lane turns it
;; into a check. If either emitter's banner, escaping, or trailing
;; framing drifts, the diff surfaces here rather than as a
;; build-path-dependent binary.
;;
;; The representative source carries backslashes, double quotes, and
;; enough lines to exercise the char-by-char escaping both emitters
;; implement. bundle.awk stays in the tree for the mino-free
;; bootstrap path; this test is its only automated cross-check.

(def ^:private rep-src "lib/clojure/string.clj")
(def ^:private rep-sym "lib_clojure_string")
(def ^:private rep-hdr (str "src/" rep-sym ".h"))

(defn- awk-header
  "The header bundle.awk emits for the representative source, captured
   from stdout exactly as the Makefile recipe invokes it."
  []
  (let [r (sh "awk" "-v" (str "sym=" rep-sym) "-v" (str "src=" rep-src)
              "-f" "src/bundle.awk" rep-src)]
    (when-not (zero? (:exit r))
      (throw (ex-info "bundle.awk failed" {:result r})))
    (:out r)))

(defn- clj-header
  "The header gen-stdlib-headers emits for the same source. Deleting
   the target makes the emitter regenerate it (its staleness check
   treats a missing header as stale), so this reads the live output of
   the clj bundler rather than a stale on-disk copy."
  []
  (when (file-exists? rep-hdr)
    (rm-rf rep-hdr))
  (builtin/gen-stdlib-headers)
  (slurp rep-hdr))

(deftest bundlers-agree-byte-for-byte
  (let [a (awk-header)
        c (clj-header)]
    (is (= a c)
        "src/bundle.awk and gen-stdlib-headers must emit identical bytes")))

(run-tests-and-exit)
