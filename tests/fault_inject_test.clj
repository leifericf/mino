(require "tests/test")

;; Fault injection tests: verify that simulated OOM is catchable and
;; does not corrupt state.

(deftest fi-alloc-caught
  (testing "simulated OOM is caught by try/catch"
    (is (= :caught
           (try
             (do (set-fail-alloc-at! 3)
                 (vec (range 1000))
                 :not-caught)
             (catch e :caught))))))

(deftest fi-alloc-message
  (testing "OOM exception has correct message"
    (is (= "out of memory (fault injection)"
           (try
             (do (set-fail-alloc-at! 2)
                 (conj [] 1 2 3))
             (catch e (ex-message e)))))))

(deftest fi-state-recovers
  (testing "allocator works normally after fault"
    (try
      (do (set-fail-alloc-at! 2)
          (vec (range 100)))
      (catch e nil))
    ;; After the fault fires, normal allocation should work.
    (is (= [1 2 3] (vec [1 2 3])))))

(deftest fi-disabled-by-zero
  (testing "passing 0 disables fault injection"
    (set-fail-alloc-at! 0)
    (is (= 10 (count (vec (range 10)))))))
