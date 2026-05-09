(require "tests/test")

;; Software Transactional Memory: refs, dosync, alter, commute, ensure, io!.

(deftest ref-construct
  (let [r (ref 0)]
    (is (= 0 @r))
    (is (ref? r))
    (is (not (ref? 0)))
    (is (not (ref? (atom 0))))))

(deftest ref-set-basic
  (let [r (ref 0)]
    (dosync (ref-set r 5))
    (is (= 5 @r))))

(deftest alter-fn
  (let [r (ref 10)]
    (dosync (alter r + 5))
    (is (= 15 @r))))

(deftest alter-multi-args
  (let [r (ref 10)]
    (dosync (alter r + 1 2 3))
    (is (= 16 @r))))

(deftest commute-basic
  (let [r (ref 0)]
    (dosync
      (commute r inc)
      (commute r inc))
    (is (= 2 @r))))

(deftest ensure-no-write
  (let [r (ref 0)]
    (dosync (ensure r))
    (is (= 0 @r))))

(deftest in-tx-predicate
  (is (false? (in-transaction?)))
  (let [r (ref 0)
        seen-in-tx (atom nil)]
    (dosync
      (reset! seen-in-tx (in-transaction?))
      (ref-set r 1))
    (is (true? @seen-in-tx))
    (is (false? (in-transaction?)))))

(deftest tx-only-mutations
  (let [r (ref 0)]
    (is (thrown? (ref-set r 1)))
    (is (thrown? (alter r inc)))
    (is (thrown? (commute r inc)))
    (is (thrown? (ensure r)))))

(deftest io-bang-rejected
  (let [r (ref 0)]
    (is (thrown? (dosync (alter r inc) (io! (println "side-effect")))))))

(deftest io-bang-passthrough
  ;; Outside dosync, io! just runs body.
  (let [a (atom 0)]
    (io! (swap! a inc))
    (is (= 1 @a))))

(deftest watch-fires-on-commit
  (let [r (ref 0)
        seen (atom [])]
    (add-watch r :w (fn [k _ o n] (swap! seen conj [k o n])))
    (dosync (alter r inc))
    (dosync (alter r inc))
    (is (= [[:w 0 1] [:w 1 2]] @seen))))

(deftest watch-removed
  (let [r (ref 0)
        seen (atom [])]
    (add-watch r :w (fn [_ _ _ n] (swap! seen conj n)))
    (dosync (alter r inc))
    (remove-watch r :w)
    (dosync (alter r inc))
    (is (= [1] @seen))))

(deftest validator-accepts
  (let [r (ref 0)]
    (set-validator! r number?)
    (is (= number? (get-validator r)))
    (dosync (alter r inc))
    (is (= 1 @r))))

(deftest validator-rejects
  (let [r (ref 0)]
    ;; Installing a validator that would fail the current value succeeds
    ;; (JVM canon); only subsequent in-tx transitions are checked.
    (set-validator! r pos?)
    (is (= 0 @r))
    (is (thrown? (dosync (ref-set r -1))))
    ;; Failed transaction leaves the ref untouched.
    (is (= 0 @r))
    (dosync (ref-set r 5))
    (is (= 5 @r))))

(deftest nested-dosync
  (let [r (ref 0)]
    (dosync
      (alter r inc)
      (dosync (alter r inc)))
    (is (= 2 @r))))

(deftest deref-in-tx-sees-tentative
  (let [r (ref 0)]
    (dosync
      (alter r inc)
      (is (= 1 @r))
      (alter r inc)
      (is (= 2 @r)))
    (is (= 2 @r))))

(deftest commute-then-alter-fold
  (let [r (ref 0)]
    (dosync
      (commute r inc)
      (alter r * 10))
    (is (= 10 @r))))

(deftest ref-history-stubs
  (let [r (ref 0)]
    (is (= 0 (ref-min-history r)))
    (is (= 10 (ref-max-history r)))
    (is (= 0 (ref-history-count r)))))

(deftest type-keyword
  (is (= :ref (type (ref 1)))))
