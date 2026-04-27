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

(defn- ensure-len [s idx need msg]
  (when (> (+ idx need) (count s))
    (throw (ex-info (str "instant: " msg ": " s)
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
          ;; Initial defaults; layered fields overwrite.
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
        (cond
          ;; End of input.
          (= idx n) m

          ;; Date components after year: -MM at idx 4.
          (and (= idx 4) (char-at-eq? s idx \-))
          (do (ensure-len s 5 2 "month truncated")
              (recur (assoc m :months (parse-int s 5 7)) 7))

          ;; -DD at idx 7.
          (and (= idx 7) (char-at-eq? s idx \-))
          (do (ensure-len s 8 2 "day truncated")
              (recur (assoc m :days (parse-int s 8 10)) 10))

          ;; Time block: T then HH:MM[:SS[.fff]].
          (and (= idx 10) (char-at-eq? s idx \T))
          (do (ensure-len s 11 5 "hour:minute truncated")
              (when-not (char-at-eq? s 13 \:)
                (throw (ex-info "instant: missing : between HH and MM"
                                {:input s})))
              (let [m2 (-> m
                           (assoc :hours   (parse-int s 11 13))
                           (assoc :minutes (parse-int s 14 16)))]
                (recur m2 16)))

          ;; Optional :SS after HH:MM.
          (and (= idx 16) (char-at-eq? s idx \:))
          (do (ensure-len s 17 2 "second truncated")
              (recur (assoc m :seconds (parse-int s 17 19)) 19))

          ;; Optional .fff fractional seconds (variable length; up to
          ;; nanoseconds precision = 9 digits).
          (and (= idx 19) (char-at-eq? s idx \.))
          (let [frac-start (inc idx)
                end        (loop [j frac-start]
                             (if (and (< j n)
                                      (parse-long (nth s j)))
                               (recur (inc j))
                               j))
                _          (when (= end frac-start)
                             (throw (ex-info "instant: fractional seconds empty"
                                             {:input s})))
                digits     (subs s frac-start end)
                ;; Right-pad to 9 digits so :nanoseconds is consistent.
                len        (count digits)
                padded     (if (< len 9)
                             (str digits (apply str (repeat (- 9 len) \0)))
                             (subs digits 0 9))
                nanos      (parse-long padded)]
            (recur (assoc m :nanoseconds nanos) end))

          ;; UTC zone: Z.
          (char-at-eq? s idx \Z)
          (recur m (inc idx))

          ;; Numeric offset: +HH:MM or -HH:MM.
          (or (char-at-eq? s idx \+) (char-at-eq? s idx \-))
          (do (ensure-len s (+ idx 1) 5 "offset truncated")
              (when-not (char-at-eq? s (+ idx 3) \:)
                (throw (ex-info "instant: missing : in zone offset"
                                {:input s})))
              (let [sign (if (= (nth s idx) "+") 1 -1)
                    oh   (parse-int s (+ idx 1) (+ idx 3))
                    om   (parse-int s (+ idx 4) (+ idx 6))]
                (recur (-> m
                           (assoc :offset-sign    sign)
                           (assoc :offset-hours   oh)
                           (assoc :offset-minutes om))
                       (+ idx 6))))

          :else
          (throw (ex-info (str "instant: unexpected character at " idx
                               ": " s)
                          {:input s :at idx})))))))

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
