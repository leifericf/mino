(require "tests/test")
(require '[clojure.string :as str])
(require '[mino.http :as http])

;; mino.http: the plain-map client surface over the http-request prim.
;; Normalization is pinned by a mock executor (with-redefs on
;; execute-request*, the public seam); response shaping, throw policy,
;; and error translation are pinned against canned prim maps; the
;; loopback tier runs the full stack against a python3 server
;; (net_test / http_request_test fixture precedent: 127.0.0.1, chosen
;; port, detached stdio, alarm, kill in finally). POSIX-only; Windows
;; runs the mock tier.

(def ^:private hnp-posix? (nil? (getenv "OS")))

(def ^:private captured (atom nil))

(defn- mock-exec
  "Executor stub recording the prim map and answering `response`."
  [response]
  (fn [prim]
    (reset! captured prim)
    response))

(defn- via-mock
  "Runs (http/request m) against a canned executor response, returning
  [result captured-prim]."
  [m response]
  (reset! captured nil)
  (let [r (with-redefs [mino.http/execute-request* (mock-exec response)]
            (try
              (http/request m)
              (catch e e)))]
    [r @captured]))

(defn- throw-via-mock
  "Like via-mock but demands a throw; returns the thrown value."
  [m response]
  (reset! captured nil)
  (with-redefs [mino.http/execute-request* (mock-exec response)]
    (try
      (http/request m)
      (catch e e))))

(def ^:private canned-200
  {:status 200
   :headers {"content-type" "text/plain; charset=utf-8"}
   :body-bytes (byte-array (map int "hello"))
   :http-version "1.1"
   :from-pool? false
   :request-time-ms 7
   :request {}
   :trace-redirects []})

(def ^:private canned-404
  {:status 404
   :headers {"content-type" "text/plain"}
   :body-bytes (byte-array (map int "not here"))
   :http-version "1.1"
   :from-pool? false
   :request-time-ms 3
   :request {}
   :trace-redirects []})

(def ^:private canned-json
  {:status 200
   :headers {"content-type" "application/json"}
   :body-bytes (byte-array (map int "{\"a\": 1, \"b\": {\"c\": [2, 3]}}"))
   :http-version "1.1"
   :from-pool? false
   :request-time-ms 4
   :request {}
   :trace-redirects []})

(def ^:private hnp-base-headers
  {"user-agent" (str "mino/" (clojure-version))
   "accept-encoding" "gzip, deflate"})

;; ---- normalization: the prim map the executor receives ----

(deftest default-get-normalizes-to-prim-parts
  (let [[_ prim] (via-mock {:method :get :uri "http://example.com/x"}
                           canned-200)]
    (is (= {:method "GET"
            :scheme :http
            :host "example.com"
            :port 80
            :target "/x"
            :headers hnp-base-headers
            :keepalive 120000
            :connect-timeout 10000
            :read-timeout 30000
            :write-timeout 30000
            :follow-redirects true
            :max-redirects 10
            :decompress-body? true
            :insecure? false}
           prim))))

(deftest verb-sugar-builds-method-maps
  (doseq [[verb vname method] [[http/head "head" "HEAD"] [http/post "post" "POST"]
                               [http/put "put" "PUT"] [http/delete "delete" "DELETE"]
                               [http/patch "patch" "PATCH"] [http/get "get" "GET"]]]
    (reset! captured nil)
    (with-redefs [mino.http/execute-request* (mock-exec canned-200)]
      (verb "http://example.com/x"))
    (is (= method (:method @captured))
        (str vname " must send " method))))

(deftest uri-and-url-are-interchangeable-aliases
  (let [[_ p1] (via-mock {:method :get :uri "http://example.com/x"} canned-200)
        [_ p2] (via-mock {:method :get :url "http://example.com/x"} canned-200)]
    (is (= p1 p2)))
  (is (str/includes? (ex-message (throw-via-mock
                                  {:method :get
                                   :uri "http://example.com/x"
                                   :url "http://example.com/x"}
                                  canned-200))
                     "either :uri or :url")))

(deftest method-accepts-strings-and-uppercases
  (let [[_ prim] (via-mock {:method "post" :uri "http://example.com/x"}
                           canned-200)]
    (is (= "POST" (:method prim)))))

(deftest query-params-encode-into-the-target
  (let [[_ prim] (via-mock {:method :get
                            :uri "http://example.com/x"
                            :query-params {:type "backend"
                                           :tags ["clojure" "lisp"]}}
                           canned-200)]
    (is (= "/x?type=backend&tags=clojure&tags=lisp" (:target prim))))
  (let [[_ prim] (via-mock {:method :get
                            :uri "http://example.com/x"
                            :query-params {:q "a b&c"}}
                           canned-200)]
    (is (= "/x?q=a%20b%26c" (:target prim)))))

(deftest existing-query-and-params-append
  (let [[_ prim] (via-mock {:method :get
                            :uri "http://example.com/p?x=1"
                            :query-params {:y "2"}}
                           canned-200)]
    (is (= "/p?x=1&y=2" (:target prim)))))

(deftest empty-query-params-leave-the-target-alone
  (let [[_ prim] (via-mock {:method :get
                            :uri "http://example.com/p"
                            :query-params {}}
                           canned-200)]
    (is (= "/p" (:target prim)))))

(deftest nested-query-params-are-rejected
  (let [e1 (throw-via-mock {:method :get :uri "http://h/p"
                            :query-params {:a {:b "c"}}}
                           canned-200)
        e2 (throw-via-mock {:method :get :uri "http://h/p"
                            :query-params {:xs [["a"]]}}
                           canned-200)]
    (is (str/includes? (ex-message e1) "nested"))
    (is (str/includes? (ex-message e2) "nested"))))

(deftest long-query-values-stringify
  (let [[_ prim] (via-mock {:method :get :uri "http://h/p"
                            :query-params {:n 42 :m -7}}
                           canned-200)]
    (is (= "/p?n=42&m=-7" (:target prim)))))

(deftest headers-merge-with-user-winning
  (let [[_ prim] (via-mock {:method :get :uri "http://h/x"
                            :headers {:accept-encoding "identity"
                                      "X-Custom" "v"
                                      :x-keyword "kw"}}
                           canned-200)
        hs (:headers prim)]
    (is (= "identity" (clojure.core/get hs "accept-encoding")))
    (is (= "v" (clojure.core/get hs "x-custom")))
    (is (= "kw" (clojure.core/get hs "x-keyword")))
    (is (= (str "mino/" (clojure-version))
           (clojure.core/get hs "user-agent")))))

(deftest layer-owned-headers-are-rejected-with-the-owning-option
  (doseq [[name owner] [["Host" ":uri"] ["content-length" ":body"]
                        ["Transfer-Encoding" "the HTTP layer"]
                        ["transfer-encoding" "the HTTP layer"]]]
    (let [e (throw-via-mock {:method :get :uri "http://h/x"
                             :headers {name "v"}}
                            canned-200)]
      (is (str/includes? (ex-message e) owner)
          (str name " error must name " owner)))))

(deftest case-variant-duplicate-headers-are-rejected
  (let [e (throw-via-mock {:method :get :uri "http://h/x"
                           :headers {:X-Dup "a" "x-dup" "b"}}
                          canned-200)]
    (is (str/includes? (ex-message e) "case"))
    (is (str/includes? (ex-message e) ":X-Dup"))
    (is (str/includes? (ex-message e) "x-dup"))))

(deftest form-params-become-an-urlencoded-body
  (let [[_ prim] (via-mock {:method :post :uri "http://h/p"
                            :form-params {:a "b c" :n 1}}
                           canned-200)]
    (is (= "a=b%20c&n=1" (:body prim)))
    (is (= "application/x-www-form-urlencoded"
           (clojure.core/get (:headers prim) "content-type")))))

(deftest body-and-form-params-conflict
  (let [e (throw-via-mock {:method :post :uri "http://h/p"
                           :body "raw" :form-params {:a "b"}}
                          canned-200)]
    (is (str/includes? (ex-message e) "mutually exclusive"))))

(deftest body-passes-through-unencoded
  (let [[_ prim] (via-mock {:method :post :uri "http://h/p"
                            :body "{\"json\": true}"
                            :content-type :json}
                           canned-200)]
    (is (= "{\"json\": true}" (:body prim)))
    (is (= "application/json"
           (clojure.core/get (:headers prim) "content-type"))))
  (let [[_ prim] (via-mock {:method :post :uri "http://h/p" :body "plain"}
                           canned-200)]
    (is (nil? (clojure.core/get (:headers prim) "content-type"))
        "a plain body sets no content-type")))

(deftest basic-auth-encodes-the-authorization-header
  (let [expected (str "Basic " (base64-encode "u:p"))
        [_ p1] (via-mock {:method :get :uri "http://h/x"
                          :basic-auth ["u" "p"]}
                         canned-200)
        [_ p2] (via-mock {:method :get :uri "http://h/x"
                          :basic-auth "u:p"}
                         canned-200)]
    (is (= expected (clojure.core/get (:headers p1) "authorization")))
    (is (= expected (clojure.core/get (:headers p2) "authorization")))))

(deftest oauth-token-and-basic-auth-conflict
  (let [e1 (throw-via-mock {:method :get :uri "http://h/x"
                            :basic-auth ["u" "p"] :oauth-token "tk"}
                           canned-200)
        e2 (throw-via-mock {:method :get :uri "http://h/x"
                            :oauth-token "tk"
                            :headers {:authorization "Custom x"}}
                           canned-200)]
    (is (str/includes? (ex-message e1) "mutually exclusive"))
    (is (str/includes? (ex-message e2) "Authorization"))))

(deftest oauth-token-sends-bearer
  (let [[_ prim] (via-mock {:method :get :uri "http://h/x"
                            :oauth-token "tk"}
                           canned-200)]
    (is (= "Bearer tk"
           (clojure.core/get (:headers prim) "authorization")))))

(deftest accept-and-content-type-expand-media-keywords
  (let [[_ p1] (via-mock {:method :get :uri "http://h/x" :accept :json}
                        canned-200)
        [_ p2] (via-mock {:method :get :uri "http://h/x"
                          :accept "application/vnd.api+json"}
                         canned-200)
        e (throw-via-mock {:method :get :uri "http://h/x" :accept :weird}
                          canned-200)]
    (is (= "application/json" (clojure.core/get (:headers p1) "accept")))
    (is (= "application/vnd.api+json"
           (clojure.core/get (:headers p2) "accept")))
    (is (str/includes? (ex-message e) ":accept"))))

(deftest content-type-is-sent-and-validated-without-a-body
  (let [[_ prim] (via-mock {:method :get :uri "http://h/x" :content-type :json}
                          canned-200)]
    (is (= "application/json"
           (clojure.core/get (:headers prim) "content-type"))))
  (is (str/includes? (ex-message (throw-via-mock
                                   {:method :get :uri "http://h/x"
                                    :content-type :weird}
                                   canned-200))
                     ":content-type")))

(deftest timeout-and-policy-options-map-onto-the-prim
  (let [[_ prim] (via-mock {:method :get :uri "http://h/x"
                            :timeout 2500
                            :connect-timeout 500
                            :keepalive 0
                            :max-redirects 3
                            :insecure? true
                            :decompress-body? false}
                           canned-200)
        hs (:headers prim)]
    (is (= 2500 (:read-timeout prim)))
    (is (= 500 (:connect-timeout prim)))
    (is (= 0 (:keepalive prim)))
    (is (= 3 (:max-redirects prim)))
    (is (true? (:insecure? prim)))
    (is (false? (:decompress-body? prim)))
    (is (nil? (clojure.core/get hs "accept-encoding"))
        "no accept-encoding default when decompression is off")))

(deftest unknown-request-keys-are-named-in-the-error
  (let [e (throw-via-mock {:method :get :uri "http://h/x"
                           :nope 1 :also-bad 2}
                          canned-200)]
    (is (str/includes? (ex-message e) ":nope"))
    (is (str/includes? (ex-message e) ":also-bad"))))

;; ---- response shaping ----

(deftest response-is-the-plain-shape-with-string-body
  (let [[r _] (via-mock {:method :get :uri "http://h/x"} canned-200)]
    (is (= 200 (:status r)))
    (is (= "hello" (:body r)))
    (is (= {"content-type" "text/plain; charset=utf-8"} (:headers r)))
    (is (= 7 (:request-time r)))
    (is (= [] (:trace-redirects r)))))

(deftest request-echo-carries-the-canonical-uri
  (let [[r _] (via-mock {:method :get :url "http://example.com:8080/x?a=1"
                         :user-agent "probe"}
                        canned-200)
        echo (:request r)]
    (is (= "http://example.com:8080/x?a=1" (:uri echo)))
    (is (nil? (:url echo)))
    (is (= "probe" (:user-agent echo)))))

(deftest as-bytes-returns-the-raw-body
  (let [[r _] (via-mock {:method :get :uri "http://h/x" :as :bytes}
                        canned-200)]
    (is (bytes? (:body r)))
    (is (= (seq (byte-array (map int "hello"))) (seq (:body r))))))

(deftest as-json-reads-keywordized-data
  (let [[r _] (via-mock {:method :get :uri "http://h/x" :as :json}
                        canned-json)]
    (is (= {:a 1 :b {:c [2 3]}} (:body r)))))

(deftest invalid-as-coercion-throws
  (is (str/includes? (ex-message (throw-via-mock
                                  {:method :get :uri "http://h/x" :as :stream}
                                  canned-200))
                     ":as")))

(deftest error-statuses-throw-with-the-response-as-ex-data
  (let [e (throw-via-mock {:method :get :uri "http://h/missing"} canned-404)
        d (ex-data e)]
    (is (= "HTTP 404" (ex-message e)))
    (is (= 404 (:status d)))
    (is (= "not here" (:body d)))
    (is (= {"content-type" "text/plain"} (:headers d)))))

(deftest throw-false-returns-the-error-status-as-data
  (let [[r _] (via-mock {:method :get :uri "http://h/missing" :throw false}
                        canned-404)]
    (is (= 404 (:status r)))
    (is (= "not here" (:body r)))))

(deftest error-status-throws-before-body-coercion
  (let [canned (assoc canned-200
                      :status 502
                      :headers {"content-type" "text/html"}
                      :body-bytes (byte-array (map int "<html>oops</html>")))
        e (throw-via-mock {:method :get :uri "http://h/x" :as :json} canned)
        d (ex-data e)]
    (is (= "HTTP 502" (ex-message e)))
    (is (= 502 (:status d)))
    (is (bytes? (:body d)))
    (is (= (seq (byte-array (map int "<html>oops</html>"))) (seq (:body d)))
        "json coercion of an HTML error body falls back to raw bytes")))

(deftest thrown-error-body-coerces-when-it-can
  (let [canned (assoc canned-json :status 502)
        d (ex-data (throw-via-mock {:method :get :uri "http://h/x" :as :json}
                                   canned))]
    (is (= 502 (:status d)))
    (is (= {:a 1 :b {:c [2 3]}} (:body d)))))

(deftest throw-false-error-body-falls-back-to-bytes
  (let [canned (assoc canned-200
                      :status 500
                      :body-bytes (byte-array [0x47 0xFF 0x46]))
        [r _] (via-mock {:method :get :uri "http://h/x" :throw false}
                        canned)]
    (is (= 500 (:status r)))
    (is (bytes? (:body r)))
    (is (= (seq (byte-array [0x47 0xFF 0x46])) (seq (:body r))))))

(deftest success-status-body-coercion-stays-strict
  (let [canned (assoc canned-200
                      :body-bytes (byte-array (map int "<html>not json</html>")))
        e (throw-via-mock {:method :get :uri "http://h/x" :as :json} canned)]
    (is (str/includes? (ex-message e) "Unexpected character"))))

(deftest transport-errors-translate-to-error-kinds
  (doseq [[kind want] [[:net/dns :dns] [:net/connect :connect]
                       [:net/timeout :timeout] [:tls :tls]
                       [:codec/limit :http] [:net/overflow :http]
                       [:http/request :http]]]
    (let [e (with-redefs
              [mino.http/execute-request*
               (fn [_] (throw {:mino/kind kind
                               :mino/message "fixture failure"}))]
              (try
                (http/request {:method :get :uri "http://h/x"})
                (catch e e)))
          d (ex-data e)]
      (is (= want (-> d :error :kind))
          (str kind " must translate to " want))
      (is (= "fixture failure" (-> d :error :message))))))

(deftest invalid-utf-8-body-throws-on-string-coercion
  (let [canned (assoc canned-200
                      :body-bytes (byte-array [0x47 0xFF 0x46]))
        e (throw-via-mock {:method :get :uri "http://h/x"} canned)]
    (is (str/includes? (ex-message e) "UTF-8"))))

(deftest multibyte-utf-8-bodies-decode
  (let [canned (assoc canned-200
                      :body-bytes (byte-array
                                   (concat (map int "h")
                                           [0xC3 0xA9]
                                           (map int "llo")
                                           [0xF0 0x9F 0x98 0x80])))
        [r _] (via-mock {:method :get :uri "http://h/x"} canned)]
    (is (= "héllo😀" (:body r)))))

(deftest async-returns-a-dereferable-future
  (let [resp (with-redefs
               [mino.http/execute-request* (mock-exec canned-200)]
               (let [fut (http/request {:method :get
                                        :uri "http://h/x"
                                        :async true})]
                 (deref fut)))]
    (is (= 200 (:status resp)))
    (is (= "hello" (:body resp)))))

;; ---- loopback end to end (POSIX) ----

(def ^:private hnp-srv-code
  "import gzip, json, os, signal, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, countfile):
        self.nconn = 0
        self.countfile = countfile
        super().__init__(addr, Handler)

    def get_request(self):
        conn, addr = self.socket.accept()
        self.nconn += 1
        with open(self.countfile, \"w\") as f:
            f.write(str(self.nconn))
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

def json_route(h, code, obj):
    body = json.dumps(obj).encode()
    h.send_response(code)
    h.send_header(\"Content-Type\", \"application/json\")
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
    elif p == \"/echo-headers\":
        json_route(h, 200, {\"user-agent\": h.headers.get(\"User-Agent\", \"\"),
                            \"accept\": h.headers.get(\"Accept\", \"\")})
    elif p.startswith(\"/items\"):
        if \"page=1\" in p:
            json_route(h, 200, {\"page\": 1, \"items\": [\"a\", \"b\"], \"more\": True})
        elif \"page=2\" in p:
            json_route(h, 200, {\"page\": 2, \"items\": [\"c\"], \"more\": False})
        else:
            json_route(h, 200, {\"page\": 0, \"items\": [], \"more\": True})
    elif p == \"/gzip\":
        blob = gzip.compress((\"gz-body-\" * 30).encode())
        text(h, 200, blob, [(\"Content-Encoding\", \"gzip\")])
    elif p == \"/r1\":
        text(h, 301, \"moved\", [(\"Location\", \"/r2\")])
    elif p == \"/r2\":
        text(h, 301, \"moved\", [(\"Location\", \"/final\")])
    elif p == \"/final\":
        text(h, 200, \"final-landing\")
    else:
        text(h, 404, \"not here\")

class Handler(BaseHTTPRequestHandler):
    protocol_version = \"HTTP/1.1\"
    def do_GET(self):
        route(self)
    def do_POST(self):
        route(self)
    def log_message(self, fmt, *args):
        pass

countfile = sys.argv[1]
srv = Server((\"127.0.0.1\", 0), countfile)
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

(def ^:private hnp-count-seq (atom 0))

(defn- hnp-count-file []
  (str (or (getenv "TMPDIR") "/tmp/")
       "mino-http-ns-count-" (time-ms) "-"
       (swap! hnp-count-seq inc) ".txt"))

(defn- hnp-start-server []
  (let [path (hnp-count-file)]
    (sh! "touch" path)
    (let [out (sh! "python3" "-c" hnp-srv-code path)
          bits (str/split out #" ")]
      (when (not= 2 (count bits))
        (throw (str "http ns fixture printed no port line: " out)))
      {:port (parse-long (nth bits 0))
       :pid  (nth bits 1)
       :path path})))

(defn- hnp-stop-server [srv]
  (when (and srv (:pid srv))
    (sh "kill" (:pid srv))))

(defn- hnp-with-server [body]
  (let [srv (hnp-start-server)]
    (try
      (body (str "http://127.0.0.1:" (:port srv)) srv)
      (finally
        (hnp-stop-server srv)))))

(defn- hnp-conn-count [srv]
  (try (or (parse-long (str/trim (slurp (:path srv)))) 0)
       (catch e 0)))

(defn- hnp-wait-for-count
  [srv want]
  (let [t0 (time-ms)]
    (loop []
      (let [n (hnp-conn-count srv)]
        (if (or (>= n want) (> (- (time-ms) t0) 5000))
          n
          (do (thread-sleep 25) (recur)))))))

(when hnp-posix?
  (deftest get-returns-the-response-map
    (hnp-with-server
      (fn [base _]
        (let [r (http/get (str base "/hello"))]
          (is (= 200 (:status r)))
          (is (= "hello world" (:body r)))
          (is (string? (clojure.core/get (:headers r) "content-type")))
          (is (nat-int? (:request-time r)))
          (is (= [] (:trace-redirects r)))
          (is (= (str base "/hello") (get-in r [:request :uri])))))))

  (deftest post-echoes-its-body
    (hnp-with-server
      (fn [base _]
        (let [r (http/post (str base "/echo-body") {:body "upload 123"})]
          (is (= 200 (:status r)))
          (is (= "upload 123" (:body r)))))))

  (deftest query-params-reach-the-server
    (hnp-with-server
      (fn [base _]
        (let [r (http/get (str base "/echo-path")
                          {:query-params {:type "backend"}})]
          (is (= "/echo-path?type=backend" (:body r)))))))

  (deftest form-params-post-urlencoded
    (hnp-with-server
      (fn [base _]
        (let [r (http/post (str base "/echo-body")
                           {:form-params {:a "b c" :n 1}})]
          (is (= "a=b%20c&n=1" (:body r)))))))

  (deftest headers-reach-the-server
    (hnp-with-server
      (fn [base _]
        (let [r (http/get (str base "/echo-headers")
                          {:user-agent "probe-agent" :accept :json
                           :as :json})]
          (is (= 200 (:status r)))
          (is (= "probe-agent" (:user-agent (:body r))))
          (is (= "application/json" (:accept (:body r))))))))

  (deftest not-found-throws-with-the-response-map
    (hnp-with-server
      (fn [base _]
        (let [e (try
                  (http/get (str base "/missing"))
                  (catch e e))
              d (ex-data e)]
          (is (= "HTTP 404" (ex-message e)))
          (is (= 404 (:status d)))
          (is (= "not here" (:body d)))))))

  (deftest throw-false-returns-not-found-as-data
    (hnp-with-server
      (fn [base _]
        (let [r (http/get (str base "/missing") {:throw false})]
          (is (= 404 (:status r)))
          (is (= "not here" (:body r)))))))

  (deftest redirect-chain-records-the-trace
    (hnp-with-server
      (fn [base _]
        (let [r (http/get (str base "/r1"))]
          (is (= 200 (:status r)))
          (is (= "final-landing" (:body r)))
          (is (= [(str base "/r2") (str base "/final")]
                 (:trace-redirects r)))))))

  (deftest keep-alive-serves-two-requests-over-one-connection
    (hnp-with-server
      (fn [base srv]
        (let [r1 (http/get (str base "/hello"))
              r2 (http/get (str base "/echo-path?x=1"))]
          (is (= 200 (:status r1)))
          (is (= 200 (:status r2)))
          (is (= "/echo-path?x=1" (:body r2)))
          (is (= 1 (hnp-wait-for-count srv 1)))))))

  (deftest gzip-body-decompresses-by-default
    (hnp-with-server
      (fn [base _]
        (let [r (http/get (str base "/gzip"))]
          (is (= 200 (:status r)))
          (is (= (apply str (repeat 30 "gz-body-")) (:body r)))))))

  (deftest json-bodies-read-as-keywordized-data
    (hnp-with-server
      (fn [base _]
        (let [r (http/get (str base "/items?page=1") {:as :json})]
          (is (= 200 (:status r)))
          (is (= 1 (:page (:body r))))
          (is (= ["a" "b"] (:items (:body r))))
          (is (true? (:more (:body r))))))))

  (deftest async-deref-matches-the-sync-result
    (hnp-with-server
      (fn [base _]
        (let [sync  (http/get (str base "/hello"))
              fut   (http/get (str base "/hello") {:async true})
              async (deref fut)]
          (is (= (:status sync) (:status async)))
          (is (= (:body sync) (:body async)))))))

  (deftest pmap-composes-over-plain-map-requests
    (hnp-with-server
      (fn [base _]
        (let [results (pmap http/request
                            [{:method :get :uri (str base "/hello")}
                             {:method :get :uri (str base "/final")
                              :throw false}])]
          (is (= [200 200] (map :status results)))
          (is (= ["hello world" "final-landing"] (map :body results)))))))

  (deftest bb-gist-pagination-iterates-two-pages-as-plain-maps
    (hnp-with-server
      (fn [base _]
        (let [pages (->> (range 1 3)
                         (map (fn [page]
                                (http/request
                                  {:method :get
                                   :uri (str base "/items")
                                   :headers {:accept "application/json"}
                                   :query-params {:page page}
                                   :as :json
                                   :throw false})))
                         (map :body))
              all   (mapcat :items pages)]
          (is (= [1 2] (map :page pages)))
          (is (= ["a" "b" "c"] (vec all)))
          (is (false? (:more (nth pages 1)))))))))

(run-tests-and-exit)
