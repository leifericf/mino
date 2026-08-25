(ns vendor.tzdata.tools.gen-tzdata
  "Regenerate src/prim/tzdata_blob.c and tzdata_blob.h from the
  vendored zoneinfo snapshot (ADR 27).

  Runs under ./mino as the gen-tzdata task. Parses each bundle
  line's TZif v2 64-bit data block HERE, at generation time, and
  emits one compact little-endian blob compiled into the binary:
  a name-sorted zone table, deduped streams (zones with identical
  transitions, type tables, footer, and initial type share one
  stream), transitions as sign-extended 40-bit absolute seconds
  (random access, so the C lookup is a plain binary search) with
  parallel u8 type indices, and NUL-terminated POSIX footer
  strings. Nothing parses TZif at runtime.

  Deterministic by construction: sorted zone names, streams in
  first-appearance order, no timestamps. The bundle's sha256 is
  stamped into the generated header comment.

  Usage: ./mino task gen-tzdata")

(require '[clojure.string :as str])

(def ^:private bundle-path "src/vendor/tzdata/zoneinfo.bundle")
(def ^:private out-path "src/prim/tzdata_blob.c")
(def ^:private hdr-path "src/prim/tzdata_blob.h")

;; ---- byte building (eager vectors; no deep lazy chains) --------------

(defn- bappend
  "Append the bytes of coll onto vector v."
  [v coll]
  (reduce conj v coll))

(defn- le
  "Little-endian bytes of v over n bytes (two's complement)."
  [v n]
  (let [mask (- (bit-shift-left 1 (* 8 n)) 1)
        u (bit-and v mask)]
    (loop [u u i n acc []]
      (if (zero? i)
        acc
        (recur (quot u 256) (dec i) (conj acc (mod u 256)))))))

(defn- cstr [s] (bappend [] (concat (map int s) [0])))

;; ---- TZif parsing -----------------------------------------------------

(defn- u8 [b i] (nth b i))

(defn- u32be [b i]
  (+ (* (u8 b i) 16777216)
     (* (u8 b (inc i)) 65536)
     (* (u8 b (+ i 2)) 256)
     (u8 b (+ i 3))))

(defn- i64be
  [b i]
  (let [hi (u32be b i)
        lo (u32be b (+ i 4))
        shi (if (>= hi 2147483648) (- hi 4294967296) hi)]
    (+ (* shi 4294967296) lo)))

(defn- i32be [b i]
  (let [v (u32be b i)]
    (if (>= v 2147483648) (- v 4294967296) v)))

(defn- tzif-block-size
  [c]
  (let [[isutcnt isstdcnt leapcnt timecnt typecnt charcnt] c]
    (+ (* 4 timecnt) timecnt (* 6 typecnt) charcnt
       (* 8 leapcnt) isstdcnt isutcnt)))

(defn- read-counts
  "The six TZif header count fields for the block whose header
  starts at file offset off."
  [b off]
  (mapv #(u32be b (+ off 20 (* 4 %))) (range 6)))

(defn- parse-tzif
  "One zone from its TZif bytes: {:name :trans :tidx :types
  :footer :init}. Uses the v2 64-bit block (every snapshot file is
  v2+). A leading sentinel transition (< -2^40) defines the type
  in effect before the first real transition; otherwise that type
  is the first non-DST type (the RFC 8536 rule)."
  [b zname]
  (when-not (and (> (count b) 44)
                 (= "TZif" (apply str (map char (take 4 b))))
                 (>= (u8 b 4) (int \2)))
    (throw (ex-info (str "gen-tzdata: " zname " is not TZif v2+") {})))
  (let [off2 (+ 44 (tzif-block-size (read-counts b 0)))
        _ (when-not (= "TZif"
                       (apply str (map char
                                       (map #(u8 b (+ off2 %)) (range 4)))))
            (throw (ex-info (str "gen-tzdata: " zname " missing v2 block") {})))
        [isutcnt isstdcnt leapcnt timecnt typecnt charcnt]
        (read-counts b off2)
        d (+ off2 44)
        trans (mapv #(i64be b (+ d (* 8 %))) (range timecnt))
        ioff (+ d (* 8 timecnt))
        tidx (mapv #(u8 b (+ ioff %)) (range timecnt))
        toff (+ ioff timecnt)
        types (mapv #(i32be b (+ toff (* 6 %))) (range typecnt))
        isdst (mapv #(pos? (u8 b (+ toff (* 6 %) 4))) (range typecnt))
        ftr (+ toff (* 6 typecnt) charcnt (* 12 leapcnt) isstdcnt isutcnt)
        footer (if (and (< ftr (count b)) (= (u8 b ftr) 10))
                 (let [end (loop [i (inc ftr)]
                             (if (or (>= i (count b)) (= (u8 b i) 10))
                               i (recur (inc i))))]
                   (apply str (map char (map #(u8 b %)
                                             (range (inc ftr) end)))))
                 "")
        sentineled (and (seq trans) (< (first trans) -1099511627776))
        real (if sentineled [(subvec trans 1) (subvec tidx 1)]
                [trans tidx])
        init (if sentineled
               (first tidx)
               (loop [i 0]
                 (cond (>= i (count isdst)) 0
                       (not (nth isdst i)) i
                       :else (recur (inc i)))))]
    (loop [i 1]
      (when (< i (count (first real)))
        (when-not (> (nth (first real) i) (nth (first real) (dec i)))
          (throw (ex-info (str "gen-tzdata: " zname
                               " transitions not increasing") {})))
        (recur (inc i))))
    (when (or (>= (count types) 65536) (>= (count (first real)) 65536))
      (throw (ex-info (str "gen-tzdata: " zname " over table limits") {})))
    {:name zname :trans (first real) :tidx (second real)
     :types types :footer footer :init init}))

(defn- parse-bundle
  []
  (mapv (fn [line]
          (let [ti (tab-index line)]
            (when (or (<= ti 0) (= ti (dec (count line))))
              (throw (ex-info "gen-tzdata: malformed bundle line" {})))
            (let [zname (subs line 0 ti)
                  b64 (subs line (inc ti))]
              (doseq [c zname]
                (when (or (> (int c) 126) (< (int c) 32))
                  (throw (ex-info (str "gen-tzdata: non-ASCII zone name "
                                       zname) {}))))
              (parse-tzif (vec (seq (base64-decode b64))) zname))))
        (str/split-lines (slurp bundle-path))))

;; ---- stream dedup -----------------------------------------------------

(defn- stream-key [z] (select-keys z [:trans :tidx :types :footer :init]))

(defn- tab-index
  [s]
  (loop [i 0]
    (cond (>= i (count s)) -1
          (= (nth s i) \tab) i
          :else (recur (inc i)))))

(defn- stream-id
  "Index of stream k in streams by =; -1 when absent. The scan is
  quadratic in zone count but runs once at generation time."
  [k streams]
  (loop [i 0]
    (cond (>= i (count streams)) -1
          (= k (nth streams i)) i
          :else (recur (inc i)))))

(defn- assign-streams
  "Zones (name-sorted) -> streams in first-appearance order."
  [zones]
  (loop [zs zones streams []]
    (if (empty? zs)
      streams
      (let [k (stream-key (first zs))]
        (recur (rest zs)
               (if (>= (stream-id k streams) 0)
                 streams
                 (conj streams k)))))))

;; ---- blob assembly ----------------------------------------------------

(defn- build-blob
  [zones streams]
  (let [n-zones (count zones)
        n-streams (count streams)
        names-off (+ 12 (* 8 n-zones) (* 18 n-streams))
        name-offs (loop [zs zones off names-off acc []]
                    (if (empty? zs)
                      acc
                      (recur (rest zs) (+ off 1 (count (:name (first zs))))
                             (conj acc off))))
        name-blob (reduce bappend [] (map cstr (map :name zones)))
        footer-blob (reduce bappend [] (map cstr (map :footer streams)))
        footers-off (+ names-off (count name-blob))
        data-off (+ footers-off (count footer-blob))
        ;; per-stream layout: types, then transitions, then indices
        layouts (loop [ss streams off data-off acc []]
                  (if (empty? ss)
                    acc
                    (let [s (first ss)
                          types-size (* 4 (count (:types s)))
                          trans-size (* 5 (count (:trans s)))]
                      (recur (rest ss)
                             (+ off types-size trans-size (count (:trans s)))
                             (conj acc {:types-off off
                                        :trans-off (+ off types-size)})))))
        footer-offs (loop [ss streams off footers-off acc []]
                      (if (empty? ss)
                        acc
                        (recur (rest ss)
                               (+ off 1 (count (:footer (first ss))))
                               (conj acc off))))
        zone-table (loop [i 0 acc []]
                     (if (>= i n-zones)
                       acc
                       (recur (inc i)
                              (-> acc
                                  (bappend (le (nth name-offs i) 4))
                                  (bappend (le (stream-id (stream-key
                                                           (nth zones i))
                                                          streams)
                                               4))))))
        stream-table (loop [i 0 acc []]
                       (if (>= i n-streams)
                         acc
                         (let [s (nth streams i)
                               lay (nth layouts i)]
                           (recur (inc i)
                                  (-> acc
                                      (bappend (le (:types-off lay) 4))
                                      (bappend (le (:trans-off lay) 4))
                                      (bappend (le (nth footer-offs i) 4))
                                      (bappend (le (count (:types s)) 2))
                                      (bappend (le (count (:trans s)) 2))
                                      (bappend (le (:init s) 1))
                                      (bappend (le 0 1)))))))
        _ (when (not= (count stream-table) (* 18 n-streams))
            (throw (ex-info (str "stream-table size " (count stream-table)
                                 " expected " (* 18 n-streams)) {})))
        _ (when (not= (count zone-table) (* 8 n-zones))
            (throw (ex-info (str "zone-table size " (count zone-table)
                                 " expected " (* 8 n-zones)) {})))
        data (loop [i 0 acc []]
               (if (>= i n-streams)
                 acc
                 (let [s (nth streams i)]
                   (recur (inc i)
                          (-> acc
                              (bappend (mapcat #(le % 4) (:types s)))
                              (bappend (mapcat #(le % 5) (:trans s)))
                              (bappend (:tidx s)))))))]
    (-> []
        (bappend (le 0x4D5A5431 4))
        (bappend (le n-zones 4))
        (bappend (le n-streams 4))
        (bappend zone-table)
        (bappend stream-table)
        (bappend name-blob)
        (bappend footer-blob)
        (bappend data))))

(defn- hex-lines
  [blob]
  (loop [i 0 acc []]
    (if (>= i (count blob))
      acc
      (recur (+ i 16)
             (conj acc
                   (str "    "
                        (str/join ","
                                  (map #(format "0x%02x" %)
                                       (take 16 (drop i blob))))
                        ","))))))

(defn run
  []
  (let [zones (vec (sort-by :name (parse-bundle)))
        streams (assign-streams zones)
        blob (build-blob zones streams)
        body (str "/* Timezone database blob. Generated by\n"
                  " * src/vendor/tzdata/tools/gen_tzdata.clj from\n"
                  " * src/vendor/tzdata/zoneinfo.bundle; do not edit by hand.\n"
                  " *\n"
                  " * Data is public-domain IANA tzdata as compiled into\n"
                  " * TZif by the host libc (src/vendor/tzdata/README.md).\n"
                  " *\n"
                  " * Bundle sha256: " (hex-encode (sha256
                                                   (slurp bundle-path)))
                  "\n"
                  " * " (count zones) " zones, " (count streams)
                  " streams, " (count blob) " bytes.\n"
                  " */\n"
                  "#include \"tzdata_blob.h\"\n\n"
                  "const unsigned char mino_tzdata_blob[] = {\n"
                  (str/join "\n" (hex-lines blob)) "\n"
                  "};\n\n"
                  "const size_t mino_tzdata_blob_size = " (count blob)
                  ";\n")
        hdr (str "/* Declarations for the generated tzdata blob (ADR 27).\n"
                 " * Generated alongside src/prim/tzdata_blob.c; do not\n"
                 " * edit by hand.\n"
                 " */\n"
                 "#ifndef MINO_TZDATA_BLOB_H\n"
                 "#define MINO_TZDATA_BLOB_H\n\n"
                 "#include <stddef.h>\n\n"
                 "extern const unsigned char mino_tzdata_blob[];\n"
                 "extern const size_t mino_tzdata_blob_size;\n\n"
                 "#endif /* MINO_TZDATA_BLOB_H */\n")]
    (spit out-path body)
    (spit hdr-path hdr)
    (println "gen-tzdata:" out-path)
    (println "  zones:" (count zones) "streams:" (count streams)
             "blob bytes:" (count blob))))
