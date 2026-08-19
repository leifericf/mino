(require "tests/test")
(require '[clojure.string :as str])

;; TLS client layer (tls-connect / tls-read / tls-read-all / tls-write /
;; tls-close) over the vendored BearSSL engine.
;;
;; Verification defaults ON: chain against the vendored Mozilla roots
;; plus SAN/CN host match, with SNI always sent. The fixture CA is
;; unknown to that root store, so the default path must refuse every
;; fixture server; :insecure? true skips chain and host checks (still
;; full TLS) so the behaviour tests can drive real handshakes against
;; the fixtures.
;;
;; Loopback servers follow tests/net_test.clj: python3 on 127.0.0.1,
;; kernel-chosen port, detached stdio, an alarm so a crashed run never
;; leaks the process, kill in finally. POSIX-only (os.fork); Windows
;; runs the argument-shape tests.

(def ^:private tls-posix? (nil? (getenv "OS")))

(def ^:private tls-srv-code
  "import os, random, signal, socket, ssl, sys, threading, time

FIX = \"tests/fixtures/tls\"

def finish(conn, wrapped):
    # A TLS peer closes with close_notify; unwrap sends ours and
    # completes the shutdown handshake. unwrap blocks until the
    # client answers, so it stays inside the per-connection daemon
    # thread. Plain TCP modes just close.
    if wrapped:
        try:
            conn.unwrap()
            return
        except (OSError, ssl.SSLError):
            pass
    try:
        conn.close()
    except OSError:
        pass

def wrap(conn, cert, key):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.path.join(FIX, cert), os.path.join(FIX, key))
    return ctx.wrap_socket(conn, server_side=True)

def serve(conn, mode, idx):
    wrapped = mode in (\"tls\", \"blob\", \"wronghost\", \"expired\",
                       \"silent\")
    try:
        if mode == \"tls\" or mode == \"blob\":
            conn = wrap(conn, \"server.pem\", \"server.key\")
            if mode == \"blob\":
                blob = bytes((i * 7 + 13) & 0xFF for i in range(10240))
                conn.sendall(blob)
            else:
                req = b\"\"
                while b\"\\r\\n\\r\\n\" not in req and len(req) < 65536:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    req += chunk
                line = req.split(b\"\\r\\n\", 1)[0].decode(\"latin-1\")
                parts = line.split(\" \")
                path = parts[1] if len(parts) > 1 else \"/\"
                body = path.encode(\"latin-1\")
                head = (\"HTTP/1.1 200 OK\\r\\nContent-Type: text/plain\\r\\n\"
                        \"Content-Length: \" + str(len(body))
                        + \"\\r\\nConnection: close\\r\\n\\r\\n\")
                conn.sendall(head.encode(\"latin-1\") + body)
        elif mode == \"wronghost\":
            conn = wrap(conn, \"wrong-host.pem\", \"wrong-host.key\")
            conn.recv(4096)
        elif mode == \"expired\":
            conn = wrap(conn, \"expired.pem\", \"expired.key\")
            conn.recv(4096)
        elif mode == \"silent\":
            conn = wrap(conn, \"server.pem\", \"server.key\")
            conn.recv(65536)
            time.sleep(5)
        elif mode == \"plain\":
            conn.sendall(b\"HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\n\"
                         b\"Connection: close\\r\\n\\r\\nhi\")
        elif mode.startswith(\"garbage:\"):
            _, glen, seed = mode.split(\":\")
            # Per-connection derived seed keeps the fuzz run
            # deterministic while every connection sends different
            # bytes.
            rng = random.Random(int(seed) * 1000003 + idx)
            conn.sendall(bytes(rng.getrandbits(8)
                               for _ in range(int(glen))))
    except (OSError, ssl.SSLError):
        pass
    finish(conn, wrapped)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((\"127.0.0.1\", 0))
srv.listen(16)
port = srv.getsockname()[1]
pid = os.fork()
if pid == 0:
    devnull = os.open(os.devnull, os.O_RDWR)
    os.dup2(devnull, 0); os.dup2(devnull, 1); os.dup2(devnull, 2)
    os.setsid()
    signal.alarm(300)
    mode = sys.argv[1]
    nconn = 0
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        # Daemon thread per connection so a stalling client never
        # blocks later ones.
        threading.Thread(target=serve, args=(conn, mode, nconn),
                         daemon=True).start()
        nconn += 1
    os._exit(0)
else:
    sys.stdout.write(\"%d %d\" % (port, pid))
    sys.exit(0)")

(defn- tls-start-server [mode]
  (let [out  (sh! "python3" "-c" tls-srv-code mode)
        bits (str/split out #" ")]
    (when (not= 2 (count bits))
      (throw (str "tls server fixture printed no port line: " out)))
    {:port (parse-long (nth bits 0))
     :pid  (nth bits 1)}))

(defn- tls-stop-server [srv]
  (when (and srv (:pid srv))
    (sh "kill" (:pid srv))))

(defn- tls-with-server
  [mode body]
  (let [srv (tls-start-server mode)]
    (try
      (body srv)
      (finally
        (tls-stop-server srv)))))

;; Fast fixtures: the behaviour suite should not sit in default
;; timeouts anywhere.
(def ^:private t-opts {:insecure? true :connect-timeout 3000
                       :read-timeout 3000 :write-timeout 3000})

(defn- bytes-text
  "Fixture responses are ASCII; map bytes through char for the
   substring assertions."
  [b]
  (apply str (map char (seq b))))

(defn- tls-socket
  "Connected, verified-skipping TLS socket against the fixture server,
   socket-arity form."
  [srv]
  (tls-connect (net-connect "127.0.0.1" (:port srv) t-opts)
               "localhost" t-opts))

;; ---- capability metadata ----

(deftest tls-prims-labelled-with-net-capability
  (is (= :net (mino-capability 'tls-connect)))
  (is (= :net (mino-capability 'tls-read)))
  (is (= :net (mino-capability 'tls-read-all)))
  (is (= :net (mino-capability 'tls-write)))
  (is (= :net (mino-capability 'tls-close))))

;; ---- argument shapes (no server; every platform) ----

(deftest tls-connect-validates-arguments
  (is (thrown? (tls-connect)))
  (is (= :eval/type
         (try (tls-connect 42 "localhost") (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (tls-connect :keyword "localhost")
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (tls-connect "127.0.0.1" "not-a-port")
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (tls-connect "127.0.0.1" :keyword)
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (tls-connect "127.0.0.1" 70000)
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (tls-connect "127.0.0.1" 80 {:read-timeout -1})
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (tls-connect "127.0.0.1" 80 {:insecure? "yes"})
              (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (tls-connect "127.0.0.1" 80 0)
              (catch e (:mino/kind e))))))

(deftest tls-io-prims-validate-socket-arguments
  (is (= :eval/type
         (try (tls-read "not-a-socket" 1) (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (tls-read-all 7) (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (tls-write 7 "x") (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (tls-close :keyword) (catch e (:mino/kind e))))))

;; ---- loopback behaviour (POSIX) ----

(when tls-posix?
  (deftest tls-get-round-trips-over-verified-skipping-handshake
    (tls-with-server "tls"
      (fn [srv]
        (let [s    (tls-socket srv)
              req  (str "GET /echo-path HTTP/1.1\r\nHost: localhost\r\n"
                        "Connection: close\r\n\r\n")]
          (is (= :handle (type s)))
          (is (= (count req) (tls-write s req)))
          (let [r (bytes-text (tls-read-all s))]
            (is (str/includes? r "HTTP/1.1 200 OK"))
            (is (str/includes? r "/echo-path")))
          (is (nil? (tls-close s)))))))

  (deftest tls-host-port-arity-connects-and-reads
    (tls-with-server "tls"
      (fn [srv]
        (let [s (tls-connect "127.0.0.1" (:port srv) t-opts)]
          (is (= :handle (type s)))
          (tls-write s (str "GET /arity HTTP/1.1\r\nHost: localhost"
                            "\r\n\r\n"))
          (is (str/includes? (bytes-text (tls-read-all s)) "/arity"))
          (is (nil? (tls-close s)))))))

  (deftest tls-default-verification-refuses-untrusted-chain
    (tls-with-server "tls"
      (fn [srv]
        (let [r (try (tls-connect (net-connect "127.0.0.1"
                                               (:port srv) t-opts)
                                  "localhost"
                                  (dissoc t-opts :insecure?))
                     (catch e e))]
          (is (= :tls (:mino/kind r)))
          (is (str/includes? (:mino/message r) "certificate"))
          (is (str/includes? (:mino/message r) "not trusted"))))))

  (deftest tls-host-port-arity-default-verification-refuses
    ;; Mirror of the socket-arity refusal above through the host+port
    ;; arity: the two arities have separate dispatch, so each needs
    ;; its own coverage of the default-verification path.
    (tls-with-server "tls"
      (fn [srv]
        (let [r (try (tls-connect "localhost" (:port srv)
                                  (dissoc t-opts :insecure?))
                     (catch e e))]
          (is (= :tls (:mino/kind r)))
          (is (str/includes? (:mino/message r) "certificate"))
          (is (str/includes? (:mino/message r) "not trusted"))))))

  (deftest tls-insecure-skips-verification-but-still-tls
    (tls-with-server "tls"
      (fn [srv]
        ;; Same server that default verification refused above.
        (let [s (tls-socket srv)]
          (tls-write s (str "GET /ok HTTP/1.1\r\nHost: localhost"
                            "\r\n\r\n"))
          (is (str/includes? (bytes-text (tls-read-all s)) "200 OK"))
          (tls-close s)))))

  (deftest tls-hostname-mismatch-refused
    (tls-with-server "wronghost"
      (fn [srv]
        (let [r (try (tls-connect (net-connect "127.0.0.1"
                                               (:port srv) t-opts)
                                  "localhost"
                                  (dissoc t-opts :insecure?))
                     (catch e e))]
          ;; Default verification: the SNI name check runs even though
          ;; the chain is also untrusted, and the certificate covers
          ;; other.example, not localhost.
          (is (= :tls (:mino/kind r)))
          (is (str/includes? (:mino/message r) "server name"))))))

  (deftest tls-expired-certificate-refused
    (tls-with-server "expired"
      (fn [srv]
        (let [r (try (tls-connect (net-connect "127.0.0.1"
                                               (:port srv) t-opts)
                                  "localhost"
                                  (dissoc t-opts :insecure?))
                     (catch e e))]
          (is (= :tls (:mino/kind r)))
          (is (str/includes? (:mino/message r) "expired"))))))

  (deftest tls-plain-http-on-tls-port-refused
    (tls-with-server "plain"
      (fn [srv]
        (let [r (try (tls-connect (net-connect "127.0.0.1"
                                               (:port srv) t-opts)
                                  "localhost"
                                  (dissoc t-opts :insecure?))
                     (catch e e))]
          (is (= :tls (:mino/kind r)))))))

  (deftest tls-byte-transfer-fidelity
    (tls-with-server "blob"
      (fn [srv]
        (let [s      (tls-socket srv)
              got    (tls-read-all s)
              expect (byte-array (mapv (fn [i] (mod (+ (* i 7) 13) 256))
                                       (range 10240)))]
          (is (= 10240 (count got)))
          (is (= expect got))
          (is (nil? (tls-close s)))))))

  (deftest tls-read-returns-short-reads-and-nil-on-eof
    (tls-with-server "blob"
      (fn [srv]
        (let [s (tls-socket srv)]
          (is (= 10 (count (tls-read s 10))))
          (is (= 4096 (count (tls-read s 4096))))
          (is (= 0 (count (tls-read s 0))))
          ;; drain the rest, then EOF
          (is (= 6134 (count (tls-read-all s 65536))))
          (is (nil? (tls-read s 1)))
          (tls-close s)))))

  (deftest tls-read-all-cap-throws-overflow
    (tls-with-server "blob"
      (fn [srv]
        (let [s (tls-socket srv)
              r (try (tls-read-all s 100) (catch e e))]
          (is (= :net/overflow (:mino/kind r)))
          (tls-close s)))))

  (deftest tls-read-timeout-classifies-and-fires-on-schedule
    (tls-with-server "silent"
      (fn [srv]
        (let [s (tls-connect (net-connect "127.0.0.1" (:port srv)
                                          {:read-timeout 400
                                           :write-timeout 2000})
                             "localhost"
                             {:insecure? true :read-timeout 400
                              :write-timeout 2000})
              t0 (time-ms)
              r  (try (do (tls-write s "GET /slow HTTP/1.1\r\n\r\n")
                          (tls-read s 10))
                      (catch e e))
              dt (- (time-ms) t0)]
          (is (= :net/timeout (:mino/kind r)))
          (is (< dt 2400) (str "tls read timeout fired at " dt " ms"))
          (tls-close s)))))

  (deftest tls-close-is-idempotent-and-marks-the-socket
    (tls-with-server "tls"
      (fn [srv]
        (let [s (tls-socket srv)]
          (tls-write s "GET /x HTTP/1.1\r\nHost: localhost\r\n\r\n")
          (is (nil? (tls-close s)))
          (is (nil? (tls-close s)))
          (is (= :tls (try (tls-read s 1) (catch e (:mino/kind e)))))
          (is (= :tls (try (tls-write s "y") (catch e (:mino/kind e)))))))))

  (deftest tls-socket-finalizer-cleans-dropped-sockets
    (tls-with-server "blob"
      (fn [srv]
        ((fn []
           (let [doomed (tls-socket srv)]
             (tls-read doomed 64)
             :dropped)))
        (gc!)
        (let [s (tls-socket srv)]
          (is (= 32 (count (tls-read s 32))))
          (is (nil? (tls-close s)))))))

  (deftest tls-seeded-garbage-handshakes-never-crash
    ;; Untrusted-input discipline: random bytes as the server's first
    ;; write must classify as a :tls error, never a crash. The garbage
    ;; mode is a plain TCP server sending len seeded-random bytes.
    (tls-with-server "garbage:64:99"
      (fn [srv]
        (let [lens [1 5 16 64 200 1000 5000]]
          (dotimes [i 2000]
            (let [len (nth lens (mod i (count lens)))
                  r   (try (tls-connect
                             (net-connect "127.0.0.1" (:port srv)
                                          {:connect-timeout 3000
                                           :read-timeout 500
                                           :write-timeout 500})
                             "localhost"
                             {:insecure? true :read-timeout 500
                              :write-timeout 500})
                            (catch e e))]
              (is (= :tls (:mino/kind r))
                  (str "iteration " i " len " len " got "
                       (pr-str r))))))))))

(run-tests-and-exit)
