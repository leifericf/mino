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

(deftest refer-accepts-list-and-vector
  ;; JVM Clojure accepts any sequential collection for :refer; the
  ;; canonical idiom is a vector but portable code (and several
  ;; widely-used libraries) uses a parenthesised list:
  ;;   [clojure.string :refer (blank? join)]
  ;; mino used to reject the list form with
  ;;   ":refer requires a vector of symbols or :all"
  ;; even though canon treats list and vector identically here.
  (ns test.refer.list-form
    (:require [clojure.string :refer (blank? join)]))
  (is (true?  (blank? "")))
  (is (= "a,b" (join "," ["a" "b"])))
  ;; Same shape from the top-level (require) form path.
  (require '[clojure.string :refer (trim)])
  (is (= "abc" (trim "  abc  "))))

(deftest refer-macros-imports-like-refer
  ;; mino has no JVM-compile-time vs runtime split, so :refer-macros
  ;; carries the same meaning as :refer -- the listed names are bound
  ;; in the current namespace. CLJS-shaped portability requires that
  ;; specify macro imports via :refer-macros load cleanly on mino.
  (ns test.refer.macros1
    (:require [clojure.string :refer-macros [blank?]]))
  (is (false? (blank? "x")))
  (is (true?  (blank? "")))
  ;; Mixed form: :refer-macros and :refer in the same spec.
  (ns test.refer.macros2
    (:require [clojure.string :refer-macros [trim] :refer [join]]))
  (is (= "abc" (trim "  abc  ")))
  (is (= "a,b" (join "," ["a" "b"])))
  ;; :refer-macros :all mirrors :refer :all.
  (ns test.refer.macros3
    (:require [clojure.string :refer-macros :all]))
  (is (false? (blank? "x"))))
