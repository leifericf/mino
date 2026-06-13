(require "tests/test")

(require '[clojure.spec.alpha :as s])
(require '[clojure.spec.test.alpha :as stest])
(require '[clojure.core.specs.alpha])

;; ---------------------------------------------------------------------------
;; Predicate specs and core API.
;; ---------------------------------------------------------------------------

(deftest pred-spec-valid?
  (is (true?  (s/valid? string? "x")))
  (is (false? (s/valid? string? 42))))

(deftest pred-spec-conform
  (is (= "x"        (s/conform string? "x")))
  (is (= ::s/invalid (s/conform string? 42))))

(s/def ::name string?)

(deftest registered-spec-roundtrip
  (is (true?  (s/valid? ::name "Ada")))
  (is (false? (s/valid? ::name 42)))
  (is (some?  (s/get-spec ::name))))

(deftest explain-data-shape
  (let [d (s/explain-data ::name 42)]
    (is (= 42 (::s/value d)))
    (is (seq (::s/problems d)))
    (is (= 42 (-> d ::s/problems first :val)))))

(deftest explain-pass-returns-nil
  (is (nil? (s/explain-data ::name "Ada"))))

(deftest explain-str-success-message
  (is (= "Success!\n" (s/explain-str ::name "Ada"))))

;; ---------------------------------------------------------------------------
;; and / or / nilable / tuple.
;; ---------------------------------------------------------------------------

(s/def ::age (s/and integer? pos?))

(deftest and-spec-chain
  (is (true?  (s/valid? ::age 30)))
  (is (false? (s/valid? ::age -3)))
  (is (false? (s/valid? ::age "x"))))

(s/def ::role (s/or :admin #{:admin} :user #{:user :guest}))

(deftest or-spec-tags-match
  (is (= [:admin :admin] (s/conform ::role :admin)))
  (is (= [:user :user]   (s/conform ::role :user)))
  (is (= [:user :guest]  (s/conform ::role :guest)))
  (is (= ::s/invalid     (s/conform ::role :other))))

(s/def ::nilable-name (s/nilable string?))

(deftest nilable-allows-nil-or-pred
  (is (true?  (s/valid? ::nilable-name nil)))
  (is (true?  (s/valid? ::nilable-name "x")))
  (is (false? (s/valid? ::nilable-name 42))))

(s/def ::point (s/tuple number? number?))

(deftest tuple-fixed-length
  (is (true?  (s/valid? ::point [1 2])))
  (is (false? (s/valid? ::point [1 2 3])))
  (is (false? (s/valid? ::point [1])))
  (is (false? (s/valid? ::point '(1 2)))))

;; ---------------------------------------------------------------------------
;; keys.
;; ---------------------------------------------------------------------------

(s/def ::user (s/keys :req [::name ::age] :opt [::role]))

(deftest keys-required-present-and-valid
  (is (true? (s/valid? ::user {::name "Ada" ::age 30}))))

(deftest keys-missing-required
  (is (false? (s/valid? ::user {::name "Ada"})))
  (is (false? (s/valid? ::user {::age 30}))))

(deftest keys-bad-value-fails
  (is (false? (s/valid? ::user {::name 42 ::age 30}))))

(deftest keys-optional-not-required
  (is (true? (s/valid? ::user {::name "Ada" ::age 30 ::role :admin}))))

;; ---------------------------------------------------------------------------
;; coll-of / map-of.
;; ---------------------------------------------------------------------------

(s/def ::names (s/coll-of string?))

(deftest coll-of-validates-elements
  (is (true?  (s/valid? ::names ["a" "b"])))
  (is (true?  (s/valid? ::names [])))
  (is (false? (s/valid? ::names ["a" 1])))
  (is (false? (s/valid? ::names "abc"))))

(s/def ::str->int (s/map-of string? integer?))

(deftest map-of-validates-pairs
  (is (true?  (s/valid? ::str->int {"a" 1 "b" 2})))
  (is (true?  (s/valid? ::str->int {})))
  (is (false? (s/valid? ::str->int {"a" "x"})))
  (is (false? (s/valid? ::str->int {1 1})))
  (is (false? (s/valid? ::str->int [1 2 3]))))

;; ---------------------------------------------------------------------------
;; Regex ops: cat, *, +, ?, alt.
;; ---------------------------------------------------------------------------

(s/def ::pair (s/cat :k keyword? :v any?))

(deftest cat-conforms-to-named-map
  (is (= {:k :a :v 42} (s/conform ::pair [:a 42]))))

(deftest cat-rejects-extra
  (is (false? (s/valid? ::pair [:a 1 :b 2]))))

(deftest cat-rejects-too-few
  (is (false? (s/valid? ::pair [:a]))))

(s/def ::ints  (s/* integer?))
(s/def ::ints+ (s/+ integer?))
(s/def ::int?  (s/? integer?))

(deftest star-allows-empty-and-many
  (is (true? (s/valid? ::ints [])))
  (is (true? (s/valid? ::ints [1 2 3])))
  (is (false? (s/valid? ::ints [1 :bad 3]))))

(deftest plus-requires-one-or-more
  (is (false? (s/valid? ::ints+ [])))
  (is (true?  (s/valid? ::ints+ [1])))
  (is (true?  (s/valid? ::ints+ [1 2 3]))))

(deftest q-allows-zero-or-one
  (is (true?  (s/valid? ::int? [])))
  (is (true?  (s/valid? ::int? [42])))
  (is (false? (s/valid? ::int? [1 2]))))

(s/def ::pairs (s/* (s/cat :k keyword? :v any?)))

(deftest star-of-cat-is-greedy
  (is (= [{:k :a :v 1} {:k :b :v 2}] (s/conform ::pairs [:a 1 :b 2])))
  (is (= []                          (s/conform ::pairs [])))
  (is (false? (s/valid? ::pairs [:a 1 :b]))))

(s/def ::int-or-str (s/alt :i integer? :s string?))

(deftest alt-tags-first-match
  (is (= [:i 42]    (s/conform ::int-or-str [42])))
  (is (= [:s "abc"] (s/conform ::int-or-str ["abc"])))
  (is (false? (s/valid? ::int-or-str [:nope]))))

;; ---------------------------------------------------------------------------
;; spec wraps a regex spec for element-level conform.
;; ---------------------------------------------------------------------------

(s/def ::wrapped-pair (s/spec (s/cat :a integer? :b string?)))

(deftest spec-wrap-element-level
  (is (true?  (s/valid? ::wrapped-pair [1 "x"])))
  (is (false? (s/valid? ::wrapped-pair "x")))
  (is (false? (s/valid? ::wrapped-pair [1])))
  (is (false? (s/valid? ::wrapped-pair [1 "x" "extra"]))))

;; ---------------------------------------------------------------------------
;; gen / exercise (backed by clojure.test.check generators since v0.98.5).
;; ---------------------------------------------------------------------------

(deftest gen-returns-generator
  (is (map? (s/gen ::name)))
  (is (true? (:test.check/generator (s/gen ::name)))))

(deftest exercise-returns-pairs
  (let [pairs (s/exercise ::name 5)]
    (is (vector? pairs))
    (is (= 5 (count pairs)))
    (doseq [[v conformed] pairs]
      (is (string? v))
      (is (= v conformed)))))

;; ---------------------------------------------------------------------------
;; assert.
;; ---------------------------------------------------------------------------

(deftest assert-passes-conforming
  (is (= 42 (s/assert (s/and integer? pos?) 42))))

(deftest assert-throws-on-violation
  (is (thrown? (s/assert (s/and integer? pos?) -3))))

;; ---------------------------------------------------------------------------
;; fdef + instrument / unstrument.
;; ---------------------------------------------------------------------------

(defn add-numbers [x y] (+ x y))
(s/fdef add-numbers
        :args (s/cat :x integer? :y integer?)
        :ret  integer?)

(deftest fdef-registers-by-qualified-symbol
  (let [s (s/get-spec 'user/add-numbers)]
    (is (some? s))
    (is (= :clojure.spec.alpha/fspec (:clojure.spec.alpha/kind s)))))

(deftest instrument-validates-args
  (stest/instrument 'user/add-numbers)
  (try
    (is (= 3 (add-numbers 1 2)))
    (is (thrown? (add-numbers 1 "str")))
    (finally
      (stest/unstrument 'user/add-numbers))))

(deftest unstrument-restores-original
  (stest/instrument 'user/add-numbers)
  (stest/unstrument 'user/add-numbers)
  ;; Original (raw) fn does not validate; calling with bad arg fails
  ;; inside + instead of inside spec.
  (is (true? (integer? (add-numbers 1 2)))))

;; ---------------------------------------------------------------------------
;; clojure.core.specs.alpha -- destructure-form specs.
;; ---------------------------------------------------------------------------

(deftest local-name-rejects-amp
  (is (true?  (s/valid? :clojure.core.specs.alpha/local-name 'x)))
  (is (false? (s/valid? :clojure.core.specs.alpha/local-name '&)))
  (is (false? (s/valid? :clojure.core.specs.alpha/local-name :keyword))))

(deftest seq-binding-form-shapes
  (is (true? (s/valid? :clojure.core.specs.alpha/seq-binding-form '[a b])))
  (is (true? (s/valid? :clojure.core.specs.alpha/seq-binding-form '[a & rest])))
  (is (true? (s/valid? :clojure.core.specs.alpha/seq-binding-form '[a b :as v])))
  (is (false? (s/valid? :clojure.core.specs.alpha/seq-binding-form 'not-a-vec))))

(deftest defn-args-shapes
  (is (true? (s/valid? :clojure.core.specs.alpha/defn-args
                       '(foo [x] x))))
  (is (true? (s/valid? :clojure.core.specs.alpha/defn-args
                       '(foo "doc" [x] (+ x 1)))))
  (is (true? (s/valid? :clojure.core.specs.alpha/defn-args
                       '(foo "doc" ([x] x) ([x y] (+ x y))))))
  (is (false? (s/valid? :clojure.core.specs.alpha/defn-args
                        '(42 [x] x)))))

;; ---------------------------------------------------------------------------
;; registry.
;; ---------------------------------------------------------------------------

(deftest registry-contains-defined-keys
  (let [r (s/registry)]
    (is (contains? r ::name))
    (is (contains? r ::user))
    (is (contains? r ::pairs))))

(deftest get-spec-returns-nil-for-unknown
  (is (nil? (s/get-spec :clojure.spec.alpha.test/unknown-spec))))

;; ---------------------------------------------------------------------------
;; unform: inverse of conform.
;; ---------------------------------------------------------------------------

(s/def ::distance (s/cat :amount (s/and number? pos?) :unit #{:meters :miles}))

(deftest unform-cat-reconstructs-sequence
  (is (= '(3 :meters) (s/unform ::distance {:amount 3 :unit :meters})))
  (is (= '(3 :foo)    (s/unform ::distance {:amount 3 :unit :foo})))
  ;; Missing key drops that position
  (is (= '(3)         (s/unform ::distance {:amount 3 :foo :miles})))
  (is (= '()          (s/unform ::distance {:bar 3 :foo :miles}))))

(s/def ::ll (s/or :kw keyword? :n number?))

(deftest unform-or-extracts-from-tagged-pair
  (is (= :foo (s/unform ::ll [:kw :foo])))
  (is (= 42   (s/unform ::ll [:n 42]))))

(s/def ::pos-num (s/and number? pos?))

(deftest unform-and-passes-through-pred-chain
  (is (= 3 (s/unform ::pos-num 3))))

(deftest unform-pred-is-identity
  (is (= 42 (s/unform number? 42)))
  (is (= :x (s/unform keyword? :x))))

(s/def ::tuple-pair (s/tuple keyword? number?))

(deftest unform-tuple-roundtrip
  (let [c (s/conform ::tuple-pair [:age 30])]
    (is (= [:age 30] (s/unform ::tuple-pair c)))))

;; ---------------------------------------------------------------------------
;; conformer: arbitrary fn-driven specs.
;; ---------------------------------------------------------------------------

(deftest conformer-uses-fn-as-conform-path
  (is (true? (s/conform (s/conformer map?) {:a 1})))
  (is (false? (s/conform (s/conformer map?) [:a 1])))
  (is (= :b (s/conform (s/conformer second) [:a :b :c]))))

(deftest conformer-explain-reports-invalid
  (let [spec (s/conformer (fn [x] (if (pos? x) x :clojure.spec.alpha/invalid)))]
    (is (= 3 (s/conform spec 3)))
    (is (= :clojure.spec.alpha/invalid (s/conform spec -1)))))

(deftest conformer-unform-uses-supplied-fn
  (let [spec (s/conformer #(* 2 %) #(/ % 2))]
    (is (= 6 (s/conform spec 3)))
    (is (= 3 (s/unform  spec 6)))))

(deftest conformer-unform-default-identity
  (let [spec (s/conformer keyword)]
    (is (= :hello (s/conform spec "hello")))
    (is (= :hello (s/unform  spec :hello)))))

;; ---------------------------------------------------------------------------
;; with-gen: attach a generator to a spec.
;; ---------------------------------------------------------------------------

;; ---------------------------------------------------------------------------
;; s/& : regex op that wraps a regex spec with extra predicates.
;; ---------------------------------------------------------------------------

(s/def ::amp-tagged-children
  (s/cat :tag keyword?
         :children (s/& (s/+ integer?) (fn [%] (>= (count %) 2)))))

(deftest amp-passes-when-pred-holds
  (is (= {:tag :a :children [3 4 5]}
         (s/conform ::amp-tagged-children [:a 3 4 5]))))

(deftest amp-fails-when-pred-rejects
  (is (= :clojure.spec.alpha/invalid
         (s/conform ::amp-tagged-children [:a 3]))))

;; ---------------------------------------------------------------------------
;; :via path propagation in explain-data and explain-str format.
;; ---------------------------------------------------------------------------

(s/def ::via-string string?)

(deftest explain-data-propagates-via-with-name
  (let [data (s/explain-data ::via-string 0)
        problems (:clojure.spec.alpha/problems data)
        first-p (first problems)]
    (is (some? data))
    (is (= 1 (count problems)))
    (is (contains? (set (:via first-p)) ::via-string))))

(deftest explain-str-format-matches-canon
  (is (= "Success!\n" (s/explain-str ::via-string "foo")))
  (let [s (s/explain-str ::via-string 0)]
    (is (some? (re-find #"0 - failed" s)))
    (is (some? (re-find #"string\?" s)))
    ;; spec: <qualified keyword> — the exact ns varies with the active
    ;; namespace at load time; just assert the prefix.
    (is (some? (re-find #"spec: :[^/]+/via-string" s)))))

;; ---------------------------------------------------------------------------

(deftest with-gen-attaches-generator
  (let [base   (s/and number? pos?)
        marker (fn [] :a-generator)
        with   (s/with-gen base marker)]
    (is (= marker (:clojure.spec.alpha/gen with)))
    ;; Conform / valid? still work on the underlying spec
    (is (= 3 (s/conform with 3)))
    (is (true? (s/valid? with 3)))))
