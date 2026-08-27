(require "tests/test")
(require '[clojure.string :as str])
(require '[clojure.test.check :as tc])
(require '[clojure.test.check.properties :as prop])
(require '[clojure.test.check.generators :as gen])

;; parse-url: hierarchical http(s) URLs into a plain map with
;; :scheme :host :port :path :query :fragment :userinfo and
;; :explicit-port?. Vector cases pin the RFC 3986 splits; properties
;; check generated URLs against an independently built expectation and
;; garbage against the no-crash contract.

(def trials 50)

(defn url-qc [p seed]
  (:result (tc/quick-check trials p :seed seed)))

(def url-keys
  [:scheme :host :port :path :query :fragment :userinfo :explicit-port?])

(defn shape-ok? [m]
  (and (map? m)
       (= (set url-keys) (set (keys m)))
       (string? (:scheme m))
       (string? (:host m))
       (int? (:port m))
       (string? (:path m))
       (or (nil? (:query m)) (string? (:query m)))
       (or (nil? (:fragment m)) (string? (:fragment m)))
       (or (nil? (:userinfo m)) (string? (:userinfo m)))
       (boolean? (:explicit-port? m))))

(deftest minimal-http-url
  (is (= {:scheme "http" :host "example.com" :port 80 :path "/"
          :query nil :fragment nil :userinfo nil :explicit-port? false}
         (parse-url "http://example.com"))))

(deftest https-defaults-to-443
  (is (= 443 (:port (parse-url "https://example.com/"))))
  (is (= false (:explicit-port? (parse-url "https://example.com/")))))

(deftest explicit-port-and-all-components
  (is (= {:scheme "http" :host "example.com" :port 8080 :path "/a/b"
          :query "x=1&y=2" :fragment "frag" :userinfo nil
          :explicit-port? true}
         (parse-url "http://example.com:8080/a/b?x=1&y=2#frag"))))

(deftest port-80-spelled-out-is-explicit
  (let [m (parse-url "http://example.com:80/")]
    (is (= 80 (:port m)))
    (is (= true (:explicit-port? m)))))

(deftest empty-port-text-means-default
  ;; "host:" with no digits: RFC 3986 permits an empty port; it is
  ;; treated as the scheme default, not an explicit one.
  (let [m (parse-url "http://example.com:/x")]
    (is (= 80 (:port m)))
    (is (= false (:explicit-port? m)))))

(deftest userinfo-captured-verbatim
  (is (= "user:pass" (:userinfo (parse-url "http://user:pass@example.com/")))))

(deftest embedded-at-in-userinfo-splits-at-last-at
  (let [m (parse-url "http://a@b@example.com/")]
    (is (= "a@b" (:userinfo m)))
    (is (= "example.com" (:host m)))))

(deftest scheme-and-host-lowercased-path-preserved
  (let [m (parse-url "HTTP://EXAMPLE.COM/Path?Q=1#F")]
    (is (= "http" (:scheme m)))
    (is (= "example.com" (:host m)))
    (is (= "/Path" (:path m)))
    (is (= "Q=1" (:query m)))
    (is (= "F" (:fragment m)))))

(deftest empty-path-becomes-slash
  (is (= "/" (:path (parse-url "http://example.com"))))
  (is (= "/" (:path (parse-url "http://example.com?q=1"))))
  (is (= "/" (:path (parse-url "http://example.com#f")))))

(deftest ipv6-literals-parse-with-brackets-kept
  (let [m (parse-url "http://[2001:db8::1]:8080/")]
    (is (= "[2001:db8::1]" (:host m)))
    (is (= 8080 (:port m)))
    (is (= true (:explicit-port? m))))
  (let [m (parse-url "http://[::1]/")]
    (is (= "[::1]" (:host m)))
    (is (= 80 (:port m))))
  (is (= "[2001:db8::1]" (:host (parse-url "http://[2001:DB8::1]/"))
         "IPv6 hex letters are lowercased with the host")))

(deftest trailing-dot-in-host-is-preserved
  ;; A DNS trailing dot marks the root domain and is kept verbatim.
  (is (= "example.com." (:host (parse-url "http://example.com./x")))))

(deftest non-http-schemes-throw-naming-the-scheme
  (is (thrown-with-msg? #"scheme 'ftp'" (parse-url "ftp://example.com/")))
  (is (thrown-with-msg? #"scheme 'file'" (parse-url "file:///tmp/x")))
  (is (thrown? (parse-url "mailto:user@example.com"))))

(deftest malformed-urls-throw
  (is (thrown-with-msg? #"empty host" (parse-url "http:///path")))
  (is (thrown-with-msg? #"empty host" (parse-url "http://:8080/")))
  (is (thrown-with-msg? #"no scheme" (parse-url "example.com/path")))
  (is (thrown-with-msg? #"//" (parse-url "http:example.com")))
  (is (thrown-with-msg? #"port" (parse-url "http://h:abc/")))
  (is (thrown-with-msg? #"port" (parse-url "http://h:99999/")))
  (is (thrown-with-msg? #"IPv6" (parse-url "http://[::1/x")))
  (is (= :eval/contract
         (try (parse-url "no-scheme") (catch e (:mino/kind e))))))

(deftest type-and-arity-errors
  (is (thrown? (parse-url 42)))
  (is (thrown? (parse-url :kw)))
  (is (thrown? (parse-url)))
  (is (thrown? (parse-url "http://a/" "extra"))))

;; --- generators ---

(def scheme-gen (gen/elements ["http" "HTTP" "https" "HTTPS"]))
(def host-gen (gen/fmap (fn [v] (apply str v))
                        (gen/vector (gen/elements "abcXYZ019") 1 12)))
(def port-gen (gen/fmap str (gen/choose 0 65535)))
(def path-gen
  (gen/fmap (fn [v] (if (empty? v) "" (apply str (cons \/ v))))
            (gen/vector (gen/elements "ab1.-_") 0 8)))
(def query-gen (gen/fmap (fn [v] (apply str v))
                         (gen/vector (gen/elements "k=v&23") 0 8)))
(def frag-gen (gen/fmap (fn [v] (apply str v))
                        (gen/vector (gen/elements "frag23") 0 6)))
(def userinfo-gen (gen/fmap (fn [v] (apply str v))
                            (gen/vector (gen/elements "user:pw1") 0 8)))

(def valid-url-gen
  (gen/bind (gen/tuple scheme-gen userinfo-gen host-gen port-gen
                       path-gen query-gen frag-gen gen/boolean)
    (fn [[scheme userinfo host port path query frag explicit?]]
      (let [lc-scheme (str/lower-case scheme)]
        (gen/return
          {:url (str scheme "://"
                     (when-not (empty? userinfo) (str userinfo "@"))
                     host
                     (when explicit? (str ":" port))
                     (when-not (empty? path) path)
                     (when-not (empty? query) (str "?" query))
                     (when-not (empty? frag) (str "#" frag)))
           :scheme lc-scheme
           :host (str/lower-case host)
           :port (if explicit?
                   (parse-long port)
                   (if (= "https" lc-scheme) 443 80))
           :path (if (empty? path) "/" path)
           :query (if (empty? query) nil query)
           :fragment (if (empty? frag) nil frag)
           :userinfo (if (empty? userinfo) nil userinfo)
           :explicit-port? explicit?})))))

(defn valid-url-str-gen []
  (gen/fmap :url valid-url-gen))

(def garbage-url-gen
  (gen/fmap (fn [v] (apply str v))
            (gen/vector (gen/choose 32 126) 0 48)))

(def truncated-url-gen
  (gen/bind (gen/tuple (valid-url-str-gen) (gen/choose 0 64))
    (fn [[u n]]
      (gen/return (subs u 0 (min n (count u)))))))

(def url-ish-gen
  (gen/one-of [garbage-url-gen
               (valid-url-str-gen)
               truncated-url-gen]))

(deftest generated-urls-match-an-independent-model
  (is (url-qc (prop/for-all [u valid-url-gen]
               (let [m (parse-url (:url u))]
                 (and (shape-ok? m)
                      (= (:scheme m) (:scheme u))
                      (= (:host m) (:host u))
                      (= (:port m) (:port u))
                      (= (:explicit-port? m) (:explicit-port? u))
                      (= (:path m) (:path u))
                      (= (:query m) (:query u))
                      (= (:fragment m) (:fragment u))
                      (= (:userinfo m) (:userinfo u)))))
             434341)))

(deftest garbage-and-truncated-urls-never-crash
  ;; Every input either parses to a correctly shaped map or throws a
  ;; classified :eval/contract error; valid URLs in the mix exercise
  ;; the map arm, truncations and garbage the error arm.
  (is (url-qc (prop/for-all [s url-ish-gen]
               (let [r (try (parse-url s) (catch Throwable e e))]
                 (if (contains? r :mino/kind)
                   (= :eval/contract (:mino/kind r))
                   (shape-ok? r))))
             434342)))

(run-tests-and-exit)
