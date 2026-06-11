(ns tools.run-state)

;; Run/round state for skill-system orchestration runs (audit-code,
;; implement-change, fix-bug). One run = one directory under
;; .local/runs/<run-id>/ holding:
;;
;;   state.edn    {:kind :audit, :scope "src/gc", :branch "audit/gc",
;;                 :round 1, :status :running,
;;                 :rounds [{:round 0 :found-new true}]}
;;   findings/    reviewer output, one EDN file per dispatch
;;   proposals/   editor changelog proposals, one EDN file per branch
;;
;; Every transition is written to disk before it is reported, so a
;; killed session resumes from `status` with nothing lost.
;;
;; CLI:
;;   ./mino tools/run_state.clj init <run-dir> --kind audit [--scope S] [--branch B] [--force true]
;;   ./mino tools/run_state.clj status <run-dir>
;;   ./mino tools/run_state.clj advance <run-dir> --found-new true|false

(require '[clojure.string :as str])

(defn- state-path [run-dir] (str run-dir "/state.edn"))

(defn- write-state! [run-dir st]
  (spit (state-path run-dir) (pr-str st))
  st)

(defn status
  "Read the current run state from disk. Throws if the run was never
   initialized -- callers must not invent state."
  [run-dir]
  (let [p (state-path run-dir)]
    (when-not (file-exists? p)
      (throw (ex-info (str "run-state: no state.edn under " run-dir
                           " (run init first)")
                      {:run-dir run-dir})))
    (read-string (slurp p))))

(defn init!
  "Create the run directory layout and a fresh state.edn. Refuses to
   clobber an existing run unless :force is set."
  [run-dir {:keys [kind scope branch force]}]
  (when-not kind
    (throw (ex-info "run-state: init requires --kind" {:run-dir run-dir})))
  (when (and (file-exists? (state-path run-dir)) (not force))
    (throw (ex-info (str "run-state: " run-dir " already initialized"
                         " (pass --force true to restart)")
                    {:run-dir run-dir})))
  (mkdir-p (str run-dir "/findings"))
  (mkdir-p (str run-dir "/proposals"))
  (write-state! run-dir {:kind   (keyword kind)
                         :scope  scope
                         :branch branch
                         :round  0
                         :status :running
                         :rounds []}))

(defn advance!
  "Record the outcome of the just-finished round. :found-new true keeps
   the run :running; false marks it :done (rounds-until-dry)."
  [run-dir {:keys [found-new]}]
  (let [st (status run-dir)]
    (when (= :done (:status st))
      (throw (ex-info "run-state: run is already done" {:state st})))
    (write-state! run-dir
                  (-> st
                      (update :rounds conj {:round     (:round st)
                                            :found-new (boolean found-new)})
                      (update :round inc)
                      (assoc :status (if found-new :running :done))))))

;; ---- CLI ----

(defn- parse-opts [args]
  (loop [m {} a args]
    (if (empty? a)
      m
      (let [[k v & more] a]
        (when-not (and v (str/starts-with? (str k) "--"))
          (throw (ex-info (str "run-state: bad option " k) {:args args})))
        (recur (assoc m (keyword (subs (str k) 2)) v) more)))))

(defn- parse-bool [s]
  (cond (= s "true")  true
        (= s "false") false
        :else (throw (ex-info (str "run-state: expected true|false, got " s) {}))))

(defn -main [& args]
  (let [[cmd run-dir & rest-args] args
        opts (parse-opts rest-args)]
    (when-not run-dir
      (throw (ex-info "usage: run_state.clj init|status|advance <run-dir> [--opt v ...]" {})))
    (case cmd
      "init"    (let [st (init! run-dir (update opts :force #(when % (parse-bool %))))]
                  (println (str "run-state: initialized " run-dir
                                " kind=" (name (:kind st)))))
      "status"  (println (pr-str (status run-dir)))
      "advance" (let [st (advance! run-dir {:found-new (parse-bool (:found-new opts))})]
                  (println (str "run-state: round " (count (:rounds st))
                                " recorded, status=" (name (:status st)))))
      (throw (ex-info (str "run-state: unknown command " cmd) {:cmd cmd})))))

(when (str/ends-with? (str *file*) "tools/run_state.clj")
  (apply -main *command-line-args*))
