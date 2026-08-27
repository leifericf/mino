(require "tests/test")

;; var-quote reader macro and var special form tests

(def my-var 42)

(deftest var-returns-var-object
  (is (= :var (type (var my-var)))))

(deftest var-quote-returns-var-object
  (is (= :var (type #'my-var))))

(deftest var-same-identity
  (is (= (var my-var) #'my-var)))

(deftest var-on-function
  (is (= :var (type #'inc))))

(deftest var-quote-round-trip
  (is (= #'map #'map)))

;; Install-time var interning: every core primitive is interned as a
;; clojure.core var at install, and the ns env binding holds the var
;; object. Reads deref; var identity is stable across spellings.
(deftest var-prim-identity-stable
  (is (identical? #'inc (var inc)))
  (is (identical? (var inc) (resolve 'inc)))
  (is (identical? (var clojure.core/inc) (var inc))))

(deftest var-prim-read-derefs
  (is (fn? inc))
  (is (= 42 (inc 41)))
  (is (= 3 (count [1 2 3]))))

(deftest var-lexical-binding-preserves-var
  ;; A lexical binding that holds a var must keep the var, not deref it.
  (is (var? (let [v (resolve 'inc)] v)))
  (is (= 42 (let [v (resolve 'inc)] (@v 41)))))

(deftest var-altered-root-visible-to-reads
  ;; The env cell holds the var, so a root swap through the var API is
  ;; visible to plain symbol reads (JVM semantics).
  (let [orig (deref #'str)]
    (try
      (alter-var-root #'str (constantly (fn [& _] :changed)))
      (is (= :changed (str 1 2)))
      (is (= :changed ((deref #'str) 1 2)))
      (finally
        (alter-var-root #'str (constantly orig)))))
  (is (= "12" (str 1 2))))

(deftest var-refer-shares-install-var
  ;; A fresh ns referring clojure.core must see the same var object.
  (in-ns 'var.quote.refer-test)
  (clojure.core/refer 'clojure.core)
  (is (identical? (resolve 'inc) (var clojure.core/inc)))
  (is (= 42 (inc 41)))
  (in-ns 'user))
