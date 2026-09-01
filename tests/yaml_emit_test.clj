(require "tests/test")
(require '[mino.yaml :as yaml])
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; YAML emitter: generate-string turns plain data back into YAML text
;; the native reader accepts, so parse-string(generate-string(x)) = x.
;;
;; Scalar quoting is the design surface. Strings that would resolve as
;; a non-string plain scalar (true/null/numbers, and the 1.1 ambiguity
;; set yes/no/on/off for downstream 1.1 consumers) are single-quoted;
;; strings with control characters are double-quoted with escapes;
;; multiline strings become literal blocks with the right chomping.
;; Keyword keys emit as plain names, nested maps and vectors emit in
;; block style by default, and {:flow true} emits flow style.
;; Unrepresentable values (functions, collection keys) throw a
;; classified :yaml/emit diagnostic, never a silent coercion.

(defn- emit-kind
  ":mino/kind of the diagnostic generate-string throws, or :no-throw."
  [x]
  (let [r (try (do (yaml/generate-string x) :no-throw)
               (catch Throwable e (:mino/kind e)))]
    r))

(defn- emit-opts-kind
  [x opts]
  (let [r (try (do (yaml/generate-string x opts) :no-throw)
               (catch Throwable e (:mino/kind e)))]
    r))

(defn- round-trips?
  "parse-string(generate-string(x)) equals x."
  [x]
  (= x (yaml/parse-string (yaml/generate-string x))))

;;;; Top-level scalars

(deftest yaml-emit-nil
  (is (= "null\n" (yaml/generate-string nil))))

(deftest yaml-emit-booleans
  (is (= "true\n" (yaml/generate-string true)))
  (is (= "false\n" (yaml/generate-string false))))

(deftest yaml-emit-integers
  (is (= "42\n" (yaml/generate-string 42)))
  (is (= "-7\n" (yaml/generate-string -7)))
  (is (= "9223372036854775807\n"
         (yaml/generate-string 9223372036854775807))))

(deftest yaml-emit-doubles
  (is (= "1.5\n" (yaml/generate-string 1.5)))
  (is (= "-0.25\n" (yaml/generate-string -0.25))))

(deftest yaml-emit-non-finite-doubles
  (is (= ".inf\n" (yaml/generate-string ##Inf)))
  (is (= "-.inf\n" (yaml/generate-string ##-Inf)))
  (is (= ".nan\n" (yaml/generate-string ##NaN))))

(deftest yaml-emit-plain-string
  (is (= "hello\n" (yaml/generate-string "hello")))
  (is (= "hello world\n" (yaml/generate-string "hello world"))))

(deftest yaml-emit-keyword-value-as-name
  ;; clj-yaml surface: keyword values emit as their name (documented
  ;; lossy direction; they read back as strings).
  (is (= "debug\n" (yaml/generate-string :debug)))
  (is (= "a/b\n" (yaml/generate-string :a/b))))

;;;; String quoting rules

(deftest yaml-emit-quotes-norway-problem-words
  ;; The 1.2 core schema reads these as strings, but 1.1 consumers do
  ;; not; quoting keeps the output unambiguous everywhere.
  (is (= "'no'\n" (yaml/generate-string "no")))
  (is (= "'yes'\n" (yaml/generate-string "yes")))
  (is (= "'on'\n" (yaml/generate-string "on")))
  (is (= "'Off'\n" (yaml/generate-string "Off"))))

(deftest yaml-emit-quotes-resolvable-scalars
  (is (= "'true'\n" (yaml/generate-string "true")))
  (is (= "'False'\n" (yaml/generate-string "False")))
  (is (= "'null'\n" (yaml/generate-string "null")))
  (is (= "'~'\n" (yaml/generate-string "~"))))

(deftest yaml-emit-quotes-number-like-strings
  (is (= "'23'\n" (yaml/generate-string "23")))
  (is (= "'017'\n" (yaml/generate-string "017")))
  (is (= "'1.5'\n" (yaml/generate-string "1.5")))
  (is (= "'-5'\n" (yaml/generate-string "-5"))))

(deftest yaml-emit-quotes-empty-string
  (is (= "''\n" (yaml/generate-string ""))))

(deftest yaml-emit-quotes-indicator-strings
  (is (= "'a: b'\n" (yaml/generate-string "a: b")))
  (is (= "'#comment'\n" (yaml/generate-string "#comment")))
  (is (= "'[x]'\n" (yaml/generate-string "[x]")))
  (is (= "'{x}'\n" (yaml/generate-string "{x}"))))

(deftest yaml-emit-single-quote-doubling
  (is (= "'it''s'\n" (yaml/generate-string "it's"))))

(deftest yaml-emit-double-quotes-control-chars
  (is (= "\"x\\ty\"\n" (yaml/generate-string "x\ty")))
  (is (= "\"x\\ry\"\n" (yaml/generate-string "x\ry")))
  (is (= "\"a\\u0001b\"\n" (yaml/generate-string "a\u0001b"))))

(deftest yaml-emit-literal-block-clipped
  ;; Ends with exactly one newline: clip chomping.
  (is (= "|\n  line1\n  line2\n"
         (yaml/generate-string "line1\nline2\n"))))

(deftest yaml-emit-literal-block-stripped
  ;; No trailing newline: strip chomping.
  (is (= "|-\n  line1\n  line2\n"
         (yaml/generate-string "line1\nline2"))))

(deftest yaml-emit-literal-block-interior-blank-line
  (is (= "|\n  l1\n\n  l2\n"
         (yaml/generate-string "l1\n\nl2\n"))))

(deftest yaml-emit-multiline-with-trailing-space-double-quoted
  ;; A line ending in a space cannot ride a literal block; fall back
  ;; to double quotes.
  (is (= "\"a \\nb\"\n" (yaml/generate-string "a \nb"))))

;;;; Maps and vectors, block style

(deftest yaml-emit-flat-map
  (is (= "a: 1\nb: x\n" (yaml/generate-string {:a 1 :b "x"}))))

(deftest yaml-emit-nested-map
  (is (= "a:\n  b: 1\nc: 2\n"
         (yaml/generate-string {:a {:b 1} :c 2}))))

(deftest yaml-emit-vector-under-key
  (is (= "a:\n  - 1\n  - 2\n" (yaml/generate-string {:a [1 2]}))))

(deftest yaml-emit-empty-collections-inline
  (is (= "{}\n" (yaml/generate-string {})))
  (is (= "[]\n" (yaml/generate-string [])))
  (is (= "a: {}\nb: []\n" (yaml/generate-string {:a {} :b []}))))

(deftest yaml-emit-top-level-vector
  (is (= "- 1\n- two\n- true\n"
         (yaml/generate-string [1 "two" true]))))

(deftest yaml-emit-vector-of-maps
  (is (= "- a: 1\n  b: 2\n- c: 3\n"
         (yaml/generate-string [{:a 1 :b 2} {:c 3}]))))

(deftest yaml-emit-nested-vectors
  (is (= "- - 1\n  - 2\n- x\n"
         (yaml/generate-string [[1 2] "x"]))))

(deftest yaml-emit-literal-block-as-map-value
  (is (= "a: |\n  l1\n  l2\nb: 2\n"
         (yaml/generate-string {:a "l1\nl2\n" :b 2}))))

(deftest yaml-emit-deep-nesting
  (is (= "a:\n  b:\n    - c: 1\n"
         (yaml/generate-string {:a {:b [{:c 1}]}}))))

;;;; Keys

(deftest yaml-emit-keyword-keys-as-plain-names
  (is (= "key: 1\n" (yaml/generate-string {:key 1}))))

(deftest yaml-emit-namespaced-keyword-key
  (is (= "a/b: 1\n" (yaml/generate-string {:a/b 1}))))

(deftest yaml-emit-string-key-with-space
  (is (= "a b: 1\n" (yaml/generate-string {"a b" 1}))))

(deftest yaml-emit-quoted-key-when-name-resolves
  ;; A key whose name would resolve as a non-string scalar is quoted;
  ;; quoted keys still keywordize on the way back.
  (is (= "'true': 1\n" (yaml/generate-string {(keyword "true") 1})))
  (is (= "'23': 1\n" (yaml/generate-string {(keyword "23") 1}))))

(deftest yaml-emit-scalar-keys
  (is (= "23: x\n" (yaml/generate-string {23 "x"})))
  (is (= "1.5: x\n" (yaml/generate-string {1.5 "x"})))
  (is (= "true: x\n" (yaml/generate-string {true "x"})))
  (is (= "~: x\n" (yaml/generate-string {nil "x"}))))

;;;; Flow style

(deftest yaml-emit-flow-map
  (is (= "{a: 1, b: [1, 2]}\n"
         (yaml/generate-string {:a 1 :b [1 2]} {:flow true}))))

(deftest yaml-emit-flow-vector
  (is (= "[1, two, {a: 1}]\n"
         (yaml/generate-string [1 "two" {:a 1}] {:flow true}))))

(deftest yaml-emit-flow-quotes-multiline
  ;; No literal blocks inside flow; multiline strings double-quote.
  (is (= "{a: \"l1\\nl2\"}\n"
         (yaml/generate-string {:a "l1\nl2"} {:flow true}))))

(deftest yaml-emit-flow-round-trips
  (let [x {:a 1 :b [1 "two" {:c "x, y"}] :d {:e nil}}]
    (is (= x (yaml/parse-string (yaml/generate-string x {:flow true}))))))

;;;; Errors: unrepresentable values and bad opts

(deftest yaml-emit-rejects-function-values
  (is (= :yaml/emit (emit-kind (fn [x] x)))))

(deftest yaml-emit-rejects-collection-keys
  ;; Complex keys are out of the reader's subset; emitting one would
  ;; write a document the reader rejects.
  (is (= :yaml/emit (emit-kind {{:a 1} 2})))
  (is (= :yaml/emit (emit-kind {[1 2] "x"}))))

(deftest yaml-emit-rejects-non-map-opts
  (is (= :yaml/opts (emit-opts-kind {:a 1} 5))))

(deftest yaml-emit-rejects-non-boolean-flow
  (is (= :yaml/opts (emit-opts-kind {:a 1} {:flow "yes"}))))

;;;; Round-trip goldens

(deftest yaml-emit-round-trip-goldens
  (is (round-trips? {:a 1 :b "x" :c nil :d true}))
  (is (round-trips? {:a {:b [1 2 {:c "d"}]}}))
  (is (round-trips? [{:name "a" :vals [1.5 -2.5E-8]} {:name "b"}]))
  (is (round-trips? {:msg "it's\na \"quoted\" line\n"}))
  (is (round-trips? {:no "no" :on "off" :n "017"}))
  (is (round-trips? {(keyword "true") "x" (keyword "a b") "y"}))
  (is (round-trips? {23 "x" 1.5 "y" true "z" nil "w"}))
  (is (round-trips? {:inf ##Inf :ninf ##-Inf}))
  (is (round-trips? {:deep {:er {:most [[1] [] {}]}}}))
  (is (round-trips? "  mixed \t controls")))

(deftest yaml-emit-string-keys-round-trip-without-keywordizing
  (let [x {"a b" 1 "true" "x"}]
    (is (= x (yaml/parse-string (yaml/generate-string x)
                                {:keywords false})))))

;;;; Round-trip property over json-shaped data

(def ^:private trials 60)

(defn- one-of* [gens]
  (gen/bind (gen/elements gens) identity))

;; NaN never compares equal to itself, so the equality property draws
;; every double but NaN; the golden above covers the .nan spelling.
(def ^:private yaml-double-gen
  (gen/such-that (fn [d] (not (NaN? d))) gen/double))

(def ^:private yaml-leaf-gen
  (one-of* [(gen/return nil)
            gen/boolean
            gen/int
            yaml-double-gen
            gen/string]))

(def ^:private yaml-key-gen
  (gen/fmap keyword (gen/not-empty gen/string-alphanumeric)))

(defn- small-map-gen [key-gen val-gen]
  (gen/fmap (fn [pairs] (into {} pairs))
            (gen/vector (gen/tuple key-gen val-gen) 0 4)))

(defn- yaml-value-gen [depth]
  (if (zero? depth)
    yaml-leaf-gen
    (one-of* [yaml-leaf-gen
              (gen/vector (yaml-value-gen (dec depth)) 0 4)
              (small-map-gen yaml-key-gen
                             (yaml-value-gen (dec depth)))])))

(deftest yaml-emit-round-trip-property
  (let [p (prop/for-all [x (yaml-value-gen 2)]
            (= x (yaml/parse-string (yaml/generate-string x))))
        r (tc/quick-check trials p :seed 92026)]
    (is (true? (:result r)) (pr-str r))))

(deftest yaml-emit-flow-round-trip-property
  (let [p (prop/for-all [x (yaml-value-gen 2)]
            (= x (yaml/parse-string
                  (yaml/generate-string x {:flow true}))))
        r (tc/quick-check trials p :seed 92027)]
    (is (true? (:result r)) (pr-str r))))

(run-tests-and-exit)
