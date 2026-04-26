;; Namespace parity runner.
;;
;; Loads the namespace-mechanic test corpus drawn from upstream sources
;; (Clojure ns_libs.clj / reader.clj / vars.clj as gold standard;
;; ClojureScript ns_test.cljs and Babashka SCI namespaces_test.cljc as
;; checkpoints) and reports PASS / FAIL / ERROR counts.
;;
;; Most tests are RED today (Phase 0 of the namespace-correctness cycle).
;; The PASS count climbs as Phase A primitives land.
;;
;; Usage:  ./mino tests/ns_parity_run.clj

(require "tests/test")

(require "tests/ns_isolation_test")          ;; mino-specific central regression
(require "tests/ns_libs_test")                ;; Clojure ns_libs.clj
(require "tests/ns_reader_test")              ;; Clojure reader.clj subset
(require "tests/ns_vars_test")                ;; Clojure vars.clj
(require "tests/ns_clojure_strict_test")      ;; Clojure-not-CLJS divergences
(require "tests/ns_cljs_checkpoint_test")     ;; ClojureScript ns_test.cljs
(require "tests/ns_sci_checkpoint_test")      ;; Babashka SCI namespaces_test.cljc

(run-tests)
