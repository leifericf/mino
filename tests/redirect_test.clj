(require "tests/test")
(require '[clojure.string :as str])

;; Redirect policy (redirect-next): the pure decision the HTTP loop
;; consults between responses. Map in, map out; no sockets, no state.
;; Every branch of the policy table (status, method rewrite, Location
;; resolution, auth stripping, downgrade blocking, the count ceiling)
;; is pinned here, plus a seeded fuzz arm asserting random response
;; maps always classify into the defined action set.

(defn- rn
  "redirect-next with the loop's default opts."
  ([req resp] (redirect-next req resp nil))
  ([req resp opts] (redirect-next req resp opts)))

(defn- base-req
  ([] (base-req :get))
  ([method]
   {:method method
    :uri "https://api.example.com/v1/users?page=1"
    :headers {"authorization" "Bearer t0"
              "cookie" "session=1"
              "accept" "application/json"}}))

(defn- resp
  ([code] (resp code "/next"))
  ([code location]
   {:status code
    :headers (if location {"location" location} {})}))

;;; status gate

(deftest non-redirect-status-stops-with-not-redirect
  (doseq [code [200 201 204 299 300 304 400 404 500 599]]
    (is (= :stop (:action (rn (base-req) (resp code "/next")))
        (str "status " code)))))

(deftest redirect-status-without-location-stops-with-no-location
  (doseq [code [301 302 303 307 308]]
    (is (= :no-location
           (:reason (rn (base-req) {:status code :headers {}}))))))

(deftest empty-location-string-is-no-location
  (is (= :no-location
         (:reason (rn (base-req) (resp 302 ""))))))

(deftest missing-status-degrades-to-not-redirect
  (is (= :not-redirect (:reason (rn (base-req) {:headers {}}))))
  (is (= :not-redirect (:reason (rn (base-req) {:status :done})))))

(deftest codec-shaped-response-code-is-accepted
  ;; The loop may hand the policy the parser's map shape (:code).
  (is (= :follow
         (:action (rn (base-req) {:code 302
                                  :headers {"location" "/next"}})))))

;;; method rewriting

(deftest post-301-and-302-rewrite-to-get-and-drop-the-body
  (doseq [code [301 302]]
    (let [r (rn (base-req :post) (resp code "/next"))]
      (is (= :follow (:action r)) (str code))
      (is (= :get (get-in r [:request :method])) (str code))
      (is (nil? (get-in r [:request :body])) (str code))
      (is (nil? (get-in r [:request :headers "content-type"]))
          (str code))
      (is (nil? (get-in r [:request :headers "content-length"]))
          (str code)))))

(deftest post-303-always-becomes-get
  (let [r (rn (base-req :post) (resp 303 "/next"))]
    (is (= :get (get-in r [:request :method])))
    (is (nil? (get-in r [:request :body])))
    (is (nil? (get-in r [:request :headers "content-type"]))))
  ;; Even HEAD: 303 means "see it as GET".
  (is (= :get
         (get-in (rn (base-req :head) (resp 303 "/next"))
                 [:request :method]))))

(deftest get-and-head-are-preserved-under-301-and-302
  (doseq [method [:get :head], code [301 302]]
    (is (= method
           (get-in (rn (base-req method) (resp code "/next"))
                   [:request :method]))
        (str method " " code))))

(deftest post-307-and-put-308-preserve-method-body-and-headers
  (doseq [method [:post :put :delete :patch], code [307 308]]
    (let [req (assoc (base-req method)
                     :body "payload"
                     :headers (assoc (:headers (base-req))
                                     "content-type" "text/plain"))
          r   (rn req (resp code "/next"))]
      (is (= :follow (:action r)) (str method " " code))
      (is (= method (get-in r [:request :method])) (str method " " code))
      (is (= "payload" (get-in r [:request :body])) (str method " " code))
      (is (= "text/plain"
             (get-in r [:request :headers "content-type"]))
          (str method " " code)))))

(deftest string-methods-rewrite-like-keywords
  (let [r (rn (assoc (base-req) :method "POST") (resp 302 "/next"))]
    (is (= :get (get-in r [:request :method])))))

;;; Location resolution

(deftest relative-path-absolute-location-replaces-path-and-query
  (let [r (rn (base-req) (resp 302 "/v2/list"))]
    (is (= "https://api.example.com/v2/list"
           (get-in r [:request :uri])))
    ;; The original query does not carry over (RFC 3986 merge).
    (is (not (str/includes? (get-in r [:request :uri]) "page")))))

(deftest relative-segment-location-merges-against-the-base-path
  (is (= "https://api.example.com/v1/users2"
         (get-in (rn (base-req) (resp 302 "users2"))
                 [:request :uri])))
  (is (= "https://h.example/a/c"
         (get-in (rn {:method :get :uri "https://h.example/a/b"}
                     (resp 302 "c"))
                 [:request :uri])))
  (is (= "https://h.example/a/c"
         (get-in (rn {:method :get :uri "https://h.example/a/"}
                     (resp 302 "c"))
                 [:request :uri]))))

(deftest query-only-location-keeps-the-base-path
  (is (= "https://api.example.com/v1/users?page=2"
         (get-in (rn (base-req) (resp 302 "?page=2"))
                 [:request :uri]))))

(deftest location-query-is-preserved
  (is (= "https://api.example.com/v2/list?a=1&b=2"
         (get-in (rn (base-req) (resp 302 "/v2/list?a=1&b=2"))
                 [:request :uri])))
  (is (= "https://other.example/x?q=z"
         (get-in (rn (base-req)
                     (resp 302 "https://other.example/x?q=z"))
                 [:request :uri]))))

(deftest fragments-are-stripped-from-the-next-uri
  (is (= "https://api.example.com/next"
         (get-in (rn (base-req) (resp 302 "/next#section"))
                 [:request :uri])))
  ;; A fragment-only reference targets the base URL, query included.
  (is (= "https://api.example.com/v1/users?page=1"
         (get-in (rn (base-req) (resp 302 "#section"))
                 [:request :uri]))))

(deftest scheme-relative-location-inherits-the-base-scheme
  (is (= "https://cdn.example.com/assets/x"
         (get-in (rn (base-req) (resp 302 "//cdn.example.com/assets/x"))
                 [:request :uri])))
  (is (= "http://cdn.example.com/assets/x"
         (get-in (rn {:method :get :uri "http://api.example.com/a"}
                     (resp 302 "//cdn.example.com/assets/x"))
                 [:request :uri]))))

(deftest default-ports-normalize-away-and-custom-ports-stay
  (is (= "http://h.example/x"
         (get-in (rn {:method :get :uri "http://h.example/a"}
                     (resp 302 "http://h.example:80/x"))
                 [:request :uri])))
  (is (= "https://h.example/x"
         (get-in (rn {:method :get :uri "https://h.example/a"}
                     (resp 302 "/x"))
                 [:request :uri])))
  (is (= "http://h.example:8080/x"
         (get-in (rn {:method :get :uri "http://h.example:8080/a"}
                     (resp 302 "/x"))
                 [:request :uri])))
  (is (= "http://h.example:9090/y"
         (get-in (rn {:method :get :uri "http://h.example:9090/a"}
                     (resp 302 "http://h.example:9090/y"))
                 [:request :uri]))))

;;; security: auth stripping and downgrade blocking

(deftest cross-host-redirects-strip-authorization-and-cookie
  (let [r (rn (base-req) (resp 302 "https://evil.example/steal"))]
    (is (= :follow (:action r)))
    (is (nil? (get-in r [:request :headers "authorization"])))
    (is (nil? (get-in r [:request :headers "cookie"])))
    (is (= "application/json"
           (get-in r [:request :headers "accept"]))
        "unrelated headers survive")))

(deftest same-origin-redirects-keep-authorization-and-cookie
  (let [r (rn (base-req) (resp 302 "https://api.example.com/v2"))]
    (is (= "Bearer t0" (get-in r [:request :headers "authorization"])))
    (is (= "session=1" (get-in r [:request :headers "cookie"]))))
  ;; The scheme's default port is the same origin spelled out.
  (let [r (rn (base-req) (resp 302 "https://api.example.com:443/v2"))]
    (is (= "Bearer t0" (get-in r [:request :headers "authorization"])))))

(deftest port-only-change-is-cross-origin-and-strips-auth
  (let [r (rn (base-req) (resp 302 "https://api.example.com:8443/v2"))]
    (is (= :follow (:action r)))
    (is (nil? (get-in r [:request :headers "authorization"]))
        "a port change is a different origin")
    (is (nil? (get-in r [:request :headers "cookie"])))
    (is (= "application/json"
           (get-in r [:request :headers "accept"]))
        "unrelated headers survive")))

(deftest scheme-only-change-is-cross-origin-and-strips-auth
  ;; http to https on the same host may be followed, but it is a
  ;; different origin: credentials do not travel.
  (let [r (rn {:method :get :uri "http://api.example.com/a"
               :headers {"authorization" "Bearer t0"
                         "cookie" "session=1"}}
              (resp 302 "https://api.example.com/a"))]
    (is (= :follow (:action r)))
    (is (nil? (get-in r [:request :headers "authorization"])))
    (is (nil? (get-in r [:request :headers "cookie"])))))

(deftest https-to-http-downgrade-is-blocked
  (is (= :downgrade-blocked
         (:reason (rn (base-req) (resp 302 "http://api.example.com/x"))))))

(deftest http-to-https-upgrade-is-allowed
  (is (= :follow
         (:action (rn {:method :get :uri "http://api.example.com/a"}
                      (resp 302 "https://api.example.com/a"))))))

;;; option gates

(deftest follow-redirects-false-stops-with-disabled
  (is (= :disabled
         (:reason (rn (base-req) (resp 302 "/next")
                      {:follow-redirects false})))))

(deftest not-redirect-wins-over-disabled
  ;; A 200 with following disabled is simply the final response.
  (is (= :not-redirect
         (:reason (rn (base-req) (resp 200 "/next")
                      {:follow-redirects false})))))

(deftest max-redirects-ceiling-stops-at-the-boundary
  ;; count 9 with the default ceiling of 10 still follows...
  (is (= :follow
         (:action (rn (base-req) (resp 302 "/next")
                      {:redirect-count 9}))))
  ;; ...count 10 stops: the 11th response is the final one.
  (is (= :max-redirects
         (:reason (rn (base-req) (resp 302 "/next")
                      {:redirect-count 10}))))
  (is (= :max-redirects
         (:reason (rn (base-req) (resp 302 "/next")
                      {:redirect-count 10 :max-redirects 10}))))
  ;; A custom ceiling binds the same way.
  (is (= :follow
         (:action (rn (base-req) (resp 302 "/next")
                      {:redirect-count 1 :max-redirects 2}))))
  (is (= :max-redirects
         (:reason (rn (base-req) (resp 302 "/next")
                      {:redirect-count 2 :max-redirects 2})))))

;;; malformed locations degrade to data

(deftest unsupported-and-malformed-locations-stop-with-bad-location
  (doseq [loc ["ftp://files.example/x"
               "mailto:someone@example.com"
               "http://"
               "https://"
               "http://host.example:99999/x"
               "http://host.example:notaport/x"
               "http://ho st.example/x"
               "//"]]
    (is (= :bad-location
           (:reason (rn (base-req) (resp 302 loc)))
        (str "location [" loc "]")))))

(deftest repeated-location-headers-use-the-first-value
  (is (= "https://api.example.com/first"
         (get-in (rn (base-req)
                     {:status 302
                      :headers {"location" ["https://api.example.com/first"
                                            "https://api.example.com/second"]}})
                 [:request :uri]))))

;;; the rest of the request travels untouched

(deftest next-request-preserves-unrelated-keys-and-drops-the-url-alias
  (let [req (assoc (base-req)
                   :url "https://api.example.com/v1/users?page=1"
                   :timeout 5000
                   :as :json)
        r   (rn req (resp 302 "/next"))]
    (is (= 5000 (get-in r [:request :timeout])))
    (is (= :json (get-in r [:request :as])))
    (is (nil? (get-in r [:request :url]))
        "the :url alias must not survive a rewrite of :uri")))

;;; type and arity contract

(deftest redirect-next-validates-arguments
  (is (thrown? (redirect-next)))
  (is (thrown? (redirect-next (base-req))))
  (is (= :eval/type
         (try (redirect-next "not-a-map" (resp 302)) (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (redirect-next (base-req) "not-a-map") (catch e (:mino/kind e)))))
  (is (= :eval/type
         (try (redirect-next (base-req) (resp 302) 7)
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (redirect-next {:headers {}} (resp 302))
              (catch e (:mino/kind e))))
      "request without :method / :uri is rejected")
  (is (= :eval/contract
         (try (redirect-next (base-req) (resp 302)
                             {:follow-redirects "no"})
              (catch e (:mino/kind e)))))
  (is (= :eval/contract
         (try (redirect-next (base-req) (resp 302)
                             {:redirect-count "many"})
              (catch e (:mino/kind e))))))

;;; seeded fuzz: random response maps always classify

(defn- rdr-xorshift [x]
  (let [x (bit-xor x (bit-shift-left x 13))
        x (bit-xor x (unsigned-bit-shift-right x 7))
        x (bit-xor x (bit-shift-left x 17))]
    (bit-and x 0x7FFFFFFFFFFFFFFF)))

(def ^:private loc-alphabet
  (vec "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/:?#.-[]@"))

(def ^:private stop-reasons
  #{:not-redirect :disabled :max-redirects :no-location
    :bad-location :downgrade-blocked})

(deftest random-response-maps-never-crash-and-always-classify
  (let [bad (atom [])]
    (loop [i 0, x 20260819]
      (when (< i 800)
        (let [x1   (rdr-xorshift x)
              code (+ 300 (rem x1 300))
              x2   (rdr-xorshift x1)
              n    (rem x2 24)
              loc  (apply str
                         (loop [j 0, y x2, acc []]
                           (if (= j n)
                             acc
                             (let [y2 (rdr-xorshift y)]
                               (recur (inc j) y2
                                      (conj acc (nth loc-alphabet
                                                     (rem y2 (count loc-alphabet)))))))))
              r    (rn (base-req)
                       {:status code :headers {"location" loc}}
                       {:redirect-count (rem (rdr-xorshift x1) 12)})]
          (when-not (or (= :follow (:action r))
                        (and (= :stop (:action r))
                             (contains? stop-reasons (:reason r))))
            (swap! bad conj [code loc (:action r) (:reason r)]))
          (when (and (= :follow (:action r))
                     (not (map? (:request r))))
            (swap! bad conj [:no-request-map code loc]))
          (recur (inc i) (rdr-xorshift x2)))))
    (is (= [] @bad)
        "every random response classifies into the defined actions")))

(run-tests-and-exit)
