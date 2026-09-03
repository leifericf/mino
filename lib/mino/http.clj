(ns mino.http
  "HTTP client: plain maps in, plain maps out, one entry point plus
  verb sugar over the http-request prim.

  (require '[mino.http :as http])
  (http/get \"https://example.com/x\" {:query-params {:type \"backend\"}})
  => {:status 200 :body \"...\" :headers {...} :request-time 7
      :request {...} :trace-redirects []}

  Because requests and responses are plain maps, iteration, pagination,
  and pmap compose directly. 4xx/5xx statuses throw ex-info whose
  ex-data is the full response map; :throw false returns them as data.
  Transport failures throw ex-info with ex-data {:error {:kind ...}}
  where :kind is :net (dns, connect, tls, or timeout), :overflow (the
  response body exceeded a size cap), :codec (a compressed body could
  not be decoded), or :http (a request-shaping fault).

  Capabilities: this client needs MINO_CAP_NET (it is not in the
  default capability set, so an embedder must install it). The server
  side (mino.http-server) needs MINO_CAP_ASYNC."
  (:require [clojure.string :as str]))

;;;; Response-body cap

;; A response body the client will accumulate and decode is untrusted:
;; a hostile or broken peer could otherwise drive unbounded memory and
;; CPU without a size bound. We bound it by default. 16 MiB matches the
;; http-request prim's own default body cap (HTTP_DEFAULT_MAX_BODY_BYTES),
;; so the two layers agree rather than the client silently leaning on a
;; prim default a reader cannot see. A body past the cap fails with
;; {:error {:kind :overflow}}. A caller expecting a larger legitimate
;; download raises the ceiling with an explicit :max-bytes.
(def ^:private default-max-bytes (* 16 1024 1024))

;;;; Public API

(defn request
  "Runs one HTTP request described by a plain map and returns the
  response map {:status :body :headers :request-time :request
  :trace-redirects}.

  Keys: :method (keyword or string, default :get), :uri canonical URL
  (:url accepted as an alias), :headers map (string or keyword names,
  values strings; user headers win over layer defaults; Host,
  Content-Length, and Transfer-Encoding are computed by the layer and
  rejected here), :query-params map (string or long values; vector
  values repeat the key; nested maps are rejected), :body (string or
  bytes), :form-params map (urlencoded into the body with content-type
  application/x-www-form-urlencoded; exclusive with :body),
  :basic-auth ([user pass] or \"user:pass\"), :oauth-token string
  (Bearer; conflicts with :basic-auth and an Authorization header),
  :accept and :content-type (:json :edn :text :html :xml :form keywords
  or a full media-type string; :content-type names the body's type,
  never encodes it, and is sent even on bodyless requests),
  :content-encoding string (sent as the content-encoding header,
  naming an already-applied body encoding; the layer never encodes the
  body; conflicts with an explicit content-encoding header),
  :max-bytes (caps the response body; default 16 MiB, so an untrusted
  peer cannot drive unbounded client allocation; a body past the cap
  fails with {:error {:kind :overflow}}; raise it for a larger
  legitimate download), :as
  (:string default, :bytes, :json; :json needs
  the json capability), :throw (default true: 4xx/5xx throw ex-info
  with the response as ex-data; a 4xx/5xx body that fails the :as
  coercion is delivered as raw bytes, while a 2xx/3xx body that fails
  coercion throws the decode error), :timeout ms (the read timeout),
  :connect-timeout ms, :follow-redirects (default true),
  :max-redirects (default 10), :user-agent (default mino/<version>),
  :keepalive ms (default 120000; 0 or less sends Connection: close),
  :insecure? (skips TLS verification, local fixtures only),
  :decompress-body? (default true), :async true (returns a mino
  future; timeouts and throws fire inside deref). Unknown keys are an
  error naming them. :request in the response is the request map as
  passed with :uri canonicalized. Redirects follow 303 to GET,
  301/302 rewrite non-GET/HEAD to GET, 307/308 preserve method and
  body, each hop recorded in :trace-redirects."
  [m]
  (let [plan (normalize m)]
    (if (:async plan)
      (future (perform plan))
      (perform plan))))

(defn get
  "GET shorthand: (get uri opts?) is (request (into {:method :get :uri uri} opts))."
  ([uri] (request {:method :get :uri uri}))
  ([uri opts] (request (into {:method :get :uri uri} opts))))

(defn head
  "HEAD shorthand: (head uri opts?) sends :method :head."
  ([uri] (request {:method :head :uri uri}))
  ([uri opts] (request (into {:method :head :uri uri} opts))))

(defn post
  "POST shorthand: (post uri opts?) is (request (into {:method :post :uri uri} opts))."
  ([uri] (request {:method :post :uri uri}))
  ([uri opts] (request (into {:method :post :uri uri} opts))))

(defn put
  "PUT shorthand: (put uri opts?) is (request (into {:method :put :uri uri} opts))."
  ([uri] (request {:method :put :uri uri}))
  ([uri opts] (request (into {:method :put :uri uri} opts))))

(defn delete
  "DELETE shorthand: (delete uri opts?) is (request (into {:method :delete :uri uri} opts))."
  ([uri] (request {:method :delete :uri uri}))
  ([uri opts] (request (into {:method :delete :uri uri} opts))))

(defn patch
  "PATCH shorthand: (patch uri opts?) is (request (into {:method :patch :uri uri} opts))."
  ([uri] (request {:method :patch :uri uri}))
  ([uri opts] (request (into {:method :patch :uri uri} opts))))

(defn options
  "OPTIONS shorthand: (options uri opts?) sends :method :options."
  ([uri] (request {:method :options :uri uri}))
  ([uri opts] (request (into {:method :options :uri uri} opts))))

(defn execute-request*
  "Executor seam: runs one normalized parts map through the http-request
  prim and returns its raw response map. Public only so tests and tools
  can substitute a mock executor with with-redefs; not part of the
  request vocabulary."
  [prim]
  (http-request prim))

;;;; Vocabulary

(def ^:private allowed-keys
  #{:method :uri :url :headers :query-params :body :form-params
    :basic-auth :oauth-token :accept :content-type :content-encoding
    :as :throw :timeout :connect-timeout :follow-redirects :max-redirects
    :user-agent :keepalive :insecure? :decompress-body? :max-bytes :async})

(def ^:private media-types
  {:json "application/json"
   :edn "application/edn"
   :text "text/plain"
   :html "text/html"
   :xml "application/xml"
   :form "application/x-www-form-urlencoded"})

(def ^:private owned-headers
  {"host" "computed from :uri"
   "content-length" "computed from :body"
   "transfer-encoding" "fixed by the HTTP layer"})

;;;; Validation helpers

(defn- bad
  [msg]
  (throw {:mino/kind :http/invalid
          :mino/code "MHTV001"
          :mino/message (str "mino.http: " msg)
          :mino/data {}}))

(defn- opt-boolean
  [v k default]
  (cond (nil? v) default
        (boolean? v) v
        :else (bad (str "key :" (name k) " must be a boolean"))))

(defn- opt-long
  [v k default]
  (cond (nil? v) default
        (int? v) v
        :else (bad (str "key :" (name k) " must be an integer"))))

(defn- media-type
  [what v]
  (cond
    (string? v) v
    (keyword? v) (or (clojure.core/get media-types v)
                     (bad (str what " keyword must be one of "
                                (pr-str (vec (keys media-types)))
                                ", got " (pr-str v))))
    :else (bad (str what " must be a keyword or a string"))))

;;;; Header assembly

(defn- header-name
  [k]
  (cond (string? k) (str/lower-case k)
        (keyword? k) (str/lower-case (name k))
        :else (bad (str "header names must be strings or keywords, got "
                        (pr-str k)))))

(defn- user-headers
  [m]
  (let [hs (or (:headers m) {})]
    (when-not (map? hs)
      (bad ":headers must be a map"))
    ;; One validation pass: a case-collision is a lowercased name already
    ;; claimed, so the accumulator carries the original key for the
    ;; message and its value, projected to name->value at the end.
    (let [by-name
          (reduce (fn [acc k]
                    (let [n (header-name k)
                          v (clojure.core/get hs k)]
                      (when-let [prev (clojure.core/get acc n)]
                        (bad (str "headers " (pr-str (:key prev)) " and "
                                  (pr-str k) " differ only by case")))
                      (when (owned-headers n)
                        (bad (str "header \"" n "\" is " (owned-headers n)
                                  "; the request layer owns it")))
                      (when-not (string? v)
                        (bad (str "header values must be strings, got "
                                  (pr-str v) " for \"" n "\"")))
                      (assoc acc n {:key k :value v})))
                  {}
                  (keys hs))]
      (reduce-kv (fn [m n entry] (assoc m n (:value entry))) {} by-name))))

(defn- authorization-header
  [m user-hdrs]
  (let [basic (:basic-auth m)
        oauth (:oauth-token m)]
    (when (and basic oauth)
      (bad ":basic-auth and :oauth-token are mutually exclusive"))
    (when (and (or basic oauth) (clojure.core/get user-hdrs "authorization"))
      (bad "an Authorization header conflicts with :basic-auth and :oauth-token"))
    (cond
      basic
      {"authorization"
       (str "Basic "
            (base64-encode
             (if (vector? basic)
               (if (and (= 2 (count basic))
                        (string? (nth basic 0))
                        (string? (nth basic 1)))
                 (str (nth basic 0) ":" (nth basic 1))
                 (bad ":basic-auth vector must be [user pass] strings"))
               (if (string? basic)
                 basic
                 (bad ":basic-auth must be a [user pass] vector or a \"user:pass\" string")))))}

      oauth
      (if (string? oauth)
        {"authorization" (str "Bearer " oauth)}
        (bad ":oauth-token must be a string"))

      :else {})))

(defn- layer-headers
  [m user-hdrs]
  (let [decompress? (opt-boolean (:decompress-body? m) :decompress-body? true)
        accept (:accept m)
        ua (:user-agent m)
        ct-opt (:content-type m)
        ce-opt (:content-encoding m)
        content-type (cond ct-opt
                           {"content-type" (media-type ":content-type" ct-opt)}

                           (:form-params m)
                           {"content-type" "application/x-www-form-urlencoded"}

                           :else {})
        content-encoding (cond (nil? ce-opt) {}
                               (string? ce-opt)
                               (if (clojure.core/get user-hdrs "content-encoding")
                                 (bad (str ":content-encoding conflicts with a "
                                           "content-encoding header"))
                                 {"content-encoding" ce-opt})
                               :else (bad ":content-encoding must be a string"))]
    (when (and ua (not (string? ua)))
      (bad ":user-agent must be a string"))
    (merge {"user-agent" (or ua (str "mino/" (clojure-version)))}
           (when decompress? {"accept-encoding" "gzip, deflate"})
           (when accept {"accept" (media-type ":accept" accept)})
           (authorization-header m user-hdrs)
           content-type
           content-encoding
           user-hdrs)))

;;;; Params and body

(defn- param-key
  [k]
  (percent-encode
   (cond (keyword? k) (name k)
         (string? k) k
         :else (bad (str "param keys must be strings or keywords, got "
                         (pr-str k))))))

(defn- param-value
  [k v]
  (cond
    (string? v) (percent-encode v)
    (int? v) (str v)
    (or (map? v) (vector? v))
    (bad (str "nested params are rejected: :" (name k)
              " carries a " (if (map? v) "map" "vector") " value"))
    :else (bad (str "param values must be strings or integers: :" (name k)))))

(defn- encode-params
  [params]
  (reduce
   (fn [pairs [k v]]
     (if (vector? v)
       (reduce (fn [acc x]
                 (conj acc (str (param-key k) "=" (param-value k x))))
               pairs v)
       (conj pairs (str (param-key k) "=" (param-value k v)))))
   []
   params))

(defn- request-body
  [m]
  (let [body (:body m)
        form (:form-params m)]
    (when (and body form)
      (bad ":body and :form-params are mutually exclusive"))
    (cond
      body (do (when-not (or (string? body) (bytes? body))
                 (bad ":body must be a string or bytes"))
               body)
      form (do (when-not (map? form)
                 (bad ":form-params must be a map"))
               (str/join "&" (encode-params form)))
      :else nil)))

;;;; Normalization

(defn- method-string
  [m]
  (let [v (or (:method m) :get)]
    (cond (keyword? v) (str/upper-case (name v))
          (string? v) (str/upper-case v)
          :else (bad ":method must be a keyword or a string"))))

(defn- request-target
  [parsed m]
  (let [qp (:query-params m)]
    (when (and qp (not (map? qp)))
      (bad ":query-params must be a map"))
    (let [pairs (encode-params (or qp {}))
          existing (:query parsed)
          joined (cond
                   (and (seq pairs) (str/blank? existing))
                   (str/join "&" pairs)

                   (seq pairs)
                   (str existing "&" (str/join "&" pairs))

                   (str/blank? existing)
                   nil

                   :else existing)]
      (if joined
        (str (:path parsed) "?" joined)
        (:path parsed)))))

(defn- canonical-uri
  [parsed target]
  (let [scheme (:scheme parsed)
        port (:port parsed)
        default (if (= "https" scheme) 443 80)]
    (str scheme "://" (:host parsed)
         (if (= port default) "" (str ":" port))
         target)))

(defn- as-coercion
  [m]
  (let [v (:as m)]
    (cond (nil? v) :string
          (contains? #{:string :bytes :json} v) v
          :else (bad ":as must be one of :string :bytes :json"))))

(defn- normalize
  "Pure: one user request map into the execution plan {:prim the
  normalized http-request parts map, :echo the user map with :uri
  canonicalized, :as, :throw, :async}. Throws ex-info on any contract
  violation."
  [m]
  (when-not (map? m)
    (bad "the request must be a map"))
  (let [unknown (filter (fn [k] (not (contains? allowed-keys k))) (keys m))]
    (when (seq unknown)
      (bad (str "unknown request key(s): "
                (str/join ", " (map pr-str unknown))))))
  (let [uri (:uri m)
        url (:url m)]
    (when (and uri url)
      (bad "supply either :uri or :url, not both"))
    (when-not (string? (or uri url))
      (bad ":uri must be a string URL")))
  (let [parsed (parse-url (or (:uri m) (:url m)))
        target (request-target parsed m)
        user-hdrs (user-headers m)
        body (request-body m)
        ;; An absent or nil :max-bytes falls back to the default cap;
        ;; an explicit integer (larger or smaller) overrides it.
        max-bytes (opt-long (:max-bytes m) :max-bytes default-max-bytes)
        prim {:method (method-string m)
              :scheme (keyword (:scheme parsed))
              :host (:host parsed)
              :port (:port parsed)
              :target target
              :headers (layer-headers m user-hdrs)
              :keepalive (opt-long (:keepalive m) :keepalive 120000)
              :connect-timeout (opt-long (:connect-timeout m)
                                         :connect-timeout 10000)
              :read-timeout (opt-long (:timeout m) :timeout 30000)
              :write-timeout 30000
              :follow-redirects (opt-boolean (:follow-redirects m)
                                             :follow-redirects true)
              :max-redirects (opt-long (:max-redirects m) :max-redirects 10)
              :decompress-body? (opt-boolean (:decompress-body? m)
                                             :decompress-body? true)
              :insecure? (opt-boolean (:insecure? m) :insecure? false)
              :max-bytes max-bytes}]
    {:prim (if (nil? body) prim (assoc prim :body body))
     :echo (assoc (dissoc m :url) :uri (canonical-uri parsed target))
     :as (as-coercion m)
     :throw (opt-boolean (:throw m) :throw true)
     :async (opt-boolean (:async m) :async false)}))

;;;; Execution and response shaping

(defn- translate-kind
  "Collapses the prim's fine-grained transport kind into the client's
  four public error kinds: :net (connection-level: dns, connect, tls,
  timeout), :overflow (a size cap tripped), :codec (a compressed body
  would not decode), and :http (a request-shaping fault)."
  [kind]
  (case kind
    (:net/dns :net/connect :net/timeout :tls) :net
    (:net/overflow :codec/limit) :overflow
    (:codec/truncated :codec/magic :codec/corrupt :codec/crc
     :codec/unsupported) :codec
    :http))

(defn- perform
  [plan]
  (let [prim (try
               (execute-request* (:prim plan))
               (catch e
                 (let [msg (ex-message e)]
                   (throw (ex-info (str "mino.http: " msg)
                                   {:error {:kind (translate-kind
                                                   (:mino/kind e))
                                            :message msg}})))))]
    (let [error? (>= (:status prim) 400)]
      (if (and (:throw plan) error?)
        (throw (ex-info (str "HTTP " (:status prim))
                        (shape-response plan prim true)))
        (shape-response plan prim error?)))))

(defn- shape-response
  "Builds the public response map. Lenient mode (4xx/5xx) keeps a
  failed body coercion from masking the status: the body falls back
  to raw bytes."
  [plan prim lenient?]
  (let [bb (:body-bytes prim)
        base {:status (:status prim)
              :body (if lenient?
                      (try (coerce-body (:as plan) bb) (catch Throwable e bb))
                      (coerce-body (:as plan) bb))
              :headers (:headers prim)
              :request-time (:request-time-ms prim)
              :request (:echo plan)
              :trace-redirects (:trace-redirects prim)}]
    (if-let [ce (:content-encoding prim)]
      (assoc base :content-encoding ce)
      base)))

(defn- coerce-body
  [as bb]
  (cond
    (= as :bytes) bb
    (= as :json) (json-body (decode-utf8 bb))
    :else (decode-utf8 bb)))

(defn- json-body
  [s]
  (when-not (mino-installed? :json)
    (bad ":as :json requires the json capability (clojure.data.json is not installed)"))
  (require '[clojure.data.json :as json])
  (clojure.data.json/read-str s :key-fn keyword))

;;;; UTF-8 decoding

(defn- decode-utf8
  ;; Delegates to the bytes->string prim (strict UTF-8, one allocation,
  ;; O(n)). The prim throws {:mino/kind "eval/contract" ...} on a bad
  ;; sequence; we catch and re-throw as an http-domain error so callers
  ;; catching :http/invalid still see the expected shape.
  [b]
  (try
    (bytes->string b)
    (catch e
      (let [msg (:mino/message e "bytes->string: invalid UTF-8")]
        ;; Extract the byte offset from the prim message when present
        ;; ("bytes->string: invalid UTF-8 at byte N") so the http error
        ;; carries the same location information as the old path.
        (bad (if (string? msg)
               (str/replace msg #"^bytes->string: " "body is not valid ")
               "body is not valid UTF-8"))))))
