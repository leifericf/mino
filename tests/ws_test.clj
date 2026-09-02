(require "tests/test")

;; RFC 6455 websocket frame codec and handshake accept-key goldens for
;; the native prims ADR 41 sends to src/prim/ws.c. The decoder eats
;; untrusted network bytes, so the rejection and malformed-corpus arms
;; below are the core of this file (the corpus doubles as the fuzz
;; seed).
;;
;; The pinned contract:
;;
;;   (ws-encode-frame frame) -> wire bytes. frame keys: :opcode (:text
;;   :binary :ping :pong :close :continuation), :payload (bytes, or a
;;   string encoded UTF-8), :fin? (default true), :mask (4 wire bytes:
;;   present means a masked client frame, absent means an unmasked
;;   server frame). A close frame carries :code and :reason instead of
;;   :payload. The caller (mino.ws) sources every mask from
;;   secure-rand-bytes; the prim applies exactly the mask it is given.
;;
;;   (ws-decode-frames bytes {:role .. :max-payload ..}) ->
;;   {:frames [..] :rest bytes}. Streaming over an accumulated buffer:
;;   :rest is the suffix from the first incomplete frame or fragmented
;;   message, so feeding (rest ++ more-bytes) continues exactly where
;;   the last call stopped. The decoder reassembles fragmented messages
;;   natively (capped like the HTTP body cap, 16 MiB default), so every
;;   emitted frame carries :fin? true; a control frame that lands
;;   behind a still-open fragment run is emitted once the run
;;   completes. :role :server requires masked (client-sent) frames and
;;   :role :client requires unmasked (server-sent) frames. A :text
;;   payload decodes to a UTF-8-validated string; :binary/:ping/:pong
;;   payloads stay bytes; :close decodes to :code (int, nil when the
;;   frame has no body) and :reason. Protocol violations throw
;;   :codec/corrupt, cap breaches throw :codec/limit, both before the
;;   declared payload is realized.
;;
;;   (ws-accept-key key) -> the Sec-WebSocket-Accept string (SHA-1 of
;;   key + the RFC 6455 GUID, base64).
;;
;; Golden byte vectors come from RFC 6455 section 5.7.

(defn- ws-bb
  "Wire bytes from a mix of ints, ASCII strings, byte arrays, and
  int seqs."
  [& xs]
  (byte-array (mapcat #(cond (bytes? %) (vec %)
                             (string? %) (map int %)
                             (number? %) [%]
                             :else (vec %))
                      xs)))

(defn- ws-kind
  "Run thunk; :ok on success, the thrown :mino/kind otherwise."
  [thunk]
  (try (do (thunk) :ok) (catch e (:mino/kind e))))

(defn- ws-xor-mask
  "The RFC 6455 masking transform, written independently of the prim
  so masked fixtures are built by a second implementation."
  [mask payload]
  (byte-array (map-indexed (fn [i b] (bit-xor b (aget mask (rem i 4))))
                           (vec payload))))

(defn- ws-take [b k] (byte-array (take k (seq b))))
(defn- ws-drop [b k] (byte-array (drop k (seq b))))

(def ^:private ws-rfc-mask (ws-bb 0x37 0xfa 0x21 0x3d))

