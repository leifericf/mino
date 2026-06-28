(require "tests/test")
(require '[mino.store :as store])

;; mino.store — EAVT fact store.
;;
;; Each store holds an appending log of facts [e a v tx instant op] and a
;; materialized entity view. These tests cover lifecycle, transactions
;; (add/retract/map sugar), point/temporal/recency reads, aggregation,
;; durability, stats, and compaction.

(def store-test-dir "/tmp/mino-store-test")

;; ---------------------------------------------------------------------------
;; Lifecycle
;; ---------------------------------------------------------------------------

(deftest store-open-in-memory
  (let [conn (store/open)]
    (is (store? conn))
    (is (map? (store/db conn)))
    (store/close conn)))

(deftest store-db-and-deref-agree
  (let [conn (store/open)]
    (is (= (store/db conn) @conn))
    (store/close conn)))

(deftest store-empty-db-shape
  (let [db (store/db (store/open))]
    (is (= {} (:entities db)))
    (is (= [] (:log db)))
    (is (= 0 (:tx db)))))

(deftest store-predicate
  (is (store? (store/open)))
  (is (not (store? 42)))
  (is (not (store? "not a store")))
  (is (not (store? nil)))
  (is (not (store? {:entities {}}))))

(deftest store-close-idempotent
  (let [conn (store/open)]
    (store/close conn)
    (is (nil? (store/close conn)) "second close is a no-op")))

(deftest store-in-memory-checkpoint-noop
  (let [conn (store/open)]
    (is (nil? (store/checkpoint conn)) "in-memory checkpoint does not throw")
    (store/close conn)))

;; ---------------------------------------------------------------------------
;; Transact — add
;; ---------------------------------------------------------------------------

(deftest store-transact-add-single
  (let [conn (store/open)
        r (store/transact conn [:db/add 1 :name "Alice"])
        db (store/db conn)]
    (is (= 0 (:tx r)) "first applied tx is 0")
    (is (= "Alice" (store/read db 1 :name)))
    (is (= 1 (:tx db)) "db advances to 1")))

(deftest store-transact-return-shape
  (let [conn (store/open)
        r (store/transact conn [:db/add 1 :a 1])]
    (is (contains? r :tx))
    (is (contains? r :db-after))
    (is (= (store/db conn) (:db-after r)))))

(deftest store-transact-map-sugar
  (let [conn (store/open)
        _ (store/transact conn {1 {:name "Alice" :age 30}})
        db (store/db conn)]
    (is (= "Alice" (store/read db 1 :name)))
    (is (= 30 (store/read db 1 :age)))
    (is (= 1 (count (store/entities db))))))

(deftest store-transact-multiple-facts
  (let [conn (store/open)
        _ (store/transact conn [[:db/add 1 :name "Alice"]
                                [:db/add 2 :name "Bob"]
                                [:db/add 3 :name "Carol"]])
        db (store/db conn)]
    (is (= #{1 2 3} (store/entities db)))
    (is (= 1 (:tx db)) "one transact advances tx by one")
    (is (= 3 (count (:log db))) "three facts in the log")))

(deftest store-tx-increments-per-transact
  (let [conn (store/open)
        r1 (store/transact conn [:db/add 1 :a 1])
        r2 (store/transact conn [:db/add 1 :a 2])
        r3 (store/transact conn [:db/add 1 :a 3])
        db (store/db conn)]
    (is (= 0 (:tx r1)))
    (is (= 1 (:tx r2)))
    (is (= 2 (:tx r3)))
    (is (= 3 (:tx db)))))

(deftest store-facts-appear-in-log
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        _ (store/transact conn [:db/add 1 :age 30])
        log (:log (store/db conn))]
    (is (= 2 (count log)))
    (let [f0 (first log) f1 (second log)]
      (is (= :db/add (:op f0)))
      (is (= 1 (:e f0)))
      (is (= :name (:a f0)))
      (is (= 0 (:tx f0)))
      (is (= 1 (:tx f1))))))

(deftest store-put-asserts-single-fact
  (let [conn (store/open)
        _ (store/put conn 1 :name "Alice")
        db (store/db conn)]
    (is (= "Alice" (store/read db 1 :name)))))

;; ---------------------------------------------------------------------------
;; Transact — retract
;; ---------------------------------------------------------------------------

(deftest store-retract-attribute
  (let [conn (store/open)
        _ (store/transact conn {1 {:name "Alice" :age 30}})
        _ (store/transact conn [:db/retract 1 :name])
        db (store/db conn)]
    (is (nil? (store/read db 1 :name)))
    (is (= 30 (store/read db 1 :age)))
    (is (store/entity-exists? db 1) "other attributes keep the entity alive")))

(deftest store-retract-specific-value-matching
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        _ (store/transact conn [:db/retract 1 :name "Alice"])
        db (store/db conn)]
    (is (nil? (store/read db 1 :name)))))

(deftest store-retract-specific-value-nonmatching
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        _ (store/transact conn [:db/retract 1 :name "Bob"])
        db (store/db conn)]
    (is (= "Alice" (store/read db 1 :name)) "mismatched value is a no-op")))

(deftest store-retract-nonexistent-entity-noop
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        _ (store/transact conn [:db/retract 999 :name])
        db (store/db conn)]
    (is (= "Alice" (store/read db 1 :name)))
    (is (not (store/entity-exists? db 999)))))

