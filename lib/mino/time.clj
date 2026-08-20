(ns mino.time
  "Time and date: epoch-ms instants in plain data, one parser, four
  formats, calendar arithmetic, and human-readable differences over
  the time prims (ADR 21).

  (require '[mino.time :as t])
  (t/now)                            ; epoch-ms right now
  (t/parse \"2026-08-20T10:00:00Z\")  ; {:epoch-ms ... :format :iso8601}
  (t/format (t/now) :rfc1123)        ; \"Thu, 20 Aug 2026 03:12:00 GMT\"
  (t/add (t/parse \"2026-01-31\") {:months 1}) ; clamps to Feb 28
  (t/human (t/parse \"2024-08-20\"))  ; \"1 year ago\" style

  The instant is an integer: epoch milliseconds since
  1970-01-01T00:00:00Z, the same value inst-ms produces. Broken-down
  time is a plain map {:year :month :day :hour :min :sec :ms :wday
  :offset-min} with 1-based months and :wday 0 = Sunday. Offsets are
  fixed minutes east of UTC carried in the map; there is no named
  zone database (that layer can be added later without breaking any
  map). The representable range is years 1..9999; anything outside
  throws :time/range. Parsing is strict: impossible dates, wrong day
  names, ambiguous named zones (EST), and trailing junk throw
  :time/parse.

  JSON responses keep ISO strings as strings; there is no automatic
  coercion to instants. HTTP Date and Last-Modified header strings
  parse with t/parse (:rfc1123 / :rfc2822 / ISO all accepted).

  The monotonic clock is nano-time; monotonic-ms re-exports it for
  timing code (the wall clock can jump). cpu-ms measures process
  CPU time.")

;;;; Clocks

(defn now
  "Wall clock right now as epoch-ms (an integer)."
  []
  (clojure.core/now))

(defn now-s
  "Wall clock right now as epoch seconds (an integer)."
  []
  (clojure.core/now-s))

(defn cpu-ms
  "Process CPU time in milliseconds (user plus kernel on Windows,
  clock() elsewhere). A work metric, not a clock."
  []
  (clojure.core/cpu-ms))

(defn monotonic-ms
  "Monotonic elapsed-time milliseconds. Runs on the nano-time prim;
  use this for timing code, not now (the wall clock can jump)."
  []
  (quot (nano-time) 1000000))

;;;; Instant coercion (the namespace seam)

(defn instant
  "Coerces to epoch-ms: an integer passes through, a parse result
  yields its :epoch-ms, a time map converts strictly. Most mino.time
  verbs accept anything this accepts."
  [t]
  (cond
    (int? t) t
    (and (map? t) (contains? t :epoch-ms)) (:epoch-ms t)
    (map? t) (time-map->epoch t)
    :else (throw (ex-info "t/instant: expected epoch-ms, a parse "
                          "result, or a time map"
                          {:got t}))))

;;;; Parsing and formatting

(defn parse
  "Parses a date or datetime string into {:epoch-ms :offset-min
  :format :date-only?}. ISO 8601 / RFC 3339 and the RFC 1123 / 2822
  comma form are accepted; see the parse-time prim docstring for the
  exact strictness. Throws :time/parse on malformed input. Every
  other mino.time verb accepts this map as an instant."
  [s]
  (clojure.core/parse-time s))

(defn format
  "Formats an instant (epoch-ms, parse result, or time map) as a
  string. fmt is :iso8601 (default, Z form, .SSS only when the
  milliseconds are nonzero), :iso8601-date, :rfc1123 (HTTP Date,
  always GMT), or :rfc2822. An optional offset-min renders the
  offset-capable forms at a fixed offset. No pattern strings:
  compose custom formats from the time map and str."
  ([t] (clojure.core/format-time (instant t)))
  ([t fmt] (clojure.core/format-time (instant t) fmt))
  ([t fmt offset-min]
   (clojure.core/format-time (instant t) fmt offset-min)))

;;;; Time maps

(defn epoch->time-map
  "Converts an instant (epoch-ms, parse result, or time map) to the
  plain time map, optionally rendered at a fixed offset-min. The map
  carries :offset-min so it round-trips."
  ([t] (clojure.core/epoch->time-map (instant t)))
  ([t offset-min] (clojure.core/epoch->time-map (instant t)
                                                offset-min)))

(defn time-map->epoch
  "Converts a time map back to epoch-ms. Strict: :year :month :day
  required, unknown keys and impossible dates throw :time/field."
  [m]
  (clojure.core/time-map->epoch m))

(defn today
  "Today's UTC date as a date-only time map (midnight, :ms 0)."
  []
  (epoch->time-map (* (quot (now) 86400000) 86400000)))

;;;; Arithmetic

(def ^:private add-units #{:ms :days :months})

(defn add
  "Adds a units map to an instant (epoch-ms, parse result, or time
  map), returning the same kind: integer inputs answer integers,
  parse results and time maps answer time maps (a parse result
  becomes its UTC time map). Units: :ms (raw milliseconds), :days
  (exact 86400000-ms days), :months (calendar months with day
  clamping; January 31 plus one month is February 28 or 29). Units
  apply in the order ms, days, months; months operate on the UTC
  civil date; a time map's :offset-min is preserved. Unknown units
  are an error naming them."
  [t units]
  (let [extra (filter #(not (contains? add-units %)) (keys units))]
    (when (seq extra)
      (throw (ex-info (str "t/add: unknown units " (pr-str (vec extra)))
                      {:units (vec extra)}))))
  (let [map-out (map? t)
        off (when (and map-out (contains? t :offset-min))
              (:offset-min t))
        e0 (instant t)
        e1 (+ e0 (or (:ms units) 0) (* (or (:days units) 0) 86400000))
        e2 (if (nil? (:months units)) e1
               (clojure.core/add-months e1 (:months units)))]
    (if map-out
      (epoch->time-map e2 (or off 0))
      e2)))

(defn diff
  "Calendar difference between two instants (each an epoch-ms, parse
  result, or time map) as {:months :days :ms}: whole months first
  (the largest n with (add-months a n) <= b), then whole days of
  the remainder, then leftover milliseconds. Every component's sign
  follows b - a."
  [a b]
  (let [ea (instant a)
        eb (instant b)
        flip (< eb ea)
        [lo hi] (if flip [eb ea] [ea eb])
        mo (clojure.core/months-between lo hi)
        after-mo (clojure.core/add-months lo mo)
        dd (clojure.core/days-between after-mo hi)
        after-d (+ after-mo (* dd 86400000))
        rem (- hi after-d)]
    (if flip
      {:months (- mo) :days (- dd) :ms (- rem)}
      {:months mo :days dd :ms rem})))

(defn human
  "The largest-unit phrase for the difference between an instant and
  now (or between a and b): \"3 days ago\", \"in 5 minutes\".
  Vocabulary pinned in ADR 21."
  ([t] (clojure.core/human-diff (instant t)))
  ([a b] (clojure.core/human-diff (instant a) (instant b))))

;;;; Calendar facts

(defn leap-year?
  "True for a Gregorian leap year."
  [y]
  (clojure.core/leap-year? y))

(defn days-in-month
  "Number of days in the month (1..12) of the year."
  [y m]
  (clojure.core/days-in-month y m))

(defn weekday
  "Day of the week of an instant (epoch-ms, parse result, or time
  map), 0..6, 0 = Sunday."
  [t]
  (if (map? t)
    (clojure.core/weekday (if (contains? t :epoch-ms)
                            (epoch->time-map (:epoch-ms t))
                            t))
    (clojure.core/weekday t)))

;;;; clojure.instant interop

(defn from-inst
  "Converts an inst value (a #inst literal or read-instant-date map)
  to epoch-ms. Strict: the date must be possible (February 30th
  rejects) and the offset fields are honored; nanoseconds truncate
  to milliseconds."
  [v]
  (when-not (inst? v)
    (throw (ex-info "t/from-inst: not an inst" {:got v})))
  (let [{:keys [years months days hours minutes seconds nanoseconds
                offset-sign offset-hours offset-minutes]} v
        off-min (* (or offset-sign 1)
                   (+ (* 60 (or offset-hours 0))
                      (or offset-minutes 0)))]
    (time-map->epoch {:year years :month months :day days
                      :hour (or hours 0) :min (or minutes 0)
                      :sec (min 59 (or seconds 0))
                      :ms (quot (or nanoseconds 0) 1000000)
                      :offset-min off-min})))

(defn to-inst
  "Converts an instant (epoch-ms, parse result, or time map) to an
  inst map carrying the :mino/instant marker, so pr-str prints a
  #inst literal the reader round-trips. Renders at UTC."
  [t]
  (let [m (epoch->time-map t)]
    (with-meta
      {:years (:year m) :months (:month m) :days (:day m)
       :hours (:hour m) :minutes (:min m) :seconds (:sec m)
       :nanoseconds (* 1000000 (:ms m))
       :offset-sign 1 :offset-hours 0 :offset-minutes 0}
      {:mino/instant true})))

(comment
  (now)
  (parse "Sun, 06 Nov 1994 08:49:37 GMT")
  (format 0 :rfc1123)
  (add 0 {:days 3 :months 1})
  (human (- (now) (* 3 86400000)))
  (from-inst #inst "2026-05-21T00:00:00Z")
  (to-inst 0)
  )
