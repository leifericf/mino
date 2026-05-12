(require "tests/test")

;; ns special form tests

(deftest ns-basic-accepted
  (is (nil? (ns test.ns.basic))))

(deftest ns-with-empty-require
  (is (nil? (ns test.ns.req
              (:require)))))

(deftest ns-rejects-jvm-only-clauses
  ;; Mino targets pure portable Clojure — :import and :gen-class
  ;; aren't honored, and the ns macro should fail loudly so users
  ;; know their JVM-coupled code can't run on this platform.
  (is (thrown? (ns test.ns.import-rejected (:import java.util.Date))))
  (is (thrown? (ns test.ns.genclass-rejected (:gen-class)))))

(deftest refer-binds-callable-vars-into-target-ns
  ;; (clojure.core/refer 'clojure.core) binds the SOURCE VAR into the
  ;; destination namespace's env (so syntax-quote sees the source ns).
  ;; The eval_symbol path must auto-deref a var found via the ns env
  ;; chain or the user sees "not a function (got var)" the moment they
  ;; try to call any referred fn.
  (in-ns 'test.refer-call)
  (clojure.core/refer 'clojure.core)
  ;; All three should resolve to callable fns; before the fix these
  ;; each surfaced as "not a function (got var)".
  (is (= 3 (+ 1 2)))
  (is (= "hi" (str "h" "i")))
  ;; Syntax-quote still qualifies the symbol back to its source ns.
  (is (= 'clojure.core/inc `inc))
  (in-ns 'user))

(deftest refer-lexical-binding-of-var-stays-a-var
  ;; The auto-deref must be ns-env-only -- a `(let [v (resolve ...)])`
  ;; binds the result lexically; the user expects to receive the var
  ;; itself (so they can call alter-var-root etc. on it).
  (def refer-test-fn (fn [x] (* x 2)))
  (let [v (resolve 'refer-test-fn)]
    (is (var? v))))
