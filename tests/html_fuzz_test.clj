(require "tests/test")
(require '[clojure.string :as str])
(require '[clojure.edn :as edn])
(require '[mino.html :as html])
(require '[tests.html-fixture :as hfix])

;; Tolerant HTML fuzz lane (html-xml campaign p6t2, design A-1).
;; Truncate and byte-flip the generated 1MB fixture and the golden
;; corpus, 3 fixed seeds x 400 mutations = 1200 parses, each parse
;; bounded under 5s; assert no crash, no hang, and a tree or a
;; positioned ex-info on every mutation. The mutation engine is
;; seeded arithmetic only (an LCG stepped through the loop, the
;; yaml_perf fixture-generator discipline): deterministic, identical
;; on every run and platform, no runtime randomness.
;;
;; Mutations per seed: 16 fixture truncations over the full length
;; range, 64 fixture byte-flips inside a 128KB prefix (bounded so
;; the lane stays inside the suite budget; truncations already
;; exercise the full-length document), 320 corpus mutations (the
;; golden corpus inputs, truncated and byte-flipped). This file
;; joins the nightly MINO_TEST_EXCLUDE list with the perf gates
;; (the toml/yaml precedent); it runs in the ordinary test lane and
;; under the sanitizer builds.

(def ^:private html-fz-seeds
  "Three fixed seeds; changing them changes every derived mutation."
  [20260826 3141592 271828182])

(def ^:private html-fz-flip-chars
  "The flip alphabet: structural markup bytes, NUL, and a spread of
  ordinary bytes, so a flip sometimes breaks structure and sometimes
  only dirties content."
  "<>/\"'&;!=-\u0000ABCabcxyz0123456789 #")

(def ^:private html-fz-flip-window 131072)

(defn- html-fz-next
  "One LCG step (rand()/BSD constants); deterministic everywhere.
  The int coercion keeps the state a fixnum: mod over the widened
  product stays bigint otherwise, and nth/subs reject bigint
  indices."
  [x]
  (int (mod (+ (* x 1103515245) 12345) 2147483648)))

(defn- html-fz-flip
  "Replaces the byte at index i with c."
  [s i c]
  (str (subs s 0 i) c (subs s (inc i))))

(defn- html-fz-rep
  "n copies of unit by doubling (O(log n) concatenations, each at
  most the final size; no long lazy chains, the p7 suite rule)."
  [n unit]
  (loop [need n buf unit out ""]
    (if (zero? need)
      out
      (if (odd? need)
        (recur (quot need 2) (str buf buf) (str out buf))
        (recur (quot need 2) (str buf buf) out)))))

(def ^:private html-fz-doc (hfix/html-fixture-doc))
(def ^:private html-fz-doc-len (count html-fz-doc))
(def ^:private html-fz-doc-prefix (subs html-fz-doc 0 html-fz-flip-window))

(def ^:private html-fz-corpus
  "The golden corpus inputs (p1t3); fuzzing needs only the strings,
  never the oracle expectations."
  (vec (filter #(string? (:input %))
               (edn/read-string (slurp "tests/fixtures/html/golden.edn")))))

(defn- html-fz-mutation
  "Derives the j-th mutated input for one seed. j < 16: fixture
  truncation. j < 80: fixture byte-flip in the bounded prefix.
  j >= 80: corpus entry (strided index), truncated on even j,
  byte-flipped on odd j."
  [seed j]
  (let [st0 (html-fz-next (+ seed j))
        st1 (html-fz-next st0)
        st2 (html-fz-next st1)]
    (cond
      (< j 16)
      (subs html-fz-doc 0 (mod st0 (dec html-fz-doc-len)))

      (< j 80)
      (html-fz-flip html-fz-doc-prefix
                    (mod st0 html-fz-flip-window)
                    (nth html-fz-flip-chars
                         (mod st1 (count html-fz-flip-chars))))

      :else
      (let [v (nth html-fz-corpus
                   (mod (+ (* j 7919) st0) (count html-fz-corpus)))
            s (:input v)
            n (count s)]
        (if (zero? n)
          s
          (if (even? j)
            (subs s 0 (mod st1 n))
            (html-fz-flip s (mod st1 n)
                          (nth html-fz-flip-chars
                               (mod st2 (count html-fz-flip-chars))))))))))

(defn- html-fz-classify
  "One parse under measurement: a :document map is a tree; anything
  else must be a positioned diagnostic (:mino/kind :html/parse, :code
  keyword, integral :location line and col). Returns the record;
  :bad carries the first offender's description."
  [s]
  (try
    (let [t0 (nano-time)
          t (html/parse s)
          ms (quot (- (nano-time) t0) 1000000)]
      (if (and (map? t) (= :document (:type t)))
        {:parses 1 :errors 0 :max-ms ms :bad nil}
        {:parses 1 :errors 0 :max-ms 0
         :bad (str "non-document result from "
                   (pr-str (subs s 0 (min 40 (count s)))))}))
    (catch e
      (let [d (ex-data e)]
        (if (and (map? d)
                 (= :html/parse (:mino/kind e))
                 (keyword? (:code d))
                 (map? (:location d))
                 (int? (:line (:location d)))
                 (int? (:col (:location d))))
          {:parses 1 :errors 1 :max-ms 0 :bad nil}
          {:parses 1 :errors 1 :max-ms 0
           :bad (str "unpositioned error: " (pr-str d))})))))

(deftest html-fz-mutations-never-crash-or-hang
  ;; 3 seeds x 400 mutations: every parse answers a tree or a
  ;; positioned error, each inside the 5s bound (A-1). Tolerant HTML
  ;; recovers nearly everything, so most parses are trees; the
  ;; interesting assertion is that NONE of them crash, hang, or
  ;; return shapeless results.
  (let [res (reduce
              (fn [acc seed]
                (loop [j 0 acc acc]
                  (if (= j 400)
                    acc
                    (let [r (html-fz-classify (html-fz-mutation seed j))]
                      (if (:bad r)
                        (assoc acc :bad (:bad r))
                        (recur (inc j)
                               (-> acc
                                   (update :parses + (:parses r))
                                   (update :errors + (:errors r))
                                   (assoc :max-ms (max (:max-ms acc)
                                                       (:max-ms r))))))))))
              {:parses 0 :errors 0 :max-ms 0 :bad nil}
              html-fz-seeds)]
    (is (nil? (:bad res)) (:bad res))
    (is (= 1200 (:parses res))
        (str "expected 1200 parses, saw " (:parses res)))
    (is (< (:max-ms res) 5000)
        (str "slowest single parse " (:max-ms res) "ms; bound 5000ms"))
    ;; Tolerant HTML may ALWAYS tree (A-1): zero errors across the
    ;; whole mutation set is the expected outcome -- the tokenizer
    ;; recovers everything these mutations produce, and the only
    ;; non-recovering edges (:max-depth, eof-in-tag drops) are
    ;; exercised by the hostile vectors below and the p2 hand pins.
    (is (>= (:parses res) 1200)
        "every mutation must have parsed or errored, none skipped")))

;;; ---- hostile corpus (A-1: cap-deep nesting, megabyte attributes,
;;; entity floods, NUL-laden text) ----

(defn- html-fz-err-data
  [thunk]
  (try (thunk) :no-throw (catch e (assoc (ex-data e) :mino/kind (:mino/kind e)))))

(defn- html-fz-timed
  "Runs thunk, asserts the elapsed bound, returns [ms value]."
  [thunk]
  (let [t0 (nano-time)
        v (thunk)
        ms (quot (- (nano-time) t0) 1000000)]
    (is (< ms 5000) (str "hostile vector took " ms "ms; bound 5000ms"))
    [ms v]))

(deftest html-fz-hostile-cap-deep-nesting
  ;; 255 open elements parse (fragment mode; the wrappers would
  ;; count in document mode, the p2 A-2 boundary pin); 257 throws
  ;; :max-depth with a position (tier rule 14)
  (let [[_ t] (html-fz-timed #(html/parse-fragment
                                (html-fz-rep 255 "<div>")))]
    (is (vector? t))
    (is (= :div (:tag (nth t 0)))))
  (let [d (html-fz-err-data #(html/parse-fragment
                               (html-fz-rep 257 "<div>")))]
    (is (map? d))
    (is (= :html/parse (:mino/kind d)))
    (is (= :max-depth (:code d)))
    (is (map? (:location d)))))

(deftest html-fz-hostile-megabyte-attribute
  (let [[_ t] (html-fz-timed
                #(first (html/parse-fragment
                          (str "<p title=\"" (html-fz-rep 1048576 "x")
                               "\">t</p>"))))]
    (is (= :p (:tag t)))
    (is (= 1048576 (count (:title (:attrs t)))))
    (is (= ["t"] (:content t)))))

