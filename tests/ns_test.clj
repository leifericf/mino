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
