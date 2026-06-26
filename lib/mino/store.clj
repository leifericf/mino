;; mino.store — EAVT fact store for embedded per-runtime memory.
;;
;; Each store holds an accumulating log of facts [e a v tx instant op]
;; and a materialized view (entity -> attribute -> value). Stores are
;; per-state isolated; cross-runtime transfer uses mino_clone.
;;
;; The db value is a persistent map:
;;   {:entities {}   ;; entity-id -> {attr -> value}
;;    :log []        ;; flat vector of fact maps {:e :a :v :tx :instant :op}
;;    :tx 0}         ;; next transaction number (monotonic per-store)
;;
;; In-memory by default; durability via snapshot-on-checkpoint. The C
;; primitives store-open*, store-commit*, store-checkpoint*, store-close*,
;; store?, and store-clock* are installed in clojure.core by the runtime;
;; this namespace wraps them with the query, transaction, and aggregation
;; API.

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
  "Returns a fresh db value with no entities, an empty log, and a zero
  transaction counter."
  []
  {:entities {} :log [] :tx 0})

(defn- make-fact
  "Builds a single fact map from its EAVT coordinates plus the tx number,
  a wall-clock instant, and the operation (:db/add or :db/retract)."
  [e a v tx instant op]
  {:e e :a a :v v :tx tx :instant instant :op op})

;; ---------------------------------------------------------------------------
;; Lifecycle
;; ---------------------------------------------------------------------------

(defn open
  "Opens a store connection. With no args, opens an in-memory store.
  With a path string, opens a durable store (the C side reads the
  snapshot if the file exists). The options map is reserved for future
  use (schema, indexes, history config) and ignored in v1."
  ([] (store-open* (empty-db) nil))
  ([path] (store-open* (empty-db) path))
  ([path opts] (store-open* (empty-db) path)))

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
  entities. :db/add overwrites the attribute (single-valued in v1).
  :db/retract with a nil value drops the whole attribute; with a
  concrete value it drops the attribute when the current value matches
  (single-valued) or disj's the value from a multi-valued set, removing
  the attribute when the set becomes empty."
  [entities {:keys [e a v op]}]
  (let [entity (or (get entities e) {})]
    (case op
      :db/add
      (assoc entities e (assoc entity a v))

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
            :else entities))))))

(defn- apply-tx
  "Applies a parsed transaction to the db value, returning a new db
  value. Each op becomes a fact tagged with tx-num and instant; the
  materialized view is rebuilt by folding the facts, and the flat log
  grows by the new facts. The tx counter advances by one."
  [db tx-num instant tx-data]
  (let [ops (parse-tx-data tx-data)
        facts (vec (for [[op e a v] ops]
                     (make-fact e a v tx-num instant op)))
        entities (reduce apply-fact (:entities db) facts)]
    {:entities entities
     :log (into (:log db) facts)
     :tx (inc tx-num)}))

;; ---------------------------------------------------------------------------
;; Public transaction API
;; ---------------------------------------------------------------------------

(defn transact
  "Transacts facts against the store connection. Atomic: all-or-nothing.
  tx-data accepts any of the parse-tx-data forms. Returns a map of the
  tx number just applied and the resulting db value under :db-after."
  [conn tx-data]
  (let [cur @conn
        tx-num (:tx cur)
        instant (store-clock* conn)
        new-db (apply-tx cur tx-num instant tx-data)]
    (store-commit* conn new-db)
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
  "Finds entity ids where attr equals value via a linear scan. Returns
  the single id when exactly one entity matches, a set of ids when
  several match, and nil when none do."
  [db-val attr value]
  (let [found (for [[e attrs] (:entities db-val)
                    :when (= (get attrs attr) value)]
                e)]
    (cond
      (empty? found) nil
      (= (count found) 1) (first found)
      :else (set found))))

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
  in tx order. The returned :tx is preserved from the input."
  [db-val point]
  (let [instant? (point-as-instant? point)
        facts-before (filter (fn [f]
                                (if instant?
                                  (< (:instant f) point)
                                  (< (:tx f) point)))
                              (:log db-val))
        ordered (sort-by :tx facts-before)
        entities (reduce apply-fact {} ordered)]
    {:entities entities :log (vec ordered) :tx (:tx db-val)}))

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
  by :instant. The tx counter is the max of the two."
  [db-a db-b]
  (let [all-facts (sort-by :instant (concat (:log db-a) (:log db-b)))
        entities (reduce apply-fact {} all-facts)]
    {:entities entities
     :log (vec all-facts)
     :tx (max (:tx db-a) (:tx db-b))}))

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
  facts while preserving the materialized view.

  With one arg, drops the entire log (only the current view survives).
  With a keep-spec map:
    {:keep-last N}     keep the last N facts
    {:keep-since T}    keep facts at or after instant T"
  ([conn]
   (let [cur @conn]
     (store-commit* conn {:entities (:entities cur)
                          :log []
                          :tx (:tx cur)})))
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
                          :tx (:tx cur)}))))

;; ---------------------------------------------------------------------------
;; Schema (reserved for future use)
;; ---------------------------------------------------------------------------

(defn schema
  "Returns the store's schema map. Empty in v1; schema validation is
  not yet implemented."
  [db-val]
  {})