(deftest html-fz-hostile-entity-flood
  ;; a known-entity flood decodes to the run of ampersands
  (let [[_ t] (html-fz-timed
                #(first (html/parse-fragment
                          (str "<p>" (html-fz-rep 100000 "&amp;")
                               "</p>"))))]
    (is (= 100000 (count (first (:content t)))))
    (is (= (html-fz-rep 100000 "&") (first (:content t)))))
  ;; an unknown-entity flood stays literal (rule 8: a bare ampersand
  ;; matching no table entry stays literal)
  (let [[_ t] (html-fz-timed
                #(first (html/parse-fragment
                          (str "<p>" (html-fz-rep 50000 "&nope;")
                               "</p>"))))]
    (is (= 300000 (count (first (:content t)))))))

(deftest html-fz-hostile-nul-laden-text
  ;; NUL in text becomes U+FFFD (tier rule 13), never a crash
  (let [[_ t] (html-fz-timed
                #(first (html/parse-fragment
                          (str "<p>" (html-fz-rep 100000 "a\u0000")
                               "</p>"))))]
    (is (= :p (:tag t)))
    (let [s (first (:content t))]
      (is (str/includes? s "\uFFFD"))
      (is (not (str/includes? s "\u0000")))
      (is (= 200000 (count s))))))

(run-tests-and-exit)
