(require "tests/test")
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; parse-time: ISO 8601 / RFC 3339 and the RFC 1123/2822 comma form
;; (ADR 21). Positive and negative vectors come from the time-date
;; spike (oracle-checked there against gmtime_r/strftime); the fuzz
;; tier proves the untrusted-input contract: every input either
;; parses or throws classified :time/parse, never anything else.

(def ms-min -62135596800000)
(def ms-max 253402300799999)

(defn pq [p seed n]
  (:result (tc/quick-check n p :seed seed)))

(defn parses? [s]
  (try (parse-time s) (catch e nil)))

(defn rejected-kind [s]
  (try (parse-time s) (catch e (:mino/kind e))))

;;; ISO 8601 / RFC 3339 positive vectors

(deftest iso-vectors
  (are [s ms off donly] (= {:epoch-ms ms :offset-min off :format :iso8601
                            :date-only? donly}
                           (parse-time s))
    "2026-08-20"                   1787184000000 0 true
    "2026-08-20T10:00:00Z"         1787220000000 0 false
    "2026-08-20t10:00:00z"         1787220000000 0 false
    "2026-08-20 10:00:00Z"         1787220000000 0 false
    "2026-08-20T10:00:00+02:00"    1787212800000 120 false
    "2026-08-20T10:00:00-0530"     1787239800000 -330 false
    "2026-08-20T10:00:00.123456Z"  1787220000123 0 false
    "2026-08-20T10:00:00.12Z"      1787220000120 0 false
    "2026-08-20T10:00:00.1Z"       1787220000100 0 false
    "2026-08-20T10:00Z"            1787220000000 0 false
    "2026-08-20T23:59:60Z"         1787270399000 0 false
    "2020-02-29T23:59:59Z"         1583020799000 0 false
    "1969-12-31T23:59:59Z"         -1000 0 false
    "0001-01-01T00:00:00Z"         ms-min 0 false
    "9999-12-31T23:59:59.999Z"     ms-max 0 false))

(deftest iso-cross-checks-against-converter
  ;; a parsed instant renders back to the same fields
  (let [r (parse-time "2026-08-20T10:00:00.5+02:00")]
    (is (= {:year 2026 :month 8 :day 20 :hour 10 :min 0 :sec 0 :ms 500
            :wday 4 :offset-min 120}
           (epoch->time-map (:epoch-ms r) (:offset-min r))))
    (is (= (:epoch-ms r)
           (time-map->epoch (epoch->time-map (:epoch-ms r)
                                             (:offset-min r)))))))

;;; RFC 1123 / 2822 comma form

(deftest rfc-comma-form-vectors
  (are [s ms off fmt] (= {:epoch-ms ms :offset-min off :format fmt
                          :date-only? false}
                         (parse-time s))
    "Sun, 06 Nov 1994 08:49:37 GMT"    784111777000 0 :rfc1123
    "6 Nov 1994 08:49:37 GMT"          784111777000 0 :rfc1123
    "sun, 06 nov 1994 08:49:37 gmt"    784111777000 0 :rfc1123
    "21 Aug 2026 04:31 UTC"            1787286660000 0 :rfc1123
    "Fri, 21 Aug 2026 04:31:22 +0200"  1787279482000 120 :rfc2822
    "Fri, 21 Aug 2026 04:31:22 -0530"  1787306482000 -330 :rfc2822
    "Tue, 1 Jul 2003 10:52:37 +0200"   1057049557000 120 :rfc2822
    "Wed, 31 Dec 1969 23:59:59 UT"     -1000 0 :rfc1123))

(deftest real-world-http-date-header-parses
  ;; shape every real server sends today
  (is (= :rfc1123
         (:format (parse-time "Fri, 21 Aug 2026 04:31:22 GMT")))))

;;; malformed input rejects with :time/parse

(deftest iso-malformed-rejects
  (are [s] (= :time/parse (rejected-kind s))
    "2026-13-01"
    "2026-00-10"
    "2026-02-30"
    "2023-02-29"
    "2026-8-20"
    "20260820"
    "2026-08-20T25:00:00Z"
    "2026-08-20T10:60:00Z"
    "2026-08-20T10:00:00+25:00"
    "not-a-date"
    "2026-08-20 "
    ""
    "2026-08-20T10:00:00Zx"
    "0000-01-01"
    "10000-01-01"
    "2026-08-20T10:00:00."
    "2026-08-20T10:00:00-0500x"
    "2026-08-20T10:00:61Z"))

(deftest rfc-malformed-rejects
  (are [s] (= :time/parse (rejected-kind s))
    "Sun, 06 Nov 1994 08:49:37 EST"     ; named non-UTC zone
    "Sun, 06 Nov 1994 08:49:37 J"       ; military zone
    "Sun, 32 Nov 1994 08:49:37 GMT"
    "Sun, 06 Nov 94 08:49:37 GMT"       ; 2-digit year
    "Sun, 06 Nov 1994 8:49:37 GMT"      ; 1-digit hour
    "Sun, 06 Xyz 1994 08:49:37 GMT"
    "Sun 06 Nov 1994 08:49:37 GMT"      ; missing comma
    "Sun, 06 Nov 1994 08:49:37 GMT (comment)"
    "Sun, 06 Nov 1994 08:49:37 +02:00"  ; colon in numeric zone
    "Sun, 06 Nov 1994 08:49:37"
    "Sun, 06 Nov 1994 08:49:37 GMT "
    ""
    "Mon, 06 Nov 1994 08:49:37 GMT"))   ; day name contradicts date

