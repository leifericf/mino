(require "tests/test")

;; binding inside a defn body so the bytecode compiler emits PUSHDYN /
;; POPDYN. Tree-walker parity is the contract, not new semantics.

(def ^:dynamic *bc-tx* 0)
(def ^:dynamic *bc-ty* "outer")

(deftest bc-binding-basic
  (testing "binding sets dynamic value inside body"
    (defn t [] (binding [*bc-tx* 42] *bc-tx*))
    (is (= 42 (t))))
  (testing "binding restores after body"
    (defn t [] (binding [*bc-tx* 7] *bc-tx*))
    (t)
    (is (= 0 *bc-tx*)))
  (testing "multiple bindings in one form"
    (defn t [] (binding [*bc-tx* 1 *bc-ty* "inner"]
                 [*bc-tx* *bc-ty*]))
    (is (= [1 "inner"] (t)))
    (is (= 0 *bc-tx*))
    (is (= "outer" *bc-ty*))))

(deftest bc-binding-nested
  (testing "inner binding shadows, restores on exit"
    (defn t [] (binding [*bc-tx* 1]
                 (let [v1 *bc-tx*
                       _  (binding [*bc-tx* 2]
                            (is (= 2 *bc-tx*)))
                       v2 *bc-tx*]
                   [v1 v2])))
    (is (= [1 1] (t))))
  (testing "binding inside fn called from another binding"
    (defn inner [] (binding [*bc-tx* 99] *bc-tx*))
    (defn outer [] (binding [*bc-tx* 1]
                     [*bc-tx* (inner) *bc-tx*]))
    (is (= [1 99 1] (outer)))))

(deftest bc-binding-with-throw
  (testing "binding restores even when body throws"
    (defn t [] (try (binding [*bc-tx* 42]
                      (throw "boom"))
                    (catch e (ex-data e))))
    (is (= "boom" (t)))
    (is (= 0 *bc-tx*)))
  (testing "binding visible inside catch when binding wraps try"
    (defn t [] (binding [*bc-tx* 7]
                 (try (throw "x")
                      (catch e *bc-tx*))))
    (is (= 7 (t)))
    (is (= 0 *bc-tx*)))
  (testing "binding restores even on uncaught propagating throw"
    (defn raises [] (binding [*bc-tx* 100]
                      (throw "deep")))
    (try (raises) (catch _ nil))
    (is (= 0 *bc-tx*))))

(deftest bc-binding-empty
  (testing "empty binding vector is a no-op"
    (defn t [] (binding [] *bc-tx*))
    (is (= 0 (t)))))

(deftest bc-binding-throw-unwinds-to-catch
  ;; The bc catch landing pad needs to pop dyn frames that the body
  ;; pushed but never popped because the longjmp bypassed the matching
  ;; OP_POPDYN. Without the unwind, the catch handler observes the
  ;; throw-site's binding instead of the surrounding root / outer
  ;; binding -- matching `eval_try`'s `saved_dyn` discipline.
  (testing "catch handler sees root after inner binding throws"
    (defn t [] (try (binding [*bc-tx* 999]
                      (throw "boom"))
                    (catch e *bc-tx*)))
    (is (= 0 (t))))
  (testing "outer binding visible after inner throws across nested binding"
    (defn t [] (binding [*bc-tx* :outer]
                 (try (binding [*bc-tx* :inner]
                        (throw "boom"))
                      (catch e *bc-tx*))))
    (is (= :outer (t)))
    (is (= 0 *bc-tx*)))
  (testing "catch rethrow from inner binding still reports outer state"
    (defn t [] (try
                 (binding [*bc-tx* :A]
                   (try
                     (binding [*bc-tx* :B]
                       (throw "inner"))
                     (catch e (throw "rethrown"))))
                 (catch e *bc-tx*)))
    (is (= 0 (t)))))
