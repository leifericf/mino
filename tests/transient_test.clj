(require "tests/test")

;; Transients: transient, persistent!, conj!, assoc!, dissoc!, disj!,
;; pop!, transient?

;; --- transient? predicate ---

(deftest transient?-basic
  (is (transient? (transient [])))
  (is (transient? (transient {})))
  (is (transient? (transient #{})))
  (is (not (transient? [])))
  (is (not (transient? {})))
  (is (not (transient? #{})))
  (is (not (transient? nil))))

;; --- vector transients ---

(deftest transient-vector-conj
  (let [v (persistent! (-> (transient [])
                           (conj! 1)
                           (conj! 2)
                           (conj! 3)))]
    (is (= [1 2 3] v))))

(deftest transient-vector-assoc
  (let [v (persistent! (-> (transient [0 0 0])
                           (assoc! 0 :a)
                           (assoc! 2 :c)))]
    (is (= [:a 0 :c] v))))

(deftest transient-vector-pop
  (let [v (persistent! (-> (transient [1 2 3])
                           (pop!)))]
    (is (= [1 2] v))))

;; --- map transients ---

(deftest transient-map-assoc
  (let [m (persistent! (-> (transient {})
                           (assoc! :a 1)
                           (assoc! :b 2)))]
    (is (= {:a 1 :b 2} m))))

(deftest transient-map-dissoc
  (let [m (persistent! (-> (transient {:a 1 :b 2 :c 3})
                           (dissoc! :b)))]
    (is (= {:a 1 :c 3} m))))

(deftest transient-map-conj-pair
  (let [m (persistent! (-> (transient {})
                           (conj! [:a 1])
                           (conj! [:b 2])))]
    (is (= {:a 1 :b 2} m))))

;; --- set transients ---

(deftest transient-set-conj
  (let [s (persistent! (-> (transient #{})
                           (conj! 1)
                           (conj! 2)
                           (conj! 1)))]
    (is (= #{1 2} s))))

(deftest transient-set-disj
  (let [s (persistent! (-> (transient #{1 2 3})
                           (disj! 2)))]
    (is (= #{1 3} s))))

;; --- sealing: operations after persistent! must throw ---

(deftest transient-sealed-after-persistent
  (let [t (transient [])]
    (persistent! t)
    (is (thrown? (conj! t 1)))))

(deftest transient-double-persistent
  (let [t (transient [])]
    (persistent! t)
    (is (thrown? (persistent! t)))))

;; --- type mismatches throw ---

(deftest transient-type-mismatches
  (is (thrown? (dissoc! (transient [1 2]) 0)))     ; dissoc! on vec
  (is (thrown? (disj! (transient {:a 1}) :a)))     ; disj! on map
  (is (thrown? (pop! (transient #{1 2}))))         ; pop! on set
  (is (thrown? (transient 42)))                    ; transient on int
  (is (thrown? (persistent! 42))))                 ; persistent! on int

;; --- Cortex Q3: escape-route coverage ---

;; Transient captured by a lazy seq and realized after persistent!.
;; Realization after sealing must throw, not corrupt or silently succeed.
(deftest transient-lazy-capture-after-seal
  (let [t   (transient [])
        lz  (map (fn [x] (conj! t x)) [1 2 3])]
    ;; Seal the transient first, then realize the seq.
    (persistent! t)
    (is (thrown? (doall lz)))))

;; Transient stored in an atom and mutated through the atom.
;; The sealing still takes effect at the underlying transient.
(deftest transient-in-atom
  (let [a (atom (transient []))]
    (swap! a (fn [t] (conj! t 1)))
    (swap! a (fn [t] (conj! t 2)))
    (is (= [1 2] (persistent! @a)))
    ;; Further mutation after seal must throw.
    (is (thrown? (swap! a (fn [t] (conj! t 3)))))))

;; Transient survives a forced GC between mutations: the write barrier
;; on the inner-pointer store is what keeps this correct.
(deftest transient-survives-gc-yield
  (let [t (transient [])]
    (conj! t 1)
    (gc!)
    (conj! t 2)
    (gc!)
    (conj! t 3)
    (is (= [1 2 3] (persistent! t)))))

;; --- Builder-pattern compile-time rewrite safety ---
;;
;; The (loop [... acc []] (if <test> (recur ... (conj acc x)) acc)) shape
;; is rewritten to a transient-driven form at compile time. The rewrite
;; is only safe when the loop body never reads the accumulator outside
;; the bare-exit branch -- transient semantics do not cover seq/=/contains?
;; in mino's model, and a rewrite that exposed those reads to a transient
;; would silently diverge from the persistent value the user wrote.

;; <test> reads acc through contains? -- must stay on the persistent path.
(deftest builder-rewrite-test-uses-contains
  (let [r (loop [i 0 acc []]
            (if (and (< i 5) (not (contains? acc i)))
              (recur (+ i 1) (conj acc i))
              acc))]
    (is (= [0 1 2 3 4] r))))

;; <test> reads acc through = with a literal -- must stay on the
;; persistent path (= against a transient is not a defined operation).
(deftest builder-rewrite-test-uses-equals
  (let [r (loop [i 0 acc []]
            (if (= [] acc)
              (recur (+ i 1) (conj acc i))
              acc))]
    (is (= [0] r))))

;; <test> reads acc through peek/empty? -- must stay on the persistent path.
(deftest builder-rewrite-test-uses-peek
  (let [r (loop [i 0 acc []]
            (if (and (< i 3) (or (empty? acc) (< (peek acc) 10)))
              (recur (+ i 1) (conj acc i))
              acc))]
    (is (= [0 1 2] r))))

;; A non-step recur arg that references acc -- must stay on the
;; persistent path. Here the second binding's recur step computes
;; (count acc), which would observe the transient under a rewrite.
(deftest builder-rewrite-non-step-references-acc
  (let [r (loop [i 0 n 0 acc []]
            (if (< i 4)
              (recur (+ i 1) (count acc) (conj acc i))
              [n acc]))]
    (is (= [3 [0 1 2 3]] r))))

;; A step whose <x> sub-expression references acc -- must stay on the
;; persistent path. Reading `(peek acc)` inside `<x>` would observe
;; the transient.
(deftest builder-rewrite-step-x-references-acc
  (let [r (loop [i 0 acc [10]]
            (if (< i 3)
              (recur (+ i 1) (conj acc (+ i (peek acc))))
              acc))]
    (is (= [10 10 11 13] r))))

;; Two acc-build steps in the same recur (would-be ambiguous) -- the
;; find_acc_step recognizer picks the first match, so the second one's
;; presence triggers the non-step guard.
(deftest builder-rewrite-multiple-acc-steps
  (let [r (loop [i 0 acc []]
            (if (< i 2)
              (recur (+ i 1) (-> acc (conj i) (conj (* i 10))))
              acc))]
    (is (= [0 0 1 10] r))))

;; --- Regression: safe builder shapes still rewrite (and run correctly) ---

;; The canonical vector-builder shape -- this is exactly what the
;; v0.166.0 rewrite targets. The rewrite must still fire here.
(deftest builder-rewrite-safe-vec-conj
  (let [r (loop [i 0 acc []]
            (if (< i 5)
              (recur (+ i 1) (conj acc i))
              acc))]
    (is (= [0 1 2 3 4] r))))

;; Map-builder shape with assoc -- the v0.166.0 sister rewrite.
(deftest builder-rewrite-safe-map-assoc
  (let [r (loop [i 0 acc {}]
            (if (< i 4)
              (recur (+ i 1) (assoc acc i (* i 10)))
              acc))]
    (is (= {0 0 1 10 2 20 3 30} r))))

;; Then/else order reversed -- bare-acc as the then-branch, recur as
;; the else-branch. Must still rewrite and produce the same answer.
(deftest builder-rewrite-safe-reversed-branches
  (let [r (loop [i 0 acc []]
            (if (>= i 3)
              acc
              (recur (+ i 1) (conj acc i))))]
    (is (= [0 1 2] r))))

;; A non-step recur arg that does NOT reference acc -- must still
;; rewrite. This is the dominant shape: index counter + builder.
(deftest builder-rewrite-safe-counter
  (let [r (loop [i 0 j 100 acc []]
            (if (< i 3)
              (recur (+ i 1) (- j 1) (conj acc i))
              [j acc]))]
    (is (= [97 [0 1 2]] r))))

;; (run-tests) -- called by tests/run.clj
