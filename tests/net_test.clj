(require "tests/test")

;; TCP net capability. The CLI installs every capability, so the net
;; bit is present here; the sandbox preset (MINO_CAP_DEFAULT) keeps
;; net out, pinned at the C level in tests/embed_api_test.c where a
;; state can be built without it.

(deftest net-capability-present-under-cli
  (is (true? (mino-installed? :net))))

(deftest net-capability-lookup-takes-any-label-kind
  (is (true? (mino-installed? "net")))
  (is (true? (mino-installed? 'net))))

(deftest net-prims-absent-before-socket-table
  ;; The capability bit lands first; the socket prims arrive with the
  ;; prim table. Until that unit, resolve finds nothing.
  (is (nil? (resolve 'net-connect)))
  (is (nil? (resolve 'net-read)))
  (is (nil? (resolve 'net-read-all)))
  (is (nil? (resolve 'net-write)))
  (is (nil? (resolve 'net-close))))

(run-tests-and-exit)
