(ns tools.flatten-branch)

;; Flatten the feature branch once the run is dry: rebase it onto the
;; base branch so the landing history is linear -- the integration
;; merge commits are working-state scaffolding, not history the
;; maintainer keeps.
;;
;; Safety contract: the flattened tip's tree must be bit-identical to
;; the pre-flatten tip's tree. Verified lane results carry over only
;; because nothing changed; on any mismatch (or a rebase conflict) the
;; branch is restored to the pre-flatten tip and the tool throws --
;; flattening never alters content and never guesses.
;;
;; Cleanup: local `unit/*` and `fix/*` branches whose tips are
;; ancestors of the pre-flatten tip are fully integrated and are
;; deleted. The pre-flatten tip is recorded in <run-dir>/flatten.edn
;; (and the reflog) for recovery.
;;
;; CLI: ./mino tools/flatten_branch.clj <run-dir> --repo <dir> --target <branch> [--onto main]

(require '[clojure.string :as str])

(defn- git [repo & args]
  (apply sh "git" "-C" repo args))

(defn- git-ok! [repo & args]
  (let [r (apply git repo args)]
    (when-not (= 0 (:exit r))
      (throw (ex-info (str "flatten: git " (str/join " " args)
                           " failed: " (str/trim (str (:err r))))
                      {:repo repo :args args :exit (:exit r)})))
    r))

(defn- rev [repo spec]
  (str/trim (str (:out (git-ok! repo "rev-parse" spec)))))

(defn- branch-exists? [repo branch]
  (= 0 (:exit (git repo "rev-parse" "--verify" "-q" (str "refs/heads/" branch)))))

(defn- ancestor? [repo a b]
  (= 0 (:exit (git repo "merge-base" "--is-ancestor" a b))))

(defn- integrated-branches
  "Local unit/* and fix/* branches whose tips are ancestors of `tip`."
  [repo tip]
  (->> (str/split-lines
         (str (:out (git-ok! repo "for-each-ref" "--format=%(refname:short)"
                             "refs/heads/unit" "refs/heads/fix"))))
       (filterv #(and (not (str/blank? %)) (ancestor? repo % tip)))))

(defn flatten!
  "Rebase `target` onto `onto` inside `repo`, verify the tree is
   unchanged, and delete integrated unit/fix branches. Returns
   {:old-tip ... :new-tip ... :merges n :deleted [...]}. Restores the
   pre-flatten tip and throws on conflict or tree mismatch."
  [run-dir {:keys [repo target onto]}]
  (doseq [b [target onto]]
    (when-not (branch-exists? repo b)
      (throw (ex-info (str "flatten: branch does not exist: " b)
                      {:repo repo :branch b}))))
  (mkdir-p run-dir)
  (git-ok! repo "checkout" "-q" target)
  (let [old-tip  (rev repo target)
        old-tree (rev repo (str target "^{tree}"))
        merges   (count (filterv #(not (str/blank? %))
                                 (str/split-lines
                                   (str (:out (git-ok! repo "rev-list" "--merges"
                                                       (str onto ".." target)))))))]
    (when (pos? merges)
      (let [r (git repo "rebase" "-q" onto)]
        (when-not (= 0 (:exit r))
          (git repo "rebase" "--abort")
          (git-ok! repo "reset" "--hard" "-q" old-tip)
          (throw (ex-info (str "flatten: rebase conflicted; " target
                               " restored to " old-tip " -- escalate to the maintainer")
                          {:repo repo :target target :old-tip old-tip}))))
      (when-not (= old-tree (rev repo (str target "^{tree}")))
        (git-ok! repo "reset" "--hard" "-q" old-tip)
        (throw (ex-info (str "flatten: tree mismatch after rebase; " target
                             " restored to " old-tip)
                        {:repo repo :target target :old-tip old-tip}))))
    (let [deleted (integrated-branches repo old-tip)]
      (doseq [b deleted]
        (git-ok! repo "branch" "-D" b))
      (let [result {:target target :onto onto :old-tip old-tip
                    :new-tip (rev repo target) :merges merges :deleted deleted}]
        (spit (str run-dir "/flatten.edn") (pr-str result))
        result))))

(defn -main [& args]
  (let [[run-dir & rest-args] args
        opts (loop [m {} a rest-args]
               (if (empty? a)
                 m
                 (let [[k v & more] a]
                   (recur (assoc m (keyword (subs (str k) 2)) v) more))))]
    (when-not (and run-dir (:repo opts) (:target opts))
      (throw (ex-info "usage: flatten_branch.clj <run-dir> --repo <dir> --target <branch> [--onto main]" {})))
    (let [r (flatten! run-dir {:repo   (:repo opts)
                               :target (:target opts)
                               :onto   (or (:onto opts) "main")})]
      (println (str "flatten: " (:target r)
                    (if (pos? (:merges r))
                      (str " rebased onto " (:onto r) " (" (:merges r)
                           " merge(s) flattened, tree identical, old tip " (subs (:old-tip r) 0 10) ")")
                      " already flat")
                    (when (seq (:deleted r))
                      (str "; deleted " (str/join ", " (:deleted r)))))))))

(when (str/ends-with? (str *file*) "tools/flatten_branch.clj")
  (apply -main *command-line-args*))
