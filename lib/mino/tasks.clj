(ns mino.tasks)

;; Task runner: load tasks from mino.edn, resolve dependencies,
;; and execute task functions in topological order.

(defn load-tasks
  "Read mino.edn and return the :tasks map.
   Throws if the file is missing or :tasks is not a map."
  [path]
  (let [raw (read-string (slurp path))]
    (when-not (map? raw)
      (throw (ex-info (str "mino.edn must be a map, got: " (type raw))
                      {:path path})))
    (let [tasks (:tasks raw)]
      (when (nil? tasks)
        (throw (ex-info "mino.edn has no :tasks key" {:path path})))
      (when-not (map? tasks)
        (throw (ex-info (str ":tasks must be a map, got: " (type tasks))
                        {:path path})))
      tasks)))

(defn- resolve-order*
  "DFS helper. Returns [order visited] or throws on cycle."
  [task-key tasks visiting visited order]
  (cond
    (contains? visited task-key)
    [order visited]

    (contains? visiting task-key)
    (throw (ex-info (str "circular dependency: " (name task-key))
                    {:task task-key}))

    :else
    (let [spec (get tasks task-key)]
      (when (nil? spec)
        (throw (ex-info (str "unknown task: " (name task-key))
                        {:task task-key})))
      (let [visiting (conj visiting task-key)
            deps     (if (map? spec) (:deps spec) nil)
            result   (reduce (fn [[ord vis] dep]
                               (resolve-order* dep tasks visiting vis ord))
                             [order visited]
                             (or deps []))
            ord      (first result)
            vis      (second result)]
        [(conj ord task-key) (conj vis task-key)]))))

(defn resolve-order
  "Return a vector of task keys in dependency order, ending with target.
   Throws on cycles or missing tasks."
  [target tasks]
  (first (resolve-order* target tasks #{} #{} [])))

(defn- require-ns
  "Dynamically require a namespace by name string. Uses the function
   form of `require` so the namespace load happens with a clean top-
   level evaluation rather than re-entering `eval` from a fn body —
   the latter exposes the file's top-level forms to the caller's
   lexical bindings and trips a bc-compile-time fold on shared local
   names."
  [ns-name]
  (require (symbol ns-name)))

(defn- ensure-task-fn
  "Require the namespace of task-sym and verify it resolves."
  [task-sym]
  (let [ns-name (namespace task-sym)]
    (when ns-name
      (require-ns ns-name))
    (when (nil? (resolve task-sym))
      (throw (ex-info (str "cannot resolve task function: " task-sym)
                      {:symbol task-sym})))))

(defn- prepare-task
  "Resolve a single task entry: validate the symbol, require its
   namespace, and return [task-key task-sym task-fn]. Pulled out of
   run-task! and called via mapv before the run loop so all module
   loads happen up front rather than inside the bc-compiled doseq
   body — a load triggered from inside an active bc frame can
   interact badly with the compiler's const-pool layout when the
   loaded file's top-level forms close over locals named the same as
   the caller's, and pre-resolving avoids that interaction without
   any change to user-visible task semantics."
  [tasks task-key]
  (let [spec  (get tasks task-key)
        sym   (if (map? spec) (:task spec) spec)]
    (when (nil? sym)
      (throw (ex-info (str "task " (name task-key) " has no :task")
                      {:task task-key})))
    (ensure-task-fn sym)
    [task-key sym (deref (resolve sym))]))

(defn run-task!
  "Run a task and all its dependencies in topological order."
  [target tasks]
  (let [order    (resolve-order target tasks)
        prepared (mapv (fn [k] (prepare-task tasks k)) order)]
    (doseq [[task-key _ task-fn] prepared]
      (let [start (time-ms)]
        (println (str "--- " (name task-key) " ---"))
        (task-fn)
        (println (str "--- " (name task-key) " ("
                      (- (time-ms) start) "ms) ---"))))))

(defn list-tasks
  "Print available tasks with their :doc strings."
  [tasks]
  (println "Available tasks:")
  (doseq [[k v] tasks]
    (let [doc (if (map? v) (:doc v) nil)]
      (println (str "  " (name k)
                    (when doc (str "  " doc)))))))
