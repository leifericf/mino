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
