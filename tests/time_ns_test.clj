(require "tests/test")
(require "tests/fixtures/http/server")
(require '[clojure.data.json :as json])
(require '[mino.http])
(require '[mino.time :as t])

;; mino.time namespace: the plain-data surface over the time prims
;; (ADR 21). Docstring examples are pinned here verbatim; the instant
;; coercion seam accepts epoch-ms, parse results, and time maps in
;; every verb; HTTP Date headers from the in-process server parse.

(deftest ns-docstring-examples
  (is (int? (t/now)))
  (is (= {:epoch-ms 1787184000000 :offset-min 0 :format :iso8601
          :date-only? true}
         (t/parse "2026-08-20")))
  (is (= "Thu, 01 Jan 1970 00:00:00 GMT" (t/format 0 :rfc1123)))
  (is (= {:year 2026 :month 2 :day 28}
         (select-keys (t/add (t/parse "2026-01-31") {:months 1})
                      [:year :month :day])))
  (is (= "3 days ago"
         (t/human (t/parse "2026-08-20") (t/parse "2026-08-23")))
      "the ns docstring example, pinned")
  (is (= "3 days ago"
         (t/human (- (t/now) (* 3 86400000)) (t/now)))))

(deftest inst-round-trip-literal
  ;; the #inst acceptance criterion, verbatim
  (is (= "2026-01-15T10:30:00.250Z"
         (t/format (t/parse "2026-01-15T10:30:00.250Z"))))
  (is (= "2026-05-21T00:00:00Z"
         (t/format (t/parse "2026-05-21T00:00:00Z")))))

(deftest instant-coercion-seam
  (let [pr (t/parse "2026-08-20T12:00:00Z")
        tm (t/epoch->time-map pr)]
    ;; every verb accepts every instant shape
    (is (= 1787227200000 (t/instant pr)))
    (is (= 1787227200000 (t/instant tm)))
    (is (= 1787227200000 (t/instant 1787227200000)))
    (is (= "2026-08-20T12:00:00Z" (t/format pr)))
    (is (= "2026-08-20T12:00:00Z" (t/format tm)))
    (is (= :rfc1123 (:format (t/parse (t/format pr :rfc1123)))))
    (is (map? (t/add pr {:days 1})))       ; parse result answers a map
    (is (map? (t/add tm {:days 1})))
    (is (= 1 (:months (t/diff pr (t/add pr {:months 1})))))
    (is (= "1 month ago" (t/human pr (t/add pr {:months 1}))))
    (is (= 4 (t/weekday pr)))
    (is (thrown? (t/instant "2026-08-20")))
    (is (thrown? (t/instant nil)))))

(deftest add-unit-composition-and-errors
  (is (= (+ 86400000 2678400000)      ; 1 day + 1 month of Jan 1970
         (t/add 0 {:days 1 :months 1})))
  (is (= 500 (t/add 0 {:ms 500})))
  (is (= 0 (t/add 0 {})))
  (is (= {:offset-min 120 :hour 2}
         (select-keys (t/add (t/epoch->time-map 0 120) {:days 1})
                      [:offset-min :hour])))
  (is (thrown-with-msg? #"unknown units"
                        (t/add 0 {:fortnights 2})))
  (is (thrown-with-msg? #"unknown units"
                        (t/add 0 {:days 1 :sennights 1}))))

(deftest diff-decomposes-calendar-units
  (is (= {:months 13 :days 4 :ms 0}
         (t/diff 0 (* 400 86400000))))     ; 400 days = 13 months 4 days
  (is (= {:months -13 :days -4 :ms 0}
         (t/diff (* 400 86400000) 0)))
  (is (= {:months 0 :days 0 :ms 1500}
         (t/diff 0 1500)))
  ;; whole-unit extraction: 1 month + 2 days + 3 ms
  (let [a 0
        b (+ (t/add a {:months 1}) (* 2 86400000) 3)]
    (is (= {:months 1 :days 2 :ms 3} (t/diff a b)))))

(deftest today-and-clocks
  (let [td (t/today)]
    (is (= (:year (t/epoch->time-map (t/now))) (:year td))
        "today is this calendar year, derived not hard-coded")
    (is (= 0 (:hour td)))
    (is (= 0 (:ms td))))
  (is (int? (t/now-s)))
  (is (int? (t/cpu-ms)))
  (is (int? (t/monotonic-ms)))
  (is (<= (t/monotonic-ms) (t/monotonic-ms))))

(deftest leap-and-calendar-facts
  (is (true? (t/leap-year? 2024)))
  (is (false? (t/leap-year? 2100)))
  (is (= 29 (t/days-in-month 2024 2))))

;;; clojure.instant interop

(deftest from-inst-matches-inst-ms
  (is (= (inst-ms #inst "2026-05-21T00:00:00Z")
         (t/from-inst #inst "2026-05-21T00:00:00Z")))
  (is (= 0 (t/from-inst #inst "1970-01-01T00:00:00Z")))
  ;; offsets honored: +02:00 wall is 2h earlier UTC
  (is (= (inst-ms #inst "2026-05-21T12:00:00Z")
         (t/from-inst #inst "2026-05-21T14:00:00+02:00")))
  ;; nanoseconds truncate
  (is (= 250 (rem (t/from-inst #inst "2026-01-15T10:30:00.2505Z")
                  1000)))
  ;; strict: from-inst rejects an impossible date. The reader itself
  ;; rejects Feb 30 (matching canon), so feed a hand-built inst map to
  ;; reach the converter's day check.
  (is (thrown-with-msg? #"day 30 invalid"
                        (t/from-inst
                          (with-meta {:years 2023 :months 2 :days 30
                                      :hours 0 :minutes 0 :seconds 0
                                      :nanoseconds 0 :offset-sign 1
                                      :offset-hours 0 :offset-minutes 0}
                                     {:mino/instant true}))))
  ;; a plain map without the instant marker is not an inst
  (is (thrown-with-msg? #"not an inst"
                        (t/from-inst {:years 2026 :months 1 :days 1})))
  (is (thrown? (t/from-inst "not an inst"))))

(deftest to-inst-prints-and-reads-back
  (let [v (t/to-inst (t/parse "2026-05-21T00:00:00Z"))]
    (is (inst? v))
    (is (inst? (read-string (pr-str v))))
    (is (= (t/from-inst v)
           (t/from-inst (read-string (pr-str v))))))
  (is (= 0 (t/from-inst (t/to-inst 0)))))

;;; HTTP integration: Date headers parse through mino.time

(deftest http-date-header-parses-via-mino-time
  ;; the in-process fixture server sends a real RFC 1123 Date header
  ;; on every framed response; parse it and sanity-check the instant
  (fx-with-server
    (fn [srv]
      (let [r (mino.http/get (str "http://127.0.0.1:" (:port srv)
                                  "/hello"))
            d (clojure.core/get (:headers r) "date")]
        (is (string? d))
        (let [p (t/parse d)]
          (is (int? (:epoch-ms p)))
          (is (= :rfc1123 (:format p)))
          (is (< (- (t/now) (:epoch-ms p)) 600000)
              "Date header is fresh"))))))

(deftest json-iso-strings-stay-strings
  ;; documented no-auto-coercion contract, pinned through a real
  ;; JSON decode: ISO date fields arrive as strings, never instants
  (let [m (json/read-str "{\"at\":\"2026-08-20T10:00:00Z\",\"n\":3}")]
    (is (string? (clojure.core/get m "at")))
    (is (= "2026-08-20T10:00:00Z" (clojure.core/get m "at")))
    (is (not (inst? (clojure.core/get m "at"))))))

(deftest instant-error-renders
  ;; review round finding: the seam's error path must render its
  ;; message and data, not fail inside ex-info
  (is (thrown-with-msg? #"expected epoch-ms"
                        (t/instant "2026-08-20")))
  (let [e (try (t/instant "2026-08-20") (catch Throwable e e))]
    (is (map? (ex-data e)))))

(deftest add-months-clamp-holds-on-offset-maps
  ;; review round finding: ADR clamp must hold on the wall clock,
  ;; not the UTC civil date, for offset-carrying inputs
  (is (= [2026 2 28]
         ((juxt :year :month :day)
          (t/add {:year 2026 :month 1 :day 31 :offset-min 120}
                 {:months 1}))))
  (is (= [2026 2 28]
         ((juxt :year :month :day)
          (t/add (t/parse "2026-01-31T00:00:00+02:00") {:months 1})))))

(deftest weekday-map-agrees-with-wday-field
  ;; review round finding: the map reading and the field must agree
  (let [e (:epoch-ms (t/parse "2026-08-20T00:30:00+02:00"))
        m (t/epoch->time-map e 120)]
    (is (= (:wday m) (t/weekday m)))
    (is (= (:wday m) (t/weekday (t/parse
                                 "2026-08-20T00:30:00+02:00"))))))

(run-tests-and-exit)
