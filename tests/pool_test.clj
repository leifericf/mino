(require "tests/test")

;; Keep-alive connection pool (pool-checkout / pool-return /
;; pool-close-all), keyed per endpoint {:scheme :host :port} and held
;; per state. The pool never opens connections: checkout answers a
;; live idle handle or nil, and the caller connects on nil. Liveness
;; is a zero-timeout poll at checkout (a readable idle socket means
;; the peer closed or desynced); expiry is a keepalive-ms age check.
;;
;; Behaviour tests drive a loopback server built from mino's own
;; listener prims: the accept loop runs in a future and holds each
;; accepted connection open on its own future (:keep) or closes it
;; shortly after accept (:close), counting accepts into an atom.
;; Reuse is asserted as "the accept tally did not grow", expiry and
;; dead peers as "it did". In process, so every platform the CLI
;; builds on runs the full file.
;;
;; Concurrency: one mutex per endpoint pool guards queue ops only;
;; no blocking IO runs under it (the host_threads lock discipline).
;; The state lock already serialises prims on one state, so the suite
;; exercises the queue logic single-threaded; the sanitizer lanes are
;; the leak and race oracle (pool files are not in the TSan
;; concurrency set, mirroring net_test).

(defn- pool-serve
  "Hold one accepted connection: :keep holds it open until the peer
  closes or teardown begins; :close shuts it shortly after accept so
  the client socket sees EOF while it sits idle in the pool."
  [mode c running?]
  (if (= mode :close)
    (do (thread-sleep 50)
        (try (net-close c) (catch e nil)))
    (do
      (loop []
        (when @running?
          (let [b (try (net-read c 65536) (catch e ::tick))]
            (when (some? b)
              (recur)))))
      (try (net-close c) (catch e nil)))))

(defn- pool-start-server
  "Counting loopback fixture (:keep :close). Accepts are tallied into
  an atom at accept time; each connection is served on its own future
  so two live sockets never serialise behind one. Returns the port,
  the tally atom, and a stop fn with guaranteed teardown."
  [mode]
  (let [l (net-listen "127.0.0.1" 0 {})
        running? (atom true)
        conns (atom [])
        handlers (atom [])
        accepts (atom 0)
        port (net-listener-port l)
        fut (future
              (loop []
                (when @running?
                  (try
                    (let [c (net-accept l {:accept-timeout 250
                                           :read-timeout 300})]
                      (swap! accepts inc)
                      (swap! conns conj c)
                      (try
                        (swap! handlers conj
                               (future (pool-serve mode c running?)))
                        ;; A handler spawn past the host thread grant
                        ;; leaves the connection unserved; closing it
                        ;; keeps the peer and the tally in step.
                        (catch e (try (net-close c) (catch e nil)))))
                    (catch e nil))
                  (recur))))
        stop (fn []
               (reset! running? false)
               (try (net-close l) (catch e nil))
               ;; Join the handlers while their reads can still end on
               ;; their own, so the sweep never closes a socket
               ;; underneath a blocked read.
               (doseq [h @handlers]
                 (try (deref h 1500 ::stopped) (catch e nil)))
               (doseq [c @conns]
                 (try (net-close c) (catch e nil)))
               (deref fut 2000 ::stopped))]
    {:port port :accepts accepts :stop stop}))

(defn- pool-with-server
  [mode body]
  (let [srv (pool-start-server mode)]
    (try
      (body srv)
      (finally
        ((:stop srv))))))

(defn- pool-wait-for-count
  "Block until the server has accepted at least `want` connections or
  5 s pass; the accept and its tally race the client's connect
  return, so every count assertion goes through here."
  [srv want]
  (let [t0 (time-ms)]
    (loop []
      (let [n @(:accepts srv)]
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

;; ---- loopback behaviour ----

(deftest two-borrows-without-return-open-two-connections
  (pool-with-server :keep
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
  (pool-with-server :keep
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
          (is (= 1 @(:accepts srv)))
          (net-close h2)
          (is (nil? (pool-close-all))))))))

(deftest expired-entries-are-dropped-and-not-reused
  (pool-with-server :keep
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
  (pool-with-server :close
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
  (pool-with-server :keep
    (fn [srv]
      (let [e  (pool-ep srv)
            h1 (net-connect "127.0.0.1" (:port srv))]
        (pool-wait-for-count srv 1)
        (is (nil? (pool-return e h1 {:keepalive 0})))
        (is (= :net (try (net-read h1 1) (catch e (:mino/kind e))))
            "return with :keepalive 0 closes the handle")
        (is (nil? (pool-checkout e {:keepalive 0})))
        (is (= 1 @(:accepts srv)))
        (is (nil? (pool-close-all)))))))

(deftest pool-close-all-closes-pooled-sockets
  ;; The pool roots the handle through a full collection: the fixture
  ;; is torn down first (its futures joined) so the collection
  ;; exercises the pool's rooting, not fixture-worker lifetimes.
  (let [srv (pool-start-server :keep)
        e (pool-ep srv)
        h (net-connect "127.0.0.1" (:port srv))]
    (pool-wait-for-count srv 1)
    (pool-return e h nil)
    ((:stop srv))
    (gc!)
    (is (nil? (pool-close-all)))
    (is (= :net (try (net-write h "x") (catch e (:mino/kind e)))))
    ;; Idempotent: a second sweep over an empty pool is a no-op.
    (is (nil? (pool-close-all)))
    (is (= 1 @(:accepts srv)))))

(deftest pools-are-keyed-per-endpoint
  (pool-with-server :keep
    (fn [a]
      (pool-with-server :keep
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
  (pool-with-server :keep
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
  (pool-with-server :keep
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

(run-tests-and-exit)
