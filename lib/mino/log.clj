(ns mino.log
  "A logging facade, events as data-shaped lines on stderr.

  (require '[mino.log :as log])
  (log/info \"started\")                        ; one line on stderr
  (log/debug \"count=%d items=%d\" 3 9)         ; printf-style formatting
  (log/error ex \"fetch failed for %s\" url)    ; exception plus context

  One line per event, EDN-shaped, readable back with read-string:

  {:ts \"2026-08-25T06:24:28.653Z\" :level :info :ns \"user\"
   :msg \"started\"}

  :ts is ISO 8601 UTC from the time prims (ADR 21), :level the level
  keyword, :ns the namespace of the calling form (resolved at
  macroexpansion), :msg the message.
  Exception calls append :ex-message and, when the exception carries
  data, :ex-data, rendered readably. Newlines and tabs inside values
  print as their escape sequences, so an event is always one physical
  line.

  Levels rank trace < debug < info < warn < error. The dynamic *level*
  is consulted at log time; the default is :info, and an unknown
  threshold reads as :info. (log/with-level :debug body...) binds it
  for the body; plain binding works too. enabled? answers the gate for
  callers guarding expensive messages.

  Logging never throws: a format call that fails (unknown directive,
  wrong argument types, arity miss) logs the raw fmt as the message,
  and the first argument counts as an exception only when it is a
  caught diagnostic or ex-info map followed by more arguments. Output
  goes through *err*: the default :mino/stderr sink, or a string atom
  a caller bound *err* over (the io.c capture contract). The level
  macros expand to log* calls; log* is the fn under them.")

;; Functional core: the pure event formatting.

(def ^:private level-ord
  {:trace 0 :debug 1 :info 2 :warn 3 :error 4})

(defn- render-event
  "Pure: the event map -> its one-line EDN-shaped string, key order
  :ts :level :ns :msg then the exception fields. Values print
  readably; pr folds newlines and tabs into their escape sequences,
  so the line reads back with read-string into the same data."
  [m]
  (str "{:ts " (pr-str (:ts m))
       " :level " (pr-str (:level m))
       " :ns " (pr-str (:ns m))
       " :msg " (pr-str (:msg m))
       (when (contains? m :ex-message)
         (str " :ex-message " (pr-str (:ex-message m))))
       (when (some? (:ex-data m))
         (str " :ex-data " (pr-str (:ex-data m))))
       "}"))

(defn- ex?
  "True for the two exception shapes a catch or ex-info hands out: a
  diagnostic map (:mino/kind) or an ex-info map (:message)."
  [x]
  (and (map? x)
       (or (contains? x :mino/kind)
           (contains? x :message))))

(defn- format-msg
  "fmt with args applied. A format failure never escapes the logger:
  the raw fmt becomes the message."
  [fmt args]
  (try (apply format fmt args)
       (catch Throwable e fmt)))

;; Imperative shell: the level gate, the timestamp, the stderr write.

(def ^:dynamic *level*
  "Threshold consulted at log time: trace < debug < info < warn <
  error. Default :info."
  :info)

(defn enabled?
  "True when an event at level would be emitted under the current
  *level*. Unknown event levels rank as :error; an unknown threshold
  reads as :info."
  [level]
  (>= (get level-ord level 4)
      (get level-ord *level* 2)))

(defmacro with-level
  "Binds *level* around the body and returns the body's value."
  [level & body]
  `(binding [*level* ~level]
     ~@body))

(defn- write-line
  "One line onto the *err* route: binding *out* over the current *err*
  value sends the println through the io.c sink resolution, stderr by
  default and the caller's capture atom when *err* holds one."
  [line]
  (binding [*out* *err*]
    (println line)))

(defn log*
  "The fn under the level macros: level, the calling form's namespace
  as a string, and the arguments. The first argument is an exception
  carrier only when it is ex? shaped and more arguments follow; a
  single argument is always the message, a format string with
  arguments is format-ed with the raw fmt as the fallback."
  [level ns & args]
  (when (enabled? level)
    (let [lead (first args)
          ex (when (and (next args) (ex? lead)) lead)
          body (if ex (next args) args)
          event {:ts (format-time (now) :iso8601)
                 :level level
                 :ns ns
                 :msg (if (next body)
                        (format-msg (first body) (next body))
                        (first body))}]
      (write-line
       (render-event
        (if ex
          (assoc event
                 :ex-message (ex-message ex)
                 :ex-data (ex-data ex))
          event))))))

(defmacro trace
  "Logs msg at :trace, or formats fmt with args. Suppressed unless
  *level* is :trace."
  ([msg] `(log* :trace ~(str (ns-name *ns*)) ~msg))
  ([fmt & args] `(log* :trace ~(str (ns-name *ns*)) ~fmt ~@args)))

(defmacro debug
  "Logs msg at :debug, or formats fmt with args. Suppressed while
  *level* ranks above :debug."
  ([msg] `(log* :debug ~(str (ns-name *ns*)) ~msg))
  ([fmt & args] `(log* :debug ~(str (ns-name *ns*)) ~fmt ~@args)))

(defmacro info
  "Logs msg at :info, or formats fmt with args. Emitted at the
  default *level*."
  ([msg] `(log* :info ~(str (ns-name *ns*)) ~msg))
  ([fmt & args] `(log* :info ~(str (ns-name *ns*)) ~fmt ~@args)))

(defmacro warn
  "Logs msg at :warn, or formats fmt with args."
  ([msg] `(log* :warn ~(str (ns-name *ns*)) ~msg))
  ([fmt & args] `(log* :warn ~(str (ns-name *ns*)) ~fmt ~@args)))

(defmacro error
  "Logs msg at :error, or formats fmt with args. A leading exception
  (a caught diagnostic or ex-info map) followed by more arguments
  appends its :ex-message and :ex-data to the event."
  ([msg] `(log* :error ~(str (ns-name *ns*)) ~msg))
  ([fmt & args] `(log* :error ~(str (ns-name *ns*)) ~fmt ~@args)))
