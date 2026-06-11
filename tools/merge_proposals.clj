(ns tools.merge-proposals)

;; Changelog-via-proposal. Parallel editor agents never edit
;; CHANGELOG.md (a guaranteed merge conflict); each lands its branch
;; and writes a proposal EDN instead:
;;
;;   <run-dir>/proposals/<branch-slug>.edn
;;   {:branch    "fix/gc-driver-001"
;;    :changelog ["GC: Fix sweep ordering in driver tick"]
;;    :commits   ["abc123"]}          ; optional
;;
;; merge! folds the proposals in filename order and appends their
;; changelog lines exactly once under "## Unreleased" (created right
;; below the "# Changelog" title when missing). A line is skipped if
;; it already appears in the changelog or was merged earlier in this
;; run (recorded in <run-dir>/merged.edn) -- re-running after a crash
;; is safe.
;;
;; CLI: ./mino tools/merge_proposals.clj <run-dir> [--changelog <path>] [--dry-run true]

(require '[clojure.string :as str])

(defn- validate-proposal [p source]
  (let [problem
        (cond
          (not (map? p))                       "proposal is not a map"
          (not (string? (:branch p)))          ":branch must be a string"
          (not (vector? (:changelog p)))       ":changelog must be a vector of strings"
          (not (every? string? (:changelog p))) ":changelog entries must be strings"
          :else nil)]
    (when problem
      (throw (ex-info (str "merge-proposals: invalid proposal in " source ": " problem)
                      {:source source :proposal p})))))

(defn- proposal-files [run-dir]
  (sort (filterv #(str/ends-with? % ".edn")
                 (file-seq (str run-dir "/proposals")))))

(defn- read-proposals [run-dir]
  (mapv (fn [path]
          (let [p (read-string (slurp path))]
            (validate-proposal p path)
            p))
        (proposal-files run-dir)))

(defn- merged-path [run-dir] (str run-dir "/merged.edn"))

(defn- read-merged [run-dir]
  (if (file-exists? (merged-path run-dir))
    (set (:merged-lines (read-string (slurp (merged-path run-dir)))))
    #{}))

(defn- insert-lines
  "Insert `- <line>` bullets under ## Unreleased, creating the section
   under the # Changelog title when absent. Pure text -> text."
  [text lines]
  (let [bullets (mapv #(str "- " %) lines)
        rows    (str/split-lines text)
        has-unreleased? (some #(= "## Unreleased" (str/trim %)) rows)]
    (if has-unreleased?
      ;; Append at the end of the Unreleased section (just before the
      ;; next "## " heading after it).
      (loop [out [] in rows seen-unreleased? false inserted? false]
        (if (empty? in)
          (str/join "\n" (if inserted? out (into out bullets)))
          (let [row (first in)
                next-section? (and seen-unreleased?
                                   (not inserted?)
                                   (str/starts-with? row "## "))]
            (if next-section?
              (recur (into out (conj bullets "")) in seen-unreleased? true)
              (recur (conj out row)
                     (rest in)
                     (or seen-unreleased? (= "## Unreleased" (str/trim row)))
                     inserted?)))))
      ;; Create the section right below the title line.
      (let [[title & body] rows]
        (str/join "\n"
                  (concat [title "" "## Unreleased" ""]
                          bullets
                          body))))))

(defn merge!
  "Fold <run-dir>/proposals/*.edn into the changelog. Returns
   {:appended [...] :skipped [...]}. :dry-run reports without writing."
  [run-dir {:keys [changelog dry-run]}]
  (when-not changelog
    (throw (ex-info "merge-proposals: :changelog path is required" {})))
  (let [proposals (read-proposals run-dir)
        candidate (vec (mapcat :changelog proposals))
        text      (slurp changelog)
        existing  (set (mapv #(str/trim (subs % 2))
                             (filterv #(str/starts-with? % "- ")
                                      (str/split-lines text))))
        merged    (read-merged run-dir)
        {:keys [appended skipped]}
        (reduce (fn [{:keys [appended skipped seen] :as acc} line]
                  (if (or (existing line) (merged line) (seen line))
                    (-> acc (update :skipped conj line))
                    (-> acc
                        (update :appended conj line)
                        (update :seen conj line))))
                {:appended [] :skipped [] :seen #{}}
                candidate)]
    (when (and (seq appended) (not dry-run))
      (spit changelog (insert-lines text appended))
      (spit (merged-path run-dir)
            (pr-str {:merged-lines (vec (concat merged appended))})))
    {:appended appended :skipped skipped}))

(defn -main [& args]
  (let [[run-dir & rest-args] args]
    (when-not run-dir
      (throw (ex-info "usage: merge_proposals.clj <run-dir> [--changelog <path>] [--dry-run true]" {})))
    (let [opts (loop [m {} a rest-args]
                 (if (empty? a)
                   m
                   (let [[k v & more] a]
                     (recur (assoc m (keyword (subs (str k) 2)) v) more))))
          r (merge! run-dir {:changelog (get opts :changelog "CHANGELOG.md")
                             :dry-run   (= "true" (:dry-run opts))})]
      (println (str "merge-proposals: " (count (:appended r)) " line(s) appended, "
                    (count (:skipped r)) " skipped"
                    (when (= "true" (:dry-run opts)) " (dry run)"))))))

(when (str/ends-with? (str *file*) "tools/merge_proposals.clj")
  (apply -main *command-line-args*))
