(require "tests/test")

;; Extended require syntax tests

(deftest require-string-still-works
  (is (= 1 1))) ;; if we got here, string require worked (test.clj loaded)

(deftest require-vector-syntax-succeeds-for-existing-module
  ;; Vector form must successfully load a real module and install the
  ;; alias. Returns nil from the ns form on success.
  (is (nil? (ns test.require.vec
              (:require [tests.test :as t])))))

(deftest require-vector-syntax-throws-on-missing-module
  ;; Missing modules used to be silently ignored inside ns :require
  ;; clauses; surface them as a load error instead so the programmer
  ;; gets a diagnostic instead of a broken alias.
  (is (thrown? (ns test.require.missing
                 (:require [tests.module.does.not.exist :as x]))))
  (is (thrown? (ns test.require.missing2
                 (:require tests.module.does.not.exist)))))
