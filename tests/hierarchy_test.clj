(require "tests/test")

;; --- make-hierarchy ---

(deftest make-hierarchy-empty
  (let [h (make-hierarchy)]
    (is (= {} (:parents h)))
    (is (= {} (:ancestors h)))
    (is (= {} (:descendants h)))))

;; --- derive ---

(deftest derive-basic
  (let [h (derive (make-hierarchy) :square :shape)]
    (is (= #{:shape} (parents h :square)))
    (is (= #{:shape} (ancestors h :square)))
    (is (= #{:square} (descendants h :shape)))))

(deftest derive-chain
  (let [h (-> (make-hierarchy)
              (derive :square :rect)
              (derive :rect :shape))]
    (is (= #{:rect} (parents h :square)))
    (is (= #{:rect :shape} (ancestors h :square)))
    (is (= #{:square :rect} (descendants h :shape)))
    (is (= #{:square} (descendants h :rect)))))

(deftest derive-diamond
  (let [h (-> (make-hierarchy)
              (derive :d :b)
              (derive :d :c)
              (derive :b :a)
              (derive :c :a))]
    (is (= #{:b :c} (parents h :d)))
    (is (= #{:a :b :c} (ancestors h :d)))
    (is (= #{:b :c :d} (descendants h :a)))
    (is (= #{:d} (descendants h :b)))
    (is (= #{:d} (descendants h :c)))))

(deftest derive-cycle-detection
  (let [h (derive (make-hierarchy) :a :b)]
    (is (thrown? (derive h :b :a)))))

(deftest derive-self-throws
  (is (thrown? (derive (make-hierarchy) :a :a))))

;; --- underive ---

(deftest underive-basic
  (let [h (-> (make-hierarchy)
              (derive :square :shape)
              (underive :square :shape))]
    (is (nil? (parents h :square)))
    (is (nil? (ancestors h :square)))
    (is (nil? (descendants h :shape)))))

(deftest underive-chain
  (let [h (-> (make-hierarchy)
              (derive :square :rect)
              (derive :rect :shape)
              (underive :square :rect))]
    (is (nil? (parents h :square)))
    (is (nil? (ancestors h :square)))
    (is (nil? (descendants h :rect)))
    (is (= #{:rect} (descendants h :shape)))
    (is (= #{:shape} (ancestors h :rect)))))

(deftest underive-noop
  (let [h (derive (make-hierarchy) :a :b)]
    (is (= h (underive h :a :c)))))

;; --- parents / ancestors / descendants ---

(deftest parents-query
  (let [h (-> (make-hierarchy)
              (derive :a :b)
              (derive :a :c))]
    (is (= #{:b :c} (parents h :a)))
    (is (nil? (parents h :b)))))

(deftest ancestors-query
  (let [h (-> (make-hierarchy)
              (derive :a :b)
              (derive :b :c)
              (derive :b :d))]
    (is (= #{:b :c :d} (ancestors h :a)))
    (is (= #{:c :d} (ancestors h :b)))))

(deftest descendants-query
  (let [h (-> (make-hierarchy)
              (derive :a :b)
              (derive :b :c))]
    (is (= #{:a :b} (descendants h :c)))
    (is (= #{:a} (descendants h :b)))
    (is (nil? (descendants h :a)))))

;; --- isa? ---

(deftest isa-equality
  (is (isa? :a :a))
  (is (isa? 42 42))
  (is (isa? "hello" "hello")))

(deftest isa-hierarchy
  (let [h (derive (make-hierarchy) :child :parent)]
    (is (isa? h :child :parent))
    (is (not (isa? h :parent :child)))))

(deftest isa-transitive
  (let [h (-> (make-hierarchy)
              (derive :a :b)
              (derive :b :c))]
    (is (isa? h :a :c))
    (is (not (isa? h :c :a)))))

(deftest isa-vector
  (let [h (-> (make-hierarchy)
              (derive :square :shape))]
    (is (isa? h [:square :x] [:shape :x]))
    (is (not (isa? h [:shape :x] [:square :x])))
    (is (not (isa? h [:square] [:shape :x])))))

(deftest isa-negative
  (is (not (isa? :a :b)))
  (is (not (isa? (make-hierarchy) :x :y))))

;; --- global hierarchy ---

(deftest global-hierarchy-derive
  (derive :test-cat :test-animal)
  (is (isa? :test-cat :test-animal))
  (is (= #{:test-animal} (parents :test-cat)))
  (is (= #{:test-animal} (ancestors :test-cat)))
  (is (= #{:test-cat} (descendants :test-animal)))
  (underive :test-cat :test-animal))

(deftest global-hierarchy-underive
  (derive :test-x :test-y)
  (underive :test-x :test-y)
  (is (not (isa? :test-x :test-y))))

(deftest global-hierarchy-isa
  (derive :test-dog :test-pet)
  (is (isa? :test-dog :test-pet))
  (is (not (isa? :test-pet :test-dog)))
  (underive :test-dog :test-pet))

;; --- 2-arity derive / underive return nil (Clojure dialect) ---

(deftest global-derive-returns-nil
  (is (nil? (derive :gd-child :gd-parent)))
  (is (nil? (underive :gd-child :gd-parent))))
