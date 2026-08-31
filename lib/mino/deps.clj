(ns mino.deps
  "Dependency management for mino projects. Reads mino.edn, fetches :git
  dependencies into .mino/deps/, and returns resolved source paths for
  the module resolver."
  (:require [clojure.edn :as edn]
            [clojure.string :as str]))

(def ^:private deps-dir ".mino/deps")

(defn- deps-fail
  "Throws a classified mino.deps diagnostic (ADR 37/38): :mino/kind names
  the error class, :mino/message the specific human string, :mino/data
  the detail."
  [kind code msg data]
  (throw {:mino/kind kind :mino/code code :mino/message msg :mino/data data}))

(defn load-manifest
  "Read and validate mino.edn. Returns the manifest map. Reads EDN data
  only (never the code reader). Unknown keys are ignored for forward
  compatibility."
  [path]
  (let [raw (edn/read-string (slurp path))]
    (when-not (map? raw)
      (deps-fail :deps/manifest "MDPM001"
                 (str "mino.edn must be a map, got: " (pr-str raw))
                 {:path path}))
    (when (contains? raw :paths)
      (when-not (vector? (:paths raw))
        (deps-fail :deps/manifest "MDPM002"
                   (str ":paths must be a vector of strings, got: "
                        (pr-str (:paths raw)))
                   {:paths (:paths raw)}))
      (doseq [p (:paths raw)]
        (when-not (string? p)
          (deps-fail :deps/manifest "MDPM003"
                     (str ":paths entry must be a string, got: " (pr-str p))
                     {:entry p}))))
    (when (contains? raw :deps)
      (when-not (map? (:deps raw))
        (deps-fail :deps/manifest "MDPM004"
                   (str ":deps must be a map, got: " (pr-str (:deps raw)))
                   {:deps (:deps raw)})))
    raw))

(defn- fetch-git
  "Clone a git repo and checkout the pinned rev. The url and rev are
  validated non-option-like by validate-dep-spec; the -- stops git
  reading the url as an option even so."
  [dep-name spec]
  (let [dest (str deps-dir "/" (name dep-name))]
    (when-not (directory? dest)
      (println (str "Fetching " (name dep-name) " from " (:git spec) "..."))
      (sh! "git" "clone" "--quiet" "--" (:git spec) dest))
    (sh! "git" "-C" dest "checkout" "--quiet" (:rev spec))
    dest))

(defn- validate-git-arg
  "Reject a :git/:rev value git would read as an option: it must be a
  non-empty string that does not begin with '-'."
  [dep-name k v]
  (when-not (and (string? v) (seq v) (not (str/starts-with? v "-")))
    (deps-fail :deps/spec "MDPS004"
               (str "dep " dep-name " " k " must be a string not beginning "
                    "with '-' (got: " (pr-str v) "); a leading '-' would be "
                    "read by git as an option")
               {:dep dep-name :key k :value v})))

(defn validate-dep-spec
  "Validate a single dependency spec. Returns nil on success; throws a
  :deps/spec diagnostic naming the offending spec when it is malformed.
  A :git url and :rev must be non-empty strings that cannot be read as
  git options."
  [dep-name spec]
  (when-not (map? spec)
    (deps-fail :deps/spec "MDPS001"
               (str "dep " dep-name " spec must be a map, got: " (pr-str spec))
               {:dep dep-name :spec spec}))
  (when-not (or (:path spec) (:git spec))
    (deps-fail :deps/spec "MDPS002"
               (str "dep " dep-name " must have :path or :git")
               {:dep dep-name :spec spec}))
  (when (:git spec)
    (when-not (:rev spec)
      (deps-fail :deps/spec "MDPS003"
                 (str "git dep " dep-name " must have :rev")
                 {:dep dep-name :spec spec}))
    (validate-git-arg dep-name :git (:git spec))
    (validate-git-arg dep-name :rev (:rev spec))))

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
