(require "tests/test")
(require '[clojure.string :as str])
(require '[clojure.edn :as edn])
(require '[clojure.xml :as xml])
(require '[tests.xml-fixture :as xfix])

;; Strict XML fuzz lane (html-xml campaign p6t2, design A-1).
;; Truncate and byte-flip the generated 1MB fixture and the golden
;; corpus, 3 fixed seeds x 400 mutations = 1200 parses, each parse
;; bounded under 5s; assert no crash, no hang, and a tree or a
;; POSITIONED ex-info on every mutation. Where HTML tolerates
;; nearly everything, strict XML must refuse most corruption
;; (FR-9: never silently misparse), so this lane also pins an
;; error-rate floor: most mutations must error, and every error
;; carries :mino/kind :xml/parse, a :code keyword, and integral
;; :location line and col. The mutation engine is the html lane's
;; (seeded LCG arithmetic, yaml_perf fixture-generator discipline):
;; deterministic, identical on every run and platform. This file
;; joins the nightly MINO_TEST_EXCLUDE list with the perf gates
;; (the toml/yaml precedent); it runs in the ordinary test lane and
;; under the sanitizer builds.

(def ^:private xml-fz-seeds
  "Three fixed seeds; changing them changes every derived mutation."
  [20260826 3141592 271828182])

(def ^:private xml-fz-flip-chars
  "The flip alphabet: structural markup bytes, NUL, and a spread of
  ordinary bytes."
  "<>/\"'&;!=-\u0000ABCabcxyz0123456789 #")

(def ^:private xml-fz-flip-window 131072)

(defn- xml-fz-next
  "One LCG step (rand()/BSD constants); deterministic everywhere.
  The int coercion keeps the state a fixnum: mod over the widened
  product stays bigint otherwise, and nth/subs reject bigint
  indices."
  [x]
  (int (mod (+ (* x 1103515245) 12345) 2147483648)))

(defn- xml-fz-flip
  "Replaces the byte at index i with c."
  [s i c]
  (str (subs s 0 i) c (subs s (inc i))))

(defn- xml-fz-rep
  "n copies of unit by doubling (O(log n) concatenations, each at
  most the final size; no long lazy chains, the p7 suite rule)."
  [n unit]
  (loop [need n buf unit out ""]
    (if (zero? need)
      out
      (if (odd? need)
        (recur (quot need 2) (str buf buf) (str out buf))
        (recur (quot need 2) (str buf buf) out)))))

(def ^:private xml-fz-doc (xfix/xml-fixture-doc))
(def ^:private xml-fz-doc-len (count xml-fz-doc))
(def ^:private xml-fz-doc-prefix (subs xml-fz-doc 0 xml-fz-flip-window))

(def ^:private xml-fz-corpus
  "The strict-XML input corpus (p5); fuzzing needs only the
  strings, never the oracle expectations."
  (vec (filter #(string? (:input %))
               (edn/read-string (slurp "tests/fixtures/xml/inputs.edn")))))

(defn- xml-fz-mutation
  "Derives the j-th mutated input for one seed. j < 16: fixture
  truncation. j < 80: fixture byte-flip in the bounded prefix.
  j >= 80: corpus entry (strided index), truncated on even j,
  byte-flipped on odd j."
  [seed j]
  (let [st0 (xml-fz-next (+ seed j))
        st1 (xml-fz-next st0)
        st2 (xml-fz-next st1)]
    (cond
      (< j 16)
      (subs xml-fz-doc 0 (mod st0 (dec xml-fz-doc-len)))

      (< j 80)
      (xml-fz-flip xml-fz-doc-prefix
                   (mod st0 xml-fz-flip-window)
                   (nth xml-fz-flip-chars
                        (mod st1 (count xml-fz-flip-chars))))

      :else
      (let [v (nth xml-fz-corpus
                   (mod (+ (* j 7919) st0) (count xml-fz-corpus)))
            s (:input v)
            n (count s)]
        (if (zero? n)
          s
          (if (even? j)
            (subs s 0 (mod st1 n))
            (xml-fz-flip s (mod st1 n)
                         (nth xml-fz-flip-chars
                              (mod st2 (count xml-fz-flip-chars))))))))))

(defn- xml-fz-classify
  "One parse under measurement: a root element map is a tree;
  anything else must be a positioned diagnostic (:mino/kind :xml/parse,
  :code keyword, integral :location line and col). Returns the
  record; :bad carries the first offender's description."
  [s]
  (try
    (let [t0 (nano-time)
          t (xml/parse s)
          ms (quot (- (nano-time) t0) 1000000)]
      (if (and (map? t) (keyword? (:tag t)) (map? (:attrs t)))
        {:parses 1 :errors 0 :max-ms ms :bad nil}
        {:parses 1 :errors 0 :max-ms 0
         :bad (str "non-element result from "
                   (pr-str (subs s 0 (min 40 (count s)))))}))
    (catch e
      (let [d (ex-data e)]
        (if (and (map? d)
                 (= :xml/parse (:mino/kind e))
                 (keyword? (:code d))
                 (map? (:location d))
                 (int? (:line (:location d)))
                 (int? (:col (:location d))))
          {:parses 1 :errors 1 :max-ms 0 :bad nil}
          {:parses 1 :errors 1 :max-ms 0
           :bad (str "unpositioned error: " (pr-str d))})))))

(deftest xml-fz-mutations-error-positioned-or-tree
  ;; 3 seeds x 400 mutations: every parse answers a tree or a
  ;; positioned error, each inside the 5s bound (A-1), and MOST
  ;; mutations error: strict XML refuses corruption instead of
  ;; recovering (the error-rate floor; measured 88% at land, floor
  ;; pinned at 60% so the set cannot silently drift easy).
  (let [res (reduce
              (fn [acc seed]
                (loop [j 0 acc acc]
                  (if (= j 400)
                    acc
                    (let [r (xml-fz-classify (xml-fz-mutation seed j))]
                      (if (:bad r)
                        (assoc acc :bad (:bad r))
                        (recur (inc j)
                               (-> acc
                                   (update :parses + (:parses r))
                                   (update :errors + (:errors r))
                                   (assoc :max-ms (max (:max-ms acc)
                                                       (:max-ms r))))))))))
              {:parses 0 :errors 0 :max-ms 0 :bad nil}
              xml-fz-seeds)]
    (is (nil? (:bad res)) (:bad res))
    (is (= 1200 (:parses res))
        (str "expected 1200 parses, saw " (:parses res)))
    (is (< (:max-ms res) 5000)
        (str "slowest single parse " (:max-ms res) "ms; bound 5000ms"))
    (is (>= (:errors res) 720)
        (str "strict XML must refuse most corruption: only "
             (:errors res) " of 1200 mutations errored; floor 720"))
    (is (< (:errors res) 1200)
        "some mutations must still parse: the well-formed resilient
         half of the surface (text-content flips) stays readable")))

;;; ---- hostile corpus (A-1: cap-deep nesting, megabyte attributes,
;;; entity floods, NUL-laden text) ----

(defn- xml-fz-err-data
  "Returns the caught diagnostic's ex-data detail merged with its
  :mino/kind, so callers read the class and the :code/:location detail
  from one map."
  [thunk]
  (try (thunk) :no-throw
       (catch e (assoc (ex-data e) :mino/kind (:mino/kind e)))))

(defn- xml-fz-timed
  "Runs thunk, asserts the elapsed bound, returns the value."
  [thunk]
  (let [t0 (nano-time)
        v (thunk)
        ms (quot (- (nano-time) t0) 1000000)]
    (is (< ms 5000) (str "hostile vector took " ms "ms; bound 5000ms"))
    v))

(deftest xml-fz-hostile-cap-deep-nesting
  ;; 255 nested elements parse; 257 throws :max-depth positioned
  ;; (the cap is 256 in both modes, A-2)
  (let [t (xml-fz-timed #(xml/parse (str (xml-fz-rep 255 "<a>")
                                         (xml-fz-rep 255 "</a>"))))]
    (is (= :a (:tag t)))
    (is (= 1 (count (:content t)))))
  (let [d (xml-fz-err-data #(xml/parse (xml-fz-rep 257 "<a>")))]
    (is (map? d))
    (is (= :xml/parse (:mino/kind d)))
    (is (= :max-depth (:code d)))
    (is (map? (:location d)))))

(deftest xml-fz-hostile-megabyte-attribute
  ;; a well-formed megabyte attribute value parses inside the bound
  (let [t (xml-fz-timed #(xml/parse (str "<e v=\""
                                         (xml-fz-rep 1048576 "x")
                                         "\">t</e>")))]
    (is (= :e (:tag t)))
    (is (= 1048576 (count (:v (:attrs t)))))
    (is (= ["t"] (:content t)))))

(deftest xml-fz-hostile-entity-flood
  ;; a predefined-entity flood decodes; an undefined-entity flood
  ;; errors positioned at the first reference (:undefined-entity,
  ;; D3: only the five predefined plus numeric resolve)
  (let [t (xml-fz-timed #(xml/parse (str "<a>"
                                         (xml-fz-rep 100000 "&amp;")
                                         "</a>")))]
    (is (= 100000 (count (first (:content t))))))
  (let [d (xml-fz-err-data #(xml/parse (str "<a>"
                                            (xml-fz-rep 500 "&nosuch;")
                                            "</a>")))]
    (is (map? d))
    (is (= :xml/parse (:mino/kind d)))
    (is (= :undefined-entity (:code d)))
    (is (map? (:location d)))))

(deftest xml-fz-hostile-nul-laden-text
  ;; NUL is not a legal XML character anywhere: a positioned error,
  ;; never a crash (strict XML rejects instead of replacing)
  (let [d (xml-fz-err-data #(xml/parse "<a>x\u0000y</a>"))]
    (is (map? d))
    (is (= :xml/parse (:mino/kind d)))
    (is (keyword? (:code d)))
    (is (and (map? (:location d))
             (int? (:line (:location d)))
             (int? (:col (:location d)))))))

(run-tests-and-exit)
