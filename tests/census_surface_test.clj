(require "tests/test")

;; Spec-first tests for upcoming src/core.clj changes.
;; Most of these FAIL on the current branch (expected): they describe
;; the intended behavior and will pass once each implementation lands.
;;
;; (1) refer-clojure macro  — resolves nil today; all its tests fail
;; (2) *clojure-version* ^:dynamic — not dynamic today; binding fails
;;
;; GC-stress note: all tests here are allocation-light and pass
;; under MINO_GC_STRESS=1 (no large heap builds).

;; ---------------------------------------------------------------------------
;; (1) refer-clojure macro
;;
;; Clojure canonical: (defmacro refer-clojure [& filters]
;;                       (refer 'clojure.core ~@filters))
;;
;; The standalone macro must work the same way the :refer-clojure ns
;; clause does — it delegates to the existing `refer` primitive so
;; :exclude / :only / :rename are honored by that primitive's logic.
;;
;; NOTE: these tests will fail until the macro is defined, producing
;; an unbound-symbol or compile error shape.
;; ---------------------------------------------------------------------------

(deftest cs-refer-clojure-resolves-as-macro
  ;; refer-clojure should exist in clojure.core and be a macro.
  (is (some? (resolve 'refer-clojure)))
  (is (true? (:macro (meta (resolve 'refer-clojure))))))

(deftest cs-refer-clojure-exclude-hides-name
  ;; After entering a fresh ns and calling (refer-clojure :exclude [inc]),
  ;; `inc` must not resolve in that ns but `dec` must still resolve.
  ;; (The ns macro already wires clojure.core; refer-clojure re-runs
  ;; the filter so excluded names are gone.)
  (in-ns 'test.cs.rc-exclude)
  (clojure.core/refer-clojure :exclude [inc])
  (is (nil? (resolve 'inc)))
  (is (some? (resolve 'dec)))
  (in-ns 'user))

(deftest cs-refer-clojure-rename-adds-alias
  ;; After (refer-clojure :rename {inc plus1}) in a fresh ns:
  ;;   plus1 resolves and calls the original clojure.core/inc
  ;;   inc itself no longer resolves under the original name
  (in-ns 'test.cs.rc-rename)
  (clojure.core/refer-clojure :rename {inc plus1})
  (is (some? (resolve 'plus1)))
  (is (nil? (resolve 'inc)))
  (is (= 6 (plus1 5)))
  (in-ns 'user))

(deftest cs-refer-clojure-only-limits-to-set
  ;; (refer-clojure :only [+ -]) brings in exactly those two names.
  (in-ns 'test.cs.rc-only)
  (clojure.core/refer-clojure :only [+ -])
  (is (some? (resolve '+)))
  (is (some? (resolve '-)))
  ;; A core fn not in the :only list must not be directly referred.
  (is (nil? (resolve 'inc)))
  (in-ns 'user))

(deftest cs-refer-clojure-no-args-refers-all-public
  ;; (refer-clojure) with no filters is equivalent to (refer 'clojure.core)
  ;; — all public core names become available.
  (in-ns 'test.cs.rc-no-args)
  (clojure.core/refer-clojure)
  (is (some? (resolve 'map)))
  (is (some? (resolve 'filter)))
  (is (some? (resolve 'reduce)))
  (in-ns 'user))

;; ---------------------------------------------------------------------------
;; (2) *clojure-version* becomes ^:dynamic
;;
;; Today: `(def *clojure-version* ...)` — no :dynamic flag, binding throws.
;; Target: `(def ^:dynamic *clojure-version* ...)` — binding is allowed;
;;   the default value shape stays unchanged (map with :major/:minor keys);
;;   (clojure-version) still returns a string.
;;
;; The binding test will fail until ^:dynamic is added, with:
;;   :eval/binding / Can't dynamically bind non-dynamic var
;; ---------------------------------------------------------------------------

(deftest cs-clojure-version-var-is-dynamic
  ;; The var's meta must carry :dynamic true.
  (is (true? (:dynamic (meta (resolve '*clojure-version*))))))

(deftest cs-clojure-version-binding-shadows
  ;; After the fix, binding must override the default value inside
  ;; the scope and restore it outside.
  (let [result (binding [*clojure-version* {:major 99 :minor 0
                                            :incremental 0 :qualifier nil}]
                 *clojure-version*)]
    (is (= 99 (:major result))))
  ;; Outside the binding, the original value is restored.
  (is (= 1 (:major *clojure-version*))))

(deftest cs-clojure-version-binding-does-not-affect-string-fn
  ;; (clojure-version) reads *clojure-version* dynamically, so a binding
  ;; that changes :major must be reflected in the string result.
  (let [s (binding [*clojure-version* {:major 99 :minor 0
                                       :incremental 0 :qualifier nil}]
            (clojure-version))]
    (is (clojure.string/starts-with? s "99."))))

(deftest cs-clojure-version-default-value-unchanged
  ;; The default unbound value still has the canonical shape.
  (is (map? *clojure-version*))
  (is (integer? (:major *clojure-version*)))
  (is (integer? (:minor *clojure-version*)))
  (is (integer? (:incremental *clojure-version*)))
  (is (contains? *clojure-version* :qualifier))
  (is (string? (clojure-version)))
  (is (re-find #"\d+\.\d+\.\d+" (clojure-version))))

;; ---------------------------------------------------------------------------
;; (3) vswap! is a macro
;;
;; All vswap! assertions pass. vswap! is a defmacro matching JVM Clojure.
;;
;; In JVM Clojure, vswap! is defined as:
;;   (defmacro vswap! [vol f & args]
;;     (list 'vreset! vol (list* f (list 'clojure.core/deref vol) args)))
;;
;; The macro form lets the compiler inline the deref+call+vreset! without
;; boxing the extra args into a variadic apply call. Like the JVM macro,
;; the vol expression appears twice in the expansion (once for vreset!, once
;; for deref), matching canonical JVM behavior.
;; ---------------------------------------------------------------------------

(deftest cs-vswap-behavior-inc
  ;; vswap! applies f to the current value and stores/returns the result.
  (let [v (volatile! 0)]
    (is (= 1 (vswap! v inc)))
    (is (= 1 @v))))

(deftest cs-vswap-behavior-variadic
  ;; vswap! passes extra args after the volatile to f.
  (let [v (volatile! 1)]
    (is (= 6 (vswap! v + 2 3)))
    (is (= 6 @v))))

(deftest cs-vswap-mutates-in-place
  ;; Each vswap! sees the updated value from the previous call.
  (let [v (volatile! 0)]
    (vswap! v inc)
    (vswap! v inc)
    (vswap! v inc)
    (is (= 3 @v))))

(deftest cs-vswap-returns-new-value
  ;; Return value is the new value (same as vreset!'s return).
  (let [v (volatile! 10)]
    (is (= 20 (vswap! v * 2)))
    (is (= 40 (vswap! v * 2)))))

(deftest cs-vswap-is-macro
  ;; After the implementation change, vswap! must carry :macro true,
  ;; matching every other defmacro'd name (e.g. `when`).
  ;;
  ;; Canonical assertion shape sourced from macro_test.clj:
  ;;   (is (true? (:macro (meta (resolve s)))))
  (is (true? (:macro (meta (resolve 'vswap!))))))

(run-tests-and-exit)
