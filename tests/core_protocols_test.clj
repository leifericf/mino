;; Tests for clojure.core.protocols (CollReduce, IKVReduce, Datafiable, Navigable)
;; and the cross-namespace extension path that the canonical namespace enables.

(require '[clojure.core.protocols :as p])
(require '[clojure.datafy :as d])

;; --- Datafiable / Navigable defaults ---

(deftest datafy-default-identity
  (testing "datafy on built-ins returns the value unchanged"
    (is (= 42 (datafy 42)))
    (is (= "hello" (datafy "hello")))
    (is (= [1 2 3] (datafy [1 2 3])))
    (is (= {:a 1} (datafy {:a 1})))
    (is (nil? (datafy nil)))))

(deftest nav-default-returns-value
  (testing "nav default returns v unchanged"
    (is (= 99 (nav [1 2 3] 0 99)))
    (is (= :x (nav {:a 1} :a :x)))))

;; --- Cross-namespace identity ---

(deftest datafy-namespaces-share-atom
  (testing "core, clojure.core.protocols, clojure.datafy all see one atom"
    ;; Use a marker type tag that no other test uses to avoid leakage.
    (extend-type :ratio Datafiable
      (datafy [r] [:ratio-marker r]))
    (try
      (is (= [:ratio-marker (/ 1 2)] (datafy (/ 1 2))))
      (is (= [:ratio-marker (/ 1 2)] (p/datafy (/ 1 2))))
      (is (= [:ratio-marker (/ 1 2)] (d/datafy (/ 1 2))))
      (finally
        (swap! Datafiable--datafy dissoc :ratio)))))

;; --- User extends CollReduce: reduce respects the override ---

(deftest coll-reduce-vector-override
  (testing "extending CollReduce for :vector overrides reduce"
    (extend-protocol p/CollReduce
      :vector
      (coll-reduce [v f init]
        ;; Marker: tag the result so we can confirm we hit the override.
        (let [base (clojure.core/internal-reduce f init v)]
          [::overridden base])))
    (try
      (is (= [::overridden 6] (reduce + 0 [1 2 3])))
      (testing "lists fall through to internal reduce"
        (is (= 6 (reduce + 0 (list 1 2 3)))))
      (finally
        (swap! CollReduce--coll-reduce dissoc :vector)))))

;; --- User extends IKVReduce ---

(deftest kv-reduce-map-override
  (testing "extending IKVReduce for :map overrides reduce-kv"
    (extend-protocol p/IKVReduce
      :map
      (kv-reduce [m f init]
        ;; Build a sorted summary so the test is order-stable across hash maps.
        (str "kv:" (count m))))
    (try
      (is (= "kv:2" (reduce-kv (fn [a k v] (assoc a k v)) {} {:a 1 :b 2})))
      (finally
        (swap! IKVReduce--kv-reduce dissoc :map)))))

;; --- :default override on Datafiable ---

(deftest datafy-default-override
  (testing "extending Datafiable :default re-routes everything"
    (let [original (get @Datafiable--datafy :default)]
      (extend-type :default Datafiable
        (datafy [o] [:wrapped o]))
      (try
        (is (= [:wrapped 42] (datafy 42)))
        (is (= [:wrapped "x"] (datafy "x")))
        (finally
          (swap! Datafiable--datafy assoc :default original))))))

;; --- Protocol map shape matches Greetable in protocol_test ---

(deftest core-protocols-map-shape
  (testing "the four protocols expose the canonical shape"
    (is (= "CollReduce"  (:name CollReduce)))
    (is (= "IKVReduce"   (:name IKVReduce)))
    (is (= "Datafiable"  (:name Datafiable)))
    (is (= "Navigable"   (:name Navigable)))
    (is (contains? (:methods CollReduce) :coll-reduce))
    (is (contains? (:methods IKVReduce)  :kv-reduce))
    (is (contains? (:methods Datafiable) :datafy))
    (is (contains? (:methods Navigable)  :nav))))

;; --- Re-exported namespace shares identity, not just shape ---

(deftest re-export-identity
  (testing "clojure.core.protocols re-exports point to the same atoms"
    (is (identical? CollReduce--coll-reduce p/CollReduce--coll-reduce))
    (is (identical? IKVReduce--kv-reduce    p/IKVReduce--kv-reduce))
    (is (identical? Datafiable--datafy      p/Datafiable--datafy))
    (is (identical? Navigable--nav          p/Navigable--nav))))
