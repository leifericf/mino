;; clojure.repl - interactive helpers for the REPL.
;;
;; The data accessors (`doc-string`, `source-form`, `apropos`) are interned
;; into this namespace as C primitives. The user-facing `doc` and `source`
;; macros, plus `dir`, `find-doc`, and `pst`, layer print formatting on
;; top so REPL ergonomics match the canonical surface.

(ns clojure.repl)

(defmacro doc
  "Prints documentation for the named var. Takes an unquoted symbol
  naming a var, prints its documentation, and returns nil."
  [name]
  `(let [s# (clojure.repl/doc-string (quote ~name))]
     (when s# (println s#))
     nil))

(defmacro source
  "Prints the source form of the named var."
  [name]
  `(let [s# (clojure.repl/source-form (quote ~name))]
     (when s# (prn s#))
     nil))

(defn find-doc
  "Prints documentation for any var whose documentation or name contains
  a match for `re-string-or-pattern`, where the pattern is a regex or a
  plain substring (substring matches case-sensitively)."
  [re-string-or-pattern]
  (let [matches (clojure.repl/apropos "")
        match?  (if (string? re-string-or-pattern)
                  (fn [s] (clojure.string/includes? s re-string-or-pattern))
                  (fn [s] (boolean (re-find re-string-or-pattern s))))]
    (doseq [sym matches]
      (let [d (clojure.repl/doc-string sym)]
        (when (and d (or (match? (str sym)) (match? (str d))))
          (println sym)
          (println " " d)
          (println))))
    nil))

(defn dir-fn
  "Returns a sorted seq of symbols for public names in the namespace named
  by `ns-sym`. Helper for the `dir` macro."
  [ns-sym]
  (sort (keys (ns-publics ns-sym))))

(defmacro dir
  "Prints a sorted list of public names in the given namespace symbol."
  [ns-sym]
  `(do
     (doseq [sym# (clojure.repl/dir-fn (quote ~ns-sym))]
       (println sym#))
     nil))

(defn pst
  "Prints the most recent exception (`*e`) as a formatted summary. With
  no argument, prints `*e`; with an explicit map argument, prints that
  map. Returns nil."
  ([]
   (pst *e))
  ([e]
   (when (map? e)
     (let [kind     (:mino/kind e)
           code     (:mino/code e)
           msg      (:mino/message e)
           loc      (:mino/location e)
           file     (:file loc)
           line     (:line loc)
           col      (:column loc)
           cause    (:mino/cause e)]
       (println (str kind " "
                     (when code (str "[" code "] "))
                     (or msg "")))
       (when (and file line)
         (println (str "  at " file ":" line
                       (when col (str ":" col)))))
       (when cause
         (println "  caused by:")
         (pst cause))))
   nil))
