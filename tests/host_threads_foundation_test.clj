(require "tests/test")

;; ---------------------------------------------------------------------------
;; Host-thread API surface — foundation slice (v0.84.0).
;;
;; Cycle G4 ships in stages. This test file pins the v0.84.0 surface:
;; the per-state thread-limit knob is honored, and every
;; future/promise/deliver/thread/future-* entry point throws
;; :mino/unsupported with a message that distinguishes "host has not
;; granted threads" from "host granted, runtime impl in flight."
;; ---------------------------------------------------------------------------

(deftest mino-thread-limit-default-or-granted
  ;; Standalone `./mino` grants cpu_count after mino_install_all, so
  ;; the limit is >= 1 and typically much higher; embedders that haven't
  ;; called mino_set_thread_limit see exactly 1. The value is just an
  ;; integer either way.
  (is (integer? (mino-thread-limit)))
  (is (>= (mino-thread-limit) 1)))

(deftest mino-thread-count-zero-while-stubs-throw
  ;; No host thread can spawn yet — every future/thread entry point
  ;; throws — so the count is always 0 in this slice.
  (is (= 0 (mino-thread-count))))

(defn- thrown-with-mino-key [thunk expected-key]
  (try
    (thunk)
    (is false (str "expected throw, got value"))
    (catch e
      (let [d (ex-data e)]
        (is (= expected-key (:mino/unsupported d))
            (str "expected :mino/unsupported = " expected-key
                 ", got " d))
        (is (integer? (:mino/thread-limit d))
            "expected :mino/thread-limit in the data map")))))

(deftest future-throws-with-tagged-data
  (thrown-with-mino-key #(future (+ 1 1)) :future))

(deftest thread-throws-with-tagged-data
  (thrown-with-mino-key #(thread (+ 1 1)) :thread))

(deftest promise-throws-with-tagged-data
  (thrown-with-mino-key #(promise) :promise))

(deftest deliver-throws-with-tagged-data
  (thrown-with-mino-key #(deliver nil 1) :deliver))

(deftest future-cancel-throws-with-tagged-data
  (thrown-with-mino-key #(future-cancel nil) :future-cancel))

(deftest future-done-throws-with-tagged-data
  (thrown-with-mino-key #(future-done? nil) :future-done?))

(deftest future-cancelled-throws-with-tagged-data
  (thrown-with-mino-key #(future-cancelled? nil) :future-cancelled?))

(deftest future?-returns-false
  ;; future? is a predicate, not a throwing stub — anything user code
  ;; passes is not a future since none can be created yet.
  (is (false? (future? nil)))
  (is (false? (future? 1)))
  (is (false? (future? :x))))

(deftest message-distinguishes-grant-state
  ;; The thrown :mino/message names the grant state. We can't easily
  ;; flip the limit from script side (no setter exposed; embedders use
  ;; the C API), but we can at least verify the message is the granted
  ;; one when limit > 1 and otherwise.
  (let [msg (try (future :x) (catch e (:mino/message e)))]
    (if (> (mino-thread-limit) 1)
      (do
        (is (clojure.string/includes? msg "have been granted"))
        (is (clojure.string/includes? msg "in flight")))
      (do
        (is (clojure.string/includes? msg "not granted"))
        (is (clojure.string/includes? msg "mino_set_thread_limit"))))))
