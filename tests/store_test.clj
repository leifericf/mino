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

(run-tests-and-exit)
