;; Translated from upstream Clojure test suite:
;;   clojure/test-clojure/ns_libs.clj
;; Many of these will fail today; they specify target behavior for
;; first-class namespaces in mino.

(require "tests/test")

(deftest test-alias
  ;; upstream also matched: #"No namespace: epicfail found"
  (is (thrown? (alias 'bogus 'epicfail))))

(deftest test-require
  (is (thrown? (require :foo)))
  (is (thrown? (require))))

(deftest test-use
  (is (thrown? (use :foo)))
  (is (thrown? (use))))

#_ ;; JVM-only: defrecord, import, clojure.lang.Compiler$CompilerException
(deftest reimporting-deftypes
  (let [inst1 (binding [*ns* *ns*]
                (eval '(do (ns exporter)
                           (defrecord ReimportMe [a])
                           (ns importer)
                           (import exporter.ReimportMe)
                           (ReimportMe. 1))))
        inst2 (binding [*ns* *ns*]
                (eval '(do (ns exporter)
                           (defrecord ReimportMe [a b])
                           (ns importer)
                           (import exporter.ReimportMe)
                           (ReimportMe. 1 2))))]
    (testing "you can reimport a changed class and see the changes"
      (is (= [:a] (keys inst1)))
      (is (= [:a :b] (keys inst2))))))

#_ ;; JVM-only: definterface, deftype, defrecord, java.lang.* clash, IllegalStateException
(deftest naming-types
  (testing "you cannot use a name already referred from another namespace"
    (is (thrown? (definterface String)))
    (is (thrown? (deftype StringBuffer [])))
    (is (thrown? (defrecord Integer [])))))

(deftest resolution
  (let [s (gensym)]
    (are [result expr] (= result expr)
      #'clojure.core/first (ns-resolve 'clojure.core 'first)
      nil (ns-resolve 'clojure.core s)
      nil (ns-resolve 'clojure.core {'first :local-first} 'first)
      nil (ns-resolve 'clojure.core {'first :local-first} s))))

(deftest refer-error-messages
  (let [temp-ns (gensym)]
    (binding [*ns* *ns*]
      (in-ns temp-ns)
      (eval '(def ^{:private true} hidden-var)))
    (testing "referring to something that does not exist"
      ;; upstream also matched: #"nonexistent-var does not exist"
      (is (thrown? (refer temp-ns :only '(nonexistent-var)))))
    (testing "referring to something non-public"
      ;; upstream also matched: #"hidden-var is not public"
      (is (thrown? (refer temp-ns :only '(hidden-var)))))))

#_ ;; JVM-only: defrecord, deftype, clojure.lang.Compiler$CompilerException, thrown-with-cause-msg?
(deftest test-defrecord-deftype-err-msg
  (is (thrown? (eval '(defrecord MyRecord [:shutdown-fn]))))
  (is (thrown? (eval '(deftype MyType [:key1])))))

(deftest require-as-alias
  ;; :as-alias does not load
  (require '[not.a.real.ns [foo :as-alias foo]
                           [bar :as-alias bar]])
  (let [aliases (ns-aliases *ns*)
        foo-ns (get aliases 'foo)
        bar-ns (get aliases 'bar)]
    (is (= 'not.a.real.ns.foo (ns-name foo-ns)))
    (is (= 'not.a.real.ns.bar (ns-name bar-ns))))

  (is (= :not.a.real.ns.foo/baz (read-string "::foo/baz")))

  ;; can use :as-alias in use, but load will occur
  (use '[clojure.walk :as-alias e1])
  (is (= 'clojure.walk (ns-name (get (ns-aliases *ns*) 'e1))))
  (is (= :clojure.walk/walk (read-string "::e1/walk")))

  ;; can use both :as and :as-alias
  (require '[clojure.set :as n1 :as-alias n2])
  (let [aliases (ns-aliases *ns*)]
    (is (= 'clojure.set (ns-name (get aliases 'n1))))
    (is (= 'clojure.set (ns-name (get aliases 'n2))))
    (is (= (resolve 'n1/union) #'clojure.set/union))
    (is (= (resolve 'n2/union) #'clojure.set/union))))

(deftest require-as-alias-then-load-later
  ;; alias but don't load
  (require '[clojure.test-clojure.ns-libs-load-later :as-alias alias-now])
  (is (contains? (ns-aliases *ns*) 'alias-now))
  (is (not (nil? (find-ns 'clojure.test-clojure.ns-libs-load-later))))

  ;; not loaded!
  (is (nil? (resolve 'alias-now/example)))

  ;; load
  (require 'clojure.test-clojure.ns-libs-load-later)

  ;; now loaded!
  (is (not (nil? (resolve 'alias-now/example)))))
