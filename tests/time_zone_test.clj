(require "tests/test")
(require '[mino.time :as t])

;; Timezones over the ADR 21 civil core (ADR 27). Every vector below
;; was derived by running python3 zoneinfo on this machine (the
;; campaign oracle), never from memory. The pins cover: fixed-offset
;; :zone values, named-zone transition boundaries (NY spring forward,
;; Oslo, Lord Howe's half-hour DST both directions, Kathmandu), the
;; footer-governed years past the last stored transition (2040,
;; 2500), fold-0 local->UTC semantics at gaps and overlaps, the
;; documented minute-granular LMT approximation, round-trips, and
;; the error contract (:time/zone for unknown names).

(defn tz-kind [thunk]
  (try (thunk) (catch e (:mino/kind e))))

;;; zone-offset-mins: the introspection prim

(deftest tz-offset-vectors
  (are [zone ms off] (= off (zone-offset-mins zone ms))
    "America/New_York"    1768478400000 -300  ; 2026-01-15 winter EST
    "America/New_York"    1786795200000 -240  ; 2026-08-15 summer EDT
    "America/New_York"    1772953199000 -300  ; second before spring forward
    "America/New_York"    1772953200000 -240  ; the spring-forward instant
    "Europe/Oslo"         1768478400000 60
    "Europe/Oslo"         1784116800000 120
    "Europe/Oslo"         1774745999000 60    ; second before CEST
    "Europe/Oslo"         1774746000000 120
    "Australia/Lord_Howe" 1768478400000 660   ; Jan, +11:00 DST
    "Australia/Lord_Howe" 1784116800000 630   ; Jul, +10:30 standard
    "Australia/Lord_Howe" 1759591799000 630   ; second before DST starts
    "Australia/Lord_Howe" 1759591800000 660
    "Australia/Lord_Howe" 1775314799000 660   ; second before DST ends
    "Australia/Lord_Howe" 1775314800000 630
    "Asia/Kathmandu"      0                330   ; +05:30 until 1986
    "Asia/Kathmandu"      1787659200000    345
    "UTC"                 0                0
    ;; fixed-offset integers pass through (the ADR 21 arithmetic path)
    -480                   0                -480
    345                    1787659200000    345))

;;; the footer era: instants past the last stored transition (2037)

(deftest tz-footer-era-offsets
  (are [zone ms off] (= off (zone-offset-mins zone ms))
    "America/New_York"    2225966400000 -240  ; 2040-07-15
    "America/New_York"    2212920000000 -300  ; 2040-02-15
    "America/New_York"    16742116800000 -240 ; 2500-07-15
    "America/New_York"    16726478400000 -300 ; 2500-01-15
    "Europe/Oslo"         2225966400000 120
    "Australia/Lord_Howe" 2212920000000 660   ; southern summer, Feb 2040
    "Australia/Lord_Howe" 2225966400000 630))

(deftest tz-footer-era-local-fields
  (is (= {:year 2500 :month 7 :day 15 :hour 8 :min 0 :sec 0 :ms 0
          :wday 4 :offset-min -240}
         (epoch->time-map 16742116800000 {:zone "America/New_York"})))
  (is (= {:year 2500 :month 1 :day 15 :hour 23 :min 0 :sec 0 :ms 0
          :wday 5 :offset-min 660}
         (epoch->time-map 16726478400000
                          {:zone "Australia/Lord_Howe"}))))

;;; epoch->time-map in a named zone (transition boundaries pinned)

(deftest tz-epoch-to-map-named
  (are [ms zone m] (= m (epoch->time-map ms {:zone zone}))
    1772953199000 "America/New_York"
    {:year 2026 :month 3 :day 8 :hour 1 :min 59 :sec 59 :ms 0
     :wday 0 :offset-min -300}
    1772953200000 "America/New_York"
    {:year 2026 :month 3 :day 8 :hour 3 :min 0 :sec 0 :ms 0
     :wday 0 :offset-min -240}
    1768478400000 "America/New_York"
    {:year 2026 :month 1 :day 15 :hour 7 :min 0 :sec 0 :ms 0
     :wday 4 :offset-min -300}
    1774745999000 "Europe/Oslo"
    {:year 2026 :month 3 :day 29 :hour 1 :min 59 :sec 59 :ms 0
     :wday 0 :offset-min 60}
    1774746000000 "Europe/Oslo"
    {:year 2026 :month 3 :day 29 :hour 3 :min 0 :sec 0 :ms 0
     :wday 0 :offset-min 120}
    ;; Lord Howe falls back half an hour: 01:59:59 -> 01:30:00
    1775314799000 "Australia/Lord_Howe"
    {:year 2026 :month 4 :day 5 :hour 1 :min 59 :sec 59 :ms 0
     :wday 0 :offset-min 660}
    1775314800000 "Australia/Lord_Howe"
    {:year 2026 :month 4 :day 5 :hour 1 :min 30 :sec 0 :ms 0
     :wday 0 :offset-min 630}
    1759591800000 "Australia/Lord_Howe"
    {:year 2025 :month 10 :day 5 :hour 2 :min 30 :sec 0 :ms 0
     :wday 0 :offset-min 660}
    1787659200000 "Asia/Kathmandu"
    {:year 2026 :month 8 :day 25 :hour 17 :min 45 :sec 0 :ms 0
     :wday 2 :offset-min 345}
    ;; fixed-offset :zone is the same map shape
    0 345
    {:year 1970 :month 1 :day 1 :hour 5 :min 45 :sec 0 :ms 0
     :wday 4 :offset-min 345}
    ;; keyword zone names resolve by their text
    1787659200000 :Asia/Kathmandu
    {:year 2026 :month 8 :day 25 :hour 17 :min 45 :sec 0 :ms 0
     :wday 2 :offset-min 345}))

(deftest tz-lmt-minute-granularity
  ;; Oslo 1850 is LMT +00:53:28; the vocabulary is minutes, so the
  ;; offset rounds to the nearest minute (ADR 27's recorded
  ;; divergence from second-exact tzdata).
  (is (= {:year 1850 :month 6 :day 1 :hour 12 :min 53 :sec 0 :ms 0
          :wday 6 :offset-min 53}
         (epoch->time-map -3773736000000 {:zone "Europe/Oslo"})))
  ;; Monrovia 1970 is LMT -00:44:30, an exact half minute: mino
  ;; rounds half away from zero (-45) where python zoneinfo
  ;; banker-rounds to -44; the divergence is documented and pinned.
  (is (= -45 (zone-offset-mins "Africa/Monrovia" 0))))

;;; parse-time with :zone: naive input reads as local wall time

(deftest tz-parse-naive-in-zone
  (are [s zone ms off] (= {:epoch-ms ms :offset-min off :format :iso8601
                           :date-only? false}
                          (parse-time s {:zone zone}))
    ;; spring-forward gap: fold-0 uses the pre-transition offset, so
    ;; the nonexistent 02:30 maps forward past the gap
    "2026-03-08T02:30:00" "America/New_York" 1772955000000 -300
    ;; fall-back overlap: first occurrence (EDT reading)
    "2026-11-01T01:30:00" "America/New_York" 1793511000000 -240
    "2026-03-29T02:30:00" "Europe/Oslo"       1774747800000 60
    "2026-04-05T02:00:00" "Australia/Lord_Howe" 1775316600000 630
    "2026-08-25T12:00:00" "Asia/Kathmandu"    1787638500000 345
    ;; the footer era parses too
    "2500-03-14T02:30:00" "America/New_York"  16731473400000 -300
    "2500-11-01T01:30:00" "America/New_York"  16751511000000 -240))

(deftest tz-parse-date-only-in-zone
  ;; midnight on the zone's wall clock (Aug 20 NY is EDT, UTC-4)
  (is (= {:epoch-ms 1787198400000 :offset-min -240 :format :iso8601
          :date-only? true}
         (parse-time "2026-08-20" {:zone "America/New_York"}))))

(deftest tz-parse-fixed-offset-zone
  (is (= {:epoch-ms 1787638500000 :offset-min 345 :format :iso8601
          :date-only? false}
         (parse-time "2026-08-25T12:00:00" {:zone 345}))))

(deftest tz-parse-zone-conflict-rejects
  ;; explicit beats implicit: an input carrying its own offset plus
  ;; :zone is a field error, the ADR 21 strictness stance
  (are [s] (= :time/field
              (tz-kind #(parse-time s {:zone "America/New_York"})))
    "2026-08-20T10:00:00Z"
    "2026-08-20T10:00:00+02:00"
    "2026-08-20T10:00:00-0530"
    "Fri, 21 Aug 2026 04:31:22 GMT"))

;;; format-time with :zone

(deftest tz-format-in-zone
  (are [ms fmt zone s] (= s (format-time ms fmt {:zone zone}))
    1768478400000 :iso8601 "America/New_York"
    "2026-01-15T07:00:00-05:00"
    1786795200000 :iso8601 "America/New_York"
    "2026-08-15T08:00:00-04:00"
    1787659200000 :iso8601 "Asia/Kathmandu"
    "2026-08-25T17:45:00+05:45"
    1768478400000 :iso8601 "Australia/Lord_Howe"
    "2026-01-15T23:00:00+11:00"
    1784116800000 :iso8601 "Australia/Lord_Howe"
    "2026-07-15T22:30:00+10:30"
    1787184000000 :iso8601-date "America/New_York"
    "2026-08-19"
    1768478400000 :rfc2822 "America/New_York"
    "Thu, 15 Jan 2026 07:00:00 -0500"
    ;; map-only form: default fmt
    1787659200000 :iso8601 345
    "2026-08-25T17:45:00+05:45"))

(deftest tz-format-rfc1123-rejects-zone
  (is (= :time/field
         (tz-kind #(format-time 1768478400000 :rfc1123
                                {:zone "America/New_York"})))))

;;; time-map->epoch with :zone: local wall fields in the zone

(deftest tz-map-to-epoch-in-zone
  (are [m zone ms] (= ms (time-map->epoch m {:zone zone}))
    {:year 2026 :month 3 :day 8 :hour 2 :min 30} "America/New_York"
    1772955000000
    {:year 2026 :month 11 :day 1 :hour 1 :min 30} "America/New_York"
    1793511000000
    {:year 2026 :month 8 :day 25 :hour 12} "Asia/Kathmandu"
    1787638500000
    {:year 2026 :month 1 :day 15 :hour 7} "America/New_York"
    1768478400000
    {:year 2500 :month 7 :day 15 :hour 8} "America/New_York"
    16742116800000))

(deftest tz-map-to-epoch-in-zone-wday-checks-local-date
  ;; the supplied :wday describes the zone-local date, not UTC
  (is (= 1772953200000
         (time-map->epoch {:year 2026 :month 3 :day 8 :hour 3
                           :wday 0}
                          {:zone "America/New_York"})))
  (is (= :time/field
         (tz-kind #(time-map->epoch
                    {:year 2026 :month 3 :day 8 :hour 3 :wday 1}
                    {:zone "America/New_York"})))))

;;; round-trips: the map carries the resolved offset-min, so the
;;; plain converter returns the same instant

(deftest tz-round-trips
  (are [ms zone] (= ms (time-map->epoch (epoch->time-map ms
                                                         {:zone zone})))
    1772953199000 "America/New_York"
    1772953200000 "America/New_York"
    1775314799000 "Australia/Lord_Howe"
    1775314800000 "Australia/Lord_Howe"
    1774745999000 "Europe/Oslo"
    1787659200000 "Asia/Kathmandu"
    2225966400000 "America/New_York"
    16742116800000 "America/New_York"
    16726478400000 "Australia/Lord_Howe"
    0 345))

(deftest tz-format-parse-round-trip-in-zone
  ;; the zone-formatted string carries its own numeric offset, so the
  ;; plain parse reads it back without needing the zone again
  (are [ms zone] (= ms (:epoch-ms
                        (parse-time (format-time ms :iso8601
                                                 {:zone zone}))))
    1768478400000 "America/New_York"
    1784116800000 "Australia/Lord_Howe"
    2225966400000 "America/New_York"
    16742116800000 "Australia/Lord_Howe"))

;;; the ADR 21 surface is unchanged where no zone is asked for

(deftest tz-legacy-offsets-unchanged
  (is (= {:year 1970 :month 1 :day 1 :hour 5 :min 45 :sec 0 :ms 0
          :wday 4 :offset-min 345}
         (epoch->time-map 0 345)))
  (is (= {:epoch-ms 1787212800000 :offset-min 120 :format :iso8601
          :date-only? false}
         (parse-time "2026-08-20T10:00:00+02:00"))))

;;; errors

(deftest tz-unknown-zone-errors
  (are [thunk] (= :time/zone (tz-kind thunk))
    #(zone-offset-mins "America/Nowhere" 0)
    #(epoch->time-map 0 {:zone "America/Nowhere"})
    #(parse-time "2026-08-20T10:00:00" {:zone "Mars/Olympus"})
    #(format-time 0 :iso8601 {:zone "Mars/Olympus"})
    #(time-map->epoch {:year 2026 :month 1 :day 1} {:zone "Nope"})))

(deftest tz-zone-opt-validation
  (are [thunk] (= :time/field (tz-kind thunk))
    #(epoch->time-map 0 {:zone 1500})          ; beyond 23:59
    #(epoch->time-map 0 {:zone -1500})
    #(epoch->time-map 0 {:zone "x" :hour 3})   ; unknown option key
    #(parse-time "2026-08-20" {:offset 0})))   ; only :zone is an option

;;; mino.time zone sugar (ADR 27 facade)

(deftest tz-in-zone-sugar
  (is (= {:year 2026 :month 1 :day 15 :hour 7 :min 0 :sec 0 :ms 0
          :wday 4 :offset-min -300}
         (t/in-zone "America/New_York" 1768478400000)))
  ;; parse results and time maps flow through the instant coercion
  (is (= 1772953200000
         (t/time-map->epoch
          (t/in-zone "America/New_York"
                     (t/parse "2026-03-08T07:00:00Z")))))
  ;; the map carries the resolved offset, so the plain converter
  ;; round-trips (the documented relation to inst-ms: the instant is
  ;; the same epoch-ms everywhere, a zone only picks the wall clock)
  (are [ms zone] (= ms (t/time-map->epoch (t/in-zone zone ms)))
    1775314800000 "Australia/Lord_Howe"
    2225966400000 "America/New_York"
    16742116800000 "Australia/Lord_Howe"))

(deftest tz-zone-offset-mins-sugar
  (are [zone t off] (= off (t/zone-offset-mins zone t))
    "Australia/Lord_Howe" 1768478400000 660
    "Australia/Lord_Howe" 1784116800000 630
    :Asia/Kathmandu      1787659200000 345
    -480                 0              -480
    "Europe/Oslo"        (t/parse "2026-07-15T12:00:00Z") 120))

(deftest tz-sugar-opts-passthrough
  ;; parse accepts {:zone ...} for offset-less input
  (is (= 1772955000000
         (:epoch-ms (t/parse "2026-03-08T02:30:00"
                             {:zone "America/New_York"}))))
  ;; format renders in the zone (2- and 3-arity shapes)
  (is (= "2026-01-15T07:00:00-05:00"
         (t/format 1768478400000 :iso8601 {:zone "America/New_York"})))
  (is (= "2026-08-25T17:45:00+05:45"
         (t/format (t/parse "2026-08-25T12:00:00Z")
                   :iso8601 {:zone "Asia/Kathmandu"}))))

(deftest tz-sugar-unknown-zone-carries-data
  (let [r (try (t/in-zone "America/Nowhere" 0)
               (catch e (ex-data e)))]
    (is (= :time/zone (:kind r)))
    (is (= "America/Nowhere" (:zone r)))))
