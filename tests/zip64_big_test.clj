(require "tests/test")
(require '[clojure.string :as str])

;; Nightly zip64 size-path lane (compression-zip campaign p6t3,
;; ADR 29): one 4 GiB + 1 zero-byte member plus a small sibling,
;; written and read whole-buffer through zip-write/zip-read. The
;; member's uncomp size (2^32 + 1, one past the classic 32-bit size
;; field) forces the vendor's automatic zip64 switch: the 64-bit
;; EOCD and its locator must appear (asserted by scan, never
;; trusted from offsets), the listing must report the exact 64-bit
;; size, the read must return byte-identical zeros, and unzip -t
;; must stream the archive clean (self-skipping when unzip is
;; absent). Whole-buffer scope means the ~4.5 GB working set IS the
;; test: this file is owned by the nightly test-zip64 task (see
;; tests/run.clj other-lane-files), never the ordinary suite.
;;
;; Memory guard: hosts that cannot map the member allocation get a
;; PRINTED recorded-acceptance note and degrade to a 256 MiB
;; sub-threshold round trip (locator asserted ABSENT) -- the zip64
;; structures themselves stay covered upstream by zip_write_test's
;; forced-zip64 goldens. Never a silent skip. Residual edge: an
;; overcommitting host can pass the allocation probe and die at
;; first page touch; the nightly step timeout is the guard there.
;;
;; Generous absolute budgets (never wall-clock ratios), ~8x the
;; land-time numbers. Land-time numbers (this host, arm64 darwin,
;; cc -O2, peak RSS 5.1 GiB):
;;   zip-write  4 GiB + 1 zeros + small member:  14.5 s
;;   zip-read   the big member back:             17.2 s
;;   zip-entries of the 2-entry zip64 directory:   0 s

(def ^:private z64-size (inc 4294967296))

(def ^:private z64-write-budget-ms 120000)
(def ^:private z64-read-budget-ms 120000)

(def ^:private z64-small (byte-array (map int "tiny")))

(defn- z64-ms
  "Run (f), return elapsed milliseconds and the value."
  [f]
  (let [t0 (nano-time)
        r (f)]
    [(quot (- (nano-time) t0) 1000000) r]))

(defn- z64-scan
  "True when the 4-byte signature occurs anywhere in b (the same
  scan discipline as the write goldens: presence and absence are
  asserted by scan)."
  [b sig]
  (let [bs (vec (seq b))]
    (loop [i 0]
      (if (> i (- (count bs) 4))
        false
        (if (= sig (subvec bs i (+ i 4)))
          true
          (recur (inc i)))))))

(def ^:private z64-eocd-sig [0x50 0x4b 0x06 0x06])
(def ^:private z64-locator-sig [0x50 0x4b 0x06 0x07])

(defn- z64-spill
  "Write the archive to path through a base64 side file (spit
  writes strings, and a 4 MB archive's base64 blows argv limits as
  a printf argument)."
  [ar path]
  (spit "/tmp/mino_zip64_big.b64" (base64-encode ar))
  (sh "sh" "-c"
      (str "base64 -d < /tmp/mino_zip64_big.b64 > " path)))

(defn- z64-fallback
  "The recorded-acceptance degradation: the host could not allocate
  the working set. Runs the 256 MiB sub-threshold shape so the lane
  still exercises the size path's boundary logic (locator must stay
  ABSENT below the switch), and says loudly why the big path was
  not run."
  []
  (println "NOTE zip64-big: host cannot allocate the ~4.5 GB working"
           "set -- recorded acceptance, degrading to the 256 MiB")
  (println "NOTE zip64-big: sub-threshold check; the zip64 structures"
           "stay covered by zip_write_test's forced goldens")
  (let [n (* 256 1048576)
        mid (byte-array n)
        [ms ar] (z64-ms #(zip-write [{:name "mid.bin" :data mid}
                                     {:name "small.txt" :data z64-small}]))
        ents (zip-entries ar)]
    (is (< ms z64-write-budget-ms)
        (str "fallback write took " ms "ms"))
    (is (= [n (count z64-small)] (mapv :size ents)))
    (is (not (z64-scan ar z64-locator-sig))
        "256 MiB stays below the auto-switch (no locator)")
    (is (not (z64-scan ar z64-eocd-sig))
        "and carries no 64-bit EOCD")
    (is (= z64-small (zip-read ar "small.txt")))))

(deftest zip64-big-member-round-trips-through-the-size-switch
  ;; The allocation probe is the memory guard: an overcommitting
  ;; host maps it lazily and commits page by page during deflate;
  ;; a constrained host throws at the map and takes the fallback.
  (let [big (try (byte-array z64-size)
                 (catch e (do (z64-fallback) nil)))]
    (when big
      (let [[wms ar] (z64-ms
                       #(zip-write [{:name "big.bin" :data big}
                                    {:name "small.txt" :data z64-small}]))
            ents (zip-entries ar)
            [rms back] (z64-ms #(zip-read ar "big.bin"
                                           {:max-bytes z64-size}))]
        (is (< wms z64-write-budget-ms)
            (str "zip-write 4 GiB member took " wms "ms; budget "
                 z64-write-budget-ms "ms"))
        (is (= z64-size (count big))
            "the member really is 4 GiB + 1 bytes")
        (is (z64-scan ar z64-eocd-sig)
            "the auto-switch emitted the 64-bit EOCD")
        (is (z64-scan ar z64-locator-sig)
            "and its locator")
        (is (= ["big.bin" "small.txt"] (mapv :name ents))
            "archive order preserved")
        (is (= [z64-size (count z64-small)] (mapv :size ents))
            "the listing reports the exact 64-bit sizes")
        (is (= :deflate (:method (nth ents 0)))
            "the big member deflated (4 GiB zeros compress small)")
        (is (< (count ar) (* 64 1048576))
            (str "the archive itself is small (" (count ar) " bytes)"))
        (is (= z64-small (zip-read ar "small.txt"))
            "the small sibling survives beside the giant")
        (is (< rms z64-read-budget-ms)
            (str "zip-read 4 GiB member took " rms "ms; budget "
                 z64-read-budget-ms "ms"))
        (is (= z64-size (count back)))
        (is (= big back) "byte-identical round trip of 4 GiB + 1")
        (if (zero? (:exit (sh "sh" "-c" "command -v unzip")))
          (let [path "/tmp/mino_zip64_big.zip"]
            (z64-spill ar path)
            (let [{:keys [exit out]} (sh "sh" "-c" (str "unzip -t " path))]
              (is (zero? exit)
                  (str "unzip -t rejected the archive: " out)))
            (sh "sh" "-c" (str "rm -f " path " /tmp/mino_zip64_big.b64")))
          (println "zip64-big: unzip absent -- unzip -t skipped"))))))

(run-tests-and-exit)
