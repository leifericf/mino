(require "tests/test")

;; Records: real value types with map-isomorphic behaviour.

(defrecord Point [x y])

(deftest record-construction
  (let [p (->Point 1 2)]
    (is (record? p))
    (is (= 1 (:x p)))
    (is (= 2 (:y p)))
    (is (instance? Point p))
    (is (= Point (type p)))))

(deftest record-equality-and-type-identity
  (is (= (->Point 1 2) (->Point 1 2)))
  (is (not= (->Point 1 2) (->Point 1 3)))
  ;; Records are not equal to plain maps with the same content.
  (is (not= (->Point 1 2) {:x 1 :y 2}))
  (is (not= {:x 1 :y 2} (->Point 1 2)))
  ;; Different types with the same field values are not equal.
  (defrecord Other [x y])
  (is (not= (->Point 1 2) (->Other 1 2))))

(deftest record-hash-respects-identity
  ;; Equal records hash the same.
  (is (= (hash (->Point 1 2)) (hash (->Point 1 2))))
  ;; A record's hash differs from a plain map with the same content.
  (is (not= (hash (->Point 1 2)) (hash {:x 1 :y 2}))))

(deftest record-map-iso-get-and-default
  (let [p (->Point 1 2)]
    (is (= 1 (get p :x)))
    (is (= 2 (get p :y)))
    (is (nil? (get p :z)))
    (is (= :missing (get p :z :missing)))
    (is (= 1 (p :x)))))

(deftest record-map-iso-assoc
  (let [p (->Point 1 2)]
    ;; Declared-field assoc keeps the record type.
    (let [p2 (assoc p :x 10)]
      (is (record? p2))
      (is (= 10 (:x p2)))
      (is (= 2 (:y p2))))
    ;; Ext-key assoc still returns a record.
    (let [p3 (assoc p :z 99)]
      (is (record? p3))
      (is (= 99 (:z p3)))
      (is (= 1 (:x p3))))))

(deftest record-map-iso-dissoc
  (let [p (->Point 1 2)]
    ;; Dropping a declared field degrades to a plain map.
    (let [m (dissoc p :x)]
      (is (not (record? m)))
      (is (map? m))
      (is (= {:y 2} m)))
    ;; Dropping an ext key keeps the record.
    (let [p3 (assoc p :z 99)
          p4 (dissoc p3 :z)]
      (is (record? p4))
      (is (= p p4)))))

