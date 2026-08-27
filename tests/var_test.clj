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

;; --- var-based ns env: def/intern/defmacro bind the var --------------------

(deftest def-return-readback-is-var
  ;; (def b (def a 33)): the inner def form yields #'a, and reading b
  ;; derefs b's OWN var exactly once, so b reads back as #'a (JVM
  ;; single-level deref semantics), never as 33 or a double-deref.
  (def dr-a 33)
  (def dr-b (def dr-a 33))
  (is (var? dr-b))
  (is (identical? dr-b #'dr-a))
  (is (= "#'user/dr-a" (str dr-b))))

(deftest def-var-quote-value-readback-is-var
  ;; Same one-deref rule for a def whose value form is an explicit
  ;; var reference: the binding holds the var, the read exposes it.
  (def dr-va 5)
  (def dr-vb (var dr-va))
  (is (var? dr-vb))
  (is (identical? dr-vb #'dr-va)))

(deftest def-read-derefs-to-value
  ;; The critical invariant: reading a def'd name yields the VALUE.
  (def dr-val 7)
  (is (= 7 dr-val))
  (is (= 7 ((fn [] dr-val)))))

(deftest def-redef-reads-new-value
  ;; Redefinition must invalidate cached reads; the fn body is
  ;; bytecode-compiled on the bc/jit lanes, so this pins SETGLOBAL
  ;; redef plus inline-cache invalidation end to end.
  (def dr-r 1)
  (defn dr-read [] dr-r)
  (is (= 1 (dr-read)))
  (def dr-r 2)
  (is (= 2 dr-r))
  (is (= 2 (dr-read))))

(deftest intern-binds-var-into-ns-env
  ;; intern binds the var itself; plain reads deref to the root and
  ;; resolve reports the same var object intern returned.
  (intern *ns* 'dr-iv 9)
  (is (= 9 dr-iv))
  (is (var? (resolve 'dr-iv)))
  (is (identical? (intern *ns* 'dr-iv2 10) (var dr-iv2)))
  (alter-var-root #'dr-iv (constantly 11))
  (is (= 11 dr-iv)))

(deftest defmacro-name-resolves-and-reads
  ;; defmacro binds its var into the ns env; reads deref to the macro,
  ;; resolve yields the var, and a later def through the same name
  ;; re-roots it.
  (defmacro dr-mac [] :m)
  (is (var? (resolve 'dr-mac)))
  (is (= :m (dr-mac)))
  (def dr-mac :redef)
  (is (= :redef dr-mac)))

(deftest syntax-quote-still-qualifies-defd-names
  ;; A var binding must qualify to the var's own ns/name inside
  ;; syntax-quote; behavior is unchanged from the raw-binding era.
  (def dr-sq 1)
  (is (= '(user/dr-sq clojure.core/inc) `(dr-sq inc))))

(deftest print-length-alter-var-root-still-applies
  ;; Keep-green guard for the printer's dynvar resolution: an
  ;; alter-var-root on *print-length* must still truncate prints when
  ;; the ns env cell is a var rather than a raw value.
  (alter-var-root #'*print-length* (constantly 2))
  (is (= "[1 2 ...]" (pr-str [1 2 3 4])))
  (alter-var-root #'*print-length* (constantly nil)))


(deftest def-returns-var-in-fn-body
  ;; BC-compiled fn bodies: the def form's value is the Var, mirroring
  ;; eval_def and JVM Clojure.
  (is (true? ((fn [] (var? (def vr-bc-1 33))))))
  (is (true? (var? ((fn [] (def vr-bc-2 44) (def vr-bc-3 vr-bc-2)))))))

(run-tests-and-exit)
