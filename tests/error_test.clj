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
  (is (error? (try (throw "x") (catch e e)))))

(deftest catch-preserves-diagnostic-maps
  (let [m {:mino/kind :user :mino/code "MUS001" :mino/message "test"}]
    (is (= :user (:mino/kind (try (throw m) (catch e e)))))))