(deftest record-count-keys-vals-seq
  (let [p (->Point 1 2)]
    (is (= 2 (count p)))
    (is (= '(:x :y) (keys p)))
    (is (= '(1 2) (vals p)))
    (is (= '([:x 1] [:y 2]) (seq p)))
    ;; Ext keys appear after declared fields.
    (let [p3 (assoc p :extra 99)]
      (is (= 3 (count p3)))
      (is (= '(:x :y :extra) (keys p3))))))

(deftest record-contains-and-find
  (let [p (->Point 1 2)]
    (is (contains? p :x))
    (is (not (contains? p :z)))
    (is (= [:x 1] (find p :x)))
    (is (nil? (find p :z)))))

(deftest record-from-map-splits-declared-and-ext
  (let [p (map->Point {:x 1 :y 2 :extra 99})]
    (is (record? p))
    (is (= 1 (:x p)))
    (is (= 2 (:y p)))
    (is (= 99 (:extra p)))
    (is (= 3 (count p)))))

(deftest record-empty
  (defrecord EmptyR [])
  (let [e (->EmptyR)]
    (is (record? e))
    (is (= 0 (count e)))
    (is (nil? (seq e)))))

(deftest deftype-aliases-defrecord
  (deftype Box [v])
  (let [b (->Box 42)]
    (is (record? b))
    (is (= 42 (:v b)))))

(deftest defrecord-with-inline-protocol-spec
  (defprotocol IGreet (greet [x]))
  (defrecord Greeter [n] IGreet (greet [this] (str "hi " (:n this))))
  (let [g (->Greeter "you")]
    (is (= "hi you" (greet g)))
    (is (satisfies? IGreet g))
    (is (instance? Greeter g))))

(deftest record-seq-is-iterable
  ;; Regression: seq_iter_init didn't handle MINO_RECORD, so any seq-
  ;; iterator-based aggregate over a record came back empty.
  ;; (into {} record) returned {} and (reduce kv-fn {} record) was
  ;; a no-op. (seq record) itself worked (prim_seq has a record case)
  ;; -- the gap was in the iterator. seq_iter_init now routes records
  ;; through prim_seq up-front so every downstream consumer (into,
  ;; reduce, transduce, etc.) walks records as a seq of [k v] pairs.
  (defrecord PointForInto [x y])
  (let [p (->PointForInto 1 2)]
    (testing "(into {} record) collects fields as map entries"
      (is (= {:x 1 :y 2} (into {} p))))
    (testing "(into [] record) collects [k v] pairs in declared order"
      (is (= [[:x 1] [:y 2]] (into [] p))))
    (testing "(reduce conj #{} record) walks the kv pairs"
      (is (= #{[:x 1] [:y 2]} (reduce conj #{} p))))))

(deftest defrecord-inline-method-binds-fields
  ;; Real Clojure binds each declared field as a local inside any
  ;; inline protocol method body, so the body can write the field
  ;; name directly instead of (:field this). mino's defrecord
  ;; previously left bodies un-wrapped: bare `side` inside the method
  ;; resolved to "unbound symbol: side" at compile time, forcing
  ;; users to write (:side this) or destructure manually. The
  ;; defrecord macro now wraps each method body with a let that
  ;; binds field -> (get this :field).
  (testing "bare field name resolves inside inline method"
    (defprotocol IShape (area [s]))
    (defrecord Square [side] IShape (area [_] (* side side)))
    (is (= 16 (area (->Square 4)))))
  (testing "multiple fields, no `this` reference needed"
    (defprotocol IRect (rect-area [r]))
    (defrecord Rect [w h] IRect (rect-area [this] (* w h)))
    (is (= 12 (rect-area (->Rect 3 4)))))
  (testing "method param shadowing a field name keeps the param"
    ;; If a method param shadows a field name, the param wins -- the
    ;; auto-let does NOT shadow it back. Matches Clojure: `[this]`
    ;; is just whatever first param the user wrote.
    (defprotocol ISelf (selfp [s]))
    (defrecord Self [self] ISelf (selfp [self] self))
    (let [r (->Self :a)]
      (is (= r (selfp r))))))

(deftest extend-type-with-record-symbol
  (defprotocol IDouble (doublev [x]))
  (defrecord Wrap [v])
  (extend-type Wrap IDouble (doublev [w] (* 2 (:v w))))
  (is (= 6 (doublev (->Wrap 3))))
  (is (satisfies? IDouble (->Wrap 0))))

(deftest extend-protocol-mixed-keys
  (defprotocol IShow (show [x]))
  (defrecord Tagged [v])
  (extend-protocol IShow
    Tagged (show [t] (str "tagged:" (:v t)))
    :string (show [s] (str "string:" s)))
  (is (= "tagged:hi" (show (->Tagged "hi"))))
  (is (= "string:bye" (show "bye"))))

(deftest reify-anonymous-type
  (defprotocol IReified (rval [x]))
  (let [r (reify IReified (rval [_] :reified))]
    (is (record? r))
    (is (satisfies? IReified r))
    (is (= :reified (rval r)))))

(deftest defrecord-redefinition-preserves-identity
  (defrecord Stable [v])
  (let [s (->Stable 1)]
    (defrecord Stable [v])
    (is (instance? Stable s))))

(deftest defrecord-rejects-non-vector-fields
  ;; Real-world trigger: a portable .cljc file uses a reader conditional
  ;; for the fields slot (e.g. #?(:clj [...] :cljs [...])). On mino's
  ;; dialect that resolves to nothing, leaving the next form in the
  ;; fields position — typically a protocol name symbol. The macro must
  ;; reject this with a diagnostic that names the actual problem rather
  ;; than letting the failure surface downstream as "vec: cannot create
  ;; a vector from :symbol", which is opaque at the call site.
  (let [thrown (try
                 (eval '(defrecord Bad ProtoName))
                 nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? thrown))
    (is (some? (re-find #"defrecord" thrown)))
    (is (some? (re-find #"fields" thrown)))
    (is (some? (re-find #"vector" thrown)))))

(deftest deftype-rejects-non-vector-fields
  (let [thrown (try
                 (eval '(deftype Bad ProtoName (m [_] :x)))
                 nil
                 (catch e (if (map? e) (:mino/message e) (str e))))]
    (is (some? thrown))
    (is (some? (re-find #"deftype" thrown)))
    (is (some? (re-find #"fields" thrown)))
    (is (some? (re-find #"vector" thrown)))))
