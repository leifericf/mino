(require "tests/test")
(require '[clojure.string :as str])

;; Keep-alive connection pool (pool-checkout / pool-return /
;; pool-close-all), keyed per endpoint {:scheme :host :port} and held
;; per state. The pool never opens connections: checkout answers a
;; live idle handle or nil, and the caller connects on nil. Liveness
;; is a zero-timeout poll at checkout (a readable idle socket means
;; the peer closed or desynced); expiry is a keepalive-ms age check.
;;
;; Behaviour tests drive a python3 loopback server that counts
;; accepted connections into a file (net_test fixture style): reuse is
;; asserted as "the connection count did not grow", expiry and dead
;; peers as "it did". POSIX-only (os.fork); Windows runs the metadata
;; and argument-shape tests.
;;
;; Concurrency: one mutex per endpoint pool guards queue ops only;
;; no blocking IO runs under it (the host_threads lock discipline).
;; The state lock already serialises prims on one state, so the suite
;; exercises the queue logic single-threaded; the sanitizer lanes are
;; the leak and race oracle (pool files are not in the TSan
;; concurrency set, mirroring net_test).

(def ^:private pool-posix? (nil? (getenv "OS")))

(def ^:private pool-srv-code
  "import os, signal, socket, ssl, sys, threading, time

FIX = \"tests/fixtures/tls\"

def hold(conn):
    while True:
        data = conn.recv(65536)
        if not data:
            break

def serve(conn, mode):
    wrapped = mode == \"tls\"
    try:
        if mode == \"close\":
            # Close shortly after accept so the client socket sees EOF
            # while it sits idle in the pool.
            time.sleep(0.05)
            conn.close()
            return
        if mode == \"tls\":
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(os.path.join(FIX, \"server.pem\"),
                                os.path.join(FIX, \"server.key\"))
            conn = ctx.wrap_socket(conn, server_side=True)
        # keep / tls: hold the connection open until the peer closes.
        hold(conn)
    except (OSError, ssl.SSLError):
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
    countfile = sys.argv[2]
    nconn = 0
    while True:
        try:
            conn, _ = srv.accept()
        except OSError:
            break
        nconn += 1
        # Single accept loop: single writer, no interleaving.
        with open(countfile, \"w\") as f:
            f.write(str(nconn))
        threading.Thread(target=serve, args=(conn, mode),
                         daemon=True).start()
    os._exit(0)
else:
    sys.stdout.write(\"%d %d\" % (port, pid))
    sys.exit(0)")

(def ^:private pool-count-seq (atom 0))

(defn- pool-count-file []
  (str (or (getenv "TMPDIR") "/tmp/")
       "mino-pool-count-" (time-ms) "-"
       (swap! pool-count-seq inc) ".txt"))

(defn- pool-start-server
  "Counting loopback server in mode (keep | close). Returns the port,
  the pid, and the path the accept count is written to."
  [mode]
  (let [path (pool-count-file)]
    (sh! "touch" path)
    (let [out  (sh! "python3" "-c" pool-srv-code mode path)
          bits (str/split out #" ")]
      (when (not= 2 (count bits))
        (throw (str "pool fixture printed no port line: " out)))
      {:port (parse-long (nth bits 0))
       :pid  (nth bits 1)
       :path path})))

(defn- pool-stop-server [srv]
  (when (and srv (:pid srv))
    (sh "kill" (:pid srv))))

(defn- pool-with-server
  [mode body]
  (let [srv (pool-start-server mode)]
    (try
      (body srv)
      (finally
        (pool-stop-server srv)))))

(defn- pool-conn-count [srv]
  (try (or (parse-long (str/trim (slurp (:path srv)))) 0)
       (catch e 0)))

(defn- pool-wait-for-count
  "Block until the server has accepted at least `want` connections or
  5 s pass; returns the last seen count. The accept and its file write
  race the client's connect return, so every count assertion goes
  through here."
  [srv want]
  (let [t0 (time-ms)]
    (loop []
      (let [n (pool-conn-count srv)]
        (if (or (>= n want) (> (- (time-ms) t0) 5000))
          n
          (do (thread-sleep 25) (recur)))))))

(defn- pool-ep [srv]
  {:scheme :http :host "127.0.0.1" :port (:port srv)})

(defn- pool-borrow
  "The caller side of the pool contract: reuse when the pool has a
  live idle handle, otherwise open a fresh connection."
  [endpoint opts]
  (or (pool-checkout endpoint opts)
      (net-connect (:host endpoint) (:port endpoint))))

;; ---- capability metadata ----

(deftest pool-prims-labelled-with-net-capability
  (is (true? (mino-installed? :net)))
  (is (= :net (mino-capability 'pool-checkout)))
  (is (= :net (mino-capability 'pool-return)))
  (is (= :net (mino-capability 'pool-close-all))))

;; ---- argument shapes (no server; runs on every platform) ----

(deftest pool-prims-validate-arguments
  (is (thrown? (pool-checkout)))
  (is (= :eval/type
         (try (pool-checkout "not-a-map") (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (pool-checkout 42) (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (pool-checkout {:scheme :http :host "127.0.0.1"})
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (pool-checkout {:scheme :http :port 8080})
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (pool-checkout {:scheme :ftp :host "h" :port 21})
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (pool-checkout {:scheme :http :host "h" :port 70000})
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (pool-checkout {:scheme :http :host (apply str (repeat 300 "x"))
                              :port 80})
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (pool-checkout {:scheme :http :host "h" :port 80}
                             {:keepalive "soon"})
              (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (pool-checkout {:scheme :http :host "h" :port 80} 7)
              (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (pool-return {:scheme :http :host "h" :port 80} :keyword)
              (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (pool-return {:scheme :http :host "h" :port 80}
                           "string-not-a-socket")
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (pool-checkout {:scheme :https :host "h" :port 443
                              :insecure? "yes"})
              (catch e (:mino/kind e)))))
  (is (thrown? (pool-close-all :unexpected))))

(deftest pool-checkout-empty-pool-returns-nil
  (is (nil? (pool-checkout {:scheme :http :host "127.0.0.1" :port 9}
                           {:keepalive 5000})))
  (is (nil? (pool-checkout {:scheme :http :host "127.0.0.1" :port 9}
                           {:keepalive 0})))
  (is (nil? (pool-close-all))))

;; ---- loopback behaviour (POSIX) ----

(when pool-posix?
  (deftest two-borrows-without-return-open-two-connections
    (pool-with-server "keep"
      (fn [srv]
        (let [e (pool-ep srv)
              h1 (pool-borrow e nil)
              h2 (pool-borrow e nil)]
          (is (= :handle (type h1)))
          (is (= :handle (type h2)))
          (is (not (identical? h1 h2)))
          (is (= 2 (pool-wait-for-count srv 2)))
          (pool-return e h1 nil)
          (pool-return e h2 nil)
          (is (nil? (pool-close-all)))))))

  (deftest return-then-checkout-reuses-the-same-socket
    (pool-with-server "keep"
      (fn [srv]
        (let [e  (pool-ep srv)
              h1 (pool-borrow e nil)]
          (pool-wait-for-count srv 1)
          (is (nil? (pool-return e h1 nil)))
          (let [h2 (pool-checkout e nil)]
            (is (identical? h1 h2)
                "checkout after return must hand back the pooled handle")
            ;; No second connection was accepted.
            (is (= 1 (pool-wait-for-count srv 1)))
            (is (= 1 (pool-conn-count srv)))
            (net-close h2)
            (is (nil? (pool-close-all))))))))

  (deftest expired-entries-are-dropped-and-not-reused
    (pool-with-server "keep"
      (fn [srv]
        (let [e  (pool-ep srv)
              h1 (pool-borrow e {:keepalive 1})]
          (pool-wait-for-count srv 1)
          (pool-return e h1 {:keepalive 1})
          (thread-sleep 60)
          (is (nil? (pool-checkout e {:keepalive 1}))
              "an entry older than :keepalive must not be handed out")
          (net-close h1)
          (let [h2 (pool-borrow e {:keepalive 1})]
            (is (= 2 (pool-wait-for-count srv 2)))
            (net-close h2)
            (is (nil? (pool-close-all))))))))

  (deftest dead-sockets-are-detected-at-checkout
    ;; The server closes the connection while it sits in the pool; the
    ;; pending EOF (readable POLLIN) must fail the liveness check.
    (pool-with-server "close"
      (fn [srv]
        (let [e  (pool-ep srv)
              h1 (net-connect "127.0.0.1" (:port srv))]
          (pool-wait-for-count srv 1)
          (thread-sleep 200)
          (is (nil? (pool-return e h1 nil)))
          (is (nil? (pool-checkout e nil))
              "a peer-closed idle socket must not be handed out")
          (let [h2 (pool-borrow e nil)]
            (is (= :handle (type h2)))
            (is (= 2 (pool-wait-for-count srv 2)))
            (net-close h2)
            (is (nil? (pool-close-all))))))))

  (deftest keepalive-zero-disables-reuse-and-closes-on-return
    (pool-with-server "keep"
      (fn [srv]
        (let [e  (pool-ep srv)
              h1 (net-connect "127.0.0.1" (:port srv))]
          (pool-wait-for-count srv 1)
          (is (nil? (pool-return e h1 {:keepalive 0})))
          (is (= :net (try (net-read h1 1) (catch e (:mino/kind e))))
              "return with :keepalive 0 closes the handle")
          (is (nil? (pool-checkout e {:keepalive 0})))
          (is (= 1 (pool-conn-count srv)))
          (is (nil? (pool-close-all)))))))

  (deftest pool-close-all-closes-pooled-sockets
    (pool-with-server "keep"
      (fn [srv]
        (let [e (pool-ep srv)
              h (net-connect "127.0.0.1" (:port srv))]
          (pool-wait-for-count srv 1)
          (pool-return e h nil)
          ;; The pool roots the handle, so it survived collection.
          (gc!)
          (is (nil? (pool-close-all)))
          (is (= :net (try (net-write h "x") (catch e (:mino/kind e)))))
          ;; Idempotent: a second sweep over an empty pool is a no-op.
          (is (nil? (pool-close-all)))
          (is (= 1 (pool-conn-count srv)))))))

  (deftest pools-are-keyed-per-endpoint
    (pool-with-server "keep"
      (fn [a]
        (pool-with-server "keep"
          (fn [b]
            (let [ea (pool-ep a)
                  ha (net-connect "127.0.0.1" (:port a))]
              (pool-wait-for-count a 1)
              (pool-return ea ha nil)
              ;; Different port: a different pool, nothing to reuse.
              (is (nil? (pool-checkout (pool-ep b) nil)))
              ;; Same host and port but a different scheme is a
              ;; different endpoint too.
              (is (nil? (pool-checkout (assoc (pool-ep a) :scheme :https)
                                       nil)))
              (is (identical? ha (pool-checkout ea nil)))
              (net-close ha)
              (is (nil? (pool-close-all)))))))))

  (deftest returning-a-closed-handle-is-a-no-op
    (pool-with-server "keep"
      (fn [srv]
        (let [e (pool-ep srv)
              h (net-connect "127.0.0.1" (:port srv))]
          (pool-wait-for-count srv 1)
          (net-close h)
          (is (nil? (pool-return e h nil)))
          (is (nil? (pool-checkout e nil)))
          (is (nil? (pool-close-all)))))))

  (deftest double-return-roots-the-handle-once
    ;; Two returns without a checkout in between must not create two
    ;; entries for one descriptor: the second checkout would hand the
    ;; same socket to a second caller.
    (pool-with-server "keep"
      (fn [srv]
        (let [e (pool-ep srv)
              h (net-connect "127.0.0.1" (:port srv))]
          (pool-wait-for-count srv 1)
          (is (nil? (pool-return e h nil)))
          (is (nil? (pool-return e h nil))
              "returning an already-pooled handle is a no-op")
          (let [h2 (pool-checkout e nil)
                h3 (pool-checkout e nil)]
            (is (identical? h h2))
            (is (nil? h3) "the double return must not root a second entry")
            (net-close h2)
            (is (nil? (pool-close-all))))))))

  (deftest tls-verification-mode-partitions-the-pool
    ;; A session opened with :insecure? true must never serve a
    ;; verifying request to the same endpoint: the Authorization
    ;; headers of a default-verified call would cross an unverified
    ;; hop. Insecure checkouts reuse insecure entries.
    (pool-with-server "tls"
      (fn [srv]
        (let [secure   {:scheme :https :host "localhost" :port (:port srv)}
              insecure (assoc secure :insecure? true)
              t-opts   {:insecure? true :connect-timeout 3000
                        :read-timeout 3000 :write-timeout 3000}
              h1       (tls-connect (net-connect "127.0.0.1" (:port srv)
                                                t-opts)
                                    "localhost" t-opts)]
          (is (nil? (pool-return insecure h1 nil)))
          (is (identical? h1 (pool-checkout insecure nil))
              "an insecure entry serves the next insecure checkout")
          (is (nil? (pool-return insecure h1 nil)))
          (is (nil? (pool-checkout secure nil))
              "a verifying checkout must never reuse an insecure session")
          (is (nil? (pool-close-all)))))))

  (deftest tls-handles-pool-like-sockets
    ;; Real handshake against the self-signed fixture (tls_test
    ;; fixtures, :insecure? for local peers only).
    (pool-with-server "tls"
      (fn [srv]
        (let [e     {:scheme :https :host "localhost" :port (:port srv)}
              t-opts {:insecure? true :connect-timeout 3000
                      :read-timeout 3000 :write-timeout 3000}
              h1    (tls-connect (net-connect "127.0.0.1" (:port srv)
                                              t-opts)
                                 "localhost" t-opts)]
          (is (= :handle (type h1)))
          (is (nil? (pool-return e h1 nil)))
          (let [h2 (pool-checkout e nil)]
            (is (identical? h1 h2)
                "a pooled TLS session must be handed back whole")
            (tls-close h2)
            (is (nil? (pool-close-all)))))))))

(run-tests-and-exit)
