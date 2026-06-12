(require "tests/test")

(require '[clojure.spec.alpha :as s])
(require '[clojure.test.check.generators :as fspec-gen])

;; Spec-first tests for the wave-2 spec surface: *coll-check-limit*
;; *coll-error-limit* *compile-asserts* *explain-out* *fspec-iterations*
;; *recursion-limit* assert* double-in exercise-fn explain-data*
;; explain-out explain-printer fspec fspec-impl inst-in inst-in-range?
;; int-in int-in-range?. These tests land before the implementation
;; and fail until it does.

;; ---------------------------------------------------------------------------
;; Registered helper specs -- all prefixed fspec- to avoid collisions.
;; ---------------------------------------------------------------------------

(s/def ::fspec-int int?)
(s/def ::fspec-str string?)

;; ---------------------------------------------------------------------------
;; int-in -- end-exclusive integer range spec.
;; ---------------------------------------------------------------------------

(deftest int-in-valid-in-range
  (is (true?  (s/valid? (s/int-in 0 10) 0)))
  (is (true?  (s/valid? (s/int-in 0 10) 5)))
  (is (true?  (s/valid? (s/int-in 0 10) 9))))

(deftest int-in-end-is-exclusive
  ;; 10 is the exclusive end -- it must be rejected.
  (is (false? (s/valid? (s/int-in 0 10) 10)))
  (is (false? (s/valid? (s/int-in 0 10) 11))))

(deftest int-in-below-start-invalid
  (is (false? (s/valid? (s/int-in 0 10) -1)))
  (is (false? (s/valid? (s/int-in 5 15) 4))))

(deftest int-in-non-integer-invalid
  (is (false? (s/valid? (s/int-in 0 10) 5.0)))
  (is (false? (s/valid? (s/int-in 0 10) "5"))))

;; ---------------------------------------------------------------------------
;; int-in-range? -- standalone range predicate.
;; ---------------------------------------------------------------------------

(deftest int-in-range?-truthy-in-range
  (is (true? (s/int-in-range? 0 10 0)))
  (is (true? (s/int-in-range? 0 10 9))))

(deftest int-in-range?-exclusive-end
  (is (false? (s/int-in-range? 0 10 10)))
  (is (false? (s/int-in-range? 0 10 -1))))

(deftest int-in-range?-non-int-false
  ;; int-in-range? requires a fixed-precision integer.
  (is (false? (s/int-in-range? 0 10 5.0))))

;; ---------------------------------------------------------------------------
;; double-in -- floating-point range spec with NaN/infinite options.
;; ---------------------------------------------------------------------------

(deftest double-in-valid-in-range
  (is (true?  (s/valid? (s/double-in :min 0.0 :max 1.0) 0.0)))
  (is (true?  (s/valid? (s/double-in :min 0.0 :max 1.0) 0.5)))
  (is (true?  (s/valid? (s/double-in :min 0.0 :max 1.0) 1.0))))

(deftest double-in-out-of-range-invalid
  (is (false? (s/valid? (s/double-in :min 0.0 :max 1.0) 1.1)))
  (is (false? (s/valid? (s/double-in :min 0.0 :max 1.0) -0.1))))

(deftest double-in-nan-default-invalid
  ;; Without :NaN? true, NaN must be rejected.
  (is (false? (s/valid? (s/double-in :min 0.0 :max 1.0) ##NaN))))

(deftest double-in-nan-allowed-when-opted-in
  (is (true? (s/valid? (s/double-in :min 0.0 :max 1.0 :NaN? true) ##NaN))))

(deftest double-in-infinite-default-invalid
  ;; Without :infinite? true, Inf values must be rejected.
  (is (false? (s/valid? (s/double-in :min 0.0 :max 1.0) ##Inf)))
  (is (false? (s/valid? (s/double-in :min 0.0 :max 1.0) ##-Inf))))

(deftest double-in-infinite-allowed-when-opted-in
  (is (true? (s/valid? (s/double-in :infinite? true) ##Inf)))
  (is (true? (s/valid? (s/double-in :infinite? true) ##-Inf))))

(deftest double-in-non-double-invalid
  (is (false? (s/valid? (s/double-in :min 0.0 :max 1.0) 1)))
  (is (false? (s/valid? (s/double-in :min 0.0 :max 1.0) "0.5"))))

;; ---------------------------------------------------------------------------
;; inst-in -- end-exclusive instant range spec.
;; ---------------------------------------------------------------------------

(deftest inst-in-valid-in-range
  (is (true?  (s/valid? (s/inst-in #inst "2020-01-01" #inst "2021-01-01")
                        #inst "2020-06-15")))
  ;; Start is inclusive.
  (is (true?  (s/valid? (s/inst-in #inst "2020-01-01" #inst "2021-01-01")
                        #inst "2020-01-01"))))

(deftest inst-in-end-is-exclusive
  ;; The end instant itself must be rejected.
  (is (false? (s/valid? (s/inst-in #inst "2020-01-01" #inst "2021-01-01")
                        #inst "2021-01-01"))))

(deftest inst-in-outside-range-invalid
  (is (false? (s/valid? (s/inst-in #inst "2020-01-01" #inst "2021-01-01")
                        #inst "2019-12-31")))
  (is (false? (s/valid? (s/inst-in #inst "2020-01-01" #inst "2021-01-01")
                        #inst "2022-01-01"))))

(deftest inst-in-non-inst-invalid
  (is (false? (s/valid? (s/inst-in #inst "2020-01-01" #inst "2021-01-01")
                        "2020-06-15"))))

;; ---------------------------------------------------------------------------
;; inst-in-range? -- standalone range predicate for instants.
;; ---------------------------------------------------------------------------

(deftest inst-in-range?-truthy-in-range
  (is (true? (s/inst-in-range? #inst "2020-01-01" #inst "2021-01-01"
                               #inst "2020-06-15")))
  (is (true? (s/inst-in-range? #inst "2020-01-01" #inst "2021-01-01"
                               #inst "2020-01-01"))))

(deftest inst-in-range?-exclusive-end
  (is (false? (s/inst-in-range? #inst "2020-01-01" #inst "2021-01-01"
                                #inst "2021-01-01"))))

(deftest inst-in-range?-non-inst-false
  (is (false? (s/inst-in-range? #inst "2020-01-01" #inst "2021-01-01"
                                "2020-06-15"))))

;; ---------------------------------------------------------------------------
;; fspec / fspec-impl -- generative function spec.
;; ---------------------------------------------------------------------------

(deftest fspec-valid-on-conforming-fn
  ;; inc takes an int and returns an int: the fspec must accept it.
  (s/def ::fspec-fn-spec
    (s/fspec :args (s/cat :x int?) :ret int?))
  (is (true? (s/valid? ::fspec-fn-spec inc))))

(deftest fspec-invalid-on-wrong-return-type
  ;; A fn that returns a string when given an int must fail.
  (s/def ::fspec-fn-int->int
    (s/fspec :args (s/cat :x int?) :ret int?))
  (is (false? (s/valid? ::fspec-fn-int->int str))))

(deftest fspec-conform-on-passing-fn-returns-fn
  ;; Canonical behavior: conform of a valid fn returns the fn itself.
  (s/def ::fspec-inc-spec
    (s/fspec :args (s/cat :x int?) :ret int?))
  (is (= inc (s/conform ::fspec-inc-spec inc))))

(deftest fspec-non-fn-is-invalid
  (s/def ::fspec-fn-pred
    (s/fspec :args (s/cat :x int?) :ret int?))
  (is (false? (s/valid? ::fspec-fn-pred 42)))
  (is (false? (s/valid? ::fspec-fn-pred "not-a-fn"))))

(deftest fspec-impl-is-fn
  ;; fspec-impl is the underlying builder, exposed for canonical callers.
  (is (fn? s/fspec-impl)))

(deftest fspec-impl-builds-spec
  ;; Smoke: build an fspec from fspec-impl directly and validate with it.
  (let [args-sp (s/cat :x int?)
        ret-sp  (s/spec int?)
        sp      (s/fspec-impl args-sp '(s/cat :x int?)
                              ret-sp  'int?
                              nil     nil
                              nil)]
    (is (s/spec? sp))
    (is (true? (s/valid? sp inc)))))

;; ---------------------------------------------------------------------------
;; *fspec-iterations* -- controls the sample count for fspec validation.
;; ---------------------------------------------------------------------------

(deftest fspec-iterations-is-dynamic-var
  ;; The var must exist and hold the canon default of 21.
  (is (= 21 s/*fspec-iterations*)))

(deftest fspec-iterations-binding-respected
  ;; Binding to a tiny value must not break validation correctness for
  ;; a trivially correct fn.
  (s/def ::fspec-iter-spec
    (s/fspec :args (s/cat :x int?) :ret int?))
  (binding [s/*fspec-iterations* 1]
    (is (true? (s/valid? ::fspec-iter-spec inc)))))

;; ---------------------------------------------------------------------------
;; exercise-fn -- generate [args ret] samples for an fdef'd function.
;; ---------------------------------------------------------------------------

(defn- fspec-double-it [x] (* 2 x))

(s/fdef fspec-double-it
  :args (s/cat :x int?)
  :ret  int?)

(deftest exercise-fn-returns-n-pairs
  ;; exercise-fn must return exactly n [args ret] pairs.
  (let [pairs (s/exercise-fn `fspec-double-it 3)]
    (is (= 3 (count pairs)))))

(deftest exercise-fn-pair-shape
  ;; Each pair is [args ret] where args is a sequence and ret matches.
  (let [[args ret] (first (s/exercise-fn `fspec-double-it 1))]
    (is (sequential? args))
    (is (int? ret))))

(deftest exercise-fn-default-count
  ;; Without an explicit n, exercise-fn must return 10 pairs.
  (is (= 10 (count (s/exercise-fn `fspec-double-it)))))

;; ---------------------------------------------------------------------------
;; explain-data* -- the lower-level path/via/in-threaded entry point.
;; ---------------------------------------------------------------------------

(deftest explain-data*-returns-problem-map
  ;; explain-data* is the impl entry point: same shape as explain-data.
  (let [d (s/explain-data* int? [] [] [] "x")]
    (is (some? d))
    (is (seq (::s/problems d)))))

(deftest explain-data*-nil-on-success
  (is (nil? (s/explain-data* int? [] [] [] 5))))

(deftest explain-data*-problem-keys
  ;; Each problem must carry the canonical :path :pred :val :via :in keys.
  (let [p (first (::s/problems (s/explain-data* int? [] [] [] "x")))]
    (is (contains? p :path))
    (is (contains? p :pred))
    (is (contains? p :val))
    (is (contains? p :via))
    (is (contains? p :in))))

(deftest explain-data*-threads-path-and-via
  ;; Extra path and via are prepended to every problem.
  (let [d (s/explain-data* int? [:foo] [::fspec-int] [0] "x")
        p (first (::s/problems d))]
    (is (= [:foo] (:path p)))
    (is (= [::fspec-int] (:via p)))
    (is (= [0] (:in p)))))

;; ---------------------------------------------------------------------------
;; explain-printer -- the default *explain-out* value.
;; ---------------------------------------------------------------------------

(deftest explain-printer-is-fn
  (is (fn? s/explain-printer)))

(deftest explain-printer-produces-output
  ;; explain-printer called with explain-data output must write something
  ;; to *out*; the pred form must appear in the output.
  (let [ed  (s/explain-data int? "x")
        out (with-out-str (s/explain-printer ed))]
    (is (pos? (count out)))
    (is (some? (re-find #"int" out)))))

(deftest explain-printer-success-prints-something
  ;; explain-printer called with nil (successful validation) must print
  ;; a success indicator rather than throwing.
  (let [out (with-out-str (s/explain-printer nil))]
    (is (some? (re-find #"[Ss]uccess" out)))))

;; ---------------------------------------------------------------------------
;; *explain-out* -- dynamic var holding the active printer.
;; ---------------------------------------------------------------------------

(deftest explain-out-var-exists
  ;; The var must resolve to a function (the default explain-printer).
  (is (fn? s/*explain-out*)))

(deftest explain-out-default-is-explain-printer
  (is (= s/explain-printer s/*explain-out*)))

;; ---------------------------------------------------------------------------
;; explain-out -- calls *explain-out* with the explain-data result.
;; ---------------------------------------------------------------------------

(deftest explain-out-non-empty-on-failure
  ;; explain-out must write to *out* when given a failing explain-data map.
  (let [ed  (s/explain-data int? "x")
        out (with-out-str (s/explain-out ed))]
    (is (pos? (count out)))
    (is (some? (re-find #"int" out)))))

(deftest explain-out-rebinding-changes-output
  ;; Rebinding *explain-out* routes the call through the new printer.
  (let [called? (atom false)
        my-printer (fn [_ed] (reset! called? true))]
    (binding [s/*explain-out* my-printer]
      (s/explain-out (s/explain-data int? "x")))
    (is (true? @called?))))

;; ---------------------------------------------------------------------------
;; assert* -- the runtime assert function (not the macro).
;; ---------------------------------------------------------------------------

(deftest assert*-returns-value-on-pass
  (is (= 5   (s/assert* int? 5)))
  (is (= "hi" (s/assert* string? "hi"))))

(deftest assert*-throws-on-failure
  ;; assert* must throw an ex-info when the value does not conform.
  (let [err (try
              (s/assert* int? "x")
              nil
              (catch __e __e))]
    (is (some? err))
    ;; The thrown value must carry explain-data under ::s/problems.
    (is (seq (::s/problems err)))))

(deftest assert*-failure-key-in-exception
  ;; The exception must include ::s/failure :assertion-failed per canon.
  (let [err (try (s/assert* int? "bad") nil (catch __e __e))]
    (is (some? err))
    (is (= :assertion-failed (::s/failure err)))))

;; ---------------------------------------------------------------------------
;; *compile-asserts* -- dynamic var gating assert compilation.
;; ---------------------------------------------------------------------------

(deftest compile-asserts-is-dynamic-var
  ;; The var must exist; the default is truthy per canon.
  (is (some? s/*compile-asserts*))
  (is (boolean? s/*compile-asserts*)))

;; ---------------------------------------------------------------------------
;; *coll-check-limit* -- caps the elements sampled by every / every-kv.
;; ---------------------------------------------------------------------------

(deftest coll-check-limit-is-dynamic-var
  ;; Must exist and hold the canon default of 101.
  (is (= 101 s/*coll-check-limit*)))

(deftest coll-check-limit-binding-constrains-sampling
  ;; When *coll-check-limit* is 1, only the first element is checked.
  ;; A collection with a valid first element but invalid later elements
  ;; must be considered valid under the limit of 1.
  (binding [s/*coll-check-limit* 1]
    (let [coll (into [0] (repeat 200 "bad"))]
      (is (true? (s/valid? (s/every int?) coll))))))

;; ---------------------------------------------------------------------------
;; *coll-error-limit* -- caps the number of problems reported by explain.
;; ---------------------------------------------------------------------------

(deftest coll-error-limit-is-dynamic-var
  ;; Must exist and hold the canon default of 20.
  (is (= 20 s/*coll-error-limit*)))

;; ---------------------------------------------------------------------------
;; *recursion-limit* -- caps how deep gen recurses through branching specs.
;; ---------------------------------------------------------------------------

(deftest recursion-limit-is-dynamic-var
  ;; Must exist and hold the canon default of 4.
  (is (= 4 s/*recursion-limit*)))

(run-tests-and-exit)
