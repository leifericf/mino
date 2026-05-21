(require "tests/test")

;; inst? / inst-ms / #inst literal. mino has no host Date/Timestamp;
;; the canonical representation is a component map carrying the
;; :mino/instant true meta marker.

(deftest inst-reader-literal-parses
  (let [v #inst "2026-05-21T00:00:00Z"]
    (is (map? v))
    (is (= 2026 (:years v)))
    (is (= 5    (:months v)))
    (is (= 21   (:days v)))))

(deftest inst-question-detects-reader-literal
  (is (true?  (inst? #inst "2026-05-21T00:00:00Z")))
  (is (false? (inst? "2026-05-21T00:00:00Z")))
  (is (false? (inst? {:years 2026 :months 5 :days 21})))
  (is (false? (inst? nil))))

(deftest inst-question-detects-meta-marker
  (let [m (with-meta {:years 2026 :months 5 :days 21
                       :hours 0 :minutes 0 :seconds 0 :nanoseconds 0
                       :offset-sign 1 :offset-hours 0 :offset-minutes 0}
                     {:mino/instant true})]
    (is (true? (inst? m)))))

(deftest inst-ms-epoch-zero
  (is (= 0 (inst-ms #inst "1970-01-01T00:00:00Z"))))

(deftest inst-ms-known-points
  ;; 2000-01-01 UTC = 946684800000
  (is (= 946684800000 (inst-ms #inst "2000-01-01T00:00:00Z")))
  ;; 2026-05-21 UTC at midnight
  (is (= 1779321600000 (inst-ms #inst "2026-05-21T00:00:00Z")))
  ;; mid-day
  (is (= 1779364800000 (inst-ms #inst "2026-05-21T12:00:00Z"))))

(deftest inst-ms-offset-honored
  ;; +02:00 means local clock is 2 hours ahead of UTC; same wall time
  ;; with a +02:00 offset is 2 hours earlier in UTC.
  (let [utc-ms     (inst-ms #inst "2026-05-21T12:00:00Z")
        offset-ms  (inst-ms #inst "2026-05-21T14:00:00+02:00")]
    (is (= utc-ms offset-ms))))

(deftest inst-ms-negative-offset
  ;; -05:00 (US/Eastern winter): noon local = 17:00 UTC.
  (let [utc-ms     (inst-ms #inst "2026-05-21T17:00:00Z")
        offset-ms  (inst-ms #inst "2026-05-21T12:00:00-05:00")]
    (is (= utc-ms offset-ms))))

(deftest inst-ms-rejects-non-inst
  (is (thrown? (inst-ms "2026-05-21")))
  (is (thrown? (inst-ms {:not-an-instant true})))
  (is (thrown? (inst-ms nil))))

(deftest inst-roundtrip-read-string
  ;; read-string on a #inst literal produces an inst.
  (let [v (read-string "#inst \"2026-05-21T00:00:00Z\"")]
    (is (inst? v))
    (is (= 1779321600000 (inst-ms v)))))
