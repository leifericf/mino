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
;; is safe. Bullets in the Unreleased section are grouped by their
;; "Category: " prefix (the commit-form convention): categories in
;; first-seen order, uncategorized bullets last.
;;
;; cut-release! closes the cycle: it renames a non-empty
;; "## Unreleased" to "## <version> — <title>"; the next merge!
;; starts a fresh Unreleased above it.
;;
;; CLI: ./mino tools/merge_proposals.clj <run-dir> [--changelog <path>] [--dry-run true]
;;      ./mino tools/merge_proposals.clj cut-release --version vX.Y.Z --title "..." [--changelog <path>]

(require '[clojure.string :as str])

(defn validate-proposal
  "Throw if `p` does not match the proposal shape; nil if ok. Public:
   the skill-consistency test validates agent-stated examples with it."
  [p source]
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

(defn- bullet-category
  "\"- GC: x\" -> \"GC\"; nil when the bullet carries no category."
  [first-line]
  (let [text (subs first-line 2)
        i    (str/index-of text ": ")]
    (when (and i (pos? i)) (subs text 0 i))))

(defn- parse-bullet-blocks
  "Unreleased body lines -> vector of bullet blocks (each a vector of
   lines: the `- ` line plus its continuation lines)."
  [body-lines]
  (loop [blocks [] cur nil ls body-lines]
    (if (empty? ls)
      (if cur (conj blocks cur) blocks)
      (let [l (first ls)]
        (cond
          (str/starts-with? l "- ")
          (recur (if cur (conj blocks cur) blocks) [l] (rest ls))

          (and cur (not (str/blank? l)) (not (str/starts-with? l "## ")))
          (recur blocks (conj cur l) (rest ls))

          :else
          (recur (if cur (conj blocks cur) blocks) nil (rest ls)))))))

(defn- group-blocks
  "Order bullet blocks: categories in first-seen order (insertion
   order within a category), uncategorized blocks last."
  [blocks]
  (let [cat  #(bullet-category (first %))
        cats (distinct (keep cat blocks))]
    (into (vec (mapcat (fn [c] (filterv #(= c (cat %)) blocks)) cats))
          (filterv #(nil? (cat %)) blocks))))

(defn- insert-lines
  "Insert `- <line>` bullets under ## Unreleased (created right below
   the title when absent), regrouping the section's bullets by
   category. Pure text -> text."
  [text lines]
  (let [new-blocks (mapv (fn [l] [(str "- " l)]) lines)
        rows       (str/split-lines text)]
    (if (some #(= "## Unreleased" (str/trim %)) rows)
      (let [[before from-heading] (split-with #(not= "## Unreleased" (str/trim %)) rows)
            heading       (first from-heading)
            after-heading (rest from-heading)
            [body after]  (split-with #(not (str/starts-with? % "## ")) after-heading)
            blocks        (group-blocks (into (parse-bullet-blocks (vec body)) new-blocks))]
        (str/join "\n" (concat before [heading ""]
                               (mapcat identity blocks)
                               [""] after)))
      (let [[title & body] rows]
        (str/join "\n" (concat [title "" "## Unreleased" ""]
                               (mapcat identity (group-blocks new-blocks))
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
      (spit changelog (str (str/trim-newline (insert-lines text appended)) "\n"))
      (spit (merged-path run-dir)
            (pr-str {:merged-lines (vec (concat merged appended))})))
    {:appended appended :skipped skipped}))

(defn cut-release!
  "Rename a non-empty ## Unreleased to ## <version> — <title>. Returns
   {:version v :entries n}. Throws when there is nothing to release."
  [{:keys [changelog version title]}]
  (when (or (str/blank? (str version)) (str/blank? (str title)))
    (throw (ex-info "cut-release: :version and :title are required" {})))
  (let [rows (str/split-lines (slurp changelog))]
    (when-not (some #(= "## Unreleased" (str/trim %)) rows)
      (throw (ex-info "cut-release: no ## Unreleased section" {:changelog changelog})))
    (let [[before from-heading] (split-with #(not= "## Unreleased" (str/trim %)) rows)
          after-heading (rest from-heading)
          body          (take-while #(not (str/starts-with? % "## ")) after-heading)
          entries       (count (filterv #(str/starts-with? % "- ") body))]
      (when (zero? entries)
        (throw (ex-info "cut-release: ## Unreleased is empty" {:changelog changelog})))
      (spit changelog
            (str (str/join "\n" (concat before
                                        [(str "## " version " — " title)]
                                        after-heading))
                 "\n"))
      {:version version :entries entries})))

(defn- parse-cli-opts [args]
  (loop [m {} a args]
    (if (empty? a)
      m
      (let [[k v & more] a]
        (recur (assoc m (keyword (subs (str k) 2)) v) more)))))

(defn -main [& args]
  (if (= "cut-release" (first args))
    (let [opts (parse-cli-opts (rest args))
          r    (cut-release! {:changelog (get opts :changelog "CHANGELOG.md")
                              :version   (:version opts)
                              :title     (:title opts)})]
      (println (str "merge-proposals: cut " (:version r) " with "
                    (:entries r) " entries")))
    (let [[run-dir & rest-args] args]
      (when-not run-dir
        (throw (ex-info "usage: merge_proposals.clj <run-dir> [--changelog <path>] [--dry-run true] | cut-release --version vX.Y.Z --title \"...\"" {})))
      (let [opts (parse-cli-opts rest-args)
            r (merge! run-dir {:changelog (get opts :changelog "CHANGELOG.md")
                               :dry-run   (= "true" (:dry-run opts))})]
        (println (str "merge-proposals: " (count (:appended r)) " line(s) appended, "
                      (count (:skipped r)) " skipped"
                      (when (= "true" (:dry-run opts)) " (dry run)")))))))

(when (str/ends-with? (str *file*) "tools/merge_proposals.clj")
  (apply -main *command-line-args*))