(defn- ws-pattern
  "n deterministic non-text bytes for length-boundary payloads."
  [n]
  (byte-array (map #(rem % 251) (range n))))

;;; encoding goldens (RFC 6455 section 5.7)

(deftest ws-unmasked-text-hello-golden
  (is (= (ws-bb 0x81 0x05 "Hello")
         (ws-encode-frame {:opcode :text :payload "Hello"}))))

(deftest ws-masked-text-hello-golden
  ;; Hand-checked masking XOR: 48^37=7f 65^fa=9f 6c^21=4d 6c^3d=51
  ;; 6f^37=58, the literal example the RFC prints.
  (is (= (ws-bb 0x81 0x85 0x37 0xfa 0x21 0x3d 0x7f 0x9f 0x4d 0x51 0x58)
         (ws-encode-frame {:opcode :text :payload "Hello"
                           :mask ws-rfc-mask}))))

(deftest ws-ping-and-masked-pong-goldens
  (is (= (ws-bb 0x89 0x05 "Hello")
         (ws-encode-frame {:opcode :ping :payload "Hello"})))
  (is (= (ws-bb 0x8a 0x85 0x37 0xfa 0x21 0x3d 0x7f 0x9f 0x4d 0x51 0x58)
         (ws-encode-frame {:opcode :pong :payload "Hello"
                           :mask ws-rfc-mask}))))

(deftest ws-fragmented-text-encode-goldens
  (is (= (ws-bb 0x01 0x03 "Hel")
         (ws-encode-frame {:opcode :text :payload "Hel" :fin? false})))
  (is (= (ws-bb 0x80 0x02 "lo")
         (ws-encode-frame {:opcode :continuation :payload "lo"}))))

(deftest ws-close-encode-goldens
  (is (= (ws-bb 0x88 0x05 0x03 0xe8 "bye")
         (ws-encode-frame {:opcode :close :code 1000 :reason "bye"})))
  (is (= (ws-bb 0x88 0x02 0x03 0xe8)
         (ws-encode-frame {:opcode :close :code 1000})))
  (is (= (ws-bb 0x88 0x00)
         (ws-encode-frame {:opcode :close}))))

(deftest ws-fin-defaults-to-true
  (is (= (ws-encode-frame {:opcode :text :payload "x" :fin? true})
         (ws-encode-frame {:opcode :text :payload "x"}))))

(deftest ws-string-and-byte-payloads-encode-identically
  (is (= (ws-encode-frame {:opcode :text :payload (ws-bb "Hello")})
         (ws-encode-frame {:opcode :text :payload "Hello"}))))

;;; masking round-trip through the decoder

(deftest ws-masked-client-frame-decodes-at-the-server
  (let [r (ws-decode-frames
           (ws-bb 0x81 0x85 0x37 0xfa 0x21 0x3d 0x7f 0x9f 0x4d 0x51 0x58)
           {:role :server})]
    (is (= [{:opcode :text :fin? true :payload "Hello"}] (:frames r)))
    (is (= (ws-bb) (:rest r)))))

(deftest ws-masked-binary-round-trips-encode-to-decode
  (let [payload (ws-pattern 300)
        r (ws-decode-frames (ws-encode-frame {:opcode :binary
                                              :payload payload
                                              :mask ws-rfc-mask})
                            {:role :server})]
    (is (= [{:opcode :binary :fin? true :payload payload}] (:frames r)))
    (is (= (ws-bb) (:rest r)))))

(deftest ws-masked-extended-length-decodes-against-the-xor-oracle
  ;; A 256-byte masked frame built by the test's own XOR transform:
  ;; the decoder must agree with an independent masking
  ;; implementation, not only with its sibling encoder.
  (let [payload (ws-pattern 256)
        wire (ws-bb 0x82 0xfe 0x01 0x00 ws-rfc-mask
                    (ws-xor-mask ws-rfc-mask payload))
        r (ws-decode-frames wire {:role :server})]
    (is (= [{:opcode :binary :fin? true :payload payload}] (:frames r)))))

;;; the three payload-length encodings at their boundaries

(deftest ws-length-125-stays-in-the-seven-bit-field
  (let [enc (ws-encode-frame {:opcode :binary :payload (ws-pattern 125)})]
    (is (= 0x82 (aget enc 0)))
    (is (= 125 (aget enc 1)))
    (is (= 127 (alength enc)))
    (is (= [{:opcode :binary :fin? true :payload (ws-pattern 125)}]
           (:frames (ws-decode-frames enc {:role :client}))))))

(deftest ws-length-126-moves-to-the-sixteen-bit-field
  (let [enc (ws-encode-frame {:opcode :binary :payload (ws-pattern 126)})]
    (is (= 0x7e (aget enc 1)) "126 marker")
    (is (= 0x00 (aget enc 2)))
    (is (= 0x7e (aget enc 3)))
    (is (= 130 (alength enc)))
    (is (= [{:opcode :binary :fin? true :payload (ws-pattern 126)}]
           (:frames (ws-decode-frames enc {:role :client}))))))

(deftest ws-length-65535-fills-the-sixteen-bit-field
  (let [enc (ws-encode-frame {:opcode :binary :payload (ws-pattern 65535)})]
    (is (= 0x7e (aget enc 1)))
    (is (= 0xff (aget enc 2)))
    (is (= 0xff (aget enc 3)))
    (is (= 65539 (alength enc)))
    (is (= [{:opcode :binary :fin? true :payload (ws-pattern 65535)}]
           (:frames (ws-decode-frames enc {:role :client}))))))

(deftest ws-length-65536-moves-to-the-sixty-four-bit-field
  (let [enc (ws-encode-frame {:opcode :binary :payload (ws-pattern 65536)})]
    (is (= 0x7f (aget enc 1)) "127 marker")
    (is (= [0x00 0x00 0x00 0x00 0x00 0x01 0x00 0x00]
           (mapv #(aget enc %) (range 2 10)))
        "big-endian 64-bit length")
    (is (= 65546 (alength enc)))
    (is (= [{:opcode :binary :fin? true :payload (ws-pattern 65536)}]
           (:frames (ws-decode-frames enc {:role :client}))))))

(deftest ws-mask-bit-rides-the-length-byte
  (is (= 0xfd (aget (ws-encode-frame {:opcode :binary
                                      :payload (ws-pattern 125)
                                      :mask ws-rfc-mask}) 1)))
  (is (= 0xfe (aget (ws-encode-frame {:opcode :binary
                                      :payload (ws-pattern 126)
                                      :mask ws-rfc-mask}) 1))))

;;; fragmentation reassembly

(deftest ws-fragmented-text-reassembles-into-one-message
  (let [r (ws-decode-frames (ws-bb 0x01 0x03 "Hel" 0x80 0x02 "lo")
                            {:role :client})]
    (is (= [{:opcode :text :fin? true :payload "Hello"}] (:frames r)))
    (is (= (ws-bb) (:rest r)))))

(deftest ws-fragment-boundary-may-split-a-utf8-character
  ;; 0xc3 0xa9 is one two-byte character; UTF-8 validity holds on the
  ;; reassembled message, so a per-fragment validator fails here.
  (let [r (ws-decode-frames (ws-bb 0x01 0x01 0xc3 0x80 0x01 0xa9)
                            {:role :client})]
    (is (= 1 (count (:frames r))))
    (is (= [233] (mapv int (:payload (first (:frames r))))))))

(deftest ws-open-fragment-run-stays-in-rest
  (let [b (ws-bb 0x01 0x03 "Hel")
        r (ws-decode-frames b {:role :client})]
    (is (= [] (:frames r)))
    (is (= b (:rest r)) "an unfinished message consumes nothing")))

(deftest ws-interleaved-ping-does-not-corrupt-reassembly
  (let [r (ws-decode-frames (ws-bb 0x01 0x03 "Hel"
                                   0x89 0x02 "hi"
                                   0x80 0x02 "lo")
                            {:role :client})]
    (is (= [{:opcode :ping :fin? true :payload (ws-bb "hi")}
            {:opcode :text :fin? true :payload "Hello"}]
           (:frames r))
        "the control frame completes first and the message reassembles")
    (is (= (ws-bb) (:rest r)))))

;;; close-code parsing

(deftest ws-close-with-code-and-reason-decodes-to-data
  (is (= [{:opcode :close :fin? true :code 1000 :reason "bye"}]
         (:frames (ws-decode-frames (ws-bb 0x88 0x05 0x03 0xe8 "bye")
                                    {:role :client})))))

(deftest ws-close-without-a-body-is-a-no-code-close
  (is (= [{:opcode :close :fin? true :code nil :reason ""}]
         (:frames (ws-decode-frames (ws-bb 0x88 0x00)
                                    {:role :client})))))

;;; role enforcement and reserved-bit rejection

(deftest ws-reserved-bits-are-rejected
  (doseq [b0 [0xc1 0xa1 0x91]]
    (is (= :codec/corrupt
           (ws-kind #(ws-decode-frames (ws-bb b0 0x01 "x")
                                       {:role :client})))
        (str "first byte " b0 " sets an RSV bit"))))

(deftest ws-masked-server-frame-is-rejected-at-the-client
  ;; Servers must not mask (RFC 6455 5.1).
  (is (= :codec/corrupt
         (ws-kind #(ws-decode-frames
                    (ws-bb 0x81 0x85 0x37 0xfa 0x21 0x3d
                           0x7f 0x9f 0x4d 0x51 0x58)
                    {:role :client})))))

(deftest ws-unmasked-client-frame-is-rejected-at-the-server
  ;; Clients must mask every frame (RFC 6455 5.1).
  (is (= :codec/corrupt
         (ws-kind #(ws-decode-frames (ws-bb 0x81 0x05 "Hello")
                                     {:role :server})))))

;;; payload caps fire on the declared length, before the payload

(deftest ws-declared-length-over-the-cap-rejects-on-the-header-alone
  ;; Only the ten header bytes are present: the reject must come from
  ;; the declared length, never from realizing the payload.
  (is (= :codec/limit
         (ws-kind #(ws-decode-frames
                    (ws-bb 0x82 0x7f 0x40 0 0 0 0 0 0 0)
                    {:role :client}))))
  (is (= :codec/limit
         (ws-kind #(ws-decode-frames (ws-bb 0x82 0x0b)
                                     {:role :client :max-payload 10})))))

