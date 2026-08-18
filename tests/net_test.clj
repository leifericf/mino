(require "tests/test")
(require '[clojure.string :as str])

;; TCP net capability and socket prims.
;;
;; The CLI installs every capability, so the net bit is present here;
;; the sandbox preset (MINO_CAP_DEFAULT) keeps net out, pinned at the
;; C level in tests/embed_api_test.c where a state can be built
;; without it.
;;
;; Behaviour tests drive a real loopback server (python3, 127.0.0.1,
;; port 0 chosen by the kernel and printed before the parent exits).
;; The server child detaches its stdio so the spawning sh returns as
;; soon as the port is known, arms an alarm so it never outlives a
;; crashed test run by much, and is killed in a finally block. These
;; tests are POSIX-only (os.fork); Windows runs the metadata and
;; argument-shape tests.

(def ^:private posix? (nil? (getenv "OS")))

(def ^:private srv-code
  "import os, signal, socket, sys, threading, time

def serve(conn, mode):
    try:
        if mode == \"echo\":
            while True:
                data = conn.recv(65536)
                if not data:
                    break
                conn.sendall(data)
        elif mode == \"slow\":
            conn.recv(65536)
            time.sleep(5)
            conn.sendall(b\"late\")
        elif mode == \"payload\":
            conn.sendall(b\"0123456789\")
        elif mode == \"shut\":
            pass
    except OSError:
        pass
    try:
        conn.close()
    except OSError:
        pass

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((\"127.0.0.1\", 0))
srv.listen(8)
port = srv.getsockname()[1]
pid = os.fork()
if pid == 0:
    devnull = os.open(os.devnull, os.O_RDWR)
    os.dup2(devnull, 0); os.dup2(devnull, 1); os.dup2(devnull, 2)
    os.setsid()
    signal.alarm(120)
    mode = sys.argv[1]
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        # One daemon thread per connection: a client that stalls or
        # vanishes mid-conversation never blocks the accept loop, so
        # later tests always get served.
        threading.Thread(target=serve, args=(conn, mode), daemon=True).start()
    os._exit(0)
else:
    sys.stdout.write(\"%d %d\" % (port, pid))
    sys.exit(0)")

(defn- start-server
  "Start the loopback server in the given mode. Returns the port and
  the serving pid; the parent python exits immediately, so sh!
  returns as soon as the port line is printed."
  [mode]
  (let [out  (sh! "python3" "-c" srv-code mode)
        bits (str/split out #" ")]
    (when (not= 2 (count bits))
      (throw (str "server fixture printed no port line: " out)))
    {:port (parse-long (nth bits 0))
     :pid  (nth bits 1)}))

(defn- stop-server [srv]
  (when (and srv (:pid srv))
    (sh "kill" (:pid srv))))

(defn- with-server
  "Run body with a fresh server in mode; the server is killed after
  the body whether it passed, threw, or errored."
  [mode body]
  (let [srv (start-server mode)]
    (try
      (body srv)
      (finally
        (stop-server srv)))))

(defn- free-dead-port
  "A loopback port that was bound and then released, so connecting to
  it is refused rather than timing out."
  []
  (parse-long
    (sh! "python3" "-c"
         "import socket
s = socket.socket()
s.bind((\"127.0.0.1\", 0))
print(s.getsockname()[1], end=\"\")
s.close()")))

;; ---- capability metadata ----

(deftest net-capability-present-under-cli
  (is (true? (mino-installed? :net))))

(deftest net-prims-labelled-with-net-capability
  (is (= :net (mino-capability 'net-connect)))
  (is (= :net (mino-capability 'net-read)))
  (is (= :net (mino-capability 'net-read-all)))
  (is (= :net (mino-capability 'net-write)))
  (is (= :net (mino-capability 'net-close))))

;; ---- argument shapes (no server needed; runs on every platform) ----

(deftest net-connect-validates-arguments
  (is (thrown? (net-connect)))
  (is (thrown? (net-connect "127.0.0.1")))
  (is (= :eval/type
         (try (net-connect 42 80) (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (net-connect "127.0.0.1" 0) (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (net-connect "127.0.0.1" 70000) (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (net-connect "127.0.0.1" 80 {:read-timeout -1})
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (net-connect "127.0.0.1" 80 {:read-timeout "soon"})
              (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (net-connect "127.0.0.1" 80 :not-a-map)
              (catch e (:mino/kind e))))))

(deftest net-io-prims-validate-socket-arguments
  (is (= :eval/type
         (try (net-read "not-a-socket" 1) (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (net-read-all 7) (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (net-write 7 "x") (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (net-close :keyword) (catch e (:mino/kind e)))))
  (is (thrown? (net-read (byte-array 1)))))

;; ---- DNS failure (no server; the .invalid TLD is RFC-reserved to
;; ---- never resolve) ----

(deftest net-connect-dns-failure-classifies-as-net-dns
  (let [r (try (net-connect "host-that-cannot-resolve.invalid" 80
                            {:connect-timeout 2000})
               (catch e e))]
    (is (= :net/dns (:mino/kind r)))
    (is (str/includes? (:mino/message r) "cannot resolve"))))

;; ---- loopback behaviour (POSIX) ----

(when posix?
  (deftest net-connect-echo-round-trip-string
    (with-server "echo"
      (fn [srv]
        (let [s (net-connect "127.0.0.1" (:port srv))]
          (is (= :handle (type s)))
          (is (= 11 (net-write s "hello world")))
          ;; A single net-read returns what has arrived, up to n.
          (is (= "68656c6c6f" (hex-encode (net-read s 5))))
          (is (= "20776f726c64" (hex-encode (net-read s 100))))
          (is (nil? (net-close s)))))))

  (deftest net-connect-echo-round-trip-bytes
    (with-server "echo"
      (fn [srv]
        (let [s (net-connect "127.0.0.1" (:port srv))
              payload (byte-array [0 1 2 250 255])]
          (is (= 5 (net-write s payload)))
          (is (= payload (net-read s 5)))
          (is (nil? (net-close s)))))))

  (deftest net-write-encodes-strings-as-utf8
    (with-server "echo"
      (fn [srv]
        (let [s (net-connect "127.0.0.1" (:port srv))]
          ;; a (1) + é (2) + ☃ (3) bytes.
          (is (= 6 (net-write s "aé☃")))
          (is (= "61c3a9e29883" (hex-encode (net-read s 6))))
          (is (nil? (net-close s)))))))

  (deftest net-read-all-reads-until-eof
    (with-server "payload"
      (fn [srv]
        (let [s (net-connect "127.0.0.1" (:port srv))]
          (is (= "30313233343536373839" (hex-encode (net-read-all s))))
          ;; EOF seen again: the drained stream yields empty bytes.
          (is (= 0 (count (net-read-all s))))
          (is (nil? (net-close s)))))))

  (deftest net-read-all-cap-throws-overflow
    (with-server "payload"
      (fn [srv]
        (let [s (net-connect "127.0.0.1" (:port srv))
              r (try (net-read-all s {:max-bytes 4}) (catch e e))]
          (is (= :net/overflow (:mino/kind r)))
          (is (str/includes? (:mino/message r) "max-bytes"))
          (net-close s)))))

  (deftest net-read-all-cap-exact-length-stream-succeeds
    ;; The cap check is strict-after-read: a stream exactly at the
    ;; cap drains to EOF and returns; only data past the cap throws.
    (with-server "payload"
      (fn [srv]
        (let [s (net-connect "127.0.0.1" (:port srv))]
          (is (= "30313233343536373839"
                 (hex-encode (net-read-all s {:max-bytes 10}))))
          (net-close s)))))

  (deftest net-read-clean-eof-returns-nil
    (with-server "shut"
      (fn [srv]
        (let [s (net-connect "127.0.0.1" (:port srv))]
          (is (nil? (net-read s 10)))
          (is (nil? (net-close s)))))))

  (deftest net-read-timeout-classifies-and-fires-on-schedule
    (with-server "slow"
      (fn [srv]
        (let [s (net-connect "127.0.0.1" (:port srv)
                             {:read-timeout 400})
              t0 (time-ms)
              r  (try (do (net-write s "x")
                          (net-read s 10))
                      (catch e e))
              dt (- (time-ms) t0)]
          ;; The server sleeps 5s before answering; a working timeout
          ;; surfaces at ~400ms. The elapsed bound is what gives the
          ;; kind assertion teeth.
          (is (= :net/timeout (:mino/kind r)))
          (is (< dt 2400) (str "read timeout fired at " dt " ms"))
          (net-close s)))))

  (deftest net-connect-refused-classifies-as-net-connect
    (let [port (free-dead-port)
          r (try (net-connect "127.0.0.1" port {:connect-timeout 2000})
                 (catch e e))]
      (is (= :net/connect (:mino/kind r)))
      (is (str/includes? (:mino/message r) "cannot connect"))))

  (deftest net-connect-timeout-classifies-as-net-connect
    ;; A non-routable address: the SYN gets no answer, so the connect
    ;; deadline fires on the poll-wait path (the fd there was once
    ;; leaked; the sanitizer lanes are the leak oracle, this test pins
    ;; the classification). Some CI networks answer anything; a
    ;; surprise connection means the fixture cannot work there, so it
    ;; skips rather than fails.
    (let [r (try (net-connect "10.255.255.1" 81 {:connect-timeout 250})
                 (catch e e))]
      (if (= :handle (type r))
        (do (net-close r)
            (println "  skip: 10.255.255.1 is reachable on this network"))
        (do
          (is (= :net/connect (:mino/kind r)))
          (is (str/includes? (:mino/message r) "timed out"))))))

  (deftest net-close-is-idempotent-and-marks-the-socket
    (with-server "echo"
      (fn [srv]
        (let [s (net-connect "127.0.0.1" (:port srv))]
          (net-write s "x")
          (is (nil? (net-close s)))
          (is (nil? (net-close s)))
          (is (= :net
                 (try (net-read s 1) (catch e (:mino/kind e)))))
          (is (= :net
                 (try (net-write s "y") (catch e (:mino/kind e)))))
          (is (= :net
                 (try (net-read-all s) (catch e (:mino/kind e)))))))))

  (deftest net-socket-finalizer-closes-dropped-sockets
    ;; Drop an open socket and force a full collection; the handle
    ;; finalizer must close the descriptor without disturbing the
    ;; server or the runtime. A conservative stack pin may delay the
    ;; collection; the thread-per-connection server keeps serving
    ;; regardless. The sanitizer lanes are the leak oracle.
    (with-server "echo"
      (fn [srv]
        ((fn []
           (let [doomed (net-connect "127.0.0.1" (:port srv))]
             (net-write doomed "x")
             :dropped)))
        (gc!)
        (let [s (net-connect "127.0.0.1" (:port srv)
                             {:read-timeout 8000})]
          (is (= 2 (net-write s "ok")))
          (is (= "6f6b" (hex-encode (net-read s 2))))
          (is (nil? (net-close s))))))))

(run-tests-and-exit)
