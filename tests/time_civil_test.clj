(require "tests/test")
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; Time basics: wall/cpu clocks and the epoch-ms <-> time-map
;; converters over the own civil core (ADR 21). Field vectors below
;; are oracle values cross-checked against the platform gmtime in the
;; time-date spike (dense 1..9999 sampled round trips, 200k random
;; epochs, all fields plus inverse).

(def trials 60)

(defn time-qc [p seed]
  (:result (tc/quick-check trials p :seed seed)))

(def ms-min -62135596800000)          ; 0001-01-01T00:00:00.000Z
(def ms-max 253402300799999)          ; 9999-12-31T23:59:59.999Z

(def ms-gen
  ;; gen/choose is bounded by the 32-bit int cast (JVM parity), so
  ;; epoch-ms is composed from day index + intra-day millis.
  (gen/fmap (fn [[d ms]] (+ (* d 86400000) ms))
            (gen/tuple (gen/choose -719162 2932896)
                       (gen/choose 0 86399999))))

;;; clocks

(deftest now-returns-recent-epoch-ms
  (let [t (now)]
    (is (int? t))
    (is (> t 1700000000000))          ; after 2023-11-14
    (is (< t 4102444800000))))        ; before 2100-01-01

(deftest now-is-monotonic-across-reads
  (let [a (now)]
    (is (<= a (now) (now)))))

(deftest now-s-coheres-with-now
  (let [a (now)
        s (now-s)
        b (now)]
    (is (<= (quot a 1000) s (quot b 1000)))
    (is (< (- b a) 2000))))

(deftest cpu-ms-is-a-nonnegative-integer
  (let [c (cpu-ms)]
    (is (int? c))
    (is (>= c 0))
    (is (>= (cpu-ms) c))))            ; CPU time does not go backwards

(deftest clocks-reject-arguments
  (is (thrown? (now 1)))
  (is (thrown? (now-s 1)))
  (is (thrown? (cpu-ms 1))))

;;; epoch->time-map oracle vectors (spike-verified vs gmtime_r)

(deftest epoch-to-time-map-vectors
  (are [ms m] (= m (epoch->time-map ms))
    0                {:year 1970 :month 1 :day 1 :hour 0 :min 0
                      :sec 0 :ms 0 :wday 4 :offset-min 0}
    -1               {:year 1969 :month 12 :day 31 :hour 23 :min 59
                      :sec 59 :ms 999 :wday 3 :offset-min 0}
    951782400000     {:year 2000 :month 2 :day 29 :hour 0 :min 0
                      :sec 0 :ms 0 :wday 2 :offset-min 0}
    951868800000     {:year 2000 :month 3 :day 1 :hour 0 :min 0
                      :sec 0 :ms 0 :wday 3 :offset-min 0}
    4107542400000    {:year 2100 :month 3 :day 1 :hour 0 :min 0
                      :sec 0 :ms 0 :wday 1 :offset-min 0}
    -2208988800000   {:year 1900 :month 1 :day 1 :hour 0 :min 0
                      :sec 0 :ms 0 :wday 1 :offset-min 0}
    1787279462000    {:year 2026 :month 8 :day 21 :hour 2 :min 31
                      :sec 2 :ms 0 :wday 5 :offset-min 0}
    ms-max           {:year 9999 :month 12 :day 31 :hour 23 :min 59
                      :sec 59 :ms 999 :wday 5 :offset-min 0}
    ms-min           {:year 1 :month 1 :day 1 :hour 0 :min 0
                      :sec 0 :ms 0 :wday 1 :offset-min 0}))

(deftest epoch-to-time-map-offset-shifts-wall-fields
  ;; 1970-01-01T00:00 at +02:00 renders 02:00 local, same instant
  (is (= {:year 1970 :month 1 :day 1 :hour 2 :min 0 :sec 0 :ms 0
          :wday 4 :offset-min 120}
         (epoch->time-map 0 120)))
  ;; -05:30 crosses the day boundary backwards: midnight minus 5h30
  (is (= {:year 1969 :month 12 :day 31 :hour 18 :min 30 :sec 0 :ms 0
          :wday 3 :offset-min -330}
         (epoch->time-map 0 -330))))

(deftest epoch-to-time-map-range-errors
  (is (thrown-with-msg? #"years 1..9999"
                        (epoch->time-map (dec ms-min))))
  (is (thrown-with-msg? #"years 1..9999"
                        (epoch->time-map (inc ms-max))))
  (is (thrown? (epoch->time-map 0 1440)))
  (is (thrown? (epoch->time-map "0")))
  (is (thrown? (epoch->time-map 0 1.5))))

;;; time-map->epoch

(deftest time-map-to-epoch-inverse-of-vectors
  (are [ms m] (= ms (time-map->epoch m))
    0              {:year 1970 :month 1 :day 1}
    3600000        {:year 1970 :month 1 :day 1 :hour 1}
    86399000       {:year 1970 :month 1 :day 1 :hour 23 :min 59 :sec 59}
    -1000          {:year 1969 :month 12 :day 31 :hour 23 :min 59 :sec 59}
    951782400000   {:year 2000 :month 2 :day 29}
    1787279462000  {:year 2026 :month 8 :day 21 :hour 2 :min 31 :sec 2}))

