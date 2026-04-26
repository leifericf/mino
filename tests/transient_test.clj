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

;; (run-tests) -- called by tests/run.mino
