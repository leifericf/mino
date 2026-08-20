(require "tests/test")
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; format-time: the four keyword formats (ADR 21). RFC 1123 vectors
;; below are golden strings produced by the host date -u strftime on
;; this checkout; the round-trip properties cover the continuous
;; space. Cross-tool verification (date, HTTP servers) also runs in
;; the mino-tests satellite.

(def ms-min -62135596800000)
(def ms-max 253402300799999)

(defn fq [p seed]
  (:result (tc/quick-check 60 p :seed seed)))

(def ms-gen
  (gen/fmap (fn [[d ms]] (+ (* d 86400000) ms))
            (gen/tuple (gen/choose -719162 2932896)
                       (gen/choose 0 86399999))))

;;; golden vectors

(deftest iso8601-vectors
  (are [s ms] (= s (format-time ms))
    "1970-01-01T00:00:00Z"        0
    "1970-01-01T00:00:00.123Z"    123
    "1969-12-31T23:59:59Z"        -1000
    "1969-12-31T23:59:59.999Z"    -1
    "2000-02-29T12:00:00Z"        951825600000
    "9999-12-31T23:59:59.999Z"    ms-max
    "0001-01-01T00:00:00Z"        ms-min))

(deftest iso8601-fraction-emitted-only-when-nonzero
  (is (= "1970-01-01T00:00:01Z" (format-time 1000)))
  (is (= "1970-01-01T00:00:01.001Z" (format-time 1001))))

(deftest iso8601-offset-rendering
  (are [s ms off] (= s (format-time ms :iso8601 off))
    "1970-01-01T02:00:00+02:00"   0 120
    "1969-12-31T19:30:00-04:30"   0 -270
    "1970-01-01T00:30:00+00:30"   0 30))

(defn- fmt-date
  "iso8601-date with an optional offset (nil = UTC)."
  [ms off]
  (if (nil? off)
    (format-time ms :iso8601-date)
    (format-time ms :iso8601-date off)))

(defn- fmt-2822
  "rfc2822 with an optional offset (nil = +0000)."
  [ms off]
  (if (nil? off)
    (format-time ms :rfc2822)
    (format-time ms :rfc2822 off)))

(deftest iso8601-date-vectors
  (are [s ms off] (= s (fmt-date ms off))
    "1970-01-01"    0 nil
    "1969-12-31"    -1000 nil
    "1969-12-31"    -1 nil
    "1970-01-01"    43199999 nil    ; 11:59:59.999 same date
    "1970-01-02"    86400000 nil
    "1969-12-31"    0 -60           ; shifted across midnight
    "2100-03-01"    4107542400000 nil))

;; Golden strings from the host `date -u "+%a, %d %b %Y %H:%M:%S GMT"`
(deftest rfc1123-golden-vectors
  (are [s ms] (= s (format-time ms :rfc1123))
    "Thu, 01 Jan 1970 00:00:00 GMT"   0
    "Wed, 31 Dec 1969 23:59:59 GMT"   -1000
    "Sun, 06 Nov 1994 08:49:37 GMT"   784111777000
    "Fri, 21 Aug 2026 02:31:02 GMT"   1787279462000
    "Mon, 01 Jan 1900 00:00:00 GMT"   -2208988800000
    "Mon, 01 Mar 2100 00:00:00 GMT"   4107542400000
    "Wed, 01 Mar 2000 00:00:00 GMT"   951868800000
    "Fri, 31 Dec 9999 23:59:59 GMT"   253402300799000))

(deftest rfc2822-vectors
  (are [s ms off] (= s (fmt-2822 ms off))
    "Thu, 01 Jan 1970 00:00:00 +0000"  0 nil
    "Sun, 06 Nov 1994 08:49:37 +0000"  784111777000 nil
    "Sun, 06 Nov 1994 08:49:37 +0200"  (- 784111777000 7200000) 120
    "Thu, 20 Aug 2026 21:31:02 -0500"  1787279462000 -300))

;;; errors

