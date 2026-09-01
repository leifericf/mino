(require "tests/test")
(require '[mino.toml :as toml])
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; TOML emitter: generate-string turns nested plain maps back into
;; TOML text the native reader accepts, so
;; parse-string(generate-string(x)) = x for representable data.
;;
;; Nested maps become table headers, vectors of maps become arrays of
;; tables, scalar entries precede subtables inside every table body,
;; and keys are bare when they can be and basic-quoted otherwise.
;; TOML has no null and no keyword scalar, so nil values, keyword
;; values, and non keyword-or-string keys throw a classified
;; :toml/emit diagnostic, never a silent coercion.

(defn- toml-emit-kind
  ":mino/kind of the diagnostic generate-string throws, or :no-throw."
  [x]
  (let [r (try (do (toml/generate-string x) :no-throw)
               (catch Throwable e (:mino/kind e)))]
    r))

(defn- toml-emit-opts-kind
  [x opts]
  (let [r (try (do (toml/generate-string x opts) :no-throw)
               (catch Throwable e (:mino/kind e)))]
    r))

(defn- toml-round-trips?
  "parse-string(generate-string(x)) equals x."
  [x]
  (= x (toml/parse-string (toml/generate-string x))))

;;;; Scalars and flat tables

(deftest toml-emit-empty-map
  (is (= "" (toml/generate-string {}))))

(deftest toml-emit-flat-scalars
  (is (= "a = 1\ns = \"x\"\n" (toml/generate-string {:a 1 :s "x"}))))

(deftest toml-emit-booleans
  (is (= "t = true\nf = false\n"
         (toml/generate-string {:t true :f false}))))

(deftest toml-emit-integer-bounds
  (is (= "big = 9223372036854775807\nsmall = -9223372036854775808\n"
         (toml/generate-string {:big 9223372036854775807
                                :small -9223372036854775808}))))

(deftest toml-emit-floats
  (is (= "f = 1.5\n" (toml/generate-string {:f 1.5})))
  (is (= "f = -0.25\n" (toml/generate-string {:f -0.25})))
  (is (= "f = 1.0E10\n" (toml/generate-string {:f 1.0E10}))))

(deftest toml-emit-non-finite-floats
  (is (= "f = inf\n" (toml/generate-string {:f ##Inf})))
  (is (= "f = -inf\n" (toml/generate-string {:f ##-Inf})))
  (is (= "f = nan\n" (toml/generate-string {:f ##NaN})))
  (is (NaN? (:f (toml/parse-string (toml/generate-string {:f ##NaN}))))))

;;;; String escaping

(deftest toml-emit-string-escapes
  (is (= "s = \"q\\\"w\\\\e\"\n"
         (toml/generate-string {:s "q\"w\\e"})))
  (is (= "s = \"a\\nb\\tc\\rd\"\n"
         (toml/generate-string {:s "a\nb\tc\rd"}))))

(deftest toml-emit-control-char-unicode-escape
  (is (= "s = \"a\\u0007b\"\n" (toml/generate-string {:s "a\u0007b"}))))

(deftest toml-emit-non-ascii-raw
  (is (= "s = \"café\"\n" (toml/generate-string {:s "café"}))))

;;;; Keys: bare, quoted, namespaced

(deftest toml-emit-bare-keys
  (is (= "a-b_c9 = 1\n" (toml/generate-string {:a-b_c9 1}))))

(deftest toml-emit-quoted-key-with-space
  (is (= "\"a b\" = 1\n" (toml/generate-string {(keyword "a b") 1}))))

(deftest toml-emit-string-keys-like-keyword-keys
  ;; The reader keywordizes every key, so string and keyword keys
  ;; emit identically.
  (is (= "k = 1\n" (toml/generate-string {"k" 1}))))

(deftest toml-emit-namespaced-keyword-key-quoted
  ;; The slash is not a bare-key character; quoting preserves the
  ;; namespace through the keywordizing reader.
  (is (= "\"a/b\" = 1\n" (toml/generate-string {:a/b 1})))
  (is (= {:a/b 1} (toml/parse-string (toml/generate-string {:a/b 1})))))

;;;; Arrays

(deftest toml-emit-arrays
  (is (= "a = [1, 2, 3]\n" (toml/generate-string {:a [1 2 3]})))
  (is (= "a = []\n" (toml/generate-string {:a []})))
  (is (= "a = [1, \"x\", true]\n"
         (toml/generate-string {:a [1 "x" true]})))
  (is (= "a = [[1], [2]]\n" (toml/generate-string {:a [[1] [2]]}))))

(deftest toml-emit-inline-table-in-mixed-array
  ;; A map in an array that is not all maps rides an inline table.
  (is (= "a = [1, {x = 1}]\n" (toml/generate-string {:a [1 {:x 1}]}))))

;;;; Tables and ordering

(deftest toml-emit-subtable
  (is (= "name = \"n\"\n\n[srv]\nhost = \"h\"\nport = 80\n"
         (toml/generate-string {:name "n"
                                :srv {:host "h" :port 80}}))))

(deftest toml-emit-scalars-before-subtables
  ;; A scalar entry after the subtable in the map still emits first;
  ;; a bare key line after a header would attach to the wrong table.
  (is (= "name = \"n\"\n\n[srv]\nx = 1\n"
         (toml/generate-string {:srv {:x 1} :name "n"}))))

(deftest toml-emit-nested-subtables
  (is (= "[a]\n\n[a.b]\nc = 1\n"
         (toml/generate-string {:a {:b {:c 1}}}))))

(deftest toml-emit-empty-subtable
  (is (= "[e]\n" (toml/generate-string {:e {}}))))

(deftest toml-emit-quoted-header-segment
  (is (= "[\"a b\"]\nc = 1\n"
         (toml/generate-string {(keyword "a b") {:c 1}}))))

(deftest toml-emit-array-of-tables
  (is (= "[[srv]]\nx = 1\n\n[[srv]]\nx = 2\n"
         (toml/generate-string {:srv [{:x 1} {:x 2}]}))))

(deftest toml-emit-array-of-tables-with-subtable
  (is (= "[[srv]]\nx = 1\n\n[srv.sub]\ny = 2\n\n[[srv]]\nx = 2\n"
         (toml/generate-string {:srv [{:x 1 :sub {:y 2}} {:x 2}]}))))

(deftest toml-emit-array-of-empty-tables
  (is (= "[[e]]\n\n[[e]]\n" (toml/generate-string {:e [{} {}]}))))

;;;; Errors: unrepresentable values and bad arguments

(deftest toml-emit-rejects-non-map-top-level
  (is (= :toml/emit (toml-emit-kind 5)))
  (is (= :toml/emit (toml-emit-kind [1 2])))
  (is (= :toml/emit (toml-emit-kind "x"))))

(deftest toml-emit-rejects-nil-values
  ;; TOML has no null; absence is the only spelling of absence.
  (is (= :toml/emit (toml-emit-kind {:a nil})))
  (is (= :toml/emit (toml-emit-kind {:a [1 nil]})))
  (is (= :toml/emit (toml-emit-kind {:a {:b nil}}))))

(deftest toml-emit-rejects-keyword-values
  ;; TOML has no keyword scalar; a name string would read back as a
  ;; different value.
  (is (= :toml/emit (toml-emit-kind {:a :kw})))
  (is (= :toml/emit (toml-emit-kind {:a [:kw]}))))

(deftest toml-emit-rejects-non-keyword-string-keys
  ;; An integer key would read back keywordized, not as the integer.
  (is (= :toml/emit (toml-emit-kind {5 "x"})))
  (is (= :toml/emit (toml-emit-kind {true "x"})))
  (is (= :toml/emit (toml-emit-kind {nil "x"})))
  (is (= :toml/emit (toml-emit-kind {:a {[1] "x"}}))))

(deftest toml-emit-rejects-function-values
  (is (= :toml/emit (toml-emit-kind {:a (fn [x] x)}))))

(deftest toml-emit-rejects-non-map-opts
  (is (= :toml/opts (toml-emit-opts-kind {:a 1} 5))))

;;;; Round-trip goldens

(deftest toml-emit-round-trip-goldens
  (is (toml-round-trips? {}))
  (is (toml-round-trips? {:a 1 :b "x" :c true :d 1.5}))
  (is (toml-round-trips? {:srv {:host "h" :port 80 :tls {:on true}}}))
  (is (toml-round-trips? {:srv [{:x 1} {:x 2 :sub {:y 3}}]}))
  (is (toml-round-trips? {:a [1 2 [3 "x"] {:in "line"}]}))
  (is (toml-round-trips? {(keyword "a b") {(keyword "c.d") 1} :a/b 2}))
  (is (toml-round-trips? {:s "quotes \" and \\ and\nnewlines\tand tabs"}))
  (is (toml-round-trips? {:inf ##Inf :ninf ##-Inf}))
  (is (toml-round-trips? {:date "1979-05-27"}))
  (is (toml-round-trips? {:e {} :f [{} {}] :g []})))

;;;; Round-trip property

(def ^:private toml-trials 60)

(defn- toml-one-of* [gens]
  (gen/bind (gen/elements gens) identity))

;; NaN never compares equal to itself, so the equality property draws
;; every double but NaN; the golden above covers the nan spelling.
(def ^:private toml-double-gen
  (gen/such-that (fn [d] (not (NaN? d))) gen/double))

(def ^:private toml-leaf-gen
  (toml-one-of* [gen/boolean
            gen/int
            toml-double-gen
            gen/string]))

(def ^:private toml-key-gen
  (gen/fmap keyword (gen/not-empty gen/string-alphanumeric)))

(defn- toml-small-map-gen [key-gen val-gen]
  (gen/fmap (fn [pairs] (into {} pairs))
            (gen/vector (gen/tuple key-gen val-gen) 0 4)))

(defn- toml-value-gen [depth]
  (if (zero? depth)
    toml-leaf-gen
    (toml-one-of* [toml-leaf-gen
              (gen/vector (toml-value-gen (dec depth)) 0 4)
              (toml-small-map-gen toml-key-gen
                             (toml-value-gen (dec depth)))])))

(def ^:private toml-doc-gen
  (toml-small-map-gen toml-key-gen (toml-value-gen 2)))

(deftest toml-emit-round-trip-property
  (let [p (prop/for-all [x toml-doc-gen]
            (= x (toml/parse-string (toml/generate-string x))))
        r (tc/quick-check toml-trials p :seed 92028)]
    (is (true? (:result r)) (pr-str r))))

(run-tests-and-exit)
