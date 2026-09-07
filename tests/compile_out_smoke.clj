;; Floor smoke for a full compile-out build (mino-min): every
;; MINO_NO_<CAP> flag set. The floor (reader, printer, eval,
;; collections, core prims) must work, and each subtracted capability
;; must throw its classified error rather than crash. Run ONLY against a
;; build with the flags set (./mino task test-compile-out builds mino-min
;; and runs this against it); against a full build the subtracted prims
;; work, so the throw assertions would fail by design. Self-checking:
;; prints a FAIL line and exits non-zero on any miss.

(def failures (atom 0))

(defn check [label ok?]
  (if ok?
    (println (str "  ok: " label))
    (do (println (str "  FAIL: " label))
        (swap! failures inc))))

(defn throws-kind
  "Call thunk; return the :mino/kind of the error it throws, or nil if it
   did not throw."
  [thunk]
  (try (thunk) nil
    (catch e (:mino/kind e))))

;; ---- the floor works ------------------------------------------------

(check "arithmetic" (= 45 (reduce + (range 10))))
(check "collections" (= [2 3 4] (mapv inc [1 2 3])))
(check "strings" (= "HELLO" (clojure.string/upper-case "hello")))
(check "reader + printer round-trip"
       (= {:a [1 2]} (read-string (pr-str {:a [1 2]}))))
(check "regex (floor)" (= "b" (re-find #"b" "abc")))

;; ---- capabilities that survive every flag ---------------------------

;; digest hashes survive MINO_NO_TLS (bearssl_hash.c).
(check "sha256 survives (digest)"
       (= "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
          (hex-encode (sha256 "abc"))))
;; websocket accept-key (SHA-1) survives MINO_NO_TLS.
(check "ws-accept-key survives"
       (= "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
          (ws-accept-key "dGhlIHNhbXBsZSBub25jZQ==")))
;; plain http codec survives MINO_NO_TLS.
(check "http codec survives"
       (= :done (:status (http-parse-response
                          "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"))))
;; UTC / fixed-offset zones survive MINO_NO_TZDATA.
(check "fixed-offset zone survives"
       (= 120 (zone-offset-mins 120 0)))
;; xml-parse survives MINO_NO_DOCUMENTS (not in the documents group).
(check "xml-parse survives"
       (= :r (:tag (xml-parse "<r>x</r>" {}))))

;; ---- subtracted capabilities throw a classified error --------------

(check "tls-connect throws :tls/unavailable"
       (= :tls/unavailable (throws-kind #(tls-connect "example.com" 443))))
(check "named zone throws :time/zone"
       (= :time/zone (throws-kind #(zone-offset-mins "Europe/Oslo" 0))))
(check "html-parse throws :html/unavailable"
       (= :html/unavailable (throws-kind #(html-parse "<p>x</p>" {}))))
(check "yaml-parse throws :yaml/unavailable"
       (= :yaml/unavailable (throws-kind #(yaml-parse "a: 1" {}))))
(check "toml-parse throws :toml/unavailable"
       (= :toml/unavailable (throws-kind #(toml-parse "a = 1" nil))))

;; ---- verdict --------------------------------------------------------

(if (zero? @failures)
  (println "compile-out smoke: all checks passed")
  (do (println (str "compile-out smoke: " @failures " FAILURES"))
      (exit 1)))
