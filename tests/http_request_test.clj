(require "tests/test")
(require '[clojure.string :as str])

;; Full HTTP request loop (http-request): the orchestration prim that
;; composes pool-checkout, net-connect, tls-connect, request encoding,
;; response parsing, redirects, and body decompression into one call.
;; The prim takes the already-normalized parts map and returns a
;; response map; mino.http builds on it.
;;
;; Loopback fixtures follow tests/net_test.clj: python3 on 127.0.0.1,
;; kernel-chosen port, detached stdio, an alarm so a crashed run never
;; leaks the process, kill in finally. The plain fixture is a
;; python http.server with route handlers and an accept counter; the
;; tls fixture wraps the same server in the self-signed test
;; certificate (tests/fixtures/tls). POSIX-only (os.fork); Windows
;; runs the argument-shape tests.

(def ^:private hr-posix? (nil? (getenv "OS")))

(def ^:private hr-srv-code
  "import gzip, os, signal, socket, ssl, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FIX = \"tests/fixtures/tls\"

class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, mode, countfile):
        self.nconn = 0
        self.countfile = countfile
        super().__init__(addr, Handler)
        if mode == \"tls\":
            self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.ctx.load_cert_chain(os.path.join(FIX, \"server.pem\"),
                                     os.path.join(FIX, \"server.key\"))

    def get_request(self):
        conn, addr = self.socket.accept()
        self.nconn += 1
        with open(self.countfile, \"w\") as f:
            f.write(str(self.nconn))
        if hasattr(self, \"ctx\"):
            conn = self.ctx.wrap_socket(conn, server_side=True)
        return conn, addr

def read_body(h):
    n = int(h.headers.get(\"Content-Length\", 0))
    return h.rfile.read(n) if n > 0 else b\"\"

def text(h, code, s, headers=()):
    body = s.encode(\"latin-1\") if isinstance(s, str) else s
    h.send_response(code)
    for k, v in headers:
        h.send_header(k, v)
    h.send_header(\"Content-Type\", \"text/plain\")
    h.send_header(\"Content-Length\", str(len(body)))
    h.end_headers()
    h.wfile.write(body)

def route(h):
    p = h.path
    if p == \"/hello\":
        text(h, 200, \"hello world\")
    elif p.startswith(\"/echo-path\"):
        text(h, 200, p)
    elif p == \"/echo-body\":
        text(h, 200, read_body(h))
    elif p == \"/chunked\":
        h.send_response(200)
        h.send_header(\"Transfer-Encoding\", \"chunked\")
        h.end_headers()
        h.wfile.write(b\"6\\r\\nhello \\r\\n\")
        h.wfile.write(b\"5\\r\\nworld\\r\\n\")
        h.wfile.write(b\"0\\r\\n\\r\\n\")
        h.wfile.flush()
    elif p == \"/gzip\":
        blob = gzip.compress((\"gz-payload-\" * 20).encode())
        text(h, 200, blob, [(\"Content-Encoding\", \"gzip\")])
    elif p == \"/r1\":
        text(h, 301, \"moved\", [(\"Location\", \"/r2\")])
    elif p == \"/r2\":
        text(h, 301, \"moved\", [(\"Location\", \"/final\")])
    elif p == \"/final\":
        text(h, 200, \"final-landing\")
    elif p == \"/r307\":
        # Drain the offered body: a redirect that keeps the connection
        # open must consume the request before the next one arrives.
        read_body(h)
        text(h, 307, \"keep it\", [(\"Location\", \"/echo-body\")])
    elif p == \"/close\":
        h.close_connection = True
        text(h, 200, \"bye-close\", [(\"Connection\", \"close\")])
    elif p == \"/slow\":
        time.sleep(3)
        text(h, 200, \"late\")
    elif p == \"/big\":
        text(h, 200, \"x\" * 100000)
    elif p == \"/http10\":
        h.close_connection = True
        h.wfile.write(b\"HTTP/1.0 200 OK\\r\\nContent-Type: text/plain\\r\\n\"
                      b\"\\r\\nclose-delimited-body\")
        h.wfile.flush()
    else:
        text(h, 200, \"default\")

class Handler(BaseHTTPRequestHandler):
    protocol_version = \"HTTP/1.1\"
    def do_GET(self):
        route(self)
    def do_POST(self):
        route(self)
    def do_HEAD(self):
        # HEAD answers with the headers a GET would send and never a
        # body; Content-Length describes the entity, not the wire bytes.
        if self.path == \"/head-cl\":
            self.send_response(200)
            self.send_header(\"Content-Type\", \"text/plain\")
            self.send_header(\"Content-Length\", \"11\")
            self.end_headers()
        elif self.path == \"/head-no-cl\":
            self.send_response(200)
            self.send_header(\"Content-Type\", \"text/plain\")
            self.end_headers()
        else:
            self.send_response(200)
            self.end_headers()
    def log_message(self, fmt, *args):
        pass

mode = sys.argv[1]
countfile = sys.argv[2]
srv = Server((\"127.0.0.1\", 0), mode, countfile)
port = srv.server_address[1]
pid = os.fork()
if pid == 0:
    devnull = os.open(os.devnull, os.O_RDWR)
    os.dup2(devnull, 0); os.dup2(devnull, 1); os.dup2(devnull, 2)
    os.setsid()
    signal.alarm(300)
    srv.serve_forever()
    os._exit(0)
else:
    sys.stdout.write(\"%d %d\" % (port, pid))
    sys.exit(0)")

(def ^:private hr-count-seq (atom 0))

(defn- hr-count-file []
  (str (or (getenv "TMPDIR") "/tmp/")
       "mino-http-count-" (time-ms) "-"
       (swap! hr-count-seq inc) ".txt"))

(defn- hr-start-server [mode]
  (let [path (hr-count-file)]
    (sh! "touch" path)
    (let [out (sh! "python3" "-c" hr-srv-code mode path)
          bits (str/split out #" ")]
      (when (not= 2 (count bits))
        (throw (str "http fixture printed no port line: " out)))
      {:port (parse-long (nth bits 0))
       :pid  (nth bits 1)
       :path path})))

(defn- hr-stop-server [srv]
  (when (and srv (:pid srv))
    (sh "kill" (:pid srv))))

(defn- hr-with-server
  [mode body]
  (let [srv (hr-start-server mode)]
    (try
      (body srv)
      (finally
        (hr-stop-server srv)))))

(defn- hr-conn-count [srv]
  (try (or (parse-long (str/trim (slurp (:path srv)))) 0)
       (catch e 0)))

(defn- hr-wait-for-count
  "Block until the server accepted at least `want` connections or 5 s
  pass; the accept and its file write race the client's return."
  [srv want]
  (let [t0 (time-ms)]
    (loop []
      (let [n (hr-conn-count srv)]
        (if (or (>= n want) (> (- (time-ms) t0) 5000))
          n
          (do (thread-sleep 25) (recur)))))))

(defn- hr-get
  "Minimal normalized GET against the fixture server."
  ([srv target] (hr-get srv target nil))
  ([srv target opts]
   (http-request (merge {:method "GET" :scheme :http
                         :host "127.0.0.1" :port (:port srv)
                         :target target
                         :connect-timeout 3000 :read-timeout 3000
                         :write-timeout 3000}
                        opts))))

(defn- hr-https-get
  [srv target opts]
  (http-request (merge {:method "GET" :scheme :https
                        :host "localhost" :port (:port srv)
                        :target target :insecure? true
                        :connect-timeout 3000 :read-timeout 3000
                        :write-timeout 3000}
                       opts)))

(defn- hr-text
  "Fixture bodies are ASCII; widen bytes through char for assertions."
  [b]
  (apply str (map char (seq b))))

;; ---- capability metadata and validation (no server; every platform) ----

(deftest http-request-labelled-with-net-capability
  (is (= :net (mino-capability 'http-request))))

(deftest http-request-validates-arguments
  (is (thrown? (http-request)))
  (is (= :eval/type
         (try (http-request 42) (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (http-request "not-a-map") (catch e (:mino/kind e)))))
  (is (= :http/method
         (try (http-request {:method "GE T" :scheme :http :host "127.0.0.1"
                             :port 80 :target "/"}) (catch e (:mino/kind e)))))
  (is (= :http/method
         (try (http-request {:method :get :scheme :http :host "127.0.0.1"
                             :port 80 :target "/"}) (catch e (:mino/kind e)))))
  (is (= :http/method
         (try (http-request {:scheme :http :host "127.0.0.1"
                             :port 80 :target "/"}) (catch e (:mino/kind e)))))
  (is (= :http/request
         (try (http-request {:method "GET" :scheme :ftp :host "127.0.0.1"
                             :port 80 :target "/"}) (catch e (:mino/kind e)))))
  (is (= :http/request
         (try (http-request {:method "GET" :scheme :http :host "127.0.0.1"
                             :port 70000 :target "/"})
              (catch e (:mino/kind e)))))
  (is (= :http/request
         (try (http-request {:method "GET" :scheme :http :host 8080
                             :port 80 :target "/"}) (catch e (:mino/kind e)))))
  (is (= :http/request
         (try (http-request {:method "GET" :scheme :http :host "127.0.0.1"
                             :port 80 :target "/a b"})
              (catch e (:mino/kind e))))))

(deftest http-request-rejects-layer-owned-headers
  (doseq [n ["Host" "host" "Content-Length" "content-length"
             "Transfer-Encoding" "transfer-encoding"]]
    (is (= :http/headers
           (try (http-request {:method "GET" :scheme :http
                               :host "127.0.0.1" :port 80 :target "/"
                               :headers [[n "v"]]})
                (catch e (:mino/kind e)))
               (str n " must be rejected")))))

;; ---- loopback behaviour (POSIX) ----

(when hr-posix?
  (deftest get-plain-returns-full-response-map
    (hr-with-server "plain"
      (fn [srv]
        (let [r (hr-get srv "/hello")]
          (is (= 200 (:status r)))
          (is (= "hello world" (hr-text (:body-bytes r))))
          (is (= "1.1" (:http-version r)))
          (is (map? (:headers r)))
          (is (string? (get (:headers r) "content-type")))
          (is (false? (:from-pool? r)))
          (is (nat-int? (:request-time-ms r)))
          (is (= [] (:trace-redirects r)))
          (is (= "GET" (get (:request r) :method)))
          (is (= "/hello" (get (:request r) :target)))))))

  (deftest post-body-round-trips
    (hr-with-server "plain"
      (fn [srv]
        (let [r (http-request {:method "POST" :scheme :http
                               :host "127.0.0.1" :port (:port srv)
                               :target "/echo-body"
                               :body "upload payload 123"
                               :connect-timeout 3000 :read-timeout 3000
                               :write-timeout 3000})]
          (is (= 200 (:status r)))
          (is (= "upload payload 123" (hr-text (:body-bytes r))))))))

  (deftest chunked-response-body-dechunks
    (hr-with-server "plain"
      (fn [srv]
        (let [r (hr-get srv "/chunked")]
          (is (= 200 (:status r)))
          (is (= "hello world" (hr-text (:body-bytes r))))))))

  (deftest gzip-response-decompresses-by-default
    (hr-with-server "plain"
      (fn [srv]
        (let [r (hr-get srv "/gzip")]
          (is (= 200 (:status r)))
          (is (= (apply str (repeat 20 "gz-payload-"))
                 (hr-text (:body-bytes r))))
          (is (nil? (get r :content-encoding)))
          ;; Opt-out returns the raw gzip bytes and names the encoding.
          (let [raw (hr-get srv "/gzip" {:decompress-body? false})]
            (is (= 200 (:status raw)))
            (is (= "gzip" (get raw :content-encoding)))
            (is (not= (count (:body-bytes raw))
                      (count (:body-bytes r)))))))))

  (deftest redirect-chain-follows-and-records-trace
    (hr-with-server "plain"
      (fn [srv]
        (let [r    (hr-get srv "/r1")
              base (str "http://127.0.0.1:" (:port srv))]
          (is (= 200 (:status r)))
          (is (= "final-landing" (hr-text (:body-bytes r))))
          (is (= 2 (count (:trace-redirects r))))
          (is (= (str base "/r2") (nth (:trace-redirects r) 0)))
          (is (= (str base "/final") (nth (:trace-redirects r) 1)))))))

  (deftest redirects-off-return-the-3xx-as-data
    (hr-with-server "plain"
      (fn [srv]
        ;; Redirects off: the 3xx is data, not an error, and no trace
        ;; is recorded.
        (let [r (hr-get srv "/r1" {:follow-redirects false})]
          (is (= 301 (:status r)))
          (is (= [] (:trace-redirects r)))))))

  (deftest redirect-307-preserves-post-body
    (hr-with-server "plain"
      (fn [srv]
        (let [r (http-request {:method "POST" :scheme :http
                               :host "127.0.0.1" :port (:port srv)
                               :target "/r307" :body "307-body"
                               :connect-timeout 3000 :read-timeout 3000
                               :write-timeout 3000})]
          (is (= 200 (:status r)))
          (is (= "307-body" (hr-text (:body-bytes r))))))))

  (deftest keep-alive-reuses-pooled-connection
    (hr-with-server "plain"
      (fn [srv]
        (let [r1 (hr-get srv "/hello")
              r2 (hr-get srv "/echo-path?a=1")]
          (is (false? (:from-pool? r1)))
          (is (true? (:from-pool? r2))
              "second call must check out the pooled socket")
          (is (= "/echo-path?a=1" (hr-text (:body-bytes r2))))
          (is (= 1 (hr-wait-for-count srv 1)))
          (is (= 1 (hr-conn-count srv))
              "one connection served both requests")))))

  (deftest connection-close-header-bypasses-pool
    (hr-with-server "plain"
      (fn [srv]
        (let [r1 (hr-get srv "/close")
              r2 (hr-get srv "/close")]
          (is (= 200 (:status r1)))
          (is (false? (:from-pool? r1)))
          (is (false? (:from-pool? r2)))
          (is (= 2 (hr-wait-for-count srv 2)))))))

  (deftest read-timeout-classifies-and-fires-on-schedule
    (hr-with-server "plain"
      (fn [srv]
        (let [t0 (time-ms)
              r  (try (hr-get srv "/slow" {:read-timeout 400})
                      (catch e e))
              dt (- (time-ms) t0)]
          (is (= :net/timeout (:mino/kind r)))
          (is (< dt 2400) (str "read timeout fired at " dt " ms"))))))

  (deftest body-cap-throws-limit
    (hr-with-server "plain"
      (fn [srv]
        (let [r (try (hr-get srv "/big" {:max-bytes 1000}) (catch e e))]
          (is (= :codec/limit (:mino/kind r)))
          (is (str/includes? (:mino/message r) "cap"))))))

  (deftest dns-failure-passthrough
    (let [r (try (http-request {:method "GET" :scheme :http
                                :host "host-that-cannot-resolve.invalid"
                                :port 80 :target "/"
                                :connect-timeout 2000})
                 (catch e e))]
      (is (= :net/dns (:mino/kind r)))
      (is (str/includes? (:mino/message r) "cannot resolve"))))

  (deftest https-happy-path-against-fixture-tls-server
    (hr-with-server "tls"
      (fn [srv]
        (let [r (hr-https-get srv "/hello" nil)]
          (is (= 200 (:status r)))
          (is (= "hello world" (hr-text (:body-bytes r))))
          (is (= "1.1" (:http-version r)))))))

  (deftest https-default-verification-refuses-fixture-certificate
    ;; The fixture CA is not in the vendored Mozilla root store, so the
    ;; default verification path must refuse it (tls_test precedent).
    (hr-with-server "tls"
      (fn [srv]
        (let [r (try (http-request {:method "GET" :scheme :https
                                    :host "localhost" :port (:port srv)
                                    :target "/hello"
                                    :connect-timeout 3000
                                    :read-timeout 3000
                                    :write-timeout 3000})
                     (catch e e))]
          (is (= :tls (:mino/kind r)))
          (is (str/includes? (:mino/message r) "not trusted"))))))

  (deftest http10-close-delimited-response-parses
    (hr-with-server "plain"
      (fn [srv]
        (let [r (hr-get srv "/http10")]
          (is (= 200 (:status r)))
          (is (= "close-delimited-body" (hr-text (:body-bytes r))))
          (is (= "1.0" (:http-version r)))
          (is (false? (:from-pool? r))
              "a close-delimited response leaves nothing to pool")))))

  (deftest head-response-with-content-length-completes-after-headers
    ;; The server advertises the entity length but sends no body; a
    ;; HEAD client that opened the Content-Length body window would
    ;; starve until :net/timeout.
    (hr-with-server "plain"
      (fn [srv]
        (let [t0 (time-ms)
              r  (http-request {:method "HEAD" :scheme :http
                                :host "127.0.0.1" :port (:port srv)
                                :target "/head-cl"
                                :connect-timeout 3000 :read-timeout 1500
                                :write-timeout 3000})
              dt (- (time-ms) t0)]
          (is (= 200 (:status r)))
          (is (= "11" (get (:headers r) "content-length")))
          (is (= 0 (count (:body-bytes r))))
          (is (< dt 1200)
              (str "HEAD must not wait for body bytes (took " dt
                   " ms)"))))))

  (deftest head-response-without-content-length-completes-after-headers
    ;; No framing header at all: still complete at the blank line, not
    ;; stuck in close-framing waiting for an EOF that a keep-alive
    ;; peer never sends.
    (hr-with-server "plain"
      (fn [srv]
        (let [t0 (time-ms)
              r  (http-request {:method "HEAD" :scheme :http
                                :host "127.0.0.1" :port (:port srv)
                                :target "/head-no-cl"
                                :connect-timeout 3000 :read-timeout 1500
                                :write-timeout 3000})
              dt (- (time-ms) t0)]
          (is (= 200 (:status r)))
          (is (nil? (get (:headers r) "content-length")))
          (is (= 0 (count (:body-bytes r))))
          (is (< dt 1200)
              (str "HEAD must not wait for EOF (took " dt " ms)"))))))

  (deftest secure-request-never-reuses-insecure-pooled-session
    ;; An :insecure? request succeeds against the self-signed fixture
    ;; and pools its unverified session; a default-verified request to
    ;; the same endpoint must not send anything over that session --
    ;; it opens a fresh handshake and refuses the certificate.
    (hr-with-server "tls"
      (fn [srv]
        (let [r1 (hr-https-get srv "/hello" nil)]
          (is (= 200 (:status r1))))
        (let [r1b (hr-https-get srv "/hello" nil)]
          (is (= 200 (:status r1b)))
          (is (true? (:from-pool? r1b))
              "same-mode checkouts keep reusing the pooled session"))
        (let [r2 (try (http-request {:method "GET" :scheme :https
                                     :host "localhost" :port (:port srv)
                                     :target "/hello"
                                     :connect-timeout 3000
                                     :read-timeout 3000
                                     :write-timeout 3000})
                      (catch e e))]
          (is (= :tls (:mino/kind r2)))
          (is (str/includes? (:mino/message r2) "not trusted")
              "the verifying request must refuse the fixture cert"))
        (let [r3 (hr-https-get srv "/hello" nil)]
          (is (= 200 (:status r3))
              "insecure requests keep working after a verifying one")))))

  (deftest pooled-https-second-request-honors-its-own-read-timeout
    ;; The pooled session carries the first request's SO_RCVTIMEO on
    ;; its descriptor; the second request's tighter :read-timeout must
    ;; be re-applied and fire on schedule.
    (hr-with-server "tls"
      (fn [srv]
        (let [r1 (hr-https-get srv "/slow" {:read-timeout 8000})]
          (is (= 200 (:status r1))))
        (let [t0 (time-ms)
              r2 (try (hr-https-get srv "/slow" {:read-timeout 400})
                      (catch e e))
              dt (- (time-ms) t0)]
          (is (= :net/timeout (:mino/kind r2)))
          (is (< dt 2400)
              (str "pooled TLS read timeout fired at " dt " ms")))))))

(run-tests-and-exit)
