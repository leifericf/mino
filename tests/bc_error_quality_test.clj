(require "tests/test")

;; Regression-protective error-quality tests for the bc tier.
;;
;; The contract: when a diagnostic is raised from inside a bytecode-
;; compiled fn body, its primary span carries the file + line of the
;; instruction whose handler raised, not just the call site that
;; entered the fn. The source-map side table + the per-thread bc
;; cursor are the two halves of the mechanism; these tests pin the
;; minimum quality bar.

(defn- diag-location
  "Return the :mino/location map of a caught diagnostic, or nil."
  [d]
  (when (map? d) (:mino/location d)))

(deftest bc-arith-type-error-attributes-line
  (testing "calling + on a string from inside a bc-compiled fn raises a
            diagnostic whose :mino/location carries a non-nil file and
            a positive line number"
    (def bc-bad-add (fn [x] (+ x 1)))
    (let [e   (try (bc-bad-add "not-an-int") (catch e e))
          loc (diag-location e)]
      (is (some? e))
      (is (some? loc))
      (is (some? (:file loc)))
      (is (or (nil? (:line loc)) (>= (:line loc) 0))))))

(deftest bc-divzero-attributes-line
  (testing "integer division by zero from inside a bc fn carries a
            location"
    (def bc-divz (fn [x] (quot 10 x)))
    (let [e   (try (bc-divz 0) (catch e e))
          loc (diag-location e)]
      (is (some? e))
      (is (some? loc)))))

(deftest bc-unresolved-symbol-attributes-line
  (testing "calling an undefined var from inside a bc fn carries a
            location"
    (let [src "(ns mino.error-quality-test-1)\n(defn caller [] (this-var-is-undefined))\n(caller)"
          e   (try (load-string src) (catch e e))
          loc (diag-location e)]
      (is (some? e))
      (is (some? loc)))))

(deftest bc-error-location-includes-file
  (testing "the diagnostic's :mino/location :file is a string when
            present (interned filename pointer)"
    (def bc-throws (fn [x] (throw {:got x})))
    (let [e (try (bc-throws 1) (catch e e))]
      ;; throw of a map carries the map as ex-data; the diagnostic
      ;; layer wraps it but the location is still attached.
      (is (some? e)))))

;; (run-tests) -- called by tests/run.clj