(deftest store-retract-removes-entity-when-empty
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        _ (store/transact conn [:db/retract 1 :name])
        db (store/db conn)]
    (is (not (store/entity-exists? db 1)))
    (is (not (contains? (:entities db) 1)))
    (is (= #{} (store/entities db)))))

(deftest store-retract-fn-attribute
  (let [conn (store/open)
        _ (store/put conn 1 :name "Alice")
        _ (store/retract conn 1 :name)
        db (store/db conn)]
    (is (nil? (store/read db 1 :name)))))

(deftest store-retract-set-member
  ;; Cardinality: a set-valued attribute supports per-member retract.
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :tags #{:a :b :c}])
        _ (store/transact conn [:db/retract 1 :tags :b])
        db (store/db conn)]
    (is (= #{:a :c} (store/read db 1 :tags)))))

(deftest store-retract-last-set-member-drops-attribute
  (let [conn (store/open)
        _ (store/transact conn {1 {:tags #{:only}}})
        _ (store/transact conn [:db/retract 1 :tags :only])
        db (store/db conn)]
    (is (nil? (store/read db 1 :tags)))
    (is (not (store/entity-exists? db 1)) "last attribute gone removes entity")))

;; ---------------------------------------------------------------------------
;; Transact -- malformed tx-data
;; ---------------------------------------------------------------------------

(deftest store-tx-data-add-missing-value-throws
  ;; [:db/add e a] (3-tuple) is missing v and must NOT be silently
  ;; rewritten as :db/retract.
  (let [conn (store/open)]
    (is (thrown? (store/transact conn [:db/add 1 :name])))
    (is (= #{} (store/entities (store/db conn))) "no facts applied on throw")
    (is (= 0 (:tx (store/db conn))) "tx counter not advanced on throw")))

(deftest store-tx-data-add-extra-value-throws
  ;; [:db/add e a v extra] (5-tuple) is malformed.
  (let [conn (store/open)]
    (is (thrown? (store/transact conn [:db/add 1 :name "X" :extra])))
    (is (= #{} (store/entities (store/db conn))))
    (is (= 0 (:tx (store/db conn))))))

(deftest store-tx-data-retract-extra-value-throws
  ;; [:db/retract e a v extra] (5-tuple) is malformed.
  (let [conn (store/open)]
    (is (thrown? (store/transact conn [:db/retract 1 :name "X" :extra])))
    (is (= 0 (:tx (store/db conn))))))

(deftest store-tx-data-retract-three-tuple-still-works
  ;; The 3-tuple [:db/retract e a] sugar for "retract whole attribute"
  ;; must keep working after the arity tightening.
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        _ (store/transact conn [:db/retract 1 :name])
        db (store/db conn)]
    (is (not (store/entity-exists? db 1)))))

;; ---------------------------------------------------------------------------
;; Point reads
;; ---------------------------------------------------------------------------

(deftest store-read-vector-form
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        db (store/db conn)]
    (is (= "Alice" (store/read db [1 :name])))
    (is (nil? (store/read db [1 :missing])))
    (is (nil? (store/read db [999 :name])))))

(deftest store-read-spread-form
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        db (store/db conn)]
    (is (= "Alice" (store/read db 1 :name)))
    (is (= "Alice" (store/read db [1 :name])) "both forms agree")))

(deftest store-entity-returns-map-with-id
  (let [conn (store/open)
        _ (store/transact conn {1 {:name "Alice" :age 30}})
        db (store/db conn)]
    (is (= {:db/id 1 :name "Alice" :age 30} (store/entity db 1)))
    (is (nil? (store/entity db 999)))))

(deftest store-entity-exists
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        db (store/db conn)]
    (is (store/entity-exists? db 1))
    (is (not (store/entity-exists? db 999)))))

;; ---------------------------------------------------------------------------
;; Empty-store edge cases
;; ---------------------------------------------------------------------------

(deftest store-empty-store-reads
  (let [db (store/db (store/open))]
    (is (= #{} (store/entities db)))
    (is (nil? (store/read db 1 :name)))
    (is (nil? (store/entity db 1)))
    (is (not (store/entity-exists? db 1)))
    (is (= 0 (count (store/history db 1))))
    (is (empty? (store/recent db 5)))))

(deftest store-add-nil-value
  ;; v1 is single-valued and nil is a storable value; the entity exists
  ;; even though reading the nil attribute looks like absence.
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :note nil])
        db (store/db conn)]
    (is (store/entity-exists? db 1) "entity with a nil attribute still exists")
    (is (nil? (store/read db 1 :note)))
    (is (= {:db/id 1 :note nil} (store/entity db 1)))))

;; ---------------------------------------------------------------------------
;; Temporal reads
;; ---------------------------------------------------------------------------

(deftest store-as-of-by-tx
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :a :v0])   ;; tx 0
        _ (store/transact conn [:db/add 1 :a :v1])   ;; tx 1
        _ (store/transact conn [:db/add 1 :a :v2])   ;; tx 2
        db (store/db conn)]                          ;; :tx 3
    (is (nil? (store/read (store/as-of db 0) 1 :a)) "before any tx")
    (is (= :v0 (store/read (store/as-of db 1) 1 :a)) "state after tx 0")
    (is (= :v1 (store/read (store/as-of db 2) 1 :a)) "state after tx 1")
    (is (= :v2 (store/read (store/as-of db 3) 1 :a)) "current state")))

(deftest store-since-by-tx
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :a :v0])   ;; tx 0
        _ (store/transact conn [:db/add 1 :a :v1])   ;; tx 1
        _ (store/transact conn [:db/add 1 :a :v2])   ;; tx 2
        db (store/db conn)]
    (is (= 3 (count (store/since db 0)) "all facts at or after tx 0"))
    (is (= 2 (count (store/since db 1)) "tx 1 and 2"))
    (is (= 1 (count (store/since db 2)) "only tx 2"))
    (is (empty? (store/since db 3)) "nothing at or after current tx")))

