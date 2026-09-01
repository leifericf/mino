(require "tests/test")

;; Wait helpers: poll a TCP port or a filesystem path until available.
;;
;; (mino.wait/for-port host port opts) returns {:elapsed-ms n} when the
;; port accepts a connection within the timeout, or throws :wait/timeout.
;;
;; (mino.wait/for-path path opts) returns {:elapsed-ms n} when the path
;; exists within the timeout, or throws :wait/timeout.

(require '[mino.wait :as wait])

;;;; for-port: success

(deftest wait-for-port-returns-elapsed-ms-on-success
  ;; Start a listener, ask wait/for-port to find it.
  (let [l    (net-listen "127.0.0.1" 0 {:backlog 4})
        port (net-listener-port l)]
    (try
      (let [result (wait/for-port "127.0.0.1" port {:timeout-ms 5000 :interval-ms 10})]
        (is (map? result))
        (is (contains? result :elapsed-ms))
        (is (>= (:elapsed-ms result) 0)))
      (finally
        (try (net-close l) (catch _ nil))))))

;;;; for-port: timeout

(deftest wait-for-port-throws-timeout-when-nothing-listens
  ;; Port 1 is almost certainly not open; very short timeout.
  (let [r (try (wait/for-port "127.0.0.1" 1
                              {:timeout-ms 200 :interval-ms 50})
               (catch Throwable e e))]
    (is (map? r))
    (is (= :wait/timeout (:mino/kind r)))
    (is (= 1 (:port (:mino/data r))))
    (is (contains? (:mino/data r) :elapsed-ms))
    (is (contains? (:mino/data r) :timeout-ms))))

;;;; for-port: result map contract

(deftest wait-for-port-result-has-elapsed-ms
  (let [l    (net-listen "127.0.0.1" 0 {:backlog 4})
        port (net-listener-port l)]
    (try
      (let [result (wait/for-port "127.0.0.1" port {:timeout-ms 5000 :interval-ms 10})]
        ;; elapsed-ms is a number and is non-negative
        (is (number? (:elapsed-ms result))))
      (finally
        (try (net-close l) (catch _ nil))))))

;;;; for-path: success

(deftest wait-for-path-returns-elapsed-ms-when-file-exists
  (let [dir   (mkdtemp "mino-wait-test")
        fpath (str dir "/ready")]
    (try
      ;; Create the file immediately so for-path succeeds on the first probe.
      (spit fpath "1")
      (let [result (wait/for-path fpath {:timeout-ms 5000 :interval-ms 10})]
        (is (map? result))
        (is (contains? result :elapsed-ms))
        (is (>= (:elapsed-ms result) 0)))
      (finally
        (try (rm-rf dir) (catch _ nil))))))

;;;; for-path: timeout

(deftest wait-for-path-throws-timeout-when-path-never-appears
  (let [path "/tmp/mino-wait-test-never-appears-xyzzy42"
        _    (try (rm-rf path) (catch _ nil))
        r    (try (wait/for-path path {:timeout-ms 200 :interval-ms 50})
                  (catch Throwable e e))]
    (is (map? r))
    (is (= :wait/timeout (:mino/kind r)))
    (is (= path (:path (:mino/data r))))
    (is (contains? (:mino/data r) :elapsed-ms))
    (is (contains? (:mino/data r) :timeout-ms))))

;;;; for-path: delayed file creation

(deftest wait-for-path-succeeds-when-file-created-during-wait
  (let [dir   (mkdtemp "mino-wait-test")
        fpath (str dir "/late")]
    (try
      ;; Start a future that writes the file after a short delay.
      (future (thread-sleep 150) (spit fpath "ok"))
      (let [result (wait/for-path fpath {:timeout-ms 5000 :interval-ms 30})]
        (is (map? result))
        (is (contains? result :elapsed-ms))
        ;; elapsed is at least the delay
        (is (>= (:elapsed-ms result) 100)))
      (finally
        (try (rm-rf dir) (catch _ nil))))))

(run-tests-and-exit)
