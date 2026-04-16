;; Tests for value metadata: meta, with-meta, vary-meta, reader syntax.

;; --- with-meta and meta ---

(deftest with-meta-basic
  (testing "attach metadata to vector"
    (let [v (with-meta [1 2 3] {:tag "nums"})]
      (is (= {:tag "nums"} (meta v)))
      (is (= [1 2 3] v))))
  (testing "attach metadata to map"
    (let [m (with-meta {:a 1} {:doc "a map"})]
      (is (= {:doc "a map"} (meta m)))
      (is (= {:a 1} m))))
  (testing "attach metadata to set"
    (let [s (with-meta #{1 2} {:source "test"})]
      (is (= {:source "test"} (meta s)))
      (is (= #{1 2} s))))
  (testing "attach metadata to list"
    (let [l (with-meta '(1 2 3) {:tag "list"})]
      (is (= {:tag "list"} (meta l)))))
  (testing "attach metadata to fn"
    (let [f (with-meta (fn [x] x) {:name "id"})]
      (is (= {:name "id"} (meta f)))
      (is (= 42 (f 42)))))
  (testing "attach metadata to symbol"
    (let [s (with-meta 'foo {:private true})]
      (is (= {:private true} (meta s))))))

(deftest meta-nil-when-absent
  (testing "fresh values have no metadata"
    (is (nil? (meta [1 2])))
    (is (nil? (meta {:a 1})))
    (is (nil? (meta #{1})))
    (is (nil? (meta '(1 2))))
    (is (nil? (meta 'foo)))))

(deftest meta-does-not-affect-equality
  (testing "values with different metadata are equal"
    (is (= (with-meta [1 2] {:a 1})
           (with-meta [1 2] {:b 2})))
    (is (= (with-meta [1 2] {:a 1})
           [1 2]))
    (is (= (with-meta {:x 1} {:doc "y"})
           {:x 1}))))

(deftest with-meta-nil-clears
  (testing "passing nil clears metadata"
    (let [v (with-meta [1 2] {:tag "x"})
          v2 (with-meta v nil)]
      (is (nil? (meta v2)))
      (is (= [1 2] v2)))))

;; --- vary-meta ---

(deftest vary-meta-basic
  (testing "apply function to metadata"
    (let [v (with-meta [1 2] {:count 0})
          v2 (vary-meta v update :count inc)]
      (is (= {:count 1} (meta v2)))
      (is (= {:count 0} (meta v)))))
  (testing "assoc new key"
    (let [v (with-meta [1] {:a 1})
          v2 (vary-meta v assoc :b 2)]
      (is (= {:a 1 :b 2} (meta v2)))))
  (testing "vary-meta on value without metadata"
    (let [v [1 2 3]
          v2 (vary-meta v assoc :tag "x")]
      (is (= {:tag "x"} (meta v2))))))

;; --- Reader syntax ---

(deftest reader-syntax-map
  (testing "^{...} attaches map metadata"
    (let [v ^{:doc "hello"} [1 2 3]]
      (is (= {:doc "hello"} (meta v)))
      (is (= [1 2 3] v)))))

(deftest reader-syntax-keyword
  (testing "^:key shorthand"
    (let [v ^:private [1 2]]
      (is (= {:private true} (meta v))))))

;; --- Metadata preserved through operations ---

(deftest metadata-preserved-by-operations
  (testing "conj preserves metadata"
    (let [v (with-meta [1 2] {:tag "x"})
          v2 (conj v 3)]
      (is (= {:tag "x"} (meta v2)))))
  (testing "assoc on map preserves metadata"
    (let [m (with-meta {:a 1} {:doc "y"})
          m2 (assoc m :b 2)]
      (is (= {:doc "y"} (meta m2))))))
