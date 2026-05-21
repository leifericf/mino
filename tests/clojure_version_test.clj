(require "tests/test")

;; *clojure-version*, (clojure-version), and the JVM AOT-compiler
;; dynvars shipped for shape parity (mino has no AOT compiler; the
;; dynvars are bindable but have no observable effect).

(deftest clojure-version-map-shape
  (is (map? *clojure-version*))
  (is (integer? (:major *clojure-version*)))
  (is (integer? (:minor *clojure-version*)))
  (is (integer? (:incremental *clojure-version*)))
  (is (contains? *clojure-version* :qualifier)))

(deftest clojure-version-string
  (let [s (clojure-version)]
    (is (string? s))
    (is (re-find #"\d+\.\d+\.\d+" s))))

(deftest jvm-aot-dynvars-have-canon-defaults
  (is (nil? *compile-path*))
  (is (= "NO_SOURCE_PATH" *source-path*))
  (is (false? *compile-files*))
  (is (false? *warn-on-reflection*))
  (is (false? *unchecked-math*)))

(deftest jvm-aot-dynvars-bindable
  (is (= "target" (binding [*compile-path* "target"] *compile-path*)))
  (is (= "foo.clj" (binding [*source-path* "foo.clj"] *source-path*)))
  (is (true?  (binding [*compile-files* true]   *compile-files*)))
  (is (true?  (binding [*warn-on-reflection* true] *warn-on-reflection*)))
  (is (true?  (binding [*unchecked-math* true]  *unchecked-math*))))
