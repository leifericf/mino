(require "tests/test")

;; --- add-watch / remove-watch ---

(deftest add-watch-basic
  (let [a    (atom 0)
        log  (atom [])]
    (add-watch a :w (fn [k r o n]
      (swap! log conj {:key k :old o :new n})))
    (swap! a inc)
    (is (= [{:key :w :old 0 :new 1}] @log))))

(deftest add-watch-multiple
  (let [a  (atom 0)
        l1 (atom [])
        l2 (atom [])]
    (add-watch a :w1 (fn [k r o n] (swap! l1 conj n)))
    (add-watch a :w2 (fn [k r o n] (swap! l2 conj n)))
    (swap! a inc)
    (is (= [1] @l1))
    (is (= [1] @l2))))

(deftest remove-watch-basic
  (let [a   (atom 0)
        log (atom [])]
    (add-watch a :w (fn [k r o n] (swap! log conj n)))
    (swap! a inc)
    (remove-watch a :w)
    (swap! a inc)
    (is (= [1] @log))
    (is (= 2 @a))))

(deftest watch-on-reset
  (let [a   (atom 0)
        log (atom [])]
    (add-watch a :w (fn [k r o n] (swap! log conj [o n])))
    (reset! a 42)
    (is (= [[0 42]] @log))))

(deftest watch-sees-new-state
  (let [a      (atom 0)
        seen   (atom nil)]
    (add-watch a :w (fn [k r o n] (reset! seen @r)))
    (reset! a 99)
    (is (= 99 @seen))))

(deftest watch-exception-ignored
  (let [a (atom 0)]
    (add-watch a :bad (fn [k r o n] (throw "boom")))
    (swap! a inc)
    (is (= 1 @a))))

(deftest watch-replace-fn
  (let [a   (atom 0)
        log (atom [])]
    (add-watch a :w (fn [k r o n] (swap! log conj :first)))
    (swap! a inc)
    (add-watch a :w (fn [k r o n] (swap! log conj :second)))
    (swap! a inc)
    (is (= [:first :second] @log))))

(deftest add-watch-returns-atom
  (let [a (atom 0)]
    (is (= a (add-watch a :w (fn [k r o n] nil))))))

(deftest remove-watch-returns-atom
  (let [a (atom 0)]
    (add-watch a :w (fn [k r o n] nil))
    (is (= a (remove-watch a :w)))))

(deftest remove-watch-missing-key
  (let [a (atom 0)]
    (is (= a (remove-watch a :nonexistent)))))

;; --- set-validator! / get-validator ---

(deftest validator-basic
  (let [a (atom 5)]
    (set-validator! a pos?)
    (is (thrown? (reset! a -1)))
    (is (= 5 @a))))

(deftest validator-allows
  (let [a (atom 5)]
    (set-validator! a pos?)
    (swap! a inc)
    (is (= 6 @a))))

(deftest validator-on-current
  (let [a (atom -1)]
    (is (thrown? (set-validator! a pos?)))
    (is (nil? (get-validator a)))))

(deftest validator-nil-removes
  (let [a (atom 5)]
    (set-validator! a pos?)
    (set-validator! a nil)
    (reset! a -1)
    (is (= -1 @a))))

(deftest get-validator-returns-fn
  (let [a (atom 5)]
    (set-validator! a pos?)
    (is (fn? (get-validator a)))
    (is (nil? (get-validator (atom 0))))))

(deftest validator-runs-before-watches
  (let [a   (atom 5)
        log (atom [])]
    (set-validator! a pos?)
    (add-watch a :w (fn [k r o n] (swap! log conj n)))
    (is (thrown? (reset! a -1)))
    (is (= [] @log))
    (reset! a 10)
    (is (= [10] @log))))

(deftest validator-on-swap
  (let [a (atom 5)]
    (set-validator! a pos?)
    (is (thrown? (swap! a - 10)))
    (is (= 5 @a))))

;; --- swap-vals! / reset-vals! ---

(deftest swap-vals-basic
  (let [a (atom 10)]
    (is (= [10 11] (swap-vals! a inc)))))

(deftest reset-vals-basic
  (let [a (atom 10)]
    (is (= [10 42] (reset-vals! a 42)))))

(deftest swap-vals-extra-args
  (let [a (atom 0)]
    (is (= [0 5] (swap-vals! a + 5)))))

(deftest swap-vals-with-watch
  (let [a   (atom 0)
        log (atom [])]
    (add-watch a :w (fn [k r o n] (swap! log conj [o n])))
    (swap-vals! a inc)
    (is (= [[0 1]] @log))))

(deftest reset-vals-with-watch
  (let [a   (atom 0)
        log (atom [])]
    (add-watch a :w (fn [k r o n] (swap! log conj [o n])))
    (reset-vals! a 99)
    (is (= [[0 99]] @log))))

(deftest swap-vals-with-validator
  (let [a (atom 5)]
    (set-validator! a pos?)
    (is (= [5 6] (swap-vals! a inc)))
    (is (thrown? (swap-vals! a - 100)))
    (is (= 6 @a))))

(deftest reset-vals-with-validator
  (let [a (atom 5)]
    (set-validator! a pos?)
    (is (thrown? (reset-vals! a -1)))
    (is (= 5 @a))
    (is (= [5 10] (reset-vals! a 10)))))
