;; mino.store — EAVT fact store for embedded per-runtime memory.
;;
;; Each store holds an accumulating log of facts [e a v tx instant op]
;; and a materialized view (entity -> attribute -> value). Stores are
;; per-state isolated; cross-runtime transfer uses mino_clone.
;;
;; The db value is a persistent map:
;;   {:entities {}   ;; entity-id -> {attr -> value}
;;    :log []        ;; flat vector of fact maps {:e :a :v :tx :instant :op}
;;    :tx 0          ;; next transaction number (monotonic per-store)
;;    :schema {}      ;; attribute -> {:type :string :cardinality :one}
;;    :closed? false} ;; when true, reject attributes not in :schema
;;
;; In-memory by default. Durability uses snapshot + WAL (see ADR 11):
;; each transact on a durable store appends a tx-info map to the WAL at
;; <path>.wal; checkpoint writes the snapshot and deletes the WAL; open
;; reads the snapshot then replays the WAL.
;;
;; Schema validation is opt-in via the :schema option on open. With
;; :closed true, only declared attributes are accepted.

(ns mino.store
  "EAVT fact store for embedded per-runtime memory.

  Each store holds an accumulating log of facts [e a v tx instant op]
  and a materialized view (entity -> attribute -> value). Stores are
  per-state isolated; cross-runtime transfer uses mino_clone.

  In-memory by default; durability via snapshot-on-checkpoint.")

;; The clojure.core store? predicate is referred into this namespace at
;; load time. Capturing it under a private alias lets the public store?
;; below delegate to the primitive instead of recursing into itself,
;; mirroring the -lock-* capture pattern used in core.clj.
(def ^:private c-store? store?)

;; ---------------------------------------------------------------------------
;; Data shape helpers
;; ---------------------------------------------------------------------------

(defn- empty-db
  "Returns a fresh db value with no entities, an empty log, a zero
  transaction counter, and an empty open schema."
  ([] {:entities {} :log [] :tx 0 :schema {} :closed? false})
  ([schema] {:entities {} :log [] :tx 0 :schema (or schema {}) :closed? false})
  ([schema closed?]
   {:entities {} :log [] :tx 0 :schema (or schema {}) :closed? (or closed? false)}))

(defn- make-fact
  "Builds a single fact map from its EAVT coordinates plus the tx number,
  a wall-clock instant, and the operation (:db/add or :db/retract)."
  [e a v tx instant op]
  {:e e :a a :v v :tx tx :instant instant :op op})

;; ---------------------------------------------------------------------------
;; Schema validation
;; ---------------------------------------------------------------------------

(defn- check-type
  "Returns nil if v matches type-spec, throws ex-info otherwise.
  Supported types: :string, :keyword, :long, :double, :boolean,
  :instant (treated as :long), :any (no check, default)."
  [attr v type-spec]
  (let [ok? (case type-spec
              :string  (string? v)
              :keyword (keyword? v)
              :long    (integer? v)
              :double  (float? v)
              :boolean (boolean? v)
              :instant (integer? v)
              :any     true
              true)]
    (when-not ok?
      (throw
        (ex-info (str "Type mismatch for attribute " attr
                      ": expected " type-spec)
                 {:attribute attr :value v :expected type-spec})))))

(defn- validate-fact
  "Validates a single fact against the schema. Throws ex-info on
  violation: unknown attribute in a closed schema, or type mismatch
  on :db/add. No-op when schema is empty."
  [schema closed? {:keys [e a v op]}]
  (let [spec (get schema a)]
    (when (and closed? (not spec))
      (throw
        (ex-info (str "Unknown attribute in closed schema: " a)
                 {:attribute a :entity e})))
    (when (and spec (:type spec) (not= (:type spec) :any)
               (= op :db/add))
      (check-type a v (:type spec)))))

;; ---------------------------------------------------------------------------
;; Index maintenance
;; ---------------------------------------------------------------------------

(defn- build-indexes
  "Builds the index map from entities for the given set of indexed
  attributes. Returns a map: attr -> {value -> #{entity-ids}}.
  Handles set-valued (:many) attributes by indexing each member."
  [entities indexed-attrs]
  (if (empty? indexed-attrs)
    {}
    (reduce (fn [idx [e attrs]]
              (reduce (fn [idx a]
                        (if (contains? indexed-attrs a)
                          (let [v   (get attrs a)
                                vals (cond
                                       (set? v) v
                                       (nil? v) nil
                                       :else #{v})]
                            (if vals
                              (reduce (fn [idx val]
                                        (let [cur (get-in idx [a val] #{})]
                                          (assoc-in idx [a val] (conj cur e))))
                                      idx vals)
                              idx))
                          idx))
                      idx (keys attrs)))
            {} entities)))

;; ---------------------------------------------------------------------------
;; Retention
;; ---------------------------------------------------------------------------

(defn- maybe-compact-log
  "Checks the :history policy on db and compacts the log if it exceeds
  the threshold. The materialized entities view is always preserved;
  only old log entries are dropped. Returns db unchanged when no
  policy is active or the threshold is not exceeded."
  [db]
  (let [history (get db :history)
        log (:log db)]
    (cond
      (nil? history) db

      (and (:keep-last history)
           (> (count log) (* 2 (:keep-last history))))
      (let [kept (take-last (:keep-last history) log)]
        (assoc db :log (vec kept)))

      (and (:keep-since history)
           (some #(< (:instant % 0) (:keep-since history)) log))
      (let [cutoff (:keep-since history)
            kept (filter #(>= (:instant % 0) cutoff) log)]
        (assoc db :log (vec kept)))

      :else db)))

;; ---------------------------------------------------------------------------
;; Lifecycle
;; ---------------------------------------------------------------------------

(defn open
  "Opens a store connection. With no args, opens an in-memory store.
  With a path string, opens a durable store (reads snapshot + replays
  WAL if files exist). The options map may carry:
    :schema    map of attribute -> {:type :cardinality} spec
    :closed    when true, reject attributes not in :schema (default false)
    :indexes   set of attributes to maintain reverse indexes for
    :history   {:keep-last N} or {:keep-since T} for auto-compaction
  Schema, indexes, and history are set at first open and stored in the
  db value; on reopen the snapshot's values take precedence."
  ([] (store-open* (empty-db) nil))
  ([path] (open path nil))
  ([path opts]
   (let [snap-db (when path (store-read-snapshot* path))
         base (or snap-db
                    (-> (empty-db (:schema opts) (:closed opts))
                        (assoc :indexed-attrs (or (:indexes opts) #{}))
                        (assoc :history (:history opts))))
         entries (when path (store-read-wal* path))
         db (if (seq entries)
              (reduce (fn [d entry]
                        (apply-tx d (:tx entry) (:instant entry)
                                  (:tx-data entry)))
                      base entries)
              base)
         db (maybe-compact-log db)]
     (store-open* db path))))

(defn close
  "Flushes (if durable) and closes the store. Idempotent."
  [conn]
  (store-close* conn)
  nil)

(defn db
  "Returns the store's current immutable db value (a snapshot)."
  [conn]
  @conn)

(defn checkpoint
  "Writes the db value to disk if the store is durable. No-op for an
  in-memory store."
  [conn]
  (store-checkpoint* conn)
  nil)

(defn store?
  "Returns true if x is a store connection."
  [x]
  (c-store? x))

;; ---------------------------------------------------------------------------
;; Transaction parsing
;; ---------------------------------------------------------------------------

(defn- parse-tx-data
  "Parses tx-data into a flat seq of [op e a v] tuples. Accepts three
  forms:

    [:db/add e a v]       — assert
    [:db/retract e a v]   — retract a specific value
    [:db/retract e a]     — retract every value of attribute a
    {e {a v a2 v2 ...}}   — map sugar (all adds)

  A seq of any of the above is flattened recursively; an item that
  matches none of the shapes throws."
  [tx-data]
  (cond
    (and (vector? tx-data)
         (or (= (first tx-data) :db/add)
             (= (first tx-data) :db/retract)))
    (if (= (count tx-data) 4)
      [[(tx-data 0) (tx-data 1) (tx-data 2) (tx-data 3)]]
      [[:db/retract (tx-data 1) (tx-data 2) nil]])

    (map? tx-data)
    (vec (for [[e attrs] tx-data
               [a v] attrs]
           [:db/add e a v]))

    (seq? tx-data)
    (mapcat parse-tx-data tx-data)

    (vector? tx-data)
    (mapcat parse-tx-data tx-data)

    :else
    (throw (ex-info "Invalid tx-data format" {:tx-data tx-data}))))

;; ---------------------------------------------------------------------------
;; Transaction application
;; ---------------------------------------------------------------------------

(defn- retract-attr
  "Removes attribute a from entity, dropping the entity entirely when
  no attributes remain. Returns the updated entities map."
  [entities e entity a]
  (let [entity' (dissoc entity a)]
    (if (empty? entity')
      (dissoc entities e)
      (assoc entities e entity'))))

(defn- apply-fact
  "Applies a single fact to the entities map, returning the updated
  entities. :db/add overwrites the attribute (single-valued) or conjs
  into a set (:many cardinality from schema). :db/retract with a nil
  value drops the whole attribute; with a concrete value it drops the
  attribute when the current value matches (single-valued) or disj's
  the value from a multi-valued set, removing the attribute when the
  set becomes empty."
  ([entities fact] (apply-fact entities fact nil))
  ([entities {:keys [e a v op]} schema]
   (let [entity  (or (get entities e) {})
         spec    (get schema a)
         many?   (= :many (:cardinality spec))]
     (case op
       :db/add
       (if many?
         (let [cur (get entity a)]
           (assoc entities e (assoc entity a (conj (or cur #{}) v))))
         (assoc entities e (assoc entity a v)))

       :db/retract
       (if (nil? v)
         (retract-attr entities e entity a)
         (let [cur (get entity a)]
           (cond
             (nil? cur) entities
             (set? cur)
             (if (contains? cur v)
               (let [cur' (disj cur v)]
                 (if (empty? cur')
                   (retract-attr entities e entity a)
                   (assoc entities e (assoc entity a cur'))))
               entities)
              (= cur v)
             (retract-attr entities e entity a)
             :else entities)))))))

(defn- apply-tx
  "Applies a parsed transaction to the db value, returning a new db
  value. Each op becomes a fact tagged with tx-num and instant; the
  materialized view is rebuilt by folding the facts, and the flat log
  grows by the new facts. The tx counter advances by one. Schema,
  closed?, indexes, and history policy are preserved from the input db.
  Each fact is validated against the schema before application."
  [db tx-num instant tx-data]
  (let [schema        (get db :schema {})
        closed?       (get db :closed? false)
        indexed-attrs (get db :indexed-attrs #{})
        history       (get db :history)
        ops           (parse-tx-data tx-data)
        facts         (vec (for [[op e a v] ops]
                             (make-fact e a v tx-num instant op)))]
    (doseq [f facts] (validate-fact schema closed? f))
    (let [entities (reduce (fn [acc f] (apply-fact acc f schema))
                           (:entities db) facts)
          indexes  (build-indexes entities indexed-attrs)]
      {:entities entities
       :log (into (:log db) facts)
       :tx (inc tx-num)
       :schema schema
       :closed? closed?
       :indexed-attrs indexed-attrs
       :indexes indexes
       :history history})))

;; ---------------------------------------------------------------------------
;; Public transaction API
;; ---------------------------------------------------------------------------

(defn transact
  "Transacts facts against the store connection. Atomic: all-or-nothing.
  tx-data accepts any of the parse-tx-data forms. Returns a map of the
  tx number just applied and the resulting db value under :db-after.
  On a durable store, the transaction is appended to the WAL before
  the in-memory publish."
  [conn tx-data]
  (let [cur @conn
        tx-num (:tx cur)
        instant (store-clock* conn)
        new-db (maybe-compact-log
                 (apply-tx cur tx-num instant tx-data))
        tx-info {:tx tx-num :instant instant :tx-data tx-data}]
    (store-commit* conn new-db tx-info)
    {:tx tx-num :db-after new-db}))

(defn with
  "Pure variant of transact: applies tx-data to a db value without
  touching a connection. Returns {:tx N :db-after new-db}. Useful for
  speculating over a snapshot."
  [db-val tx-data]
  (let [tx-num (:tx db-val)
        instant (store-clock* nil)
        new-db (apply-tx db-val tx-num instant tx-data)]
    {:tx tx-num :db-after new-db}))

(defn put
  "Asserts a single fact. Equivalent to
  (transact conn [[:db/add e a v]])."
  [conn e a v]
  (transact conn [[:db/add e a v]]))

(defn retract
  "Retracts a fact. With three args, retracts every value of attribute
  a on entity e. With four args, retracts only the specific value v."
  ([conn e a]
   (transact conn [[:db/retract e a]]))
  ([conn e a v]
   (transact conn [[:db/retract e a v]])))

;; ---------------------------------------------------------------------------
;; Point reads
;; ---------------------------------------------------------------------------

(defn read
  "Reads a single attribute value for an entity. Accepts either
  (read db-val e a) or (read db-val [e a]). Returns nil when the entity
  or attribute is absent."
  ([db-val e a]
   (get-in (:entities db-val) [e a]))
  ([db-val [e a]]
   (get-in (:entities db-val) [e a])))

(defn entity
  "Returns all attributes of entity e as a map, tagged with the entity
  id under :db/id. Returns nil when the entity is absent."
  [db-val e]
  (let [attrs (get (:entities db-val) e)]
    (when attrs (assoc attrs :db/id e))))

(defn entity-exists?
  "Returns true if entity e exists in the db value."
  [db-val e]
  (contains? (:entities db-val) e))

;; ---------------------------------------------------------------------------
;; Collection reads
;; ---------------------------------------------------------------------------

(defn entities
  "Returns the set of all entity ids in the db value."
  [db-val]
  (set (keys (:entities db-val))))

(defn where
  "Filters entities by pred, which receives each entity map (including
  :db/id). Returns a seq of the matching entity maps."
  [db-val pred]
  (filter pred
          (for [[e attrs] (:entities db-val)]
            (assoc attrs :db/id e))))

(defn find-by
  "Finds entity ids where attr equals value. Uses the reverse index if
  one is maintained for attr (via :indexes on open); otherwise falls
  back to a linear scan. Handles :many cardinality attributes by
  checking set membership. Returns the single id when exactly one entity
  matches, a set of ids when several match, and nil when none do."
  [db-val attr value]
  (if-let [index (get-in db-val [:indexes attr])]
    (let [found (get index value)]
      (cond
        (nil? found) nil
        (= (count found) 1) (first found)
        :else found))
    (let [found (for [[e attrs] (:entities db-val)
                      :let [cur (get attrs attr)]
                      :when (or (= cur value)
                                (and (set? cur) (contains? cur value)))]
                  e)]
      (cond
        (empty? found) nil
        (= (count found) 1) (first found)
        :else (set found)))))

;; ---------------------------------------------------------------------------
;; Temporal reads
;; ---------------------------------------------------------------------------

(defn- point-as-instant?
  "Distinguishes a wall-clock instant (epoch milliseconds, well above a
  billion) from a transaction number (small and monotonic). Used by
  as-of and since to interpret their point argument."
  [point]
  (> point 1000000000))

(defn as-of
  "Returns the db value as it was at tx N or instant T. Replays the log
  up to (but not including) the point, rebuilding the materialized view
  in tx order. The returned :tx, :schema, :closed?, :indexed-attrs, and
  :history are preserved from the input. Indexes are rebuilt for the
  as-of entity view."
  [db-val point]
  (let [schema (get db-val :schema {})
        indexed-attrs (get db-val :indexed-attrs #{})
        instant? (point-as-instant? point)
        facts-before (filter (fn [f]
                                (if instant?
                                  (< (:instant f) point)
                                  (< (:tx f) point)))
                              (:log db-val))
        ordered (sort-by :tx facts-before)
        entities (reduce (fn [acc f] (apply-fact acc f schema))
                         {} ordered)
        indexes (build-indexes entities indexed-attrs)]
    {:entities entities :log (vec ordered) :tx (:tx db-val)
     :schema schema :closed? (get db-val :closed? false)
     :indexed-attrs indexed-attrs :indexes indexes
     :history (get db-val :history)}))

(defn since
  "Returns the seq of facts asserted at or after tx N (or instant T),
  in log order."
  [db-val point]
  (let [instant? (point-as-instant? point)]
    (filter (fn [f]
              (if instant?
                (>= (:instant f) point)
                (>= (:tx f) point)))
            (:log db-val))))

(defn history
  "Returns the full history of an entity, optionally narrowed to one
  attribute, as a seq of facts in log order."
  ([db-val e]
   (filter #(= (:e %) e) (:log db-val)))
  ([db-val e a]
   (filter #(and (= (:e %) e) (= (:a %) a)) (:log db-val))))

;; ---------------------------------------------------------------------------
;; Recency queries
;; ---------------------------------------------------------------------------

(defn recent
  "Returns the last N facts in temporal order, most recent first.
  The optional opts map may carry:
    :where      a predicate over a fact
    :entities   a set of entity ids to scope to
    :attributes a set of attribute keys to scope to"
  ([db-val n]
   (take n (reverse (:log db-val))))
  ([db-val n opts]
   (let [log (reverse (:log db-val))
         log (if (:where opts)
               (filter (:where opts) log)
               log)
         log (if (:entities opts)
               (filter (comp (:entities opts) :e) log)
               log)
         log (if (:attributes opts)
               (filter (comp (:attributes opts) :a) log)
               log)]
     (take n log))))

;; ---------------------------------------------------------------------------
;; Projection and aggregation
;; ---------------------------------------------------------------------------

(defn project
  "Derives a small value from a db value, cheap to clone across states.
  spec is either a function (called with db-val) or a map of output key
  to [e a] path, in which case a map of output key to looked-up value
  is returned."
  [db-val spec]
  (if (fn? spec)
    (spec db-val)
    (into {}
          (for [[k path] spec]
            [k (get-in (:entities db-val) path)]))))

(defn merge
  "Merges two db values into a new one. The fact logs concatenate and
  replay in instant order, so the entity view reflects last-write-wins
  by :instant. The tx counter is the max of the two. Schema, closed?,
  indexed-attrs, and history are taken from db-a. Indexes are rebuilt
  for the merged entity view."
  [db-a db-b]
  (let [schema (get db-a :schema {})
        indexed-attrs (get db-a :indexed-attrs #{})
        all-facts (sort-by :instant (concat (:log db-a) (:log db-b)))
        entities (reduce (fn [acc f] (apply-fact acc f schema))
                         {} all-facts)
        indexes (build-indexes entities indexed-attrs)]
    {:entities entities
     :log (vec all-facts)
     :tx (max (:tx db-a) (:tx db-b))
     :schema schema
     :closed? (get db-a :closed? false)
     :indexed-attrs indexed-attrs
     :indexes indexes
     :history (get db-a :history)}))

(defn fold
  "Reduces across a collection of db values. extract-fn pulls the value
  to fold from each db (defaults to entity count); f combines the
  accumulator with each extracted value starting from init."
  ([dbs f init]
   (fold dbs f init (fn [db] (count (:entities db)))))
  ([dbs f init extract-fn]
   (reduce (fn [acc db] (f acc (extract-fn db))) init dbs)))

(defn concat-logs
  "Concatenates the fact logs of the given db values into a single flat
  seq sorted by :instant."
  [& dbs]
  (sort-by :instant (apply concat (map :log dbs))))

;; ---------------------------------------------------------------------------
;; Stats
;; ---------------------------------------------------------------------------

(defn stats
  "Returns a map of store statistics: fact count, entity count, and the
  current tx number."
  [db-val]
  {:facts (count (:log db-val))
   :entities (count (:entities db-val))
   :tx (:tx db-val)})

;; ---------------------------------------------------------------------------
;; Compaction
;; ---------------------------------------------------------------------------

(defn compact
  "Bounds the log of a durable or long-lived store by dropping old
  facts while preserving the materialized view. Does not write to the
  WAL — compaction is a maintenance operation; checkpoint afterwards to
  persist the compacted state.

  With one arg, drops the entire log (only the current view survives).
  With a keep-spec map:
    {:keep-last N}     keep the last N facts
    {:keep-since T}    keep facts at or after instant T"
  ([conn]
   (let [cur @conn]
     (store-commit* conn {:entities (:entities cur)
                          :log []
                          :tx (:tx cur)
                          :schema (get cur :schema {})
                          :closed? (get cur :closed? false)
                          :indexed-attrs (get cur :indexed-attrs #{})
                          :indexes (get cur :indexes {})
                          :history (get cur :history)})))
  ([conn keep-spec]
   (let [cur @conn
         log (:log cur)
         kept (cond
                (and (map? keep-spec) (:keep-last keep-spec))
                (take-last (:keep-last keep-spec) log)

                (and (map? keep-spec) (:keep-since keep-spec))
                (filter #(>= (:instant %) (:keep-since keep-spec)) log)

                :else log)]
     (store-commit* conn {:entities (:entities cur)
                          :log (vec kept)
                          :tx (:tx cur)
                          :schema (get cur :schema {})
                          :closed? (get cur :closed? false)
                          :indexed-attrs (get cur :indexed-attrs #{})
                          :indexes (get cur :indexes {})
                          :history (get cur :history)}))))

;; ---------------------------------------------------------------------------
;; Schema (reserved for future use)
;; ---------------------------------------------------------------------------

(defn schema
  "Returns the store's schema map (attribute -> {:type :cardinality}).
  Empty when no schema was configured."
  [db-val]
  (get db-val :schema {}))