(deftest format-time-errors
  (are [re f] (thrown-with-msg? re f)
    #"outside years 1..9999" (format-time (dec ms-min))
    #"outside years 1..9999" (format-time (inc ms-max))
    #"fmt must be" (format-time 0 :bogus)
    #"fmt must be" (format-time 0 "iso8601")
    #"must be an integer" (format-time "0")
    #"must be an integer" (format-time 0.5)
    #"always GMT" (format-time 0 :rfc1123 60)
    #"offset" (format-time 0 :iso8601 1440))
  (is (thrown? (format-time)))
  (is (thrown? (format-time 0 :iso8601 0 0))))

(deftest offset-shift-past-range-boundary-throws
  ;; review round finding: a valid epoch plus a valid offset can
  ;; push the local date outside years 1..9999; that rejects with
  ;; :time/range instead of rendering year 0000 or 10000
  (are [f] (thrown-with-msg? #"years 1..9999" (f))
    #(format-time ms-max :iso8601 1439)
    #(format-time ms-min :iso8601 -1)
    #(format-time ms-max :rfc2822 1439)
    #(format-time ms-min :rfc2822 -1439)
    #(format-time ms-min :iso8601-date -1))
  (are [f] (thrown-with-msg? #"years 1..9999" (f))
    #(epoch->time-map ms-max 1439)
    #(epoch->time-map ms-min -1)))

;;; round-trip properties: every output parses back to the same
;;; instant (and offset for the offset-carrying forms)

(deftest iso-output-parses-back
  (is (fq (prop/for-all [ms ms-gen]
           (let [s (format-time ms)]
             (and (= ms (:epoch-ms (parse-time s)))
                  (= :iso8601 (:format (parse-time s))))))
         777001)))

(deftest iso-with-offset-parses-back
  (is (fq (prop/for-all [ms ms-gen
                         off (gen/choose -1439 1439)]
           (let [s (format-time ms :iso8601 off)
                 r (parse-time s)]
             (and (= ms (:epoch-ms r))
                  (= off (:offset-min r)))))
         777002)))

(deftest iso-date-output-parses-back-as-date-only
  ;; the rendered date is the calendar date of the instant (floor
  ;; semantics on negative epochs), and it parses as date-only
  (is (fq (prop/for-all [ms ms-gen]
           (let [s (format-time ms :iso8601-date)
                 r (parse-time s)
                 m (epoch->time-map ms)]
             (and (:date-only? r)
                  (= [(:year m) (:month m) (:day m)]
                     [(:year (epoch->time-map (:epoch-ms r)))
                      (:month (epoch->time-map (:epoch-ms r)))
                      (:day (epoch->time-map (:epoch-ms r)))]))))
         777003)))

(defn- floor-sec
  "Epoch-ms floored to whole seconds (RFC formats cannot carry a
  fraction, so their round-trip law compares at this granularity)."
  [ms]
  (let [q (quot ms 1000)]
    (if (and (neg? ms) (not (zero? (rem ms 1000)))) (dec q) q)))

(deftest rfc1123-output-parses-back
  (is (fq (prop/for-all [ms ms-gen]
           (let [s (format-time ms :rfc1123)]
             (and (= (floor-sec ms) (floor-sec (:epoch-ms (parse-time s))))
                  (= :rfc1123 (:format (parse-time s))))))
         777004)))

(deftest rfc2822-output-parses-back
  (is (fq (prop/for-all [ms ms-gen
                         off (gen/choose -1439 1439)]
           (let [s (format-time ms :rfc2822 off)]
             (= (floor-sec ms) (floor-sec (:epoch-ms (parse-time s))))))
         777005)))

(deftest formats-agree-on-the-instant
  ;; all four formats of one instant parse back to the same second
  (is (fq (prop/for-all [ms ms-gen]
           (let [fmtd [(format-time ms)
                       (format-time ms :iso8601 330)
                       (format-time ms :rfc1123)
                       (format-time ms :rfc2822 -330)]]
             (every? #(= (floor-sec ms) (floor-sec %))
                     (map #(:epoch-ms (parse-time %)) fmtd))))
         777006)))

(run-tests-and-exit)
