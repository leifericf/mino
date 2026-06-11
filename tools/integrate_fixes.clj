(ns tools.integrate-fixes)

;; Land worktree fix branches on the feature branch, in the order the
;; caller gives (dependency order: tests before implementation, owners
;; before dependents).
;;
;; Each branch is merged with --no-ff so the landing history shows one
;; merge per unit of work. A branch whose merge conflicts is NEVER
;; resolved by guessing: the merge is aborted, the conflict is recorded
;; in <run-dir>/escalations.edn (a vector, appended across calls), and
;; integration continues with the remaining branches -- module-disjoint
;; branches must not be held hostage by one conflict. Escalations go to
;; a fresh editor with both diffs, or to the maintainer.
;;
;;   escalation: {:branch "fix-conflict"
;;                :target "feature"
;;                :conflict-files ["src/gc/driver.c"]
;;                :status :unresolved}
;;
;; CLI: ./mino tools/integrate_fixes.clj <run-dir> --repo <dir> --target <branch> --branches a,b,c

(require '[clojure.string :as str])

(defn- git [repo & args]
  (apply sh "git" "-C" repo args))

(defn- git-ok! [repo & args]
  (let [r (apply git repo args)]
    (when-not (= 0 (:exit r))
      (throw (ex-info (str "integrate: git " (str/join " " args)
                           " failed: " (str/trim (str (:err r))))
                      {:repo repo :args args :exit (:exit r)})))
    r))

(defn- escalations-path [run-dir] (str run-dir "/escalations.edn"))

(defn- read-escalations [run-dir]
  (if (file-exists? (escalations-path run-dir))
    (read-string (slurp (escalations-path run-dir)))
    []))

(defn- record-escalation! [run-dir esc]
  (spit (escalations-path run-dir)
        (pr-str (conj (read-escalations run-dir) esc))))

(defn- conflict-files [repo]
  (vec (sort (filterv #(not (str/blank? %))
                      (str/split-lines
                        (str (:out (git repo "diff" "--name-only" "--diff-filter=U"))))))))

(defn- branch-exists? [repo branch]
  (= 0 (:exit (git repo "rev-parse" "--verify" "-q" (str "refs/heads/" branch)))))

(defn integrate!
  "Merge `branches` (in order) into `target` inside `repo`. Returns
   {:merged [...] :escalated [escalation...]}. Conflicting branches are
   aborted and recorded in <run-dir>/escalations.edn; the rest land."
  [run-dir {:keys [repo target branches]}]
  (when-not (and repo target (seq branches))
    (throw (ex-info "integrate: :repo, :target, and :branches are required" {})))
  (doseq [b (cons target branches)]
    (when-not (branch-exists? repo b)
      (throw (ex-info (str "integrate: branch does not exist: " b)
                      {:repo repo :branch b}))))
  (mkdir-p run-dir)
  (git-ok! repo "checkout" "-q" target)
  (reduce
    (fn [acc branch]
      (let [r (git repo "merge" "--no-ff" "-q"
                   "-m" (str "Integrate " branch) branch)]
        (if (= 0 (:exit r))
          (update acc :merged conj branch)
          (let [files (conflict-files repo)
                esc   {:branch branch
                       :target target
                       :conflict-files files
                       :status :unresolved}]
            (git-ok! repo "merge" "--abort")
            (record-escalation! run-dir esc)
            (update acc :escalated conj esc)))))
    {:merged [] :escalated []}
    branches))

(defn -main [& args]
  (let [[run-dir & rest-args] args
        opts (loop [m {} a rest-args]
               (if (empty? a)
                 m
                 (let [[k v & more] a]
                   (recur (assoc m (keyword (subs (str k) 2)) v) more))))]
    (when-not (and run-dir (:repo opts) (:target opts) (:branches opts))
      (throw (ex-info "usage: integrate_fixes.clj <run-dir> --repo <dir> --target <branch> --branches a,b,c" {})))
    (let [r (integrate! run-dir {:repo     (:repo opts)
                                 :target   (:target opts)
                                 :branches (str/split (:branches opts) #",")})]
      (println (str "integrate: " (count (:merged r)) " branch(es) merged into "
                    (:target opts) ", " (count (:escalated r)) " escalated"
                    (when (seq (:escalated r))
                      (str " -> " run-dir "/escalations.edn")))))))

(when (str/ends-with? (str *file*) "tools/integrate_fixes.clj")
  (apply -main *command-line-args*))