(deftest store-as-of-by-inst
  ;; Datomic parity: (as-of db inst) interprets the arg as a wall-clock
  ;; instant by TYPE, not by magnitude. Passing #inst must always take
  ;; the instant branch, regardless of how small the epoch-ms value is.
  ;; A typical break with the old magnitude heuristic: a fresh process
  ;; whose monotonic clock was < 1e9 had every instant misread as a tx.
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :a :v0])   ;; tx 0
        _ (store/transact conn [:db/add 1 :a :v1])   ;; tx 1
        _ (store/transact conn [:db/add 1 :a :v2])   ;; tx 2
        db (store/db conn)]
    ;; A past inst (well before this test process started) sees no facts.
    (is (empty? (:entities (store/as-of db #inst "2020-01-01T00:00:00.000Z")))
        "past inst excludes all facts")
    ;; A far-future inst sees the current state.
    (is (= :v2 (store/read (store/as-of db #inst "2099-01-01T00:00:00.000Z") 1 :a))
        "future inst sees current state")))

(deftest store-since-by-inst
  ;; Symmetric: (since db inst) returns facts at or after the wall-clock
  ;; instant when the arg is an inst.
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :a :v0])
        _ (store/transact conn [:db/add 1 :a :v1])
        db (store/db conn)]
    (is (= 2 (count (store/since db #inst "2020-01-01T00:00:00.000Z"))
           "past inst includes all facts"))
    (is (empty? (store/since db #inst "2099-01-01T00:00:00.000Z"))
        "future inst excludes all facts")))

(deftest store-instants-are-wall-clock-ms
  ;; ADR 11 documents :instant as "wall-clock milliseconds". Recorded
  ;; :instant values must be in the same epoch-ms space as #inst and
  ;; inst-ms so that (as-of db #inst ...) and the recorded :instants
  ;; actually compare against each other usefully. (Pre-fix the clock
  ;; returned monotonic-ms, which only coincidentally ordered facts
  ;; within one process and never aligned with #inst.)
  (let [conn (store/open)
        before (inst-ms #inst "2024-01-01T00:00:00.000Z")
        _ (store/transact conn [:db/add 1 :a :v0])
        db (store/db conn)
        recorded (:instant (first (:log db)))]
    (is (> recorded before)
        "recorded :instant is in epoch-ms space (greater than a 2024 inst)")))

(deftest store-history-entity
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])    ;; tx 0
        _ (store/transact conn [:db/add 1 :name "Alicia"])   ;; tx 1
        _ (store/transact conn [:db/add 2 :name "Bob"])      ;; tx 2
        db (store/db conn)]
    (is (= 2 (count (store/history db 1))) "two facts for entity 1")
    (is (= 1 (count (store/history db 2))) "one fact for entity 2")
    (is (empty? (store/history db 999)) "no history for absent entity")
    (is (= ["Alice" "Alicia"] (map :v (store/history db 1))) "log order")))

(deftest store-history-entity-attribute
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        _ (store/transact conn [:db/add 1 :age 30])
        _ (store/transact conn [:db/add 1 :name "Alicia"])
        db (store/db conn)]
    (is (= 2 (count (store/history db 1 :name))))
    (is (= 1 (count (store/history db 1 :age))))))

;; ---------------------------------------------------------------------------
;; Recency
;; ---------------------------------------------------------------------------

(deftest store-recent-most-recent-first
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :a 1])
        _ (store/transact conn [:db/add 2 :a 2])
        _ (store/transact conn [:db/add 3 :a 3])
        db (store/db conn)]
    (is (= [3 2 1] (map :e (store/recent db 10))) "most recent first")
    (is (= [3 2] (map :e (store/recent db 2))) "bounded to n")
    (is (empty? (store/recent db 0)) "zero returns nothing")))

