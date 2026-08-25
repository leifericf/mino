(require "tests/test")
(require '[clojure.string :as str])
(require '[mino.log :as log])
(require '[mino.time :as t])

;; mino.log: the clojure.tools.logging call shape, events as one
;; data-shaped line per event on stderr. The line is pinned here as
;; data: each captured line read-strings back into the event map, and
;; the byte shape is pinned by anchored regex (the timestamp varies).
;; Capture rides the io.c *err* routing contract: bind *err* over a
;; string atom and the logger's *out* rebinding collects the line, the
;; same mechanism with-out-str uses for *out*. A subprocess probe via
;; core run (separate stdout and stderr pipes; sh merges the two)
;; pins that the default sink is the real stderr, not stdout.

(def ^:private windows? (some? (getenv "OS")))

(def ^:private bin
  (or (System/getenv "MINO_TEST_BIN")
      (when (file-exists? "./mino") "./mino")))

(defn- capture-err
  "f -> the text log writes while f runs (the *err* atom capture)."
  [f]
  (let [a (atom "")]
    (binding [*err* a]
      (f))
    @a))

(defn- event-of
  "f -> [the event line without its newline, the parsed event map].
  Asserts the one line per event shape along the way."
  [f]
  (let [out (capture-err f)
        lines (str/split-lines out)]
    (is (= 1 (count lines)) "exactly one line per event")
    (is (str/ends-with? out "\n") "the line is newline-terminated")
    [(first lines) (read-string (first lines))]))

(defn- emit-at
  "Direct macro calls per level (macro vars retrieved as values do not
  call cleanly here, so the dispatch is a case over direct forms)."
  [lvl]
  (case lvl
    :trace (log/trace "x")
    :debug (log/debug "x")
    :info (log/info "x")
    :warn (log/warn "x")
    :error (log/error "x")))

(def ^:private ex-boom (ex-info "boom" {:kind :x :attempt 3}))

;;;; the line shape

(deftest info-emits-one-data-shaped-line
  (let [[line m] (event-of (fn [] (log/info "hello")))]
    (is (re-find #"^\{:ts \"[^\"]+\" :level :info :ns \"[^\"]+\" :msg \"hello\"\}$"
                 line))
    (is (= #{:ts :level :ns :msg} (set (keys m))))
    (is (= :info (:level m)))
    (is (= "hello" (:msg m)))
    (is (= "user" (:ns m)) "ns is the calling form's namespace")))

(deftest ts-is-iso8601-utc-close-to-now
  (let [[_ m] (event-of (fn [] (log/info "tick")))
        ts (:ts m)
        p (t/parse ts)]
    (is (= :iso8601 (:format p)))
    ;; within a minute of now
    (is (< (- (t/now) (:epoch-ms p)) 60000))
    ;; the ts is a canonical formatted value: it round-trips
    (is (= ts (t/format p :iso8601)))))

(deftest newline-in-msg-stays-one-physical-line
  (let [[line m] (event-of (fn [] (log/info "two\nlines")))]
    (is (not (str/includes? line "\n")))
    (is (= "two\nlines" (:msg m)) "read-string unfolds the escape")))

(deftest non-string-msg-renders-readably
  (let [[_ m] (event-of (fn [] (log/info 42)))]
    (is (= 42 (:msg m))))
  (let [[_ m] (event-of (fn [] (log/info {:a 1 :b "x"})))]
    (is (= {:a 1 :b "x"} (:msg m)))))

(deftest logging-never-lands-in-stdout
  (is (= "" (with-out-str (log/info "not stdout"))))
  (is (= "kept\n"
         (with-out-str (println "kept")
                       (log/info "still not stdout")))))

;;;; levels and *level*

(deftest default-level-is-info
  (is (= "" (capture-err (fn [] (log/trace "t")))))
  (is (= "" (capture-err (fn [] (log/debug "d")))))
  (doseq [lvl [:info :warn :error]]
    (is (= 1 (count (str/split-lines
                      (capture-err (fn [] (emit-at lvl))))))
        (str "level " lvl " logs at the root threshold"))))

(deftest level-ordering-table
  ;; for each threshold, exactly which event levels get emitted
  (let [all [:trace :debug :info :warn :error]]
    (doseq [[threshold expected]
            {:trace [:trace :debug :info :warn :error]
             :debug [:debug :info :warn :error]
             :info [:info :warn :error]
             :warn [:warn :error]
             :error [:error]}]
      (let [got (log/with-level threshold
                  (vec (keep (fn [lvl]
                               (when-not (= "" (capture-err
                                                 (fn [] (emit-at lvl))))
                                 lvl))
                             all)))]
        (is (= expected got) (str "threshold " threshold))))))

(deftest with-level-binds-and-restores
  (is (true? (log/with-level :debug (log/enabled? :debug))))
  ;; the binding is dynamic: it ends with the body
  (is (not (log/enabled? :debug)) "root *level* untouched after the body")
  ;; plain binding is the same mechanism, emission included
  (is (= 1 (count (str/split-lines
                    (binding [*level* :trace]
                      (capture-err (fn [] (log/trace "t"))))))))
  (is (not (log/enabled? :trace))))

(deftest enabled?-ranks-against-the-current-threshold
  (is (log/enabled? :error))
  (is (log/enabled? :info))
  (is (not (log/enabled? :debug)))
  (is (log/with-level :trace (log/enabled? :trace)))
  (is (not (log/with-level :error (log/enabled? :warn)))))

(deftest unknown-threshold-reads-as-info
  (binding [*level* :verboze]
    (is (= "" (capture-err (fn [] (log/debug "d")))))
    (is (= 1 (count (str/split-lines
                      (capture-err (fn [] (log/info "i")))))))))

;;;; message formatting

(deftest multi-arg-calls-format-printf-style
  (let [[_ m] (event-of (fn [] (log/info "count=%d item=%s" 3 "x")))]
    (is (= "count=3 item=x" (:msg m))))
  (let [[_ m] (event-of (fn [] (log/warn "%s failed after %d tries" "fetch" 2)))]
    (is (= "fetch failed after 2 tries" (:msg m))))
  ;; debug formatting flows once the threshold lets debug through
  (let [[_ m] (event-of (fn [] (log/with-level :debug
                                 (log/debug "got %d" 7))))]
    (is (= "got 7" (:msg m)))))

(deftest single-arg-is-the-message-even-with-directives
  (let [[_ m] (event-of (fn [] (log/info "100%s")))]
    (is (= "100%s" (:msg m))))
  (let [[_ m] (event-of (fn [] (log/warn "plain %d message")))]
    (is (= "plain %d message" (:msg m)))))

(deftest bad-format-logs-the-raw-fmt
  (let [[line m] (event-of (fn [] (log/error "n=%d" "not-a-number")))]
    (is (= "n=%d" (:msg m)))
    (is (re-find #" :msg \"n=%d\"\}$" line)))
  (let [[_ m] (event-of (fn [] (log/warn "%s")))]
    (is (= "%s" (:msg m)) "a missing format argument logs the raw fmt"))
  ;; a non-string fmt with args is the raw value, not a throw
  (let [[_ m] (event-of (fn [] (log/error 42 1 2)))]
    (is (= 42 (:msg m)))))

(deftest log*-is-the-fn-under-the-macros
  ;; the macros expand to log* calls; higher-order use goes there
  (let [[_ m] (event-of (fn [] (log/log* :info "user" "via log*")))]
    (is (= "via log*" (:msg m)))
    (is (= :info (:level m))))
  (let [[_ m] (event-of (fn [] (log/log* :warn "user" "%s=%d" "n" 5)))]
    (is (= "n=5" (:msg m)))))

;;;; exceptions

(deftest error-with-exception-appends-ex-fields
  (let [[line m] (event-of (fn [] (log/error ex-boom "fetch failed")))]
    ;; the byte tail after the variable ts; ex-data key order is map
    ;; order, so only its extent is pinned here and the parsed value
    ;; below carries the exact data
    (is (re-find #" :level :error :ns \"[^\"]+\" :msg \"fetch failed\"" line))
    (is (re-find #" :ex-message \"boom\" :ex-data \{[^}]*\}\}$" line))
    (is (= #{:ts :level :ns :msg :ex-message :ex-data} (set (keys m))))
    (is (= :error (:level m)))
    (is (= "fetch failed" (:msg m)))
    (is (= "boom" (:ex-message m)))
    (is (= {:kind :x :attempt 3} (:ex-data m)))))

(deftest error-with-exception-and-format-args
  (let [[_ m] (event-of (fn [] (log/error ex-boom "fetch failed for %s" "url")))]
    (is (= "fetch failed for url" (:msg m)))
    (is (= "boom" (:ex-message m)))))

(deftest caught-diagnostics-carry-ex-fields
  ;; the diagnostic map a catch hands out answers ex-message/ex-data
  (let [[_ m] (event-of
                (fn []
                  (try (throw (ex-info "thrown" {:a 1}))
                       (catch c (log/error c "while handling")))))]
    (is (= "while handling" (:msg m)))
    (is (= "thrown" (:ex-message m)))
    (is (= {:a 1} (:ex-data m)))))

(deftest ex-fields-work-on-every-level
  (let [[_ m] (event-of (fn [] (log/warn ex-boom "degraded")))]
    (is (= :warn (:level m)))
    (is (= "degraded" (:msg m)))
    (is (= "boom" (:ex-message m))))
  (let [[_ m] (event-of (fn [] (log/info ex-boom "note")))]
    (is (= "note" (:msg m)))))

(deftest nil-ex-data-key-is-omitted
  (let [[line m] (event-of (fn [] (log/error (ex-info "m" nil) "ctx")))]
    (is (= #{:ts :level :ns :msg :ex-message} (set (keys m))))
    (is (= "m" (:ex-message m)))
    (is (not (str/includes? line ":ex-data")))))

(deftest non-exception-lead-is-just-the-first-arg
  ;; a map without :message or :mino/kind is data, not an exception
  (let [[_ m] (event-of (fn [] (log/error {:a 1} "ctx")))]
    (is (= {:a 1} (:msg m)))
    (is (not (contains? m :ex-message))))
  ;; a single exception arg logs the exception value readably
  (let [[_ m] (event-of (fn [] (log/error ex-boom)))]
    (is (map? (:msg m)))
    (is (= "boom" (get (:msg m) :message)))))

;;;; the stderr route itself

(deftest events-go-to-the-real-stderr-not-stdout
  ;; core run keeps stderr on its own pipe (sh merges the two); run
  ;; is POSIX-only, so skip on Windows rather than report an error
  (when (and (not windows?) bin)
    (let [dir (or (getenv "TMPDIR") "/tmp")
          f (str dir "/mino_log_probe.clj")
          other (str dir "/mino_log_probe_other.clj")]
      (spit f (str "(require '[mino.log :as log])\n"
                   "(println \"out-line\")\n"
                   "(log/info \"err-line\")\n"))
      (spit other (str "(ns probe.one)\n"
                       "(require '[mino.log :as log])\n"
                       "(log/warn \"other-ns-line\")\n"))
      (let [r (clojure.core/run {} bin f)
            r2 (clojure.core/run {} bin other)]
        (is (zero? (:exit r)) "probe script loads and runs cleanly")
        (is (= ["out-line"] (str/split-lines (str/trim (:out r)))))
        (is (not (str/includes? (:out r) ":msg")) "no event on stdout")
        (is (re-find #"^\{:ts \"[^\"]+\" :level :info :ns \"user\" :msg \"err-line\"\}$"
                     (str/trim (:err r))))
        ;; a second probe in its own namespace logs that namespace
        (is (zero? (:exit r2)))
        (is (re-find #"^\{:ts \"[^\"]+\" :level :warn :ns \"probe.one\" :msg \"other-ns-line\"\}$"
                     (str/trim (:err r2))))))))

(run-tests-and-exit)
