(ns mino.digest
  "Hex-digest sugar over the native digest prims.

  (require '[mino.digest :as digest])
  (digest/digest-hex :sha256 \"abc\")
  ;; => \"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"
  (digest/hmac-hex :sha256 \"Jefe\" \"what do ya want for nothing?\")
  ;; => \"5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843\"

  The prims (sha256, sha1, md5, hmac-sha256) answer raw bytes values;
  these wrappers answer the lowercase hex string scripts usually want.
  Input is a string or bytes value exactly like the prims. An algorithm
  keyword without a prim throws ex-info with :kind :digest/alg and the
  offending :alg in the data map.")

(defn- digest-fn
  "Prim for the algorithm keyword, or throws :digest/alg."
  [alg]
  (case alg
    :sha256 sha256
    :sha1   sha1
    :md5    md5
    (throw (ex-info (str "mino.digest: no digest algorithm " (pr-str alg)
                         "; known: :sha256, :sha1, :md5")
                    {:kind :digest/alg :alg alg}))))

(defn digest-hex
  "Hex digest of data under alg (:sha256, :sha1, or :md5). data is a
  string or bytes value, as the prims take."
  [alg data]
  (hex-encode ((digest-fn alg) data)))

(defn hmac-hex
  "Hex HMAC tag of data under key with alg (:sha256). key and data are
  string or bytes values, as hmac-sha256 takes."
  [alg key data]
  (if (= :sha256 alg)
    (hex-encode (hmac-sha256 key data))
    (throw (ex-info (str "mino.digest: no HMAC algorithm for " (pr-str alg)
                         "; known: :sha256")
                    {:kind :digest/alg :alg alg}))))
