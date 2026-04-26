(require "tests/test")

;; Translated from upstream test/clojure/test_clojure/vars.clj
;; Var-level operations: def, intern, find-var, var-get, var-set,
;; #'foo reader form, alter-var-root, with-redefs, binding, ^:dynamic.
;; Many tests are expected to fail today; this is test-driven for adding
;; var primitives in mino.

(def ^:dynamic a)

(deftest test-binding
  ;; Upstream evaluates: (eval `(binding [a 4] a)) => 4
  ;; mino has binding; eval+syntax-quote support may vary.
  (is (= 4 (eval (list 'binding ['a 4] 'a)))))

(deftest test-with-local-vars
  ;; Requires: with-local-vars, var-set, deref on local vars.
  (let [factorial (fn [x]
                    (with-local-vars [acc 1, cnt x]
                      (while (> @cnt 0)
                        (var-set acc (* @acc @cnt))
                        (var-set cnt (dec @cnt)))
                      @acc))]
    (is (= (factorial 5) 120))))

#_(deftest test-with-precision
    ;; Skipped: BigDecimal + with-precision is JVM-specific
    ;; (java.math.MathContext, rounding modes).
    nil)

#_(deftest test-settable-math-context
    ;; Skipped: depends on clojure.main/with-bindings, set!, *math-context*,
    ;; java.math.MathContext — JVM-only.
    nil)

(def stub-me :original)

#_(deftest test-with-redefs-fn
    ;; Skipped: depends on JVM Thread, promise/deliver semantics tied to
    ;; java.lang.Thread; mino with-redefs-fn coverage uses a different shape.
    nil)

(deftest test-with-redefs
  ;; Upstream uses (.start (Thread. ...)) to deref stub-me — JVM-only.
  ;; Reduced to the var-redef semantic core: with-redefs swaps and restores.
  (with-redefs [stub-me :temp]
    (is (= :temp stub-me)))
  (is (= :original stub-me)))

(deftest test-with-redefs-throw
  ;; Upstream variant uses Thread + Exception class; mino (is (thrown? body))
  ;; takes a body, not a class. Dropped pattern: (thrown? Exception ...).
  (is (thrown? (with-redefs [stub-me :temp]
                 (throw "simulated failure in with-redefs"))))
  (is (= :original stub-me)))

(def ^:dynamic dynamic-var 1)

(deftest test-with-redefs-inside-binding
  (binding [dynamic-var 2]
    (is (= 2 dynamic-var))
    (with-redefs [dynamic-var 3]
      (is (= 2 dynamic-var))))
  (is (= 1 dynamic-var)))

(defn sample [& args]
  0)

(deftest test-vars-apply-lazily
  ;; Upstream uses future + deref-with-timeout; both forms (sample) and
  ;; (#'sample) must apply lazily over (range).
  ;; Reduced: ensure (apply sample (range N)) and (apply #'sample (range N))
  ;; both terminate on a finite head. Future/timeout is JVM-thread-bound.
  (is (= 0 (apply sample (range 1000))))
  (is (= 0 (apply (var sample) (range 1000))))
  (is (= 0 (apply #'sample (range 1000)))))

;; Additional coverage exercising var-level ops the upstream file implies
;; via its header comments (declare intern find-var var var-get var-set
;; alter-var-root). These are test-driven additions for mino.

(def ^:dynamic root-target 1)

(deftest test-var-and-reader-form-equivalent
  (def vae-x 10)
  (is (= (var vae-x) #'vae-x)))

(deftest test-var-get
  (def vg-x 42)
  (is (= 42 (var-get #'vg-x)))
  (is (= 42 (var-get (var vg-x)))))

(deftest test-find-var
  (def fv-x 7)
  ;; find-var takes a fully-qualified symbol. Use current ns 'user' if mino
  ;; defaults there; otherwise this surfaces the ns-resolution gap.
  (is (= #'fv-x (find-var (symbol (str (ns-name *ns*)) "fv-x")))))

(deftest test-intern
  ;; intern creates/gets a var in a namespace.
  (intern *ns* 'interned-x 99)
  (is (= 99 (var-get (find-var (symbol (str (ns-name *ns*)) "interned-x"))))))

(deftest test-alter-var-root
  (alter-var-root #'root-target (fn [v] (+ v 1)))
  (is (= 2 root-target))
  (alter-var-root #'root-target (fn [v] (* v 10)))
  (is (= 20 root-target)))