(deftest store-recent-where-filter
  (let [conn (store/open)
        _ (store/transact conn [[:db/add 1 :a 1]
                                [:db/retract 1 :a]
                                [:db/add 2 :a 2]])
        db (store/db conn)]
    (is (= [:db/retract]
           (map :op (store/recent db 10 {:where #(= (:op %) :db/retract)}))))))

(deftest store-recent-entities-scope
  (let [conn (store/open)
        _ (store/transact conn [[:db/add 1 :a 1]
                                [:db/add 2 :a 2]
                                [:db/add 1 :a 3]])
        db (store/db conn)]
    (is (= #{1} (set (map :e (store/recent db 10 {:entities #{1}})))))))

(deftest store-recent-attributes-scope
  (let [conn (store/open)
        _ (store/transact conn [[:db/add 1 :x 1]
                                [:db/add 1 :y 2]
                                [:db/add 1 :x 3]])
        db (store/db conn)]
    (is (= #{:x} (set (map :a (store/recent db 10 {:attributes #{:x}})))))))

;; ---------------------------------------------------------------------------
;; Collection reads
;; ---------------------------------------------------------------------------

(deftest store-entities-set
  (let [conn (store/open)
        _ (store/transact conn [[:db/add 1 :a 1] [:db/add 2 :a 2]])
        db (store/db conn)]
    (is (= #{1 2} (store/entities db)))
    (is (= #{} (store/entities (store/db (store/open)))))))

(deftest store-where-filter
  (let [conn (store/open)
        _ (store/transact conn {1 {:role :admin} 2 {:role :user} 3 {:role :admin}})
        db (store/db conn)]
    (let [admins (store/where db #(= (:role %) :admin))]
      (is (= 2 (count admins)))
      (is (= #{1 3} (set (map :db/id admins)))))))

(deftest store-find-by-single
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :email "a@x.com"])
        db (store/db conn)]
    (is (= 1 (store/find-by db :email "a@x.com")))))

(deftest store-find-by-multiple
  (let [conn (store/open)
        _ (store/transact conn [[:db/add 1 :role :admin] [:db/add 2 :role :admin]])
        db (store/db conn)]
    (is (= #{1 2} (store/find-by db :role :admin)))))

(deftest store-find-by-none
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :role :admin])
        db (store/db conn)]
    (is (nil? (store/find-by db :role :user)))))

;; ---------------------------------------------------------------------------
;; Projection and aggregation
;; ---------------------------------------------------------------------------

(deftest store-project-fn
  (let [conn (store/open)
        _ (store/transact conn [[:db/add 1 :a 1] [:db/add 2 :a 2]])
        db (store/db conn)]
    (is (= 2 (store/project db #(count (store/entities %)))))))

(deftest store-project-map
  (let [conn (store/open)
        _ (store/transact conn {1 {:name "Alice" :age 30}})
        db (store/db conn)]
    (is (= {:name "Alice" :age 30}
           (store/project db {:name [1 :name] :age [1 :age]})))))

(deftest store-project-map-missing-path
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        db (store/db conn)]
    (is (= {:name "Alice" :age nil}
           (store/project db {:name [1 :name] :age [1 :age]})))))

(deftest store-merge-disjoint-entities
  (let [conn-a (store/open)
        conn-b (store/open)
        _ (store/transact conn-a [:db/add 1 :a 1])
        _ (store/transact conn-b [:db/add 2 :b 2])
        db-a (store/db conn-a)
        db-b (store/db conn-b)
        merged (store/merge db-a db-b)]
    (is (= #{1 2} (store/entities merged)))
    (is (= (+ (count (:log db-a)) (count (:log db-b)))
           (count (:log merged))))))

(deftest store-merge-last-write-wins
  ;; Literal db values with controlled :instant make the merge order
  ;; deterministic; the later instant wins on conflicting writes.
  (let [db-a {:entities {1 {:v 1}}
              :log [{:e 1 :a :v :v 1 :tx 0 :instant 100 :op :db/add}]
              :tx 1}
        db-b {:entities {1 {:v 2}}
              :log [{:e 1 :a :v :v 2 :tx 0 :instant 200 :op :db/add}]
              :tx 1}
        merged (store/merge db-a db-b)]
    (is (= 2 (store/read merged 1 :v)) "later instant wins")
    (is (= 1 (:tx merged)) "tx is the max of the two")))

(deftest store-fold-default-extract
  ;; fold's default extract-fn is entity count.
  (let [db-a {:entities {1 {} 2 {}} :log [] :tx 0}
        db-b {:entities {3 {}}       :log [] :tx 0}]
    (is (= 3 (store/fold [db-a db-b] + 0)))))

(deftest store-fold-custom-extract
  (let [db-a {:entities {} :log [{:tx 0} {:tx 1}] :tx 2}
        db-b {:entities {} :log [{:tx 0}]          :tx 1}]
    (is (= 3 (store/fold [db-a db-b] + 0 (fn [db] (count (:log db))))))))

(deftest store-concat-logs
  (let [db-a {:entities {} :log [{:e 1 :instant 300}] :tx 1}
        db-b {:entities {} :log [{:e 2 :instant 100}] :tx 1}
        db-c {:entities {} :log [{:e 3 :instant 200}] :tx 1}]
    (is (= [100 200 300] (map :instant (store/concat-logs db-a db-b db-c))))))

;; ---------------------------------------------------------------------------
;; Pure variant
;; ---------------------------------------------------------------------------

(deftest store-with-pure-variant
  ;; store/with applies tx-data to a db value without a connection, so it
  ;; must derive an instant without a store (the store-clock* nil path).
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :a 1])
        db (store/db conn)
        r (store/with db [:db/add 1 :a 2])
        db' (:db-after r)]
    (is (= 2 (store/read db' 1 :a)) "speculated value")
    (is (= 1 (store/read db 1 :a)) "original db unchanged")
    (is (= (:tx db) (:tx r)) ":tx returned is the applied tx")))

;; ---------------------------------------------------------------------------
;; Stats
;; ---------------------------------------------------------------------------

(deftest store-stats-empty
  (let [db (store/db (store/open))]
    (is (= {:facts 0 :entities 0 :tx 0} (store/stats db)))))

(deftest store-stats-populated
  (let [conn (store/open)
        _ (store/transact conn {1 {:a 1} 2 {:b 2}})
        db (store/db conn)]
    (is (= {:facts 2 :entities 2 :tx 1} (store/stats db)))))

;; ---------------------------------------------------------------------------
;; Compaction
;; ---------------------------------------------------------------------------

(deftest store-compact-clears-log
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :a 1])
        _ (store/transact conn [:db/add 1 :a 2])
        _ (store/transact conn [:db/add 2 :b 3])
        _ (store/compact conn)
        db (store/db conn)]
    (is (empty? (:log db)) "log cleared")
    (is (= 2 (count (:entities db))) "view preserved")
    (is (= 2 (store/read db 1 :a)) "value preserved")
    (is (= 3 (:tx db)) "tx preserved")))

(deftest store-compact-keep-last
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :a 1])
        _ (store/transact conn [:db/add 1 :a 2])
        _ (store/transact conn [:db/add 1 :a 3])
        _ (store/transact conn [:db/add 1 :a 4])
        _ (store/compact conn {:keep-last 2})
        db (store/db conn)]
    (is (= 2 (count (:log db))) "last 2 facts kept")
    (is (= [3 4] (map :v (:log db))) "kept the most recent")
    (is (= 4 (store/read db 1 :a)) "view intact")))

;; ---------------------------------------------------------------------------
;; Durability
;; ---------------------------------------------------------------------------

(deftest store-durable-survives-reopen
  (rm-rf store-test-dir)
  (mkdir-p store-test-dir)
  (let [path (str store-test-dir "/snap.db")]
    (try
      (let [conn (store/open path)
            _ (store/transact conn {1 {:name "Alice" :age 30}})
            _ (store/checkpoint conn)
            _ (store/close conn)
            conn2 (store/open path)
            db (store/db conn2)]
        (is (= "Alice" (store/read db 1 :name)))
        (is (= 30 (store/read db 1 :age)))
        (is (= #{1} (store/entities db)))
        (store/close conn2))
      (finally
        (rm-rf store-test-dir)))))

;; ---------------------------------------------------------------------------
;; WAL durability
;; ---------------------------------------------------------------------------

(deftest store-wal-survives-without-checkpoint
  ;; Transact without checkpoint, reopen: WAL replay recovers the data.
  (rm-rf store-test-dir)
  (mkdir-p store-test-dir)
  (let [path (str store-test-dir "/wal1.db")]
    (try
      (let [conn (store/open path)
            _ (store/transact conn {1 {:name "Alice"}})
            ;; Don't checkpoint or close — simulate crash by opening a new conn
            conn2 (store/open path)
            db (store/db conn2)]
        (is (= "Alice" (store/read db 1 :name)) "WAL replay recovers data"))
      (finally
        (rm-rf store-test-dir)))))

(deftest store-wal-multiple-transactions
  ;; Multiple transactions between checkpoints are all replayed.
  (rm-rf store-test-dir)
  (mkdir-p store-test-dir)
  (let [path (str store-test-dir "/wal2.db")]
    (try
      (let [conn (store/open path)
            _ (store/transact conn [:db/add 1 :name "Alice"])
            _ (store/transact conn [:db/add 2 :name "Bob"])
            _ (store/transact conn [:db/add 3 :name "Carol"])
            conn2 (store/open path)
            db (store/db conn2)]
        (is (= #{"Alice" "Bob" "Carol"}
               (set (map #(store/read db % :name) [1 2 3]))))
        (is (= 3 (:tx db)) "tx counter preserved across WAL replay"))
      (finally
        (rm-rf store-test-dir)))))

(deftest store-wal-checkpoint-deletes-wal
  ;; After checkpoint, the WAL file is deleted.
  (rm-rf store-test-dir)
  (mkdir-p store-test-dir)
  (let [path (str store-test-dir "/wal3.db")
        wal-path (str path ".wal")]
    (try
      (let [conn (store/open path)
            _ (store/transact conn [:db/add 1 :name "Alice"])
            _ (store/checkpoint conn)]
        (is (not (file-exists? wal-path)) "WAL deleted after checkpoint"))
      (finally
        (rm-rf store-test-dir)))))

(deftest store-wal-torn-write-recovery
  ;; A malformed trailing WAL line is silently dropped.
  (rm-rf store-test-dir)
  (mkdir-p store-test-dir)
  (let [path (str store-test-dir "/wal4.db")
        wal-path (str path ".wal")]
    (try
      (let [conn (store/open path)
            _ (store/transact conn [:db/add 1 :name "Alice"])
            ;; Append garbage to simulate a torn write
            _ (spit wal-path "GARBAGE{not valid" :append true)
            conn2 (store/open path)
            db (store/db conn2)]
        (is (= "Alice" (store/read db 1 :name)) "good entry recovered")
        (is (= 1 (:tx db)) "tx counter from good entry"))
      (finally
        (rm-rf store-test-dir)))))

(deftest store-wal-checkpoint-then-transact
  ;; Snapshot captures state, new WAL entries are replayed on top.
  (rm-rf store-test-dir)
  (mkdir-p store-test-dir)
  (let [path (str store-test-dir "/wal5.db")]
    (try
      (let [conn (store/open path)
            _ (store/transact conn [:db/add 1 :name "Alice"])
            _ (store/checkpoint conn)
            _ (store/transact conn [:db/add 2 :name "Bob"])
            conn2 (store/open path)
            db (store/db conn2)]
        (is (= "Alice" (store/read db 1 :name)) "from snapshot")
        (is (= "Bob" (store/read db 2 :name)) "from WAL replay"))
      (finally
        (rm-rf store-test-dir)))))

(deftest store-wal-stale-entries-skipped
  (rm-rf store-test-dir)
  (mkdir-p store-test-dir)
  (let [path (str store-test-dir "/stale.db")
        wal-path (str path ".wal")]
    (try
      (let [conn (store/open path)
            _ (store/transact conn [:db/add 1 :name "Alice"])
            stale-line (slurp wal-path)
            _ (store/transact conn [:db/add 1 :age 30])
            _ (store/checkpoint conn)
            conn2 (store/open path)
            _ (store/transact conn2 [:db/add 2 :name "Carol"])
            fresh-line (slurp wal-path)
            _ (spit wal-path (str stale-line fresh-line))
            conn3 (store/open path)
            db (store/db conn3)
            name-facts (filter #(and (= (:e %) 1) (= (:a %) :name))
                               (:log db))]
        (is (= "Alice" (store/read db 1 :name)) "snapshot value present")
        (is (= 30 (store/read db 1 :age)) "snapshot value present")
        (is (= "Carol" (store/read db 2 :name)) "fresh WAL entry replayed")
        (is (= 1 (count name-facts)) "stale entry not double-applied to log")
        (is (= 3 (:tx db)) "tx advanced past the fresh entry only")
        (store/close conn3))
      (finally
        (rm-rf store-test-dir)))))

(deftest store-snapshot-atomic-write
  ;; Checkpoint writes to <path>.tmp then renames into place, so a
  ;; stale .tmp from a crashed previous attempt is cleaned up and the
  ;; canonical snapshot is never left half-written.
  (rm-rf store-test-dir)
  (mkdir-p store-test-dir)
  (let [path (str store-test-dir "/snap_atomic.db")
        tmp-path (str path ".tmp")]
    (try
      (let [conn (store/open path)
            _ (store/transact conn [:db/add 1 :name "Alice"])
            _ (store/checkpoint conn)
            ;; Simulate a stale .tmp left by a crashed checkpoint attempt
            _ (spit tmp-path "STALE GARBAGE FROM CRASHED CHECKPOINT")
            conn2 (store/open path)
            _ (store/transact conn2 [:db/add 2 :name "Bob"])
            _ (store/checkpoint conn2)
            conn3 (store/open path)
            db (store/db conn3)]
        (is (= "Alice" (store/read db 1 :name)) "first snapshot intact")
        (is (= "Bob" (store/read db 2 :name)) "second checkpoint applied")
        (is (not (file-exists? tmp-path))
            "stale .tmp cleaned up by atomic rename")
        (store/close conn3))
      (finally
        (rm-rf store-test-dir)))))

;; ---------------------------------------------------------------------------
;; Schema validation
;; ---------------------------------------------------------------------------

(deftest store-schema-accepts-valid-type
  (let [conn (store/open nil {:schema {:name {:type :string}}})
        _ (store/transact conn [:db/add 1 :name "Alice"])
        db (store/db conn)]
    (is (= "Alice" (store/read db 1 :name)))))

(deftest store-schema-rejects-wrong-type
  (let [conn (store/open nil {:schema {:age {:type :long}}})]
    (is (thrown? (store/transact conn [:db/add 1 :age "not a number"])))))

(deftest store-schema-many-cardinality
  (let [conn (store/open nil {:schema {:tags {:cardinality :many}}})
        _ (store/transact conn [:db/add 1 :tags :a])
        _ (store/transact conn [:db/add 1 :tags :b])
        _ (store/transact conn [:db/add 1 :tags :c])
        db (store/db conn)]
    (is (= #{:a :b :c} (store/read db 1 :tags)))))

(deftest store-schema-many-retract-member
  (let [conn (store/open nil {:schema {:tags {:cardinality :many}}})
        _ (store/transact conn [:db/add 1 :tags :a])
        _ (store/transact conn [:db/add 1 :tags :b])
        _ (store/transact conn [:db/retract 1 :tags :a])
        db (store/db conn)]
    (is (= #{:b} (store/read db 1 :tags)))))

(deftest store-schema-closed-rejects-unknown
  (let [conn (store/open nil {:schema {:name {:type :string}}
                              :closed true})]
    (is (thrown? (store/transact conn [:db/add 1 :unknown "value"])))))

(deftest store-schema-closed-accepts-declared
  (let [conn (store/open nil {:schema {:name {:type :string}}
                              :closed true})
        _ (store/transact conn [:db/add 1 :name "Alice"])
        db (store/db conn)]
    (is (= "Alice" (store/read db 1 :name)))))

(deftest store-schema-open-allows-unknown
  ;; Default (open schema) allows any attribute.
  (let [conn (store/open nil {:schema {:name {:type :string}}})
        _ (store/transact conn [:db/add 1 :name "Alice"])
        _ (store/transact conn [:db/add 1 :extra "anything"])
        db (store/db conn)]
    (is (= "Alice" (store/read db 1 :name)))
    (is (= "anything" (store/read db 1 :extra)))))

(deftest store-schema-returns-schema
  (let [schema {:name {:type :string} :tags {:cardinality :many}}
        conn (store/open nil {:schema schema})
        db (store/db conn)]
    (is (= schema (store/schema db)))))

(deftest store-schema-keyword-type
  (let [conn (store/open nil {:schema {:role {:type :keyword}}})
        _ (store/transact conn [:db/add 1 :role :admin])
        db (store/db conn)]
    (is (= :admin (store/read db 1 :role)))))

(deftest store-schema-rejects-unknown-type-spec
  ;; A typo in the type keyword (e.g. :strng) must not silently disable
  ;; validation. The schema declares :str, but :str is not a supported
  ;; type. The schema should be rejected at transact time so the typo
  ;; surfaces rather than silently accepting every value.
  (let [conn (store/open nil {:schema {:name {:type :strng}}})]
    (is (thrown? (store/transact conn [:db/add 1 :name "Alice"])))
    (is (thrown? (store/transact conn [:db/add 1 :name 42])))))

(deftest store-schema-long-rejects-overflowing-bigint
  ;; :long means 64-bit signed; bigints beyond Long/MAX_VALUE must be
  ;; rejected, not silently accepted as integers.
  (let [conn (store/open nil {:schema {:n {:type :long}}})]
    (is (thrown? (store/transact conn [:db/add 1 :n 9223372036854775808N])))
    (let [r (store/transact conn [:db/add 1 :n 1])]
      (is (= 1 (store/read (:db-after r) 1 :n))
          "in-range long still accepted"))))

(deftest store-schema-many-single-overwrite-without-schema
  ;; Without schema :many, :db/add overwrites (single-valued).
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :tags :a])
        _ (store/transact conn [:db/add 1 :tags :b])
        db (store/db conn)]
    (is (= :b (store/read db 1 :tags)) "overwrites without :many")))

(deftest store-tx-data-rejects-nil-entity-id
  ;; nil as an entity id collides with "absent" in point reads and lets
  ;; callers create a phantom nil-keyed entity.
  (let [conn (store/open)]
    (is (thrown? (store/transact conn [:db/add nil :name "X"])))
    (is (= #{} (store/entities (store/db conn))))
    (is (= 0 (:tx (store/db conn))))))

(deftest store-tx-data-rejects-nil-attribute
  ;; nil as an attribute is meaningless and indistinguishable from
  ;; "absent" on read.
  (let [conn (store/open)]
    (is (thrown? (store/transact conn [:db/add 1 nil "X"])))
    (is (= #{} (store/entities (store/db conn))))
    (is (= 0 (:tx (store/db conn))))))

;; ---------------------------------------------------------------------------
;; Indexes
;; ---------------------------------------------------------------------------

(deftest store-index-find-by-single
  (let [conn (store/open nil {:indexes #{:email}})
        _ (store/transact conn [:db/add 1 :email "a@x.com"])
        db (store/db conn)]
    (is (= 1 (store/find-by db :email "a@x.com")))))

(deftest store-index-find-by-multiple
  (let [conn (store/open nil {:indexes #{:role}})
        _ (store/transact conn [[:db/add 1 :role :admin]
                                [:db/add 2 :role :admin]])
        db (store/db conn)]
    (is (= #{1 2} (store/find-by db :role :admin)))))

(deftest store-index-find-by-none
  (let [conn (store/open nil {:indexes #{:email}})
        _ (store/transact conn [:db/add 1 :email "a@x.com"])
        db (store/db conn)]
    (is (nil? (store/find-by db :email "missing@x.com")))))

(deftest store-index-maintained-on-update
  ;; When a value changes, the old index entry is removed.
  (let [conn (store/open nil {:indexes #{:email}})
        _ (store/transact conn [:db/add 1 :email "old@x.com"])
        _ (store/transact conn [:db/add 1 :email "new@x.com"])
        db (store/db conn)]
    (is (nil? (store/find-by db :email "old@x.com")) "old value gone from index")
    (is (= 1 (store/find-by db :email "new@x.com")) "new value indexed")))

(deftest store-index-maintained-on-retract
  (let [conn (store/open nil {:indexes #{:email}})
        _ (store/transact conn [:db/add 1 :email "a@x.com"])
        _ (store/transact conn [:db/retract 1 :email])
        db (store/db conn)]
    (is (nil? (store/find-by db :email "a@x.com")) "retracted value removed from index")))

(deftest store-index-fallback-scan
  ;; Non-indexed attributes fall back to linear scan.
  (let [conn (store/open)
        _ (store/transact conn [[:db/add 1 :name "Alice"]
                                [:db/add 2 :name "Bob"]])
        db (store/db conn)]
    (is (= 1 (store/find-by db :name "Alice")))
    (is (= 2 (store/find-by db :name "Bob")))))

(deftest store-index-many-cardinality
  ;; Indexes work with :many cardinality (find entities with a specific tag).
  (let [conn (store/open nil {:schema {:tags {:cardinality :many}}
                              :indexes #{:tags}})
        _ (store/transact conn [[:db/add 1 :tags :a]
                                [:db/add 1 :tags :b]
                                [:db/add 2 :tags :a]])
        db (store/db conn)]
    (is (= #{1 2} (store/find-by db :tags :a)) "both entities have :a")
    (is (= 1 (store/find-by db :tags :b)) "only entity 1 has :b")))

;; ---------------------------------------------------------------------------
;; Retention
;; ---------------------------------------------------------------------------

(deftest store-retention-keep-last
  ;; Auto-compaction triggers when log exceeds 2×keep-last.
  (let [conn (store/open nil {:history {:keep-last 3}})
        _ (store/transact conn [:db/add 1 :a 1])
        _ (store/transact conn [:db/add 1 :a 2])
        _ (store/transact conn [:db/add 1 :a 3])
        ;; At 3 facts, threshold is 6 — no compaction yet
        db3 (store/db conn)]
    (is (= 3 (count (:log db3))) "log not compacted below threshold")
    (let [_ (store/transact conn [:db/add 1 :a 4])
          _ (store/transact conn [:db/add 1 :a 5])
          _ (store/transact conn [:db/add 1 :a 6])
          _ (store/transact conn [:db/add 1 :a 7])
          db7 (store/db conn)]
      (is (<= (count (:log db7)) 6) "log compacted after exceeding 2×threshold")
      (is (= 7 (store/read db7 1 :a)) "current value preserved"))))

(deftest store-retention-keep-since
  ;; :keep-since drops facts older than the cutoff.
  (let [conn (store/open nil {:history {:keep-since 200}})
        _ (store/transact conn [:db/add 1 :a :old])  ;; instant ~100
        _ (store/transact conn [:db/add 2 :a :new])  ;; instant ~200+
        db (store/db conn)]
    ;; With keep-since 200, facts before 200 should be compacted when
    ;; the threshold is exceeded. We check entities are preserved.
    (is (= :new (store/read db 2 :a)) "entity 2 preserved")
    (is (= :old (store/read db 1 :a)) "entity 1 preserved in view")))

(deftest store-retention-preserves-tx
  (let [conn (store/open nil {:history {:keep-last 2}})
        _ (store/transact conn [:db/add 1 :a 1])
        _ (store/transact conn [:db/add 1 :a 2])
        _ (store/transact conn [:db/add 1 :a 3])
        _ (store/transact conn [:db/add 1 :a 4])
        _ (store/transact conn [:db/add 1 :a 5])
        db (store/db conn)]
    (is (= 5 (:tx db)) "tx counter preserved after compaction")))

;; ---------------------------------------------------------------------------
;; Datalog query
;; ---------------------------------------------------------------------------

(deftest store-q-basic-find
  (let [conn (store/open)
        _ (store/transact conn {1 {:name "Alice"} 2 {:name "Bob"}})
        db (store/db conn)]
    (is (= #{[1] [2]}
           (store/q db '[:find ?e :where [?e :name ?n]])))))

(deftest store-q-find-two-vars
  (let [conn (store/open)
        _ (store/transact conn {1 {:name "Alice"} 2 {:name "Bob"}})
        db (store/db conn)]
    (is (= #{[1 "Alice"] [2 "Bob"]}
           (store/q db '[:find ?e ?name :where [?e :name ?name]])))))

(deftest store-q-constant-value
  (let [conn (store/open)
        _ (store/transact conn [[:db/add 1 :name "Alice"]
                                [:db/add 2 :name "Bob"]])
        db (store/db conn)]
    (is (= #{[1]}
           (store/q db '[:find ?e :where [?e :name "Alice"]])))))

(deftest store-q-join-two-patterns
  (let [conn (store/open)
        _ (store/transact conn {1 {:name "Alice" :age 30}
                                2 {:name "Bob" :age 25}})
        db (store/db conn)]
    (is (= #{[1 "Alice" 30] [2 "Bob" 25]}
           (store/q db '[:find ?e ?name ?age
                         :where [?e :name ?name]
                                [?e :age ?age]])))))

(deftest store-q-predicate-filter
  (let [conn (store/open)
        _ (store/transact conn {1 {:name "Alice" :age 30}
                                2 {:name "Bob" :age 25}
                                3 {:name "Carol" :age 35}})
        db (store/db conn)]
    (is (= #{[1 "Alice"] [3 "Carol"]}
           (store/q db '[:find ?e ?name
                         :where [?e :name ?name]
                                [?e :age ?age]
                                [(> ?age 28)]])))))

(deftest store-q-predicate-eq
  (let [conn (store/open)
        _ (store/transact conn {1 {:role :admin} 2 {:role :user}})
        db (store/db conn)]
    (is (= #{[1]}
           (store/q db '[:find ?e
                         :where [?e :role ?r]
                                [(= ?r :admin)]])))))

(deftest store-q-empty-result
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        db (store/db conn)]
    (is (= #{}
           (store/q db '[:find ?e :where [?e :name "Nonexistent"]])))))

(deftest store-q-with-index
  ;; Datalog works with indexed attributes.
  (let [conn (store/open nil {:indexes #{:email}})
        _ (store/transact conn [[:db/add 1 :email "a@x.com"]
                                [:db/add 2 :email "b@x.com"]
                                [:db/add 1 :name "Alice"]])
        db (store/db conn)]
    (is (= #{[1 "Alice"]}
           (store/q db '[:find ?e ?name
                         :where [?e :name ?name]
                                [?e :email "a@x.com"]])))))

(deftest store-q-index-variable-value
  ;; Datalog on indexed attr with variable value uses the index.
  (let [conn (store/open nil {:indexes #{:email}})
        _ (store/transact conn [[:db/add 1 :email "a@x.com"]
                                [:db/add 2 :email "b@x.com"]
                                [:db/add 3 :email "c@x.com"]])
        db (store/db conn)]
    (is (= #{[1 "a@x.com"] [2 "b@x.com"] [3 "c@x.com"]}
           (store/q db '[:find ?e ?email
                         :where [?e :email ?email]])))))

(deftest store-q-index-constant-eid
  ;; Datalog on indexed attr with constant entity-id uses the index.
  (let [conn (store/open nil {:indexes #{:email}})
        _ (store/transact conn [[:db/add 1 :email "a@x.com"]
                                [:db/add 2 :email "b@x.com"]])
        db (store/db conn)]
    (is (= #{["a@x.com"]}
           (store/q db '[:find ?email
                         :where [1 :email ?email]])))))

(deftest store-q-index-join
  ;; Datalog joins work when one pattern uses an indexed attr.
  (let [conn (store/open nil {:indexes #{:email}})
        _ (store/transact conn {1 {:email "a@x.com" :name "Alice"}
                                2 {:email "b@x.com" :name "Bob"}})
        db (store/db conn)]
    (is (= #{["Alice"]}
           (store/q db '[:find ?name
                         :where [?e :email "a@x.com"]
                                [?e :name ?name]])))))

(deftest store-q-malformed-throws
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        db (store/db conn)]
    (is (thrown? (store/q db {:not :a-query})) "non-vector throws")
    (is (thrown? (store/q db 42)) "scalar throws")
    (is (thrown? (store/q db '[:where [?e :a ?v]])) "missing :find throws")
    (is (thrown? (store/q db nil)) "nil throws")))

(deftest store-q-unbound-find-var-throws
  ;; A var referenced in :find but never bound by any :where clause is a
  ;; query-author error. It must throw, not silently return a column of
  ;; nils.
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        db (store/db conn)]
    (is (thrown? (store/q db '[:find ?e ?unbound
                               :where [?e :name ?n]]))
        "unbound var in :find throws")))

(deftest store-q-malformed-pattern-throws
  ;; Pattern clauses must be 3-element vectors [e a v]. Wrong arities and
  ;; non-vector clauses must throw, not silently produce empty results.
  (let [conn (store/open)
        _ (store/transact conn [:db/add 1 :name "Alice"])
        db (store/db conn)]
    (is (thrown? (store/q db '[:find ?e :where [?e :name]]))
        "2-element pattern throws")
    (is (thrown? (store/q db '[:find ?e :where [?e]]))
        "1-element pattern throws")
    (is (thrown? (store/q db '[:find ?e :where [?e :n ?v ?w]]))
        "4-element pattern throws")
    (is (thrown? (store/q db '[:find ?e :where :not-a-clause]))
        "non-vector clause throws")
    (is (thrown? (store/q db '[:find ?e :where []]))
        "empty vector clause throws")))

(run-tests-and-exit)
