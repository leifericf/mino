(require "tests/test")

;; Digest, HMAC, and crc32 primitives. The hash vectors are the FIPS
;; 180 sha256/sha1 sets (including the 55/56/57 and 63/64/65 block
;; boundary runs), the RFC 1321 md5 set, and the RFC 4231 HMAC-SHA256
;; set, every value verified against python3 hashlib/hmac on this
;; machine. crc32 carries the gzip-spec vectors from python3 zlib.
;; Digests return bytes; assertions compare through hex-encode.

(defn sha256-hex [s] (hex-encode (sha256 s)))
(defn sha1-hex [s] (hex-encode (sha1 s)))
(defn md5-hex [s] (hex-encode (md5 s)))
(defn hmac256-hex [k d] (hex-encode (hmac-sha256 k d)))

(defn a-run [n] (apply str (repeat n "a")))

;;; sha256 (FIPS 180-4 vectors)

(deftest sha256-fips-vectors
  (is (= "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
         (sha256-hex "")))
  (is (= "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
         (sha256-hex "abc")))
  (is (= "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
         (sha256-hex "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))
  (is (= "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"
         (sha256-hex "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"))))

(deftest sha256-block-boundary-runs
  ;; 55 and 56 chars straddle the 64-byte block padding boundary
  ;; (message plus length fits in one block at 55, spills at 56).
  (is (= "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"
         (sha256-hex (a-run 55))))
  (is (= "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"
         (sha256-hex (a-run 56))))
  (is (= "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6"
         (sha256-hex (a-run 57))))
  (is (= "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"
         (sha256-hex (a-run 63))))
  (is (= "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"
         (sha256-hex (a-run 64))))
  (is (= "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"
         (sha256-hex (a-run 65)))))

;;; sha1 (FIPS 180 vectors)

(deftest sha1-fips-vectors
  (is (= "da39a3ee5e6b4b0d3255bfef95601890afd80709" (sha1-hex "")))
  (is (= "a9993e364706816aba3e25717850c26c9cd0d89d" (sha1-hex "abc")))
  (is (= "84983e441c3bd26ebaae4aa1f95129e5e54670f1"
         (sha1-hex "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))))

(deftest sha1-block-boundary-runs
  (is (= "c1c8bbdc22796e28c0e15163d20899b65621d65a" (sha1-hex (a-run 55))))
  (is (= "c2db330f6083854c99d4b5bfb6e8f29f201be699" (sha1-hex (a-run 56))))
  (is (= "f08f24908d682555111be7ff6f004e78283d989a" (sha1-hex (a-run 57))))
  (is (= "03f09f5b158a7a8cdad920bddc29b81c18a551f5" (sha1-hex (a-run 63))))
  (is (= "0098ba824b5c16427bd7a1122a5a442a25ec644d" (sha1-hex (a-run 64))))
  (is (= "11655326c708d70319be2610e8a57d9a5b959d3b" (sha1-hex (a-run 65)))))

;;; md5 (RFC 1321 appendix A5 set)

(deftest md5-rfc1321-vectors
  (is (= "d41d8cd98f00b204e9800998ecf8427e" (md5-hex "")))
  (is (= "0cc175b9c0f1b6a831c399e269772661" (md5-hex "a")))
  (is (= "900150983cd24fb0d6963f7d28e17f72" (md5-hex "abc")))
  (is (= "f96b697d7cb7938d525a2f31aaf161d0" (md5-hex "message digest")))
  (is (= "c3fcd3d76192e4007dfb496cca67e13b"
         (md5-hex "abcdefghijklmnopqrstuvwxyz")))
  (is (= "d174ab98d277d9f5a5611c2c9f419d9f"
         (md5-hex "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")))
  (is (= "57edf4a22be3c955ac49da2e2107b67a"
         (md5-hex "12345678901234567890123456789012345678901234567890123456789012345678901234567890"))))

;;; digest results are bytes values

(deftest digests-return-bytes
  (is (= (byte-array [0xba 0x78 0x16 0xbf 0x8f 0x01 0xcf 0xea
                      0x41 0x41 0x40 0xde 0x5d 0xae 0x22 0x23
                      0xb0 0x03 0x61 0xa3 0x96 0x17 0x7a 0x9c
                      0xb4 0x10 0xff 0x61 0xf2 0x00 0x15 0xad])
         (sha256 "abc")))
  (is (= 32 (count (sha256 "abc"))))
  (is (= 20 (count (sha1 "abc"))))
  (is (= 16 (count (md5 "abc")))))

(deftest digests-accept-bytes-like-strings
  (is (= (sha256 "abc") (sha256 (byte-array [97 98 99]))))
  (is (= (sha1 "abc") (sha1 (byte-array [97 98 99]))))
  (is (= (md5 "abc") (md5 (byte-array [97 98 99]))))
  (is (= (crc32 "abc") (crc32 (byte-array [97 98 99])))))

;;; hmac-sha256 (RFC 4231 test cases 1-7)

(deftest hmac-sha256-rfc4231-set
  ;; case 1: 20-byte 0x0b key
  (is (= "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
         (hmac256-hex (byte-array (repeat 20 0x0b)) "Hi There")))
  ;; case 2: "Jefe" key
  (is (= "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
         (hmac256-hex "Jefe" "what do ya want for nothing?")))
  ;; case 3: 20-byte 0xaa key, 50-byte 0xdd data
  (is (= "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"
         (hmac256-hex (byte-array (repeat 20 0xaa))
                      (byte-array (repeat 50 0xdd)))))
  ;; case 4: 25-byte counting key, 50-byte 0xcd data
  (is (= "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"
         (hmac256-hex (byte-array (range 1 26))
                      (byte-array (repeat 50 0xcd)))))
  ;; case 5: 20-byte 0x0c key (truncation case, full digest here)
  (is (= "a3b6167473100ee06e0c796c2955552bfa6f7c0a6a8aef8b93f860aab0cd20c5"
         (hmac256-hex (byte-array (repeat 20 0x0c)) "Test With Truncation")))
  ;; case 6: 131-byte 0xaa key
  (is (= "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"
         (hmac256-hex (byte-array (repeat 131 0xaa))
                      "Test Using Larger Than Block-Size Key - Hash Key First")))
  ;; case 7: 131-byte key, longer data
  (is (= "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"
         (hmac256-hex (byte-array (repeat 131 0xaa))
                      "This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm."))))

(deftest hmac-sha256-returns-32-bytes
  (is (bytes? (hmac-sha256 "key" "data")))
  (is (= 32 (count (hmac-sha256 "key" "data")))))

;;; crc32 (gzip spec vectors, python3 zlib oracle)

(deftest crc32-gzip-spec-vectors
  (is (= 0x0 (crc32 "")))
  (is (= 0xe8b7be43 (crc32 "a")))
  (is (= 0x352441c2 (crc32 "abc")))
  (is (= 0x20159d7f (crc32 "message digest")))
  (is (= 0x4c2750bd (crc32 "abcdefghijklmnopqrstuvwxyz")))
  (is (= 0x1fc2e6d2
         (crc32 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")))
  (is (= 0x7ca94a72
         (crc32 "12345678901234567890123456789012345678901234567890123456789012345678901234567890"))))

(deftest crc32-returns-a-nonnegative-integer
  (is (integer? (crc32 "abc"))))

;;; ADR 37: unknown-algorithm throws carry :mino/kind :digest/alg

(deftest digest-hex-unknown-alg-is-classed
  (require '[mino.digest :as digest])
  (is (= :digest/alg
         (try (digest/digest-hex :sha3-256 "abc")
              (catch e (:mino/kind e)))))
  (is (= :caught
         (try (digest/digest-hex :sha3-256 "abc")
              (catch :digest/alg _ :caught))))
  (let [e (try (digest/digest-hex :sha3-256 "abc") (catch e (do e)))]
    (is (= :sha3-256 (:alg (ex-data e))))))

(deftest hmac-hex-unknown-alg-is-classed
  (require '[mino.digest :as digest])
  (is (= :digest/alg
         (try (digest/hmac-hex :sha1 "k" "d")
              (catch e (:mino/kind e)))))
  (is (= :caught
         (try (digest/hmac-hex :sha1 "k" "d")
              (catch :digest/alg _ :caught))))
  (let [e (try (digest/hmac-hex :sha1 "k" "d") (catch e (do e)))]
    (is (= :sha1 (:alg (ex-data e))))))

;;; shared contract: type and arity errors

(deftest digest-type-and-arity-errors
  (is (thrown? (sha256 42)))
  (is (thrown? (sha1 nil)))
  (is (thrown? (md5 :kw)))
  (is (thrown? (crc32 1.5)))
  (is (thrown? (hmac-sha256 "key")))
  (is (thrown? (hmac-sha256 "key" "data" "extra")))
  (is (thrown? (hmac-sha256 42 "data")))
  (is (thrown? (sha256))))

(run-tests-and-exit)