(deftest ws-default-cap-is-the-http-body-cap
  ;; 16 MiB declared exactly is merely incomplete; one byte more
  ;; breaches the default cap.
  (let [at-cap (ws-bb 0x82 0x7f 0 0 0 0 0x01 0 0 0)
        r (ws-decode-frames at-cap {:role :client})]
    (is (= [] (:frames r)))
    (is (= at-cap (:rest r))))
  (is (= :codec/limit
         (ws-kind #(ws-decode-frames (ws-bb 0x82 0x7f 0 0 0 0 0x01 0 0 0x01)
                                     {:role :client})))))

(deftest ws-explicit-cap-bounds-the-frame
  (is (= [{:opcode :binary :fin? true :payload (ws-pattern 10)}]
         (:frames (ws-decode-frames
                   (ws-bb 0x82 0x0a (ws-pattern 10))
                   {:role :client :max-payload 10}))))
  (is (= :codec/limit
         (ws-kind #(ws-decode-frames
                    (ws-bb 0x82 0x0b (ws-pattern 11))
                    {:role :client :max-payload 10})))))

(deftest ws-reassembly-total-is-capped
  ;; Each fragment is under the cap; only the reassembled message
  ;; crosses it, so the cap must apply to the running total.
  (is (= :codec/limit
         (ws-kind #(ws-decode-frames
                    (ws-bb 0x02 0x05 "AAAAA" 0x80 0x05 "BBBBB")
                    {:role :client :max-payload 8})))))

