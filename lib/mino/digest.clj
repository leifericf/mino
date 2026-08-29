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
  keyword without a prim throws a diagnostic with :mino/kind :digest/alg
  and the offending :alg in the data map.")

;;;; Errors

(defn- digest-fail
  "Throws a classified mino.digest diagnostic (ADR 37): :mino/kind names
  the error class so classed catch dispatches on it, :mino/message the
  human string ex-message returns, :mino/data the detail ex-data reads."
  [kind code msg data]
  (throw {:mino/kind kind :mino/code code :mino/message msg
          :mino/data data}))

(defn- digest-fn
  "Prim for the algorithm keyword, or throws :digest/alg."
  [alg]
  (case alg
    :sha256 sha256
    :sha1   sha1
    :md5    md5
    (digest-fail :digest/alg "MDA001"
                 (str "mino.digest: no digest algorithm " (pr-str alg)
                      "; known: :sha256, :sha1, :md5")
                 {:alg alg})))

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
    (digest-fail :digest/alg "MDA002"
                 (str "mino.digest: no HMAC algorithm for " (pr-str alg)
                      "; known: :sha256")
                 {:alg alg})))
