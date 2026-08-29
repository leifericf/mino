(ns mino.time
  "Time and date: epoch-ms instants in plain data, one parser, four
  formats, calendar arithmetic, and human-readable differences over
  the time prims (ADR 21).

  (require '[mino.time :as t])
  (t/now)                            ; epoch-ms right now
  (t/parse \"2026-08-20T10:00:00Z\")  ; {:epoch-ms ... :format :iso8601}
  (t/format (t/now) :rfc1123)        ; \"Thu, 20 Aug 2026 03:12:00 GMT\"
  (t/add (t/parse \"2026-01-31\") {:months 1}) ; clamps to Feb 28
  (t/human (t/parse \"2026-08-20\") (t/parse \"2026-08-23\")) ; \"3 days ago\"

  The instant is an integer: epoch milliseconds since
  1970-01-01T00:00:00Z, the same value inst-ms produces. Broken-down
  time is a plain map {:year :month :day :hour :min :sec :ms :wday
  :offset-min} with 1-based months and :wday 0 = Sunday. Offsets are
  fixed minutes east of UTC carried in the map. Named IANA zones
  (ADR 27) are an additive layer over the same epoch-ms: {:zone z}
  options on parse/format and the converters, plus in-zone and
  zone-offset-mins, resolve a zone to its offset at an instant; the
  wall clock changes, the instant never does. The representable
  range is years 1..9999; anything outside
  throws :time/range. Parsing is strict: impossible dates, wrong day
  names, ambiguous named zones (EST), and trailing junk throw
  :time/parse.

  JSON responses keep ISO strings as strings; there is no automatic
  coercion to instants. HTTP Date and Last-Modified header strings
  parse with t/parse (:rfc1123 / :rfc2822 / ISO all accepted).

  The monotonic clock is nano-time; monotonic-ms re-exports it for
  timing code (the wall clock can jump). cpu-ms measures process
  CPU time.")

;;;; Errors

(defn- time-fail
  "Throws a classified mino.time diagnostic (ADR 37): :mino/kind names
  the error class so classed catch dispatches on it, :mino/message the
  human string ex-message returns, :mino/data the detail ex-data reads."
  [kind code msg data]
  (throw {:mino/kind kind :mino/code code :mino/message msg
          :mino/data data}))

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
  yields its :epoch-ms, a time map converts strictly. Every verb
  that takes an instant accepts anything this accepts."
  [t]
  (cond
    (int? t) t
    (and (map? t) (contains? t :epoch-ms)) (:epoch-ms t)
    (map? t) (time-map->epoch t)
    :else (time-fail :eval/type "MTY001"
                     (str "mino.time/instant: expected epoch-ms, a parse "
                          "result, or a time map")
                     {:got t})))

;;;; Parsing and formatting

(defn parse
  "Parses a date or datetime string into {:epoch-ms :offset-min
  :format :date-only?}. ISO 8601 / RFC 3339 and the RFC 1123 / 2822
  comma form are accepted; see the parse-time prim docstring for the
  exact strictness. Throws :time/parse on malformed input. Every
  verb that takes an instant accepts this map. opts: {:zone z}
  interprets an offset-less input as local wall time in the zone
  (fold-0: overlaps take the first occurrence, gaps shift forward)."
  ([s] (clojure.core/parse-time s))
  ([s opts]
   (when-not (map? opts)
     (time-fail :eval/type "MTY001"
                "mino.time/parse: opts must be a map" {:arg opts}))
   (clojure.core/parse-time s opts)))

(defn format
  "Formats an instant (epoch-ms, parse result, or time map) as a
  string. fmt is :iso8601 (default, Z form, .SSS only when the
  milliseconds are nonzero), :iso8601-date, :rfc1123 (HTTP Date,
  always GMT), or :rfc2822. An optional offset-min renders the
  offset-capable forms at a fixed offset; an options map
  {:zone z} (in the fmt or offset position) renders them at the
  zone's offset at the instant. No pattern strings: compose custom
  formats from the time map and str."
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
  "Today's date as a date-only time map (midnight, :ms 0). The
  0-arity gives the UTC date. The zone arity gives the civil date on
  the zone's wall clock right now: the zone is an IANA name (string or
  keyword, e.g. \"Europe/Oslo\") or a fixed offset in minutes, and the
  result carries the zone's resolved :offset-min. An unknown zone
  throws :time/zone. Either way the time fields are zeroed."
  ([]
   (epoch->time-map (* (quot (now) 86400000) 86400000)))
  ([zone]
   (let [m (in-zone zone (now))]
     (assoc m :hour 0 :min 0 :sec 0 :ms 0))))

;;;; Arithmetic

(def ^:private add-units #{:ms :days :months})

(defn add
  "Adds a units map to an instant (epoch-ms, parse result, or time
  map), returning the same kind: integer inputs answer integers,
  parse results and time maps answer time maps rendered at the
  input's own :offset-min (UTC when absent). Units: :ms (raw
  milliseconds), :days (exact 86400000-ms days), :months (calendar
  months with day clamping; January 31 plus one month is February
  28 or 29). Units apply in the order ms, days, months. For map
  inputs the months shift the map's own (offset-local) fields, so
  the clamp holds on the wall clock; for integer inputs they
  operate on the UTC civil date. Unknown units are an error naming
  them."
  [t units]
  (let [extra (filter #(not (contains? add-units %)) (keys units))]
    (when (seq extra)
      (time-fail :time/field "MTF001"
                 (str "mino.time/add: unknown units " (pr-str (vec extra)))
                 {:units (vec extra)})))
  (let [map-out (map? t)
        e0 (instant t)
        e1 (+ e0 (or (:ms units) 0) (* (or (:days units) 0) 86400000))]
    (if map-out
      (let [off (or (clojure.core/get t :offset-min) 0)
            m1 (epoch->time-map e1 off)]
        (if (nil? (:months units))
          m1
          (clojure.core/add-months m1 (:months units))))
      (if (nil? (:months units))
        e1
        (clojure.core/add-months e1 (:months units))))))

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
  "Day of the week, 0..6, 0 = Sunday. For an epoch-ms integer, the
  UTC weekday; for a time map or parse result, the weekday of the
  value's own (offset-local) date, agreeing with the map's :wday
  field."
  [t]
  (if (map? t)
    (let [m (if (contains? t :epoch-ms)
              (epoch->time-map (:epoch-ms t)
                               (or (:offset-min t) 0))
              t)]
      (clojure.core/weekday m))
    (clojure.core/weekday t)))

;;;; Named zones (ADR 27)

(defn- tz-zone-checked
  "Runs (zone-fn) and rethrows :time/zone errors with the zone in
  the diagnostic data, so facade callers see which name missed."
  [zone zone-fn]
  (try
    (zone-fn)
    (catch e
      (if (= :time/zone (:mino/kind e))
        (time-fail :time/zone "MTZ001"
                   (str "mino.time: unknown time zone " (pr-str zone))
                   {:zone zone})
        (throw e)))))

(defn in-zone
  "Renders an instant (epoch-ms, parse result, or time map) as the
  plain time map on the zone's wall clock: the offset the zone holds
  at that instant lands in :offset-min and the civil fields shift to
  match. The instant itself is unchanged: it stays the same epoch-ms
  inst-ms produces, and (time-map->epoch (in-zone zone t)) returns
  it exactly, because the map carries the resolved offset. The
  result never stores the zone name; a zone is a way to read an
  instant, not data in one. DST follows the embedded tzdata
  transitions with fold-0 semantics."
  [zone t]
  (tz-zone-checked zone
                   #(epoch->time-map (instant t) {:zone zone})))

(defn zone-offset-mins
  "The zone's UTC offset in minutes at an instant: positive east.
  The zone is an IANA name (string or keyword, e.g. \"Europe/Oslo\")
  or a fixed offset in minutes. Same relation to inst-ms as every
  verb here: the offset describes the wall clock, the instant stays
  epoch-ms."
  [zone t]
  (tz-zone-checked zone
                   #(clojure.core/zone-offset-mins zone (instant t))))

;;;; clojure.instant interop

(defn from-inst
  "Converts an inst value (a #inst literal or read-instant-date map)
  to epoch-ms. Strict: the date must be possible (February 30th
  rejects) and the offset fields are honored; seconds 60 (leap
  second) folds to 59 and nanoseconds truncate to milliseconds."
  [v]
  (when-not (inst? v)
    (time-fail :eval/type "MTY001"
               "mino.time/from-inst: not an inst" {:got v}))
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
