(require "tests/test")

;; Tests for clojure.core/-qualified special-form dispatch and the
;; syntax-quote qualification contract.
;;
;; Canonical Clojure exposes the macro-family forms (fn, let, loop,
;; lazy-seq, binding, declare, defmacro, ns, when, and, or) as
;; clojure.core macros, so:
;;   (clojure.core/let ...) evaluates the same as (let ...)
;;   `let reader-expands to clojure.core/let
;; while the true special forms (if, do, def, quote, var, recur, try,
;; and the let*/fn*/loop* internals) stay bare and are NOT resolvable
;; under clojure.core (clojure.core/if errors on the JVM too).
;;
;; The dispatch path is mirrored on both the tree-walker
;; (eval_try_special_form) and the bytecode compiler
;; (compile_expr_dispatch), so JIT/interpreter parity holds.

;; ---------------------------------------------------------------------------
;; (1) Qualified macro-family forms evaluate identically to the bare form
;;     (tree-walker / top-level eval path).
;; ---------------------------------------------------------------------------

(deftest qsf-qualified-fn-evaluates
  (is (= 25 ((clojure.core/fn [x] (* x x)) 5)))
  (is (= 7 ((clojure.core/fn [a b] (+ a b)) 3 4))))

(deftest qsf-qualified-let-evaluates
  (is (= 3 (clojure.core/let [x 1 y 2] (+ x y))))
  (is (= 6 (clojure.core/let [x 1 y (+ x 4)] (+ x y)))))

(deftest qsf-qualified-loop-evaluates
  (is (= 10 (clojure.core/loop [i 0 acc 0]
              (if (= i 5) acc (recur (inc i) (+ acc i))))))
  (is (= 0 (clojure.core/loop [] 0))))

(deftest qsf-qualified-when-evaluates
  (is (= :yes (clojure.core/when true :yes)))
  (is (nil? (clojure.core/when false :yes))))

(deftest qsf-qualified-and-or-evaluate
  (is (= 3 (clojure.core/and 1 2 3)))
  (is (false? (clojure.core/and 1 false 3)))
  (is (= :z (clojure.core/or false nil :z)))
  (is (nil? (clojure.core/or false nil))))

(deftest qsf-qualified-binding-evaluates
  (is (= 42 (clojure.core/binding [qsf-dyn 42] qsf-dyn)))
  (is (= 0 qsf-dyn)))

(def ^:dynamic qsf-dyn 0)

(deftest qsf-qualified-lazy-seq-evaluates
  (let [ones (clojure.core/lazy-seq (cons 1 (lazy-seq (cons 1 nil))))]
    (is (= 1 (first ones)))
    (is (= 1 (second ones)))))

(deftest qsf-qualified-declare-and-defmacro-evaluate
  (clojure.core/declare qsf-fwd)
  (def qsf-fwd 99)
  (is (= 99 qsf-fwd))
  (clojure.core/defmacro qsf-unless [c t f] (list 'if c f t))
  (is (= :no (qsf-unless true :yes :no)))
  (is (= :yes (qsf-unless false :yes :no))))

;; ---------------------------------------------------------------------------
;; (2) Qualified forms work inside a defn body (bytecode-compiled path),
;;     proving the BC compiler mirror dispatches the same handler.
;; ---------------------------------------------------------------------------

(defn qsf-bc-fn [x]
  (clojure.core/let [y (clojure.core/loop [i 0 a 0]
                         (if (clojure.core/and (< i x))
                           (recur (inc i) (+ a i))
                           a))]
    (clojure.core/when (pos? y) y)))

(deftest qsf-bc-compiled-body
  (is (= 10 (qsf-bc-fn 5)))
  (is (nil? (qsf-bc-fn 0))))

(defn qsf-bc-pow [n]
  (clojure.core/loop [i 0 acc 1]
    (if (= i n) acc (recur (inc i) (* acc 2)))))

(deftest qsf-bc-compiled-loop
  (is (= 1 (qsf-bc-pow 0)))
  (is (= 1024 (qsf-bc-pow 10))))

;; ---------------------------------------------------------------------------
;; (3) Syntax-quote qualifies the macro family to clojure.core/X, and keeps
;;     the true special forms bare -- matching canonical syntax-quote.
;; ---------------------------------------------------------------------------

(deftest qsf-syntax-quote-qualifies-macro-family
  (is (= 'clojure.core/when `when))
  (is (= 'clojure.core/fn `fn))
  (is (= 'clojure.core/let `let))
  (is (= 'clojure.core/loop `loop))
  (is (= 'clojure.core/and `and))
  (is (= 'clojure.core/or `or))
  (is (= 'clojure.core/binding `binding))
  (is (= 'clojure.core/lazy-seq `lazy-seq))
  (is (= 'clojure.core/declare `declare))
  (is (= 'clojure.core/defmacro `defmacro))
  (is (= 'clojure.core/ns `ns)))

(deftest qsf-syntax-quote-keeps-true-special-forms-bare
  (is (= 'if `if))
  (is (= 'do `do))
  (is (= 'def `def))
  (is (= 'quote `quote))
  (is (= 'recur `recur))
  (is (= 'try `try))
  (is (= 'var `var)))

;; ---------------------------------------------------------------------------
;; (4) A macro whose template syntax-quotes one of the family names expands
;;     to the qualified symbol and still evaluates -- the regression that
;;     motivated this change (a redefinition of `when` must not capture the
;;     macro's `when`).
;; ---------------------------------------------------------------------------

(defmacro qsf-guard [c body]
  `(when ~c ~body))

(deftest qsf-macro-template-uses-qualified-when
  (is (= :ok (qsf-guard true :ok)))
  (is (nil? (qsf-guard false :ok)))
  ;; The expansion carries the qualified symbol.
  (is (= 'clojure.core/when (first (macroexpand-1 '(qsf-guard true :ok))))))

;; ---------------------------------------------------------------------------
;; (5) The true special forms are NOT resolvable under clojure.core as a
;;     qualified head -- (clojure.core/if ...) errors, matching the JVM
;;     (if is a special form, never a clojure.core var).
;; ---------------------------------------------------------------------------

(deftest qsf-qualified-true-special-form-errors
  (is (thrown? :error (eval '(clojure.core/if true 1 2))))
  (is (thrown? :error (eval '(clojure.core/quote x))))
  (is (thrown? :error (eval '(clojure.core/recur)))))

;; ---------------------------------------------------------------------------
;; (6) macroexpand-1 of a qualified family form returns it unchanged -- the
;;     special-form handler wins over any macro lookup on the qualified head.
;; ---------------------------------------------------------------------------

(deftest qsf-macroexpand-qualified-unchanged
  (is (= '(clojure.core/let [x 1] x)
         (macroexpand-1 '(clojure.core/let [x 1] x))))
  (is (= '(clojure.core/when true 1)
         (macroexpand-1 '(clojure.core/when true 1)))))

(run-tests-and-exit)