(deftest ws-sixty-four-bit-length-with-the-top-bit-set-is-corrupt
  ;; RFC 6455 5.2: the most significant bit of the 64-bit length MUST
  ;; be 0; rejected as a protocol violation, never truncated or
  ;; wrapped.
  (is (= :codec/corrupt
         (ws-kind #(ws-decode-frames
                    (ws-bb 0x82 0x7f 0x80 0 0 0 0 0 0 0)
                    {:role :client})))))

;;; UTF-8 validation on text frames

(deftest ws-text-must-be-valid-utf8
  (is (= :codec/corrupt
         (ws-kind #(ws-decode-frames (ws-bb 0x81 0x02 0xff 0xfe)
                                     {:role :client}))))
  (is (= [233] (mapv int (:payload
                          (first (:frames
                                  (ws-decode-frames (ws-bb 0x81 0x02 0xc3 0xa9)
                                                    {:role :client}))))))
      "a valid two-byte character passes"))

;;; streaming: the accumulated-buffer contract

(deftest ws-empty-input-yields-no-frames-and-an-empty-rest
  (let [r (ws-decode-frames (ws-bb) {:role :client})]
    (is (= [] (:frames r)))
    (is (= (ws-bb) (:rest r)))))

(deftest ws-every-split-offset-agrees-with-the-single-feed
  (let [stream (ws-bb 0x89 0x02 "hi"
                      0x01 0x03 "Hel"
                      0x8a 0x00
                      0x80 0x02 "lo"
                      0x88 0x02 0x03 0xe8)
        whole (ws-decode-frames stream {:role :client})]
    (is (= [{:opcode :ping :fin? true :payload (ws-bb "hi")}
            {:opcode :pong :fin? true :payload (ws-bb)}
            {:opcode :text :fin? true :payload "Hello"}
            {:opcode :close :fin? true :code 1000 :reason ""}]
           (:frames whole)))
    (is (= (ws-bb) (:rest whole)))
    (doseq [k (range (inc (alength stream)))]
      (let [head (ws-decode-frames (ws-take stream k) {:role :client})
            tail (ws-decode-frames (ws-bb (:rest head) (ws-drop stream k))
                                   {:role :client})]
        (is (= (:frames whole)
               (into (vec (:frames head)) (:frames tail)))
            (str "frames diverged when split at " k))
        (is (= (:rest whole) (:rest tail))
            (str "leftover diverged when split at " k))))))

;;; frame maps freeze their keys

