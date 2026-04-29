;; Fault injection test runner.
;; Usage: ./mino tests/fault_inject_runner.clj

(require "tests/test")
(require "tests/fault_inject_test")

(run-tests-and-exit)
