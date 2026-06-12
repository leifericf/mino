(require "tests/test")
(require '[clojure.repl :refer [doc-string]])

;; Spec-first tests for upcoming C special-form / var introspection.
;; Covers three planned changes:
;;
;; (1) The C special forms/macros fn, let, loop, lazy-seq, binding,
;;     declare, defmacro, ns will be registered as clojure.core vars so
;;     they appear in (ns-publics 'clojure.core), (resolve '<name>)
;;     returns a var with :macro true and a non-empty :doc string.
;;     Evaluation and macroexpand-1 behavior must be UNCHANGED.
;;
;; (2) *ns* will gain a clojure.core env binding so it appears in
;;     (ns-publics 'clojure.core) and (resolve '*ns*) returns its var.
;;
;; (3) clojure.core/pr will become dynamic: (binding [pr ...] ...) must
;;     work, and normal pr behavior is restored outside the binding.
;;
;; These tests are expected to FAIL until the implementation lands.
;; Failure shape: "missing from ns-publics / resolve nil" rather than
;; test-code errors.

;; --- (1) ns-publics coverage: all 9 names must appear ---

(def intros-special-forms
  '[fn let loop lazy-seq binding declare defmacro ns])

(deftest intros-fn-in-ns-publics
  (is (contains? (ns-publics 'clojure.core) 'fn)))

(deftest intros-let-in-ns-publics
  (is (contains? (ns-publics 'clojure.core) 'let)))

(deftest intros-loop-in-ns-publics
  (is (contains? (ns-publics 'clojure.core) 'loop)))

(deftest intros-lazy-seq-in-ns-publics
  (is (contains? (ns-publics 'clojure.core) 'lazy-seq)))

(deftest intros-binding-in-ns-publics
  (is (contains? (ns-publics 'clojure.core) 'binding)))

(deftest intros-declare-in-ns-publics
  (is (contains? (ns-publics 'clojure.core) 'declare)))

(deftest intros-defmacro-in-ns-publics
  (is (contains? (ns-publics 'clojure.core) 'defmacro)))

(deftest intros-ns-in-ns-publics
  (is (contains? (ns-publics 'clojure.core) 'ns)))

(deftest intros-when-in-ns-publics
  (is (contains? (ns-publics 'clojure.core) 'when)))

(deftest intros-and-in-ns-publics
  (is (contains? (ns-publics 'clojure.core) 'and)))

(deftest intros-or-in-ns-publics
  (is (contains? (ns-publics 'clojure.core) 'or)))

;; (2) *ns* in ns-publics

(deftest intros-star-ns-in-ns-publics
  (is (contains? (ns-publics 'clojure.core) '*ns*)))

;; --- resolve returns a var for each name ---

(deftest intros-resolve-fn-is-var
  (is (var? (resolve 'fn))))

(deftest intros-resolve-let-is-var
  (is (var? (resolve 'let))))

(deftest intros-resolve-loop-is-var
  (is (var? (resolve 'loop))))

(deftest intros-resolve-lazy-seq-is-var
  (is (var? (resolve 'lazy-seq))))

(deftest intros-resolve-binding-is-var
  (is (var? (resolve 'binding))))

(deftest intros-resolve-declare-is-var
  (is (var? (resolve 'declare))))

(deftest intros-resolve-defmacro-is-var
  (is (var? (resolve 'defmacro))))

(deftest intros-resolve-ns-is-var
  (is (var? (resolve 'ns))))

(deftest intros-resolve-star-ns-is-var
  (is (var? (resolve '*ns*))))

;; --- :macro true on the 8 special-form vars ---
;; Probe pattern: (:macro (meta (resolve 'when))) => true on an existing
;; macro. The same assertion must hold for all 8 registered forms.

(deftest intros-fn-has-macro-true
  (is (true? (:macro (meta (resolve 'fn))))))

(deftest intros-let-has-macro-true
  (is (true? (:macro (meta (resolve 'let))))))

(deftest intros-loop-has-macro-true
  (is (true? (:macro (meta (resolve 'loop))))))

(deftest intros-lazy-seq-has-macro-true
  (is (true? (:macro (meta (resolve 'lazy-seq))))))

(deftest intros-binding-has-macro-true
  (is (true? (:macro (meta (resolve 'binding))))))

(deftest intros-declare-has-macro-true
  (is (true? (:macro (meta (resolve 'declare))))))

(deftest intros-defmacro-has-macro-true
  (is (true? (:macro (meta (resolve 'defmacro))))))

(deftest intros-ns-has-macro-true
  (is (true? (:macro (meta (resolve 'ns))))))

(deftest intros-when-has-macro-true
  (is (true? (:macro (meta (resolve 'when))))))

(deftest intros-and-has-macro-true
  (is (true? (:macro (meta (resolve 'and))))))

(deftest intros-or-has-macro-true
  (is (true? (:macro (meta (resolve 'or))))))

;; --- :doc strings are non-empty for all 8 forms ---
;; doc-string returns (:doc (meta (resolve name))) via clojure.repl.

(deftest intros-fn-has-doc
  (is (string? (doc-string 'fn)))
  (is (pos? (count (doc-string 'fn)))))

(deftest intros-let-has-doc
  (is (string? (doc-string 'let)))
  (is (pos? (count (doc-string 'let)))))

(deftest intros-loop-has-doc
  (is (string? (doc-string 'loop)))
  (is (pos? (count (doc-string 'loop)))))

(deftest intros-lazy-seq-has-doc
  (is (string? (doc-string 'lazy-seq)))
  (is (pos? (count (doc-string 'lazy-seq)))))

(deftest intros-binding-has-doc
  (is (string? (doc-string 'binding)))
  (is (pos? (count (doc-string 'binding)))))

(deftest intros-declare-has-doc
  (is (string? (doc-string 'declare)))
  (is (pos? (count (doc-string 'declare)))))

(deftest intros-defmacro-has-doc
  (is (string? (doc-string 'defmacro)))
  (is (pos? (count (doc-string 'defmacro)))))

(deftest intros-ns-has-doc
  (is (string? (doc-string 'ns)))
  (is (pos? (count (doc-string 'ns)))))

;; --- macroexpand-1 must return the form UNCHANGED for special forms ---
;; Special-form dispatch wins over any placeholder macro value, so the
;; registering of a var must not change expand behavior.

(deftest intros-macroexpand-1-let-unchanged
  (is (= '(let [x 1] x) (macroexpand-1 '(let [x 1] x)))))

(deftest intros-macroexpand-1-fn-unchanged
  (is (= '(fn [x] x) (macroexpand-1 '(fn [x] x)))))

(deftest intros-macroexpand-1-loop-unchanged
  (is (= '(loop [x 1] x) (macroexpand-1 '(loop [x 1] x)))))

(deftest intros-macroexpand-1-binding-unchanged
  (is (= '(binding [x 1] x) (macroexpand-1 '(binding [x 1] x)))))

;; --- evaluation regression: each special form must still work normally ---

(deftest intros-eval-let-still-works
  (is (= 1 (let [x 1] x)))
  (is (= 3 (let [a 1 b 2] (+ a b)))))

(deftest intros-eval-fn-still-works
  (let [f (fn [x] (* x x))]
    (is (= 4 (f 2)))
    (is (= 9 (f 3)))))

(deftest intros-eval-loop-still-works
  (is (= 10 (loop [i 0 acc 0]
              (if (= i 5)
                acc
                (recur (inc i) (+ acc i))))))
  (is (= 0 (loop [] 0))))

(deftest intros-eval-lazy-seq-still-works
  (let [ones (lazy-seq (cons 1 (lazy-seq (cons 1 nil))))]
    (is (= 1 (first ones)))
    (is (= 1 (second ones)))))

(def ^:dynamic intros-dyn-var 0)

(deftest intros-eval-binding-still-works
  (is (= 42 (binding [intros-dyn-var 42] intros-dyn-var)))
  (is (= 0 intros-dyn-var)))

(deftest intros-eval-declare-still-works
  (declare intros-fwd-target)
  (def intros-fwd-target 99)
  (is (= 99 intros-fwd-target)))

(deftest intros-eval-defmacro-still-works
  (defmacro intros-my-unless [c t f] (list 'if c f t))
  (is (= :yes (intros-my-unless false :yes :no)))
  (is (= :no  (intros-my-unless true  :yes :no))))

(deftest intros-eval-ns-still-works
  ;; ns form: switching namespace and back must not error.
  (let [orig (ns-name *ns*)]
    (in-ns 'intros-ns-regression-probe)
    (in-ns orig))
  (is (= 'user (ns-name *ns*))))

;; --- (2) *ns* var tracks the current namespace ---

(deftest intros-star-ns-resolve-is-var
  (let [v (resolve '*ns*)]
    (is (some? v))
    (is (var? v))))

;; Divergence from JVM Clojure: mino's *ns* var holds the namespace name as a
;; symbol, not a clojure.lang.Namespace object. On JVM, (deref (resolve '*ns*))
;; returns a Namespace and would not be = to (ns-name *ns*) which returns a Symbol.

(deftest intros-star-ns-deref-matches-current
  (let [v (resolve '*ns*)]
    (is (= (ns-name *ns*) (deref v)))))

;; --- (3) pr becomes dynamic ---
;; In JVM Clojure, clojure.core/pr is not ^:dynamic and cannot be rebound with binding.
;; mino marks it dynamic as a deliberate extension.

(deftest intros-pr-dynamic-binding-reroutes
  ;; When pr is bound to a custom fn, calling pr invokes that fn.
  (let [out (with-out-str
              (binding [pr (fn [& xs] (print "X"))]
                (pr 1)))]
    (is (= "X" out))))

(deftest intros-pr-dynamic-binding-restored-after
  ;; Outside the binding, pr must still print values in the normal
  ;; readable form (not the replacement fn's behavior).
  (binding [pr (fn [& xs] (print "REPLACED"))] nil)
  (is (= "42" (with-out-str (pr 42)))))

(deftest intros-pr-normal-outside-binding
  ;; Baseline: pr still works normally when no dynamic binding is active.
  (is (= "\"hello\"" (with-out-str (pr "hello"))))
  (is (= "42" (with-out-str (pr 42)))))

(run-tests-and-exit)
