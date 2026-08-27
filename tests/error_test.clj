(require "tests/test")

;; Error handling: try/catch/throw.

(deftest try-no-throw
  (is (= 3 (try (+ 1 2) (catch e :fail)))))

(deftest try-catch-string
  (is (= "caught: oops" (try (throw "oops") (catch e (str "caught: " (ex-data e)))))))

(deftest try-catch-number
  (is (= 420 (try (throw 42) (catch e (* (ex-data e) 10))))))

(deftest try-catch-nil
  (is (= true (try (throw nil) (catch e (nil? (ex-data e)))))))

(deftest try-catch-map
  (is (= :v (try (throw {:k :v}) (catch e (get (ex-data e) :k))))))

(deftest try-nested
  (is (= 2 (try (try (throw 1) (catch e (throw (+ (ex-data e) 1)))) (catch e (ex-data e))))))

(deftest try-fn-throw
  (def boom__et (fn [] (throw "bang")))
  (is (= "bang" (try (boom__et) (catch e (ex-data e))))))

(deftest throw-unhandled
  (def et-x (try (throw "err") (catch e (ex-data e))))
  (is (= "err" et-x)))

(deftest thrown-assertion
  (is (thrown? (throw "boom"))))

(deftest catch-is-diagnostic-map
  (is (error? (try (throw "x") (catch Throwable e e)))))

(deftest catch-preserves-diagnostic-maps
  (let [m {:mino/kind :user :mino/code "MUS001" :mino/message "test"}]
    (is (= :user (:mino/kind (try (throw m) (catch Throwable e e)))))))

(deftest call-depth-limit
  (testing "runaway non-tail recursion raises a catchable limit error"
    (is (= :caught
           (try ((fn deep [n] (+ 1 (deep (inc n)))) 0)
             (catch e :caught)))))
  (testing "the limit error carries the limit diagnostics"
    (let [e (try ((fn deep [n] (+ 1 (deep (inc n)))) 0)
              (catch Throwable e e))]
      (is (= "MLM004" (:mino/code e)))
      (is (true? (clojure.string/includes? (ex-message e) "stack overflow")))))
  (testing "bounded non-tail recursion within the limit completes"
    (is (= 500 ((fn deep [n] (if (= n 500) 0 (inc (deep (inc n))))) 0))))
  (testing "loop/recur depth is unaffected by the call-depth limit"
    (is (= 1000000 (loop [i 0] (if (= i 1000000) i (recur (inc i)))))))
  (testing "runaway recursion on a worker thread is catchable too"
    (is (= :caught
           @(future (try ((fn deep [n] (+ 1 (deep (inc n)))) 0)
                      (catch e :caught)))))))

(deftest ex-info-data-must-be-a-map
  (is (thrown? (ex-info "m" :not-a-map)))
  (is (thrown? (ex-info "m" [1 2])))
  (is (thrown? (ex-info "m" :bad (ex-info "cause" {}))))
  (is (= {:k 1} (ex-data (try (throw (ex-info "m" {:k 1})) (catch Throwable e e)))))
  (is (nil? (ex-data (try (throw (ex-info "m" nil)) (catch Throwable e e)))))
  (is (= {:a 1} (into {} (ex-data (try (throw (ex-info "m" (sorted-map :a 1)))
                                    (catch Throwable e e)))))))

(deftest throw-location-tracks-each-throw-site
  ;; Each caught error reports its own throw site, including a throw
  ;; from a lazy thunk realized after an earlier caught error.
  (let [first-line (:line (:mino/location
                            (try ((fn [] (throw (ex-info "a" {}))))
                              (catch Throwable e e))))
        lz (lazy-seq (throw (ex-info "b" {})))
        lazy-line (:line (:mino/location (try (first lz) (catch Throwable e e))))]
    (is (pos? first-line))
    (is (pos? lazy-line))
    (is (< first-line lazy-line))))

(deftest classed-catch-binds-diagnostic
  (testing "Exception catches and binds the diagnostic map"
    (is (= 1 (try (throw (ex-info "boom" {:a 1}))
                  (catch Exception e (:a (ex-data e))))))
    (is (= :user (try (throw (ex-info "x" {}))
                      (catch Exception e (:mino/kind e)))))))

(deftest classed-catch-declines-out-of-class
  (testing "an Error clause declines a user throw, outer catches"
    (is (= :outer (try (try (throw (ex-info "x" {}))
                            (catch Error e :err))
                       (catch Exception e :outer)))))
  (testing "an ExceptionInfo clause declines a system bounds error"
    (is (= :outer (try (try (nth [1] 5)
                            (catch ExceptionInfo e :ei))
                       (catch Exception e :outer))))))

(deftest classed-catch-first-match-wins
  (is (= :x (try (throw (ex-info "x" {}))
                 (catch Error e :e)
                 (catch Exception e :x))))
  (is (= :bare (try (throw (ex-info "x" {}))
                    (catch Error e :e)
                    (catch e :bare)))))

(deftest classed-catch-catch-all-forms
  (is (= :d (try (throw (ex-info "x" {})) (catch :default e :d))))
  (is (= :t (try (nth [1] 5) (catch Throwable e :t))))
  (is (= :o (try (throw (ex-info "x" {})) (catch Object e :o)))))

(deftest classed-catch-system-kinds
  (is (= :oob (try (nth [1] 5)
                   (catch IndexOutOfBoundsException e :oob))))
  (is (= :e (try (nth [1] 5) (catch Exception e :e))))
  (is (= :iae (try ((fn [a] a))
                   (catch IllegalArgumentException e :iae))))
  (is (= :ei (try (throw (ex-info "x" {}))
                  (catch clojure.lang.ExceptionInfo e :ei)))))

(deftest classed-catch-unknown-class-is-a-syntax-error
  (is (thrown? (try 1 (catch NotAClass e 2))))
  (let [msg (try (try 1 (catch NotAClass e 2))
                 (catch e (ex-message e)))]
    (is (clojure.string/includes? msg "NotAClass"))))

(deftest bare-catch-still-binds-the-diagnostic
  (is (= :user (try (throw (ex-info "x" {})) (catch e (:mino/kind e)))))
  (is (= 1 (try (throw (ex-info "boom" {:a 1})) (catch e (:a (ex-data e)))))))
