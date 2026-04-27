;; clojure.stacktrace - exception printers.
;;
;; mino represents an exception as the structured-diagnostic map you get
;; from `mino_last_error_map`, which the REPL captures into `*e`. The
;; printers below format that map: kind, code, message, location, and
;; recursively unwrap the `:mino/cause` chain. They expose the standard
;; clojure.stacktrace surface (`print-stack-trace`, `print-cause-trace`,
;; `print-throwable`, `root-cause`) so code that already walks an
;; exception via these names ports without changes.

(ns clojure.stacktrace)

(defn- diag? [e]
  (and (map? e) (contains? e :mino/kind)))

(defn- print-location [loc]
  (when (map? loc)
    (let [file   (:file loc)
          line   (:line loc)
          column (:column loc)]
      (when (and file line)
        (println (str "  at " file ":" line
                      (when column (str ":" column))))))))

(defn- print-trace-element [tr]
  (cond
    (vector? tr) (println " " (clojure.string/join " " (map str tr)))
    (string? tr) (println " " tr)
    :else        (println " " (pr-str tr))))

(defn print-throwable
  "Prints the exception's kind/code summary line, operating on mino's
  diagnostic-map representation of an exception."
  [e]
  (when (diag? e)
    (let [kind    (:mino/kind e)
          code    (:mino/code e)
          message (:mino/message e)]
      (println (str kind
                    (when code (str " [" code "]"))
                    (when message (str ": " message)))))))

(defn print-stack-trace
  "Prints the exception summary, location, and any captured trace
  entries. Does not unwrap the cause chain - use `print-cause-trace`
  for that."
  ([e]
   (print-throwable e)
   (when (diag? e)
     (print-location (:mino/location e))
     (doseq [tr (:mino/trace e)]
       (print-trace-element tr))))
  ([e _depth]
   (print-stack-trace e)))

(defn root-cause
  "Walks `:mino/cause` to the deepest non-nil entry."
  [e]
  (if (and (diag? e) (diag? (:mino/cause e)))
    (recur (:mino/cause e))
    e))

(defn print-cause-trace
  "Prints `print-stack-trace` for `e` and each entry of its cause chain."
  ([e]
   (print-stack-trace e)
   (when-let [c (and (diag? e) (:mino/cause e))]
     (println "Caused by:")
     (print-cause-trace c)))
  ([e _depth]
   (print-cause-trace e)))
