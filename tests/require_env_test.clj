(require "tests/test")

;; Regression: require used to evaluate the loaded source in the
;; caller's lexical env. When require was called from inside a fn whose
;; scope had a local shadowing a clojure.core name (the task runner's
;; ensure-task-fn binds `ns-name` to a namespace string), every fn
;; defined in the loaded namespace captured that local, so later calls
;; to the global resolved to the local value at runtime and threw
;; "not a function (got string)". The fix loads the source in a clean
;; top-level env so loaded fns only see their own namespace's bindings.

(defn- trigger-require-from-shadowed-scope [helper-sym]
  (let [ns-name (namespace helper-sym)
        ;; Inner fn forces this fn to capture, publishing ns-name into
        ;; the lexical env -- mirrors the task-runner trigger path where
        ;; the requiring fn's local is env-visible to require.
        probe (fn [] ns-name)]
    (is (some? ns-name))
    (is (some? probe))
    (require (symbol ns-name))
    ns-name))

(deftest require-from-scope-with-shadowing-local-resolves-global
  ;; The helper is NOT pre-loaded; the require inside the shadowing
  ;; scope is its first load. Before the fix, the helper's the-ns-name
  ;; captured the local `ns-name` string and threw when called.
  (is (= "tests.require-env-helper"
         (trigger-require-from-shadowed-scope
           'tests.require-env-helper/the-ns-name)))
  (let [f (deref (resolve 'tests.require-env-helper/the-ns-name))]
    (is (= 'tests.require-env-helper (f)))))
