(ns tests.require-env-helper)

;; Regression fixture for require_env_test. `the-ns-name` calls the
;; clojure.core/ns-name global. If require ever evaluates this file in
;; a caller's lexical env (the bug), this fn captures the caller's
;; local `ns-name` and the global resolves to that local's value
;; instead of clojure.core/ns-name.

(defn the-ns-name []
  (ns-name (the-ns 'tests.require-env-helper)))
