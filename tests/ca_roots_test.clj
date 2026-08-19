(require "tests/test")
(require '[clojure.string :as str])

;; Vendored CA root data (src/vendor/bearssl/): the PEM snapshot, the
;; generated roots.c, and the README facts must stay in lockstep. No
;; runtime API reads this data yet, so these are file-level guards
;; against drift: edit one artifact without regenerating the others
;; and something here fails.

(def ^:private pem-path "src/vendor/bearssl/mozilla-roots.pem")
(def ^:private roots-path "src/vendor/bearssl/roots.c")
(def ^:private readme-path "src/vendor/bearssl/README.md")

(def ^:private pem-text (slurp pem-path))
(def ^:private roots-text (slurp roots-path))
(def ^:private readme-text (slurp readme-path))

(def ^:private pem-sha (sha256 pem-text))

(defn- recorded-sha [text]
  (let [m (re-find #"PEM sha256:?\s*\n?\s*`?([0-9a-f]{64})`?" text)]
    (second m)))

(defn- pem-anchor-count []
  (count (re-seq #"-----BEGIN CERTIFICATE-----" pem-text)))

(deftest ca-roots-files-exist
  (is (file-exists? pem-path) "mozilla-roots.pem is checked in")
  (is (file-exists? roots-path) "roots.c is checked in")
  (is (file-exists? "src/vendor/bearssl/roots.h")
      "the generated declarations header is checked in")
  (is (file-exists? "src/vendor/bearssl/tools/gen_ca_roots.clj")
      "the generator script is checked in"))

(deftest ca-roots-pem-sha-matches-generated-data
  (is (= pem-sha (second (re-find #"PEM sha256: ([0-9a-f]{64})" roots-text)))
      "roots.c was generated from the checked-in PEM")
  (is (str/includes? roots-text "#include \"roots.h\"")
      "roots.c takes its declarations from the generated header"))

(deftest ca-roots-pem-sha-matches-readme
  (is (= pem-sha (recorded-sha readme-text))
      "README records the sha256 of the checked-in PEM"))

(deftest ca-roots-anchor-count-consistent
  (let [n (pem-anchor-count)]
    (is (> n 100) "Mozilla snapshot carries a realistic anchor set")
    (is (= (str n) (second (re-find #"mino_ca_anchor_count = (\d+);" roots-text)))
        "roots.c table count equals the PEM certificate count")
    (is (= n (count (re-seq #"\{ mino_ca_der_data \+ " roots-text)))
        "roots.c has one table row per PEM certificate")))

(deftest ca-roots-generated-header-carries-date-line
  (let [date (second (re-find #"Mozilla as of: ([^\n\r]+)" pem-text))]
    (is (some? date) "PEM carries the Mozilla snapshot date line")
    (is (str/includes? roots-text (str "Mozilla as of: " date))
        "roots.c header records the same Mozilla snapshot date")))

(run-tests-and-exit)
