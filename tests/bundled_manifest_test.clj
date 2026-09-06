(require "tests/test")
(require '[clojure.string :as str])
(require '[mino.path :as path])

;; Manifest parity guard. src/bundled.list is the single source of
;; truth for the bundled stdlib (the bootstrap Makefile and
;; builtin.clj's bundled-stdlib both derive from it). A namespace can
;; be listed in the manifest, have its header generated, and still
;; never reach a running binary if nothing calls
;; mino_register_bundled_lib for it -- exactly the gap that hid the
;; bundled http server namespace from installed binaries with no lib/
;; on disk. This lane fails the moment a manifest entry has no
;; corresponding C registration, so the drift cannot ship.

(defn- manifest-symbols
  "The lib_<...>_src C symbol derived from every manifest source path.
   c-symbol drops .clj and turns each non-alphanumeric character into
   _, matching both bundlers; _src is the static-source variable the
   register call reads."
  []
  (->> (str/split-lines (slurp "src/bundled.list"))
       (map str/trim)
       (remove #(or (str/blank? %) (str/starts-with? % "#")))
       (map #(first (str/split % #"\s+")))
       (map (fn [p]
              (-> p
                  (str/replace #"\.clj$" "")
                  (str/replace #"[^A-Za-z0-9]" "_")
                  (str "_src"))))))

(defn- c-source-blob
  "The concatenated text of every src/**/*.c file, read once."
  []
  (->> (path/glob "src/**/*.c")
       (map slurp)
       (str/join "\n")))

(deftest manifest-has-symbols
  ;; A live manifest with a plausible size, so a parse regression that
  ;; silently empties the list cannot make the parity check vacuous.
  (let [syms (manifest-symbols)]
    (is (> (count syms) 50))
    (is (some #(= % "lib_mino_http_server_src") syms))))

(deftest every-manifest-symbol-is-referenced
  (let [blob (c-source-blob)
        missing (remove #(str/includes? blob %) (manifest-symbols))]
    (is (empty? missing)
        (str "bundled.list entries with no src/**/*.c reference "
             "(header generated but never registered): "
             (str/join ", " missing)))))

(run-tests-and-exit)
