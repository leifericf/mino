(require "tests/test")

;; var type and var? predicate tests

(def my-val 42)
(def ^:dynamic *dyn* :initial)

(deftest var-type
  (is (= :var (type #'my-val))))

(deftest var?-true-for-var
  (is (var? #'my-val)))

(deftest var?-false-for-value
  (is (not (var? 42)))
  (is (not (var? "hello")))
  (is (not (var? :kw)))
  (is (not (var? nil))))

(deftest var-identity
  (is (= #'my-val #'my-val)))

(deftest var-on-fn
  (is (var? #'inc))
  (is (var? #'map)))

(deftest resolve-returns-var
  (is (var? (resolve 'my-val))))

(deftest resolve-nil-for-missing
  (is (nil? (resolve 'nonexistent-binding-xyz))))

(deftest resolve-core-fn
  (is (var? (resolve 'map))))

;; A call site whose head is unmapped must throw on the next call.
(defn ic-unmap-target-fn [] :before-unmap)
(defn ic-unmap-call-site [] (ic-unmap-target-fn))

(deftest ns-unmap-invalidates-call-resolution
  (is (= :before-unmap (ic-unmap-call-site)))
  (ns-unmap *ns* 'ic-unmap-target-fn)
  (let [result (try (ic-unmap-call-site)
                    (catch __e :got-error))]
    (is (= :got-error result))))

(declare unbound-fwd-var)

(deftest unbound-declared-var-throws-on-symbol-access
  ;; Matching JVM Clojure: an unbound declared var must fail loud at
  ;; the use site instead of silently resolving to nil. Otherwise a
  ;; reference-before-def bug propagates downstream as "value was nil"
  ;; and only blows up far from where the actual mistake lives.
  (let [err (try unbound-fwd-var nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? err))
    (is (some? (re-find #"unbound" err)))
    (is (some? (re-find #"unbound-fwd-var" err)))))

(deftest def-to-nil-is-not-unbound
  ;; A var explicitly `def`-d to nil is bound; reading it must return
  ;; nil silently, not throw. The unbound discriminator is the var's
  ;; `bound` flag, not the value at root.
  (def explicit-nil-var nil)
  (is (nil? explicit-nil-var))
  ;; Subsequent reads stay silent.
  (is (= [nil nil] [explicit-nil-var explicit-nil-var])))

(deftest declare-then-def-clears-unbound
  (declare def-after-declare)
  (def def-after-declare :now-bound)
  (is (= :now-bound def-after-declare)))

(deftest var-meta-carries-user-map
  (def ^{:doc "docd" :other 42} var-meta-probe 3)
  (is (= "docd" (:doc (meta #'var-meta-probe))))
  (is (= 42 (:other (meta #'var-meta-probe))))
  (is (= 'var-meta-probe (:name (meta #'var-meta-probe)))))

(deftest var-meta-doc-from-docstring
  (defn var-meta-doc-fn "the doc" [a b] a)
  (is (= "the doc" (:doc (meta #'var-meta-doc-fn))))
  (def var-meta-doc-val "vdoc" 5)
  (is (= "vdoc" (:doc (meta #'var-meta-doc-val)))))

(deftest var-meta-flags-still-present
  (def ^{:private true :doc "p"} var-meta-priv 1)
  (is (= true (:private (meta #'var-meta-priv))))
  (is (= "p" (:doc (meta #'var-meta-priv)))))

;; --- def evaluates metadata map values at definition time ---

(deftest def-meta-expr-value-arithmetic
  ;; ^{:k (+ 1 2)} must store 3, not the list (+ 1 2).
  (def ^{:k (+ 1 2)} def-meta-eval-v1 1)
  (is (= 3 (:k (meta #'def-meta-eval-v1)))))

(deftest def-meta-expr-value-fn
  ;; A fn form in metadata must produce a callable, not a list.
  (def ^{:f (fn [] 7)} def-meta-eval-v2 1)
  (let [f (:f (meta #'def-meta-eval-v2))]
    (is (fn? f))
    (is (= 7 (f)))))

(deftest def-meta-test-key-is-fn
  ;; The :test value must be a real fn so clojure.core/test can run it.
  (def ^{:test (fn [] :ran)} def-meta-eval-v3 1)
  (let [t (:test (meta #'def-meta-eval-v3))]
    (is (fn? t))
    (is (= :ran (t)))))

(deftest def-meta-reader-flags-still-work
  ;; ^:dynamic and ^:private reader shorthands must still attach true.
  (def ^:dynamic def-meta-eval-v4 1)
  (def ^:private def-meta-eval-v5 1)
  (is (true? (:dynamic (meta #'def-meta-eval-v4))))
  (is (true? (:private (meta #'def-meta-eval-v5)))))

(deftest def-meta-literal-values-unchanged
  ;; String, keyword, and symbol literals in metadata must pass through
  ;; as-is without being re-evaluated.
  (def ^{:doc "d" :tag :x} def-meta-eval-vs 1)
  (is (= "d" (:doc (meta #'def-meta-eval-vs))))
  (is (= :x (:tag (meta #'def-meta-eval-vs)))))

(deftest def-meta-expr-references-earlier-var
  ;; Metadata forms may reference earlier vars; the value at def time
  ;; must be captured, not the form.
  (def def-meta-eval-base 10)
  (def ^{:k (* def-meta-eval-base 2)} def-meta-eval-v6 1)
  (is (= 20 (:k (meta #'def-meta-eval-v6)))))

(deftest defn-meta-expr-value-arithmetic
  ;; defn with a metadata attr-map goes through the same def path.
  (defn ^{:k (+ 1 1)} def-meta-eval-f7 [] 1)
  (is (= 2 (:k (meta #'def-meta-eval-f7)))))

(run-tests-and-exit)
