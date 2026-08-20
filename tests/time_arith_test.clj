(require "tests/test")
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; Calendar arithmetic and human-diff (ADR 21): day/month addition
;; with clamping, whole-unit differences, and the pinned human
;; vocabulary. months-between is defined as the largest n with
;; (add-months a n) <= b; the properties below check that definition
;; directly.

(defn aq [p seed]
  (:result (tc/quick-check 60 p :seed seed)))

(def ms-min -62135596800000)
(def ms-max 253402300799999)

(def ms-gen
  (gen/fmap (fn [[d ms]] (+ (* d 86400000) ms))
            (gen/tuple (gen/choose -719162 2932896)
                       (gen/choose 0 86399999))))

;;; leap-year? and days-in-month

(deftest leap-year-vectors
  (are [y r] (= r (leap-year? y))
    2000 true 1900 false 2024 true 2023 false 2100 false 2400 true
    1996 true 1600 true))

(deftest days-in-month-vectors
  (are [y m r] (= r (days-in-month y m))
    2026 1 31 2026 2 28 2024 2 29 2026 4 30 2026 12 31
    2000 2 29 1900 2 28 2100 2 28)
  (is (thrown? (days-in-month 2026 0)))
  (is (thrown? (days-in-month 2026 13))))

;;; weekday

(deftest weekday-vectors
  (are [ms w] (= w (weekday ms))
    0 4            ; 1970-01-01 Thursday
    86400000 5     ; Friday
    (- 86400000) 3 ; Wednesday
    1787184000000 4) ; 2026-08-20 Thursday
  (is (= 4 (weekday (epoch->time-map 0))))
  (is (= 6 (weekday {:year 2026 :month 8 :day 22})))
  (is (thrown? (weekday "x"))))

;;; add-days

