(ns clojure.instant
  "ISO 8601 timestamp parsing.

   mino divergence from JVM Clojure: there is no Date / Timestamp /
   Calendar host type, so the parsing fns return a component map
   instead of a host-typed value. The map keys mirror the names
   canonical Clojure exposes through Calendar/Timestamp accessors:

     {:years           4-digit year, integer
      :months          1-12,         integer (defaults to 1)
      :days            1-31,         integer (defaults to 1)
      :hours           0-23,         integer (defaults to 0)
      :minutes         0-59,         integer (defaults to 0)
      :seconds         0-60,         integer (defaults to 0; 60 allowed
                                              for the leap-second case)
      :nanoseconds     0-999999999,  integer (defaults to 0)
      :offset-sign     1 or -1,      integer (defaults to 1, i.e. UTC)
      :offset-hours    0-23,         integer (defaults to 0)
      :offset-minutes  0-59,         integer (defaults to 0)}

   read-instant-date is the canonical entry point; calling code that
   wraps it in (java.util.Date.)/(js/Date.) on other Clojure dialects
   needs to consume the map directly here.")

;; --- Internal parsers ---

(defn- parse-int [s start end]
  (let [sub (subs s start end)]
    (or (parse-long sub)
        (throw (ex-info (str "instant: not a number: " sub)
                        {:input s :span [start end]})))))

(defn- char-at-eq?
  "True if the character at idx matches ch. mino's (nth string ...)
   returns a one-char string rather than a primitive char, so we
   compare against the string form."
  [s idx ch]
  (and (< idx (count s))
       (= (nth s idx) (str ch))))

(defn- digit? [c]
  (some? (parse-long c)))

(defn- ensure-len [s idx need msg]
  (when (> (+ idx need) (count s))
    (throw (ex-info (str "instant: " msg ": " s)
                    {:input s :at idx}))))

;; --- Segment parsers ---
;;
;; Each segment parser starts at the marker character (`-`, `T`, `:`,
;; `.`, `Z`, `+`, `-`) and returns [m new-idx]. The driver loop reads
;; the marker to decide which segment fires, then advances by what
;; the segment returns. No segment is invoked unless its marker has
;; already been confirmed.

(defn- parse-month-segment [s idx m]
  (ensure-len s (+ idx 1) 2 "month truncated")
  [(assoc m :months (parse-int s (+ idx 1) (+ idx 3)))
   (+ idx 3)])

(defn- parse-day-segment [s idx m]
  (ensure-len s (+ idx 1) 2 "day truncated")
  [(assoc m :days (parse-int s (+ idx 1) (+ idx 3)))
   (+ idx 3)])

(defn- parse-time-segment [s idx m]
  (ensure-len s (+ idx 1) 5 "hour:minute truncated")
  (when-not (char-at-eq? s (+ idx 3) \:)
    (throw (ex-info "instant: missing : between HH and MM" {:input s})))
  [(-> m
       (assoc :hours   (parse-int s (+ idx 1) (+ idx 3)))
       (assoc :minutes (parse-int s (+ idx 4) (+ idx 6))))
   (+ idx 6)])

(defn- parse-second-segment [s idx m]
  (ensure-len s (+ idx 1) 2 "second truncated")
  [(assoc m :seconds (parse-int s (+ idx 1) (+ idx 3)))
   (+ idx 3)])

(defn- parse-frac-segment [s idx m]
  (let [n          (count s)
        frac-start (inc idx)
        end        (loop [j frac-start]
                     (if (and (< j n) (digit? (nth s j)))
                       (recur (inc j))
                       j))]
    (when (= end frac-start)
      (throw (ex-info "instant: fractional seconds empty" {:input s})))
    (let [digits (subs s frac-start end)
          len    (count digits)
          padded (if (< len 9)
                   (str digits (apply str (repeat (- 9 len) \0)))
                   (subs digits 0 9))]
      [(assoc m :nanoseconds (parse-long padded)) end])))

(defn- parse-zone-segment [s idx m]
  (cond
    (char-at-eq? s idx \Z)
    [m (inc idx)]

    (or (char-at-eq? s idx \+) (char-at-eq? s idx \-))
    (do (ensure-len s (+ idx 1) 5 "offset truncated")
        (when-not (char-at-eq? s (+ idx 3) \:)
          (throw (ex-info "instant: missing : in zone offset" {:input s})))
        [(-> m
             (assoc :offset-sign    (if (= (nth s idx) "+") 1 -1))
             (assoc :offset-hours   (parse-int s (+ idx 1) (+ idx 3)))
             (assoc :offset-minutes (parse-int s (+ idx 4) (+ idx 6))))
         (+ idx 6)])

    :else
    (throw (ex-info (str "instant: unexpected character at " idx ": " s)
                    {:input s :at idx}))))

;; --- Public surface ---

(defn parse-timestamp
  "Parses an ISO 8601 timestamp string into a component map. Returns
   the map; throws ex-info on malformed input.

   Accepted shapes (each later component is optional but, when
   present, requires its predecessors):

     YYYY
     YYYY-MM
     YYYY-MM-DD
     YYYY-MM-DDTHH:MM
     YYYY-MM-DDTHH:MM:SS
     YYYY-MM-DDTHH:MM:SS.fff       (1 to 9 fractional digits)

   Optional zone suffix:
     Z                              (UTC)
     +HH:MM | -HH:MM                (numeric offset)"
  [s]
  (when-not (string? s)
    (throw (ex-info "instant: input must be a string" {:input s})))
  (let [n (count s)]
    (ensure-len s 0 4 "year truncated")
    (let [year (parse-int s 0 4)
          base {:years          year
                :months         1
                :days           1
                :hours          0
                :minutes        0
                :seconds        0
                :nanoseconds    0
                :offset-sign    1
                :offset-hours   0
                :offset-minutes 0}]
      (loop [m   base
             idx 4]
        (if (= idx n)
          m
          (let [[m' idx'] (cond
                            (and (= idx 4)  (char-at-eq? s idx \-))
                            (parse-month-segment s idx m)

                            (and (= idx 7)  (char-at-eq? s idx \-))
                            (parse-day-segment s idx m)

                            (and (= idx 10) (char-at-eq? s idx \T))
                            (parse-time-segment s idx m)

                            (and (= idx 16) (char-at-eq? s idx \:))
                            (parse-second-segment s idx m)

                            (and (= idx 19) (char-at-eq? s idx \.))
                            (parse-frac-segment s idx m)

                            :else
                            (parse-zone-segment s idx m))]
            (recur m' idx')))))))

(defn validated
  "Range-checks the components in a parsed timestamp map. Returns
   the map unchanged on success; throws ex-info on out-of-range
   values. Mirrors the canonical clojure.instant validation."
  [{:keys [years months days hours minutes seconds nanoseconds
           offset-hours offset-minutes]
    :as m}]
  (let [bad (cond
              (or (< months 1)  (> months 12))            "months"
              (or (< days 1)    (> days 31))              "days"
              (or (< hours 0)   (> hours 23))             "hours"
              (or (< minutes 0) (> minutes 59))           "minutes"
              ;; 60 permitted for leap second; not validated against
              ;; an actual UT1 schedule.
              (or (< seconds 0) (> seconds 60))           "seconds"
              (or (< nanoseconds 0)
                  (>= nanoseconds 1000000000))            "nanoseconds"
              (or (< offset-hours 0)   (> offset-hours 23))   "offset-hours"
              (or (< offset-minutes 0) (> offset-minutes 59)) "offset-minutes"
              :else nil)]
    (when bad
      (throw (ex-info (str "instant: " bad " out of range")
                      {:component bad :map m}))))
  m)

(defn read-instant-date
  "Parses an ISO 8601 timestamp string and returns the validated
   component map. Named for parity with canonical clojure.instant;
   on the JVM and JS this returns a host Date instance, but mino
   has no host date type so the map is the honest representation.
   Use the :years / :months / :days / etc. keys to interpret."
  [s]
  (validated (parse-timestamp s)))
