(require "tests/test")

;; mino.tar facade pins: entries/read/extract over the tar prims,
;; create from entry maps or a directory tree, and tar.gz composed
;; from the existing gzip stream at the mino level.
;;
;; The facade is called through resolve (never a top-level alias):
;; until lib/mino/tar.clj lands, the guarded require below fails and
;; every call site errors inside its own test instead of aborting
;; the shared suite load. The pinned contract:
;; - publics are exactly entries, read, extract, create;
;; - create takes an entry-map vector ({:name :data :mode :mtime
;;   :type :linkname}, defaults :type :file, file mode 0644, dir
;;   0755, symlink 0777, :mtime nil stored as the zero field) OR a
;;   directory path (sorted walk, names relative, directories with
;;   a trailing slash, symlinks stored as link entries and never
;;   followed), deterministically;
;; - {:gzip true} wraps create's output in one gzip member;
;; - the read-side facade fns transparently gunzip gzip input,
;;   while the bare tar prims stay strict (gzip bytes are
;;   :codec/magic there);
;; - stat's :mtime is MILLISECONDS and tar stores SECONDS, so every
;;   stat-basis equality here floors: (quot mtime-ms 1000) is the
;;   archive's :mtime seconds.

(def ^:private windows? (some? (getenv "OS")))

