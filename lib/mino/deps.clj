(ns mino.deps)

;; Dependency management for mino projects.
;;
;; Reads mino.edn, fetches :git dependencies into .mino/deps/,
;; and returns resolved paths for the module resolver.

(def ^:private deps-dir ".mino/deps")

(defn load-manifest
  "Read and validate mino.edn. Returns the manifest map.
   Unknown keys are ignored for forward compatibility."
  [path]
  (let [raw (read-string (slurp path))]
    (when-not (map? raw)
      (throw (ex-info (str "mino.edn must be a map, got: " (type raw))
                      {:path path})))
    (when (contains? raw :paths)
      (when-not (vector? (:paths raw))
        (throw (ex-info ":paths must be a vector of strings"
                        {:paths (:paths raw)})))
      (doseq [p (:paths raw)]
        (when-not (string? p)
          (throw (ex-info (str ":paths entry must be a string, got: " (type p))
                          {:entry p})))))
    (when (contains? raw :deps)
      (when-not (map? (:deps raw))
        (throw (ex-info ":deps must be a map"
                        {:deps (:deps raw)}))))
    raw))

(defn- fetch-git
  "Clone a git repo and checkout the pinned rev."
  [dep-name spec]
  (let [dest (str deps-dir "/" (name dep-name))]
    (when-not (directory? dest)
      (println (str "Fetching " (name dep-name) " from " (:git spec) "..."))
      (sh! "git" "clone" "--quiet" (:git spec) dest))
    (sh! "git" "-C" dest "checkout" "--quiet" (:rev spec))
    dest))

(defn validate-dep-spec
  "Validate a single dependency spec. Returns nil on success; throws
  ex-info with the offending spec when the spec is malformed."
  [dep-name spec]
  (when-not (map? spec)
    (throw (ex-info (str "dep spec for " dep-name " must be a map")
                    {:dep dep-name :spec spec})))
  (when-not (or (:path spec) (:git spec))
    (throw (ex-info (str "dep " dep-name " must have :path or :git")
                    {:dep dep-name :spec spec})))
  (when (and (:git spec) (not (:rev spec)))
    (throw (ex-info (str "git dep " dep-name " must have :rev")
                    {:dep dep-name :spec spec}))))

(defn fetch-dep
  "Fetch a single dependency. Returns the resolved directory path."
  [dep-name spec]
  (validate-dep-spec dep-name spec)
  (cond
    (:path spec) (:path spec)
    (:git spec)  (fetch-git dep-name spec)))

(defn fetch-all!
  "Fetch all dependencies from a manifest. Creates .mino/deps/ as needed."
  [manifest]
  (when-let [deps (:deps manifest)]
    (mkdir-p deps-dir)
    (doseq [[dep-name spec] deps]
      (fetch-dep dep-name spec)))
  (println "Dependencies up to date."))

(defn- detect-roots
  "Pick source roots that actually exist on disk for a fetched dep.
   Many pure-Clojure libraries follow the Maven layout
   (src/main/clojure/...); others put sources directly under src/.
   Returning both when both exist keeps require resolution working
   for sibling namespaces inside a multi-file library."
  [base]
  (let [candidates ["src" "src/main/clojure" "src/main/cljc"
                    "src/main/cljs"]
        existing   (filterv #(directory? (str base "/" %)) candidates)]
    (if (seq existing) existing ["src"])))

(defn- dep-source-paths
  "Return the source directories for a single dependency.
   :deps/root in the spec wins; otherwise we probe common conventions
   and use whichever roots exist under the fetched directory."
  [dep-name spec]
  (cond
    (:path spec)
    [(:path spec)]

    (:git spec)
    (let [base (str deps-dir "/" (name dep-name))
          roots (or (:deps/root spec) (detect-roots base))]
      (mapv #(str base "/" %) roots))))

(defn resolve-paths
  "Return a vector of all source directories: :paths + dep directories."
  [manifest]
  (let [paths (or (:paths manifest) ["src" "lib"])
        dep-dirs (when-let [deps (:deps manifest)]
                   (into [] (mapcat (fn [[dep-name spec]]
                                      (dep-source-paths dep-name spec))
                                    deps)))]
    (into paths dep-dirs)))