(deftest ws-decoded-frame-maps-freeze-their-keys
  (is (= #{:opcode :fin? :payload}
         (set (keys (first (:frames (ws-decode-frames
                                     (ws-bb 0x82 0x01 0x07)
                                     {:role :client})))))))
  (is (= #{:opcode :fin? :code :reason}
         (set (keys (first (:frames (ws-decode-frames
                                     (ws-bb 0x88 0x02 0x03 0xe8)
                                     {:role :client}))))))))

;;; handshake: the accept key and the nonce recipe

(deftest ws-accept-key-matches-the-rfc-golden
  ;; The canonical RFC 6455 1.3 vector: SHA-1 of the key + the GUID
  ;; 258EAFA5-E914-47DA-95CA-C5AB0DC85B11, base64.
  (is (= "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
         (ws-accept-key "dGhlIHNhbXBsZSBub25jZQ=="))))

(deftest ws-handshake-nonce-recipe-is-sixteen-secure-random-bytes
  ;; The Sec-WebSocket-Key mino.ws sends is exactly this composition
  ;; (never rand: a predictable nonce defeats the masking defense).
  ;; This pins the recipe's shape; the end-to-end pin against
  ;; ws-connect's handshake bytes lands with mino.ws itself.
  (let [a (base64-encode (secure-rand-bytes 16))
        b (base64-encode (secure-rand-bytes 16))]
    (is (= 24 (count a)) "16 bytes base64-encode to 24 characters")
    (is (re-find #"^[A-Za-z0-9+/]{22}==$" a) "base64 alphabet, == pad")
    (is (not= a b) "successive nonces differ")))

;;; argument contract

(deftest ws-prims-reject-malformed-arguments
  (is (thrown? (ws-decode-frames 42 {:role :client})))
  (is (thrown? (ws-decode-frames (ws-bb) {})) "role is required")
  (is (thrown? (ws-decode-frames (ws-bb) {:role :bogus})))
  (is (thrown? (ws-encode-frame 42)))
  (is (thrown? (ws-encode-frame {:opcode :nope :payload "x"})))
  (is (thrown? (ws-encode-frame {:opcode :text :payload "x"
                                 :mask (ws-bb 1 2 3)}))
      "a mask is exactly four bytes")
  (is (thrown? (ws-encode-frame {:opcode :ping :payload (ws-pattern 126)}))
      "a control payload never exceeds 125 bytes")
  (is (thrown? (ws-encode-frame {:opcode :ping :payload "x" :fin? false}))
      "a control frame is never fragmented")
  (is (thrown? (ws-accept-key 42))))

;;; malformed-frame corpus: incomplete vs corrupt, pinned per entry.
;;; Every decode here runs at :role :client over unmasked frames; the
;;; corpus is the seed for the fuzz lane.

(def ws-malformed-corpus
  [["lone header byte" [0x81] :incomplete]
   ["truncated payload" [0x81 0x05 72 101] :incomplete]
   ["missing 16-bit length" [0x81 0x7e] :incomplete]
   ["half a 16-bit length" [0x81 0x7e 0x00] :incomplete]
   ["missing 64-bit length" [0x81 0x7f 0 0 0] :incomplete]
   ["rsv1 set" [0xc1 0x00] :codec/corrupt]
   ["reserved data opcode 3" [0x83 0x00] :codec/corrupt]
   ["reserved control opcode 15" [0x8f 0x00] :codec/corrupt]
   ["fragmented ping" [0x09 0x00] :codec/corrupt]
   ["control frame declaring extended length" [0x89 0x7e 0x00 0x7e]
    :codec/corrupt]
   ["close with a one-byte body" [0x88 0x01 0x03] :codec/corrupt]
   ["continuation with no message open" [0x80 0x00] :codec/corrupt]
   ["64-bit length top bit set" [0x82 0x7f 0x80 0 0 0 0 0 0 0]
    :codec/corrupt]
   ["invalid utf-8 text" [0x81 0x01 0xff] :codec/corrupt]
   ["close code below 1000" [0x88 0x02 0x00 0x64] :codec/corrupt]
   ["reserved close code 1005" [0x88 0x02 0x03 0xed] :codec/corrupt]])

(deftest ws-malformed-corpus-classifies-exactly
  (doseq [[label bs expect] ws-malformed-corpus]
    (let [b (byte-array bs)]
      (if (= :incomplete expect)
        (let [r (ws-decode-frames b {:role :client})]
          (is (= [] (:frames r)) (str label ": no frames yet"))
          (is (= b (:rest r)) (str label ": every byte kept for later")))
        (is (= expect (ws-kind #(ws-decode-frames b {:role :client})))
            label)))))

(run-tests-and-exit)