(def ^:private tarf-load-error
  ;; nil once lib/mino/tar.clj exists; the load failure otherwise.
  (try (do (require '[mino.tar]) nil) (catch e (identity e))))

(defn- tarf
  "Resolve and call a mino.tar public by name."
  [fname & args]
  (let [v (resolve (symbol "mino.tar" (name fname)))]
    (if (nil? v)
      (throw (ex-info (str "mino.tar/" (name fname) " is unresolved")
                      {:mino/kind :name}))
      (apply v args))))

(defn- tarf-kind
  "Run thunk; return :ok on success or the thrown :mino/kind."
  [thunk]
  (try (do (thunk) :ok) (catch e (:mino/kind e))))

(defn- tarf-fixture
  "Read a binary fixture as bytes (the base64 pair round-trips the
  raw slurp bytes losslessly)."
  [name]
  (base64-decode (base64-encode (slurp (str "tests/fixtures/tar/" name)))))

(def ^:private tarf-blob
  ;; deterministic pseudo-binary bytes: a seeded LCG over 300 bytes
  (let [n 300]
    (loop [i 0 seed 20260902 acc (transient [])]
      (if (= i n)
        (byte-array (persistent! acc))
        (let [s (long (mod (+ (* seed 1103515245) 12345) 2147483648))]
          (recur (inc i) s (conj! acc (mod (quot s 65536) 256))))))))

(def ^:private tarf-entry-maps
  [{:name "report.csv" :data "a,b\n1,2\n" :mode 0640 :mtime 1700000100}
   {:name "dir/" :type :dir :mode 0750 :mtime 1700000101}
   {:name "dir/blob.bin" :data tarf-blob}
   {:name "ln" :type :symlink :linkname "report.csv"}])

;;; the facade loads and its surface is capped

(deftest tarf-namespace-loads
  (is (nil? tarf-load-error) "lib/mino/tar.clj loads"))

(deftest tarf-surface-is-exactly-entries-read-extract-create
  (is (= #{'entries 'read 'extract 'create}
         (set (keys (ns-publics 'mino.tar))))))

(deftest tarf-every-public-carries-a-docstring
  (doseq [v (vals (ns-publics 'mino.tar))]
    (is (string? (:doc (meta v))) (str (:name (meta v)) " undocumented"))))

;;; the thin aliases, pinned as equalities on real data

(deftest tarf-entries-aliases-tar-entries
  (let [b (tarf-fixture "basic.tar")]
    (is (= (tar-entries b) (tarf 'entries b)))))

(deftest tarf-read-aliases-tar-read
  (let [b (tarf-fixture "basic.tar")]
    (is (= (tar-read b "hello.txt") (tarf 'read b "hello.txt")))))

(deftest tarf-extract-aliases-tar-extract
  (when-not windows?
    (with-temp-dir [d1]
      (with-temp-dir [d2]
        (let [b (tarf-fixture "basic.tar")]
          (is (= (tar-extract b d1) (tarf 'extract b d2))
              "same names vector through prim and facade")
          (is (= (slurp (str d1 "/hello.txt"))
                 (slurp (str d2 "/hello.txt")))))))))

;;; create from entry maps

(deftest tarf-create-is-deterministic-bytes
  (let [a (tarf 'create tarf-entry-maps)]
    (is (bytes? a))
    (is (= a (tarf 'create tarf-entry-maps))
        "same entries give byte-identical archives")))

(deftest tarf-create-then-entries-round-trips-the-maps
  ;; The listing mirrors the input maps under the documented
  ;; defaults: omitted :type is :file, file mode 0644, symlink mode
  ;; 0777, omitted :mtime stores the zero field and lists as nil.
  (is (= [{:name "report.csv" :size 8 :mode 0640 :mtime 1700000100
           :type :file :linkname nil}
          {:name "dir/" :size 0 :mode 0750 :mtime 1700000101
           :type :dir :linkname nil}
          {:name "dir/blob.bin" :size 300 :mode 0644 :mtime nil
           :type :file :linkname nil}
          {:name "ln" :size 0 :mode 0777 :mtime nil
           :type :symlink :linkname "report.csv"}]
         (vec (tarf 'entries (tarf 'create tarf-entry-maps))))))

(deftest tarf-create-then-read-returns-the-written-bytes
  ;; string :data contributes UTF-8 bytes (the digest.c rule)
  (let [a (tarf 'create tarf-entry-maps)]
    (is (= (byte-array (map int "a,b\n1,2\n")) (tarf 'read a "report.csv")))
    (is (= tarf-blob (tarf 'read a "dir/blob.bin")))))

;;; tar.gz: composed from the existing gzip stream

(deftest tarf-gzip-opt-wraps-create-in-one-gzip-member
  (let [plain (tarf 'create tarf-entry-maps)
        gz (tarf 'create tarf-entry-maps {:gzip true})]
    (is (not= plain gz))
    (is (= plain (gzip-decompress gz))
        "gunzipping the .gz output yields the plain archive bytes")
    (is (= gz (tarf 'create tarf-entry-maps {:gzip true}))
        "gzip output is deterministic too")))

(deftest tarf-read-side-transparently-gunzips
  (let [plain (tarf 'create tarf-entry-maps)
        gz (tarf 'create tarf-entry-maps {:gzip true})]
    (is (= (tarf 'entries plain) (tarf 'entries gz)))
    (is (= (tarf 'read plain "report.csv") (tarf 'read gz "report.csv")))))

(deftest tarf-prims-stay-strict-on-gzip-bytes
  ;; The transparent gunzip is a facade ergonomic; the bare prim on
  ;; a gzip stream is not-a-tar.
  (is (= :codec/magic
         (tarf-kind #(tar-entries (gzip-compress (tarf-fixture "basic.tar")))))))

;;; create from a directory tree, equal on a stat basis

(deftest tarf-create-from-a-directory-round-trips-on-a-stat-basis
  ;; POSIX-only for the symlink and mode arms. The walk is sorted,
  ;; names are relative to the given directory, directories carry a
  ;; trailing slash, and the symlink is stored as a link entry (the
  ;; copy-tree link policy), never followed.
  (when-not windows?
    (with-temp-dir [src]
      (with-temp-dir [dest]
        (spit (str src "/a.txt") "alpha\n")
        (chmod (str src "/a.txt") 0640)
        (mkdir-p (str src "/sub"))
        (chmod (str src "/sub") 0750)
        (spit (str src "/sub/b.txt") "beta payload\n")
        (symlink "a.txt" (str src "/ln"))
        (let [a (tarf 'create src)
              es (tarf 'entries a)
              by-name (into {} (map (juxt :name identity) es))]
          (is (= ["a.txt" "ln" "sub/" "sub/b.txt"] (mapv :name es))
              "sorted walk, relative names, trailing slash on dirs")
          (is (= (:mode (stat (str src "/a.txt")))
                 (:mode (get by-name "a.txt"))))
          (is (= (quot (:mtime (stat (str src "/a.txt"))) 1000)
                 (:mtime (get by-name "a.txt")))
              "stat's milliseconds floor to the archive's seconds")
          (is (= :symlink (:type (get by-name "ln"))))
          (is (= "a.txt" (:linkname (get by-name "ln")))
              "the link itself is stored, never its target's bytes")
          (tarf 'extract a dest)
          (doseq [rel ["a.txt" "sub" "sub/b.txt"]]
            (let [s1 (stat (str src "/" rel))
                  s2 (stat (str dest "/" rel))]
              (is (= (:type s1) (:type s2)) (str rel " type"))
              (when (= :file (:type s1))
                ;; directory :size is filesystem bookkeeping, not
                ;; archive content; only file sizes must agree
                (is (= (:size s1) (:size s2)) (str rel " size")))
              (is (= (:mode s1) (:mode s2)) (str rel " mode"))
              (is (= (quot (:mtime s1) 1000) (quot (:mtime s2) 1000))
                  (str rel " mtime on the tar seconds basis"))))
          (is (= "a.txt" (read-symlink (str dest "/ln")))))))))

(run-tests-and-exit)