(deftest add-days-exact
  (is (= 86400000 (add-days 0 1)))
  (is (= -86400000 (add-days 0 -1)))
  (is (= 0 (add-days 0 0)))
  (is (= 123 (add-days 123 0)))
  (is (thrown-with-msg? #"years 1..9999" (add-days ms-max 1)))
  (is (thrown-with-msg? #"years 1..9999" (add-days ms-min -1)))
  (is (aq (prop/for-all [ms ms-gen
                         n (gen/choose -1000 1000)]
           (let [r (add-days ms n)]
             (and (int? r) (= n (days-between ms r)))))
         888001)))

;;; add-months clamping matrix

(deftest add-months-clamps-month-end
  (let [jan31 (epoch->time-map (time-map->epoch {:year 2026 :month 1
                                                 :day 31}))]
    (are [n y m d]
         (= [y m d]
            ((juxt :year :month :day) (add-months jan31 n)))
      1 2026 2 28
      2 2026 3 31
      3 2026 4 30
      -1 2025 12 31
      11 2026 12 31
      12 2027 1 31
      13 2027 2 28))
  ;; leap February takes 29
  (let [jan31-24 (epoch->time-map (time-map->epoch {:year 2024 :month 1
                                                    :day 31}))]
    (is (= 29 (:day (add-months jan31-24 1))))
    (is (= 28 (:day (add-months jan31-24 13))))))

(deftest add-months-ms-input-shifts-utc-civil-date
  ;; 1970-01-31 + 1 month = 1970-02-28
  (is (= {:year 1970 :month 2 :day 28}
         (select-keys (epoch->time-map (add-months
                                        (time-map->epoch {:year 1970
                                                          :month 1
                                                          :day 31})
                                        1))
                      [:year :month :day])))
  ;; intra-day milliseconds survive
  (is (= 123 (:ms (epoch->time-map (add-months 123 2))))))

(deftest add-months-map-preserves-time-and-offset
  (let [m (epoch->time-map 0 120)   ; renders local 02:00 +02:00
        r (add-months m 2)]
    (is (= 120 (:offset-min r)))
    (is (= 2 (:hour r)))
    (is (= 3 (:month r))            ; January + 2 months
        )))

(deftest add-months-out-of-range-throws
  (is (thrown-with-msg? #"years 1..9999"
                        (add-months (time-map->epoch {:year 9999
                                                      :month 12
                                                      :day 31}) 13)))
  (is (thrown-with-msg? #"years 1..9999"
                        (add-months (time-map->epoch {:year 1 :month 1
                                                      :day 1}) -1))))

(deftest add-months-clamp-idempotence-for-month-end-starts
  ;; from any month-day start, add n then -n never overshoots the day
  (is (aq (prop/for-all [m (gen/fmap
                            (fn [[mo dd]]
                              {:year 2024 :month mo
                               :day (min dd (days-in-month 2024 mo))})
                            (gen/tuple (gen/choose 1 12)
                                       (gen/choose 1 31)))
                         n (gen/choose -60 60)]
           (let [r (add-months (add-months m n) (- n))]
             (<= (:day r) (:day m))))
         888002)))

;;; days-between / months-between

(deftest days-between-floors
  (are [a b n] (= n (days-between a b))
    0 86399999 0
    0 86400000 1
    0 86400001 1
    86400000 0 -1
    -86400000 0 1
    -86400001 -1 1))    ; one day back, ending one ms before the epoch

(deftest months-between-pins
  ;; definition: largest n with (add-months a n) <= b
  (are [a b n] (= n (months-between a b))
    0 2678399999 0        ; Jan 2 1970 to Feb 1 boundary minus 1ms
    0 2678400000 1        ; 1970-02-01 exactly: one month
    0 5097600000 2        ; 1970-03-01
    2678400000 0 -1
    0 0 0)
  ;; Jan 31 to Feb 28 is a full clamped month
  (is (= 1 (months-between (time-map->epoch {:year 2026 :month 1 :day 31})
                           (time-map->epoch {:year 2026 :month 2
                                             :day 28}))))
  ;; Feb 28 to Mar 31: add-months(Feb 28, 1) = Mar 28 <= Mar 31
  (is (= 1 (months-between (time-map->epoch {:year 2026 :month 2 :day 28})
                           (time-map->epoch {:year 2026 :month 3
                                             :day 31}))))
  ;; same day one year later
  (is (= 12 (months-between (time-map->epoch {:year 2026 :month 8 :day 20})
                            (time-map->epoch {:year 2027 :month 8
                                              :day 20})))))

(deftest months-between-matches-its-definition
  (is (aq (prop/for-all [ms ms-gen
                         n (gen/choose -24 24)]
           (let [target (add-months ms n)]
             (and (= n (months-between ms target))
                  ;; one ms more never jumps a month early
                  (>= (months-between ms (+ target 1)) n)
                  (<= (months-between ms (- target 1)) n))))
         888003)))

;;; human-diff golden vectors (pinned vocabulary)

(deftest human-diff-vectors
  (let [now (now)
        h (fn [d] (human-diff (- now d) now))]
    (are [d s] (= s (h d))
      0          "just now"
      999        "just now"
      1000       "1 second ago"
      1500       "1 second ago"
      59999      "59 seconds ago"
      60000      "1 minute ago"
      3599999    "59 minutes ago"
      3600000    "1 hour ago"
      86399999   "23 hours ago"
      86400000   "1 day ago"
      (* 29 86400000) "29 days ago"
      (* 3 86400000)  "3 days ago"
      (* 400 86400000) "1 year ago"
      (* 366 86400000) "1 year ago")))

(deftest human-diff-future-and-moment
  (let [now (now)]
    (is (= "in a moment" (human-diff (+ now 500) now)))
    (is (= "in 45 seconds" (human-diff (+ now 45000) now)))
    (is (= "in 3 days" (human-diff (+ now (* 3 86400000)) now)))
    (is (= "in 2 years" (human-diff (+ now (* 740 86400000)) now)))))

(deftest human-diff-calendar-month-beats-threshold
  ;; Jan 31 to Mar 1 is 29 days but 1 calendar month: months win
  (let [a (time-map->epoch {:year 2026 :month 1 :day 31})
        b (time-map->epoch {:year 2026 :month 3 :day 1})]
    (is (= "1 month ago" (human-diff a b)))
    (is (= 1 (months-between a b)))))

(deftest human-diff-one-arg-uses-now
  (let [t (- (now) 7200000)]
    (is (= "2 hours ago" (human-diff t)))))

(deftest human-diff-errors
  (is (thrown? (human-diff "x" 0)))
  (is (thrown? (human-diff 0 1.5)))
  (is (thrown? (human-diff)))
  (is (thrown? (human-diff 0 1 2))))

(run-tests-and-exit)