(deftest leap-second-folds-and-61-rejects
  (is (= 59 (:sec (epoch->time-map (:epoch-ms (parse-time
                                               "2026-08-20T23:59:60Z"))))))
  (is (= :time/parse (rejected-kind "2026-08-20T23:59:61Z"))))

(deftest parse-time-type-and-arity
  (is (thrown? (parse-time 42)))
  (is (thrown? (parse-time nil)))
  (is (thrown? (parse-time :kw)))
  (is (thrown? (parse-time)))
  (is (thrown? (parse-time "2026" "2026"))))

(deftest over-length-input-rejects
  (is (thrown-with-msg? #"64"
                        (parse-time (apply str (repeat 65 \2)))))
  ;; embedded NUL must not silently truncate the parse
  (is (thrown-with-msg? #"NUL"
                        (parse-time (str "2026-08-20" (char 0) "junk")))))

;;; fuzz: untrusted-input contract

(def garbage-gen
  (gen/fmap (fn [v] (apply str v))
            (gen/vector (gen/choose 0 126) 0 72)))

(def digit-heavy-gen
  (gen/fmap (fn [v] (apply str v))
            (gen/vector (gen/elements [\0 \1 \2 \9 \- \: \T \. \+ \Z \space])
                        0 72)))

(deftest garbage-never-crashes-and-always-classifies
  (is (pq (prop/for-all [s garbage-gen]
           (let [r (try (parse-time s)
                        (catch e (if (= :time/parse (:mino/kind e))
                                   :ok
                                   (str "bad-kind:" (:mino/kind e)))))]
             (or (map? r) (= :ok r))))
         666001 2500))
  (is (pq (prop/for-all [s digit-heavy-gen]
           (let [r (try (parse-time s)
                        (catch e (if (= :time/parse (:mino/kind e))
                                   :ok
                                   (str "bad-kind:" (:mino/kind e)))))]
             (or (map? r) (= :ok r))))
         666002 2500)))

(def valid-strings
  ["2026-08-20" "2026-08-20T10:00:00Z" "2026-08-20T10:00:00.123Z"
   "2026-08-20T10:00:00+02:00" "2026-08-20T10:00:00-0530"
   "1969-12-31T23:59:59Z" "2020-02-29T00:00:00Z"
   "Sun, 06 Nov 1994 08:49:37 GMT" "Fri, 21 Aug 2026 04:31:22 +0200"
   "6 Nov 1994 08:49:37 GMT" "21 Aug 2026 04:31 UTC"])

(defn mutate
  "Seeded character-level mutation: replace, delete, insert, or swap."
  [s i c k]
  (let [n (count s)]
    (case (rem k 4)
      0 (if (< i n) (str (subs s 0 i) c (subs s (inc i))) s)
      1 (if (< i n) (str (subs s 0 i) (subs s (inc i))) s)
      2 (if (< i n) (str (subs s 0 i) c (subs s i)) s)
      3 (if (< i (- n 2))
         (let [a (nth s i)
               b (nth s (inc i))]
           (str (subs s 0 i) b a (subs s (+ i 2))))
         s))))

(deftest mutated-valid-strings-hold-the-contract
  (is (pq (prop/for-all [s (gen/elements valid-strings)
                         i (gen/choose 0 40)
                         c (gen/elements
                            [\0 \9 \- \: \T \. \+ \space \Z \A \\ \newline])
                         k (gen/choose 0 99)]
           (let [m (mutate s i c k)
                 r (try (parse-time m)
                        (catch e (if (= :time/parse (:mino/kind e))
                                   :ok
                                   (str "bad-kind:" (:mino/kind e)))))]
             (or (map? r) (= :ok r))))
         666003 3000)))

(deftest parsed-results-are-sane
  ;; anything that parses yields an in-range epoch and valid offset
  (is (pq (prop/for-all [s (gen/fmap (fn [[a b c d e f]]
                                       (str a "-" b "-" c "T" d ":" e
                                            ":" f "Z"))
                                     (gen/tuple (gen/choose 1000 9999)
                                                (gen/choose 1 12)
                                                (gen/choose 1 31)
                                                (gen/choose 10 23)
                                                (gen/choose 10 59)
                                                (gen/choose 10 59)))]
           (let [r (parses? s)]
             (if (nil? r)
               (= :time/parse (rejected-kind s))
               (and (>= (:epoch-ms r) ms-min)
                    (<= (:epoch-ms r) ms-max)
                    (>= (:offset-min r) -1439)
                    (<= (:offset-min r) 1439)))))
         666004 1000)))

(deftest valid-vectors-parse-back-to-themselves
  ;; the canonical battery stays parseable (regression pin)
  (is (every? parses? valid-strings)))

(run-tests-and-exit)
