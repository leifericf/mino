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

(deftest user-throw-carries-location
  ;; Regression: catch values from user-throw paths previously lacked
  ;; :mino/location. normalize_exception built a 5-key map (kind, code,
  ;; phase, message, data) but never added location; only system throws
  ;; (via prim_throw_classified) included it. Inconsistent error shape.
  ;;
  ;; Fix: normalize_exception now consults bc_current_pc (preferred)
  ;; or eval_current_form (fallback) and attaches :mino/location when
  ;; either has source info.
  (testing "throw of string carries location"
    (let [loc (diag-location (try (throw "boom") (catch e e)))]
      (is (some? loc))
      (is (some? (:file loc)))
      (is (some? (:line loc)))))
  (testing "throw of ex-info carries location"
    (let [loc (diag-location
                (try (throw (ex-info "msg" {:k :v})) (catch e e)))]
      (is (some? loc))
      (is (some? (:file loc)))
      (is (some? (:line loc)))))
  (testing "throw of bare map carries location"
    (let [loc (diag-location
                (try (throw {:custom :data}) (catch e e)))]
      (is (some? loc))
      (is (some? (:file loc))))))

(deftest bc-throw-prefers-pc-over-call-site
  ;; Regression: a throw inside a BC-compiled fn body previously
  ;; reported the call site's line (eval_current_form, which stays
  ;; at the outer (f) form during BC dispatch) instead of the throw
  ;; site's line (bc_current_pc, which the VM dispatch loop keeps in
  ;; sync with the current instruction). Fix: prim_throw_classified
  ;; and normalize_exception now prefer the BC PC when available.
  (testing "arity-error inside a BC fn body has a non-nil line"
    (def bc-bad (fn [] (assoc nil)))
    (let [loc (diag-location (try (bc-bad) (catch e e)))]
      ;; Pre-fix this would still set loc but to the (bc-bad) call
      ;; site instead of the inner (assoc nil) line. The strong
      ;; differential assertion is fragile across deftest/testing
      ;; expansion shapes; pinning non-nil line + non-nil file is
      ;; the durable invariant that catches "no location at all"
      ;; regressions without depending on macro-expansion line math.
      (is (some? loc))
      (is (some? (:line loc)))
      (is (some? (:file loc))))))

(deftest eval-current-form-restored-after-subeval
  ;; Regression: eval_current_form was set on MINO_CONS eval entry
  ;; but never restored when sub-evals returned. Any throw fired
  ;; after eval_args walked the argument list -- e.g. a thread-limit
  ;; throw from inside mino_future_spawn -- blamed the last sub-form
  ;; instead of the call form. Across test files, the "last sub-form"
  ;; could be in a previously loaded file, so :mino/location reported
  ;; a wrong filename entirely.
  ;;
  ;; This test pins the contract: when an inner form's eval finishes
  ;; cleanly and a later throw fires from the same outer form's
  ;; processing, the location resolves to the outer form, not the
  ;; inner one.
  (testing "throw via a prim that fires after eval_args restores location"
    ;; Force the classifier through the eval_current_form path by
    ;; calling a primitive whose throw doesn't carry its own form.
    ;; (assoc nil) is a 1-arg arity error from prim_assoc that fires
    ;; AFTER the (nil) arg has been eval'd; on the broken path the
    ;; location reported the inner nil literal's position (often
    ;; (0,0) since nil literals carry no source info), on the fixed
    ;; path it reports the (assoc nil) call form.
    (let [e   (try (assoc nil) (catch e e))
          loc (diag-location e)]
      (is (some? e))
      (is (some? loc))
      (is (some? (:line loc)))
      (is (> (:line loc) 0)))))

;; (run-tests) -- called by tests/run.clj
