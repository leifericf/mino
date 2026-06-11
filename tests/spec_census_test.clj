(require "tests/test")

(require '[clojure.spec.alpha :as s])
(require '[clojure.test.check.generators :as census-gen])

;; Spec-first tests for the wave-1 spec surface: Spec / Specize /
;; specize* / spec? / regex? / describe* / gen* / with-gen* / every /
;; every-kv / keys* / merge / multi-spec / nonconforming and the
;; *-impl builder fns. Behavior is pinned to clojure.spec.alpha.
;; These tests land before the implementation and fail until it does.

;; Registered helper specs. Prefixed census- names so they cannot
;; collide with other test files in the shared suite namespace.
(s/def ::census-int int?)
(s/def ::census-ka int?)
(s/def ::census-ma int?)
(s/def ::census-mb string?)
(s/def ::census-tag keyword?)
(s/def ::census-i int?)
(s/def ::census-s string?)

(defmulti spec-census-msg :census-tag)
(defmethod spec-census-msg :int [_]
  (s/keys :req-un [::census-tag ::census-i]))
(defmethod spec-census-msg :str [_]
  (s/keys :req-un [::census-tag ::census-s]))

;; ---------------------------------------------------------------------------
;; every -- samples up to *coll-check-limit* (101) elements and never
;; conforms them; the input collection is returned unchanged. All the
;; collections below are within the limit, so they are fully checked.
;; ---------------------------------------------------------------------------

(deftest every-returns-input-unchanged
  (is (= [1 2 3] (s/conform (s/every int?) [1 2 3])))
  (is (= [] (s/conform (s/every int?) [])))
  (is (= #{1 2} (s/conform (s/every int?) #{1 2}))))

(deftest every-does-not-conform-elements
  ;; coll-of would conform each element to [:i 1]; every must not.
  (is (= [1] (s/conform (s/every (s/or :i int?)) [1]))))

(deftest every-rejects-bad-element
  (is (false? (s/valid? (s/every int?) [1 :a])))
  ;; 101 elements is still within the check limit: the bad tail is seen.
  (is (false? (s/valid? (s/every int?) (vec (concat (range 100) [:bad]))))))

(deftest every-rejects-non-coll
  (is (false? (s/valid? (s/every int?) 5)))
  (is (false? (s/valid? (s/every int?) nil)))
  (is (false? (s/valid? (s/every int?) "abc"))))

(deftest every-kind-opt
  (is (true?  (s/valid? (s/every int? :kind vector?) [1 2])))
  (is (false? (s/valid? (s/every int? :kind vector?) '(1 2)))))

(deftest every-count-opt
  (is (true?  (s/valid? (s/every int? :count 2) [1 2])))
  (is (false? (s/valid? (s/every int? :count 2) [1]))))

(deftest every-min-max-count-opts
  (let [sp (s/every int? :min-count 2 :max-count 3)]
    (is (false? (s/valid? sp [1])))
    (is (true?  (s/valid? sp [1 2])))
    (is (true?  (s/valid? sp [1 2 3])))
    (is (false? (s/valid? sp [1 2 3 4])))))

(deftest every-distinct-opt
  (is (true?  (s/valid? (s/every int? :distinct true) [1 2])))
  (is (false? (s/valid? (s/every int? :distinct true) [1 1]))))

;; ---------------------------------------------------------------------------
;; every-kv -- every over the entries of an associative collection.
;; ---------------------------------------------------------------------------

(deftest every-kv-returns-input-unchanged
  (is (= {:a 1} (s/conform (s/every-kv keyword? int?) {:a 1})))
  (is (true? (s/valid? (s/every-kv keyword? int?) {}))))

(deftest every-kv-rejects-bad-entries
  (is (false? (s/valid? (s/every-kv keyword? int?) {"x" 1})))
  (is (false? (s/valid? (s/every-kv keyword? int?) {:a "x"}))))

(deftest every-kv-entry-shape
  ;; Entries are validated as [k v] tuples: a flat vector fails, a
  ;; vector of pairs passes.
  (is (false? (s/valid? (s/every-kv keyword? int?) [:a 1])))
  (is (true?  (s/valid? (s/every-kv keyword? int?) [[:a 1]]))))

;; ---------------------------------------------------------------------------
;; keys* -- keys spec as a regex op over inline key/value sequences.
;; ---------------------------------------------------------------------------

(deftest keys*-conforms-kv-sequence-to-map
  (is (= {:census-ka 1}
         (s/conform (s/keys* :req-un [::census-ka]) [:census-ka 1]))))

(deftest keys*-missing-required-key
  (is (false? (s/valid? (s/keys* :req-un [::census-ka]) []))))

(deftest keys*-validates-values
  (is (false? (s/valid? (s/keys* :req-un [::census-ka]) [:census-ka "x"]))))

(deftest keys*-composes-into-regex
  ;; Extra key/value pairs are absorbed into the map; the trailing
  ;; non-keyword element stops the greedy match.
  (is (= {:i1 42 :m {:census-ka 1 :census-extra 4} :i2 99}
         (s/conform (s/cat :i1 integer?
                           :m  (s/keys* :req-un [::census-ka])
                           :i2 integer?)
                    [42 :census-ka 1 :census-extra 4 99]))))

;; ---------------------------------------------------------------------------
;; merge -- conjunction of map specs that returns the merged map.
;; ---------------------------------------------------------------------------

(deftest merge-conforms-union-of-keys
  (let [sp (s/merge (s/keys :req-un [::census-ma])
                    (s/keys :req-un [::census-mb]))]
    (is (= {:census-ma 1 :census-mb "x"}
           (s/conform sp {:census-ma 1 :census-mb "x"})))
    ;; Open maps: unknown keys flow through.
    (is (= {:census-ma 1 :census-mb "x" :census-extra 9}
           (s/conform sp {:census-ma 1 :census-mb "x" :census-extra 9})))))

(deftest merge-rejects-any-failing-branch
  (let [sp (s/merge (s/keys :req-un [::census-ma])
                    (s/keys :req-un [::census-mb]))]
    (is (false? (s/valid? sp {:census-ma 1})))
    (is (false? (s/valid? sp {:census-mb "x"})))))

(deftest merge-explain-names-failing-branch
  (let [sp (s/merge (s/keys :req-un [::census-ma])
                    (s/keys :req-un [::census-mb]))
        d  (s/explain-data sp {:census-ma 1})
        ps (::s/problems d)]
    ;; Only the branch missing :census-mb reports a problem, and its
    ;; pred names the missing key.
    (is (= 1 (count ps)))
    (is (some? (re-find #"census-mb" (pr-str (:pred (first ps))))))))

;; ---------------------------------------------------------------------------
;; multi-spec -- open dispatch through a multimethod on a tag key.
;; ---------------------------------------------------------------------------

(deftest multi-spec-dispatches-per-tag
  (let [sp (s/multi-spec spec-census-msg :census-tag)]
    (is (= {:census-tag :int :census-i 42}
           (s/conform sp {:census-tag :int :census-i 42})))
    (is (= {:census-tag :str :census-s "hi"}
           (s/conform sp {:census-tag :str :census-s "hi"})))))

(deftest multi-spec-rejects-bad-payload
  (let [sp (s/multi-spec spec-census-msg :census-tag)]
    (is (false? (s/valid? sp {:census-tag :int :census-i "x"})))
    (is (false? (s/valid? sp {:census-tag :int})))))

(deftest multi-spec-unknown-tag-is-invalid
  (is (= ::s/invalid
         (s/conform (s/multi-spec spec-census-msg :census-tag)
                    {:census-tag :nope}))))

(deftest multi-spec-explain-carries-tag-path
  (let [sp (s/multi-spec spec-census-msg :census-tag)
        no-method (first (::s/problems (s/explain-data sp {:census-tag :nope})))
        bad-value (first (::s/problems
                          (s/explain-data sp {:census-tag :int
                                              :census-i "x"})))]
    ;; Unknown tag: the dispatch value is the path, reason "no method".
    (is (= [:nope] (:path no-method)))
    (is (= "no method" (:reason no-method)))
    ;; Known tag, bad value: path starts with the dispatch tag.
    (is (= [:int :census-i] (:path bad-value)))
    (is (= [:census-i] (:in bad-value)))))

;; ---------------------------------------------------------------------------
;; nonconforming -- same acceptance, but conform returns the input.
;; ---------------------------------------------------------------------------

(deftest nonconforming-returns-original-value
  (is (= [1] (s/conform (s/nonconforming (s/cat :a int?)) [1])))
  (is (= 5 (s/conform (s/nonconforming (s/or :i int? :s string?)) 5))))

(deftest nonconforming-still-rejects
  (is (= ::s/invalid (s/conform (s/nonconforming (s/cat :a int?)) [:x]))))

;; ---------------------------------------------------------------------------
;; spec? / regex? -- object-kind predicates returning the argument.
;; ---------------------------------------------------------------------------

(deftest spec?-true-on-spec-objects
  (let [sp (s/spec int?)]
    (is (= sp (s/spec? sp))))
  (is (s/spec? (s/get-spec ::census-int))))

(deftest spec?-false-on-non-specs
  (is (not (s/spec? int?)))
  (is (not (s/spec? ::census-int)))
  (is (not (s/spec? 42))))

(deftest regex?-true-on-regex-ops
  (is (s/regex? (s/* int?)))
  (is (s/regex? (s/cat :a int?)))
  (is (s/regex? (s/alt :i int?))))

(deftest regex?-false-on-wrapped-and-plain
  ;; s/spec wraps a regex so it conforms as a single element: the
  ;; wrapper is a spec, not a regex op.
  (is (not (s/regex? (s/spec int?))))
  (is (not (s/regex? (s/spec (s/cat :a int?)))))
  (is (not (s/regex? int?))))

;; ---------------------------------------------------------------------------
;; describe / describe*.
;; ---------------------------------------------------------------------------

(deftest describe-every-heads
  (let [d (s/describe (s/every int?))]
    (is (= 'every (first d)))
    (is (= 'int? (second d))))
  (is (= '(every-kv keyword? int?)
         (s/describe (s/every-kv keyword? int?)))))

(deftest describe-merge-head
  (let [d (s/describe (s/merge (s/keys :req-un [::census-ma])
                               (s/keys :req-un [::census-mb])))]
    (is (= 'merge (first d)))
    (is (= 'keys (first (second d))))))

(deftest describe-multi-spec-head
  (is (= '(multi-spec spec-census-msg :census-tag)
         (s/describe (s/multi-spec spec-census-msg :census-tag)))))

(deftest describe-keys*-head
  ;; keys* is built on & over a (* (cat ...)) regex; describe exposes
  ;; that composition.
  (let [d (s/describe (s/keys* :req-un [::census-ka]))]
    (is (= '& (first d)))
    (is (= '* (first (second d))))))

(deftest describe-nonconforming-head
  (is (= '(nonconforming (cat :a int?))
         (s/describe (s/nonconforming (s/cat :a int?))))))

(deftest describe*-returns-raw-form
  (is (= "int?" (name (s/describe* (s/spec int?)))))
  (is (= "every" (name (first (s/describe* (s/every int?)))))))

;; ---------------------------------------------------------------------------
;; gen* / with-gen* -- attached generators win over derived ones. The
;; default generator for int? is size-bounded (default size 30), so it
;; can never produce 42; only the attached generator can.
;; ---------------------------------------------------------------------------

(deftest with-gen*-attaches-generator
  (let [sp (s/with-gen* (s/spec int?) (fn [] (census-gen/return 42)))]
    (is (= 41 (s/conform sp 41)))
    (is (= 42 (census-gen/generate (s/gen sp))))))

(deftest gen*-honors-attached-generator
  (let [sp (s/with-gen* (s/spec int?) (fn [] (census-gen/return 7)))]
    (is (= 7 (census-gen/generate (s/gen* sp nil [] {}))))))

(deftest gen-honors-with-gen
  (let [sp (s/with-gen (s/spec int?) (fn [] (census-gen/return 42)))]
    (is (= 42 (census-gen/generate (s/gen sp))))))

;; ---------------------------------------------------------------------------
;; Spec / Specize / specize*.
;; ---------------------------------------------------------------------------

(deftest spec-protocols-exist
  (is (some? s/Spec))
  (is (some? s/Specize)))

(deftest specize*-promotes-predicates
  (let [sp (s/specize* int?)]
    (is (s/spec? sp))
    (is (= 5 (s/conform sp 5)))
    (is (= ::s/invalid (s/conform sp :x)))))

;; ---------------------------------------------------------------------------
;; *-impl builders -- presence plus one smoke construction each.
;; ---------------------------------------------------------------------------

(deftest impl-builders-are-functions
  (is (fn? s/map-spec-impl))
  (is (fn? s/merge-spec-impl))
  (is (fn? s/multi-spec-impl))
  (is (fn? s/maybe-impl))
  (is (fn? s/regex-spec-impl))
  (is (fn? s/rep+impl)))

(deftest map-spec-impl-smoke
  (let [sp (s/map-spec-impl
            {:req nil :opt nil :req-un '[::census-ka] :opt-un nil
             :req-keys [:census-ka] :req-specs [::census-ka]
             :opt-keys [] :opt-specs []
             :pred-exprs [(fn [m] (map? m))
                          (fn [m] (contains? m :census-ka))]
             :pred-forms '[(fn [%] (map? %))
                           (fn [%] (contains? % :census-ka))]
             :keys-pred (fn [m] (and (map? m) (contains? m :census-ka)))
             :gfn nil})]
    (is (s/spec? sp))
    (is (true?  (s/valid? sp {:census-ka 1})))
    (is (false? (s/valid? sp {})))
    (is (false? (s/valid? sp {:census-ka "x"})))))

(deftest merge-spec-impl-smoke
  (let [sp (s/merge-spec-impl ['(clojure.spec.alpha/keys :req-un [::census-ma])]
                              [(s/keys :req-un [::census-ma])]
                              nil)]
    (is (s/spec? sp))
    (is (true?  (s/valid? sp {:census-ma 1})))
    (is (false? (s/valid? sp {})))))

(deftest multi-spec-impl-smoke
  (let [sp (s/multi-spec-impl 'spec-census-msg
                              (resolve 'spec-census-msg)
                              :census-tag)]
    (is (s/spec? sp))
    (is (= {:census-tag :int :census-i 1}
           (s/conform sp {:census-tag :int :census-i 1})))
    (is (= ::s/invalid (s/conform sp {:census-tag :nope})))))

(deftest maybe-impl-smoke
  (let [sp (s/maybe-impl int? 'int?)]
    (is (s/regex? sp))
    (is (= 5 (s/conform sp [5])))
    (is (nil? (s/conform sp [])))
    (is (= '(? int?) (s/describe sp)))))

(deftest rep+impl-smoke
  (let [sp (s/rep+impl 'int? int?)]
    (is (s/regex? sp))
    (is (= [1 2] (s/conform sp [1 2])))
    (is (= ::s/invalid (s/conform sp [])))
    (is (= '(+ int?) (s/describe sp)))))

(deftest regex-spec-impl-smoke
  (let [sp (s/regex-spec-impl (s/cat :a int?) nil)]
    (is (s/spec? sp))
    (is (= {:a 1} (s/conform sp [1])))))

(run-tests-and-exit)