(deftest time-map-to-epoch-applies-offset
  (is (= 0 (time-map->epoch {:year 1970 :month 1 :day 1 :hour 2
                             :offset-min 120})))
  (is (= 1787212800000
         (time-map->epoch {:year 2026 :month 8 :day 20 :hour 10
                           :offset-min 120}))))

(deftest time-map-to-epoch-strict-validation
  (are [m re] (thrown-with-msg? re (time-map->epoch m))
    {:month 8 :day 20}                     #"missing required field :year"
    {:year 2026 :day 20}                   #"missing required field :month"
    {:year 2026 :month 8}                  #"missing required field :day"
    {:year 2026 :month 13 :day 1}          #"month 13"
    {:year 2026 :month 0 :day 1}           #"month 0"
    {:year 2026 :month 2 :day 30}          #"day 30 invalid"
    {:year 2023 :month 2 :day 29}          #"day 29 invalid"
    {:year 2026 :month 8 :day 20 :hour 24} #"time-of-day"
    {:year 2026 :month 8 :day 20 :min 60}  #"time-of-day"
    {:year 2026 :month 8 :day 20 :sec 60}  #"time-of-day"
    {:year 2026 :month 8 :day 20 :ms 1000} #"time-of-day"
    {:year 0 :month 1 :day 1}              #"year 0"
    {:year 10000 :month 1 :day 1}          #"year 10000"
    {:year 2026 :month 8 :day 20 :offset-min 1440} #"offset"
    {:year 2026 :month 8 :day 20 :wday 9}  #"wday 9"
    {:year 2026 :month 8 :day 20 :extra 1} #"unknown key :extra"
    {:year 2026 :month 8 :day 20 "x" 1}    #"keys must be keywords"
    {:year "2026" :month 8 :day 20}        #"must be an integer"
    {:year 2026.0 :month 8 :day 20}        #"must be an integer"))

(deftest time-map-to-epoch-leap-day-accepted-only-in-leap-years
  (is (int? (time-map->epoch {:year 2024 :month 2 :day 29})))
  (is (thrown? (time-map->epoch {:year 2100 :month 2 :day 29})))
  (is (int? (time-map->epoch {:year 2000 :month 2 :day 29})))
  (is (thrown? (time-map->epoch {:year 1900 :month 2 :day 29}))))

(deftest time-map-to-epoch-wday-must-match
  ;; 2026-08-20 is a Thursday (4); midnight is 1787184000000
  (is (= 4 (:wday (epoch->time-map 1787184000000))))
  (is (thrown-with-msg? #"contradicts the date"
                        (time-map->epoch {:year 2026 :month 8 :day 20
                                          :wday 0})))
  (is (= 1787184000000
         (time-map->epoch {:year 2026 :month 8 :day 20 :wday 4}))))

(deftest time-map-to-epoch-rejects-non-map-and-arity
  (is (thrown? (time-map->epoch 42)))
  (is (thrown? (time-map->epoch "2026-08-20")))
  (is (thrown? (time-map->epoch)))
  (is (thrown? (time-map-to-epoch {} {}))))

;;; round-trip law

(deftest epoch-round-trips-through-time-map
  (is (time-qc
       (prop/for-all [ms ms-gen]
         (let [m (epoch->time-map ms)]
           (and (= ms (time-map->epoch m))
                (= m (epoch->time-map (time-map->epoch m))))))
       555001)))

(deftest epoch-round-trips-with-offsets
  (is (time-qc
       (prop/for-all [ms ms-gen
                      off (gen/choose -1439 1439)]
         (let [m (epoch->time-map ms off)]
           (and (= ms (time-map->epoch m))
                (= m (epoch->time-map (time-map->epoch m) off)))))
       555002)))

(defn- expected-max-day [y mo]
  (case mo
    2 (if (zero? (rem y 100))
        (if (zero? (rem y 400)) 29 28)
        (if (zero? (rem y 4)) 29 28))
    (nth [0 31 0 31 30 31 30 31 31 30 31 30 31] mo)))

(deftest near-miss-maps-are-classified-not-normalized
  ;; random maps either convert or reject exactly when the day
  ;; overflows the month; nothing normalizes silently
  (is (time-qc
       (prop/for-all [y (gen/choose 1 9999)
                      mo (gen/choose 1 12)
                      d (gen/choose 1 31)
                      h (gen/choose 0 23)]
         (let [m {:year y :month mo :day d :hour h}
               r (try (time-map->epoch m) (catch e :rejected))]
           (if (= r :rejected)
             (> d (expected-max-day y mo))
             (and (int? r) (>= r ms-min) (<= r ms-max)))))
       555003)))

(run-tests-and-exit)
