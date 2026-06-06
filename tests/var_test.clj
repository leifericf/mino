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
