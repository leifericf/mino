(require "tests/test")

;; fs verbs: stat, file-size, chmod, symlink/read-symlink, copy,
;; copy-tree, mkdtemp/mkstemp, with-temp-dir/with-temp-file, and
;; advisory file locks (flock/funlock/with-file-lock).
;;
;; Pinned contracts:
;; - stat defaults to lstat semantics (:follow-links? false), matching
;;   rm-rf's AT_SYMLINK_NOFOLLOW discipline; returns nil for a missing
;;   path; the map carries exactly :type :size :mode :mtime :symlink?.
;; - file-size returns nil for a missing path, like file-mtime.
;; - copy is a regular-file verb: a symlink source throws; trees (and
;;   the links inside them) go through copy-tree, which copies a
;;   symlink entry as a symlink pointing at the same target.
;; - flock with {:block false} returns nil when the lock is held
;;   elsewhere; contention is an expected outcome, not an error.

(def ^:private windows? (some? (getenv "OS")))

(def ^:private root "/tmp/mino-fs-verbs-test")

(defn- reset-root! []
  (try (rm-rf root) (catch _ nil))
  (mkdir-p root))

(defn- await-file
  "Poll until path exists, roughly five seconds at most. True when it
  appeared, false on timeout."
  ([path] (await-file path 100))
  ([path tries]
   (cond (file-exists? path) true
         (zero? tries)       false
         :else (do (thread-sleep 50)
                   (await-file path (dec tries))))))

(deftest stat-map-freezes-its-keys
  (reset-root!)
  (let [p (str root "/plain.txt")]
    (spit p "hello")
    (let [st (stat p)]
      (is (= #{:type :size :mode :mtime :symlink?} (set (keys st)))
          "stat map carries exactly the five documented keys")
      (is (= :file (:type st)))
      (is (= 5 (:size st)))
      (is (int? (:mode st)))
      (is (<= 0 (:mode st) 07777) "mode is the low permission bits")
      (is (= (file-mtime p) (:mtime st))
          "mtime shares file-mtime's millisecond basis")
      (is (false? (:symlink? st))))
    (is (= :dir (:type (stat root))))
    (is (nil? (stat (str root "/absent"))) "missing path stats to nil")))

(deftest stat-distinguishes-link-and-target
  ;; POSIX-only: symlink creation on Windows needs a privileged shell.
  (when-not windows?
    (reset-root!)
    (let [target (str root "/target.txt")
          link   (str root "/link")]
      (spit target "1234567")
      (is (nil? (symlink target link)))
      (is (= target (read-symlink link)))
      (let [lst (stat link)]
        (is (= :symlink (:type lst)) "default stat does not follow")
        (is (true? (:symlink? lst))))
      (let [fst (stat link {:follow-links? true})]
        (is (= :file (:type fst)) "follow-links reports the target type")
        (is (= 7 (:size fst)) "follow-links reports the target size")
        (is (true? (:symlink? fst))
            ":symlink? describes the path itself, even when following")))))

(deftest file-size-counts-bytes
  (reset-root!)
  (let [p (str root "/sized.txt")]
    (spit p "abcdefgh")
    (is (= 8 (file-size p)))
    (is (= (:size (stat p)) (file-size p)))
    (is (nil? (file-size (str root "/absent")))
        "missing path is nil, like file-mtime")))

(deftest chmod-round-trips-through-stat
  (reset-root!)
  (let [p (str root "/perms.txt")]
    (spit p "x")
    (is (nil? (chmod p 0644)))
    ;; Exact permission bits are a POSIX contract; Windows keeps only
    ;; the read-only bit, so the round-trip assertions stay POSIX-only.
    (when-not windows?
      (is (= 0644 (bit-and (:mode (stat p)) 0777)))
      (chmod p 0600)
      (is (= 0600 (bit-and (:mode (stat p)) 0777))))))

(deftest copy-preserves-contents-and-mode
  (reset-root!)
  (let [src (str root "/src.txt")
        dst (str root "/dst.txt")]
    (spit src "copy me")
    (chmod src 0640)
    (copy src dst)
    (is (= "copy me" (slurp dst)))
    (when-not windows?
      (is (= 0640 (bit-and (:mode (stat dst)) 0777))
          "copy preserves the source mode"))
    ;; Without :replace an existing destination is an error, as data.
    (is (thrown? (copy src dst)))
    (spit src "fresh bytes")
    (copy src dst {:replace true})
    (is (= "fresh bytes" (slurp dst)))))

(deftest copy-rejects-a-symlink-source
  ;; copy never dereferences a symlink source into a write
  ;; (O_NOFOLLOW discipline); trees with links go through copy-tree.
  (when-not windows?
    (reset-root!)
    (let [target (str root "/real.txt")
          link   (str root "/alias")]
      (spit target "data")
      (symlink target link)
      (is (thrown? (copy link (str root "/out.txt")))))))

(deftest copy-tree-copies-nested-dirs-and-keeps-links-as-links
  (reset-root!)
  (let [src (str root "/tree")
        dst (str root "/tree-copy")]
    (mkdir-p (str src "/a/b"))
    (spit (str src "/a/b/deep.txt") "deep contents")
    (spit (str src "/top.txt") "top")
    (spit (str root "/outside.txt") "outside")
    (when-not windows?
      (symlink (str root "/outside.txt") (str src "/a/pointer")))
    (copy-tree src dst)
    (is (directory? (str dst "/a/b")))
    (is (= "deep contents" (slurp (str dst "/a/b/deep.txt"))))
    (is (= "top" (slurp (str dst "/top.txt"))))
    (when-not windows?
      ;; Pinned policy: a symlink entry is copied as a symlink to the
      ;; same target, never dereferenced into a file copy.
      (let [st (stat (str dst "/a/pointer"))]
        (is (true? (:symlink? st)))
        (is (= :symlink (:type st))))
      (is (= (str root "/outside.txt")
             (read-symlink (str dst "/a/pointer")))))))

(deftest mkdtemp-makes-unique-private-dirs
  (let [d1 (mkdtemp)
        d2 (mkdtemp)
        d3 (mkdtemp "minovtest")]
    (try
      (is (directory? d1))
      (is (directory? d2))
      (is (not= d1 d2) "successive calls yield distinct names")
      (is (re-find #"minovtest" d3) "prefix survives into the name")
      (when-not windows?
        (is (= 0700 (bit-and (:mode (stat d1)) 0777))
            "fresh temp dir is private"))
      (finally
        (rm-rf d1) (rm-rf d2) (rm-rf d3)))))

(deftest mkstemp-makes-unique-private-files
  (let [f1 (mkstemp)
        f2 (mkstemp)
        f3 (mkstemp "minovtest")]
    (try
      (is (file-exists? f1))
      (is (= :file (:type (stat f1))))
      (is (= 0 (file-size f1)) "fresh temp file is empty")
      (is (not= f1 f2) "successive calls yield distinct names")
      (is (re-find #"minovtest" f3) "prefix survives into the name")
      (when-not windows?
        (is (= 0600 (bit-and (:mode (stat f1)) 0777))
            "fresh temp file is private"))
      (finally
        (rm-rf f1) (rm-rf f2) (rm-rf f3)))))

(deftest with-temp-dir-cleans-up-on-normal-exit
  (let [seen   (atom nil)
        result (with-temp-dir [d]
                 (reset! seen d)
                 (is (directory? d) "dir exists inside the body")
                 (spit (str d "/scratch.txt") "scratch")
                 :done)]
    (is (= :done result) "body value is the expression value")
    (is (string? @seen))
    (is (not (file-exists? @seen)) "dir and contents removed on exit")))

(deftest with-temp-dir-cleans-up-when-the-body-throws
  (let [seen (atom nil)]
    (try
      (with-temp-dir [d]
        (reset! seen d)
        (throw (ex-info "boom" {})))
      (catch _ nil))
    (is (string? @seen) "body ran before throwing")
    (is (not (file-exists? @seen)) "dir removed on the unwind path")))

(deftest with-temp-file-cleans-up-on-both-paths
  (let [seen (atom nil)]
    (with-temp-file [f]
      (reset! seen f)
      (is (file-exists? f))
      (spit f "temp bytes")
      (is (= "temp bytes" (slurp f))))
    (is (not (file-exists? @seen)) "file removed on normal exit"))
  (let [seen (atom nil)]
    (try
      (with-temp-file [f]
        (reset! seen f)
        (throw (ex-info "boom" {})))
      (catch _ nil))
    (is (not (file-exists? @seen)) "file removed on the unwind path")))

(deftest flock-hands-back-a-reusable-handle
  (reset-root!)
  (let [lock (str root "/basic.lock")
        h    (flock lock)]
    (is (some? h) "acquisition yields an opaque handle")
    (is (nil? (funlock h)))
    ;; Released means a fresh non-blocking acquisition succeeds.
    (let [h2 (flock lock {:block false})]
      (is (some? h2))
      (funlock h2))))

(deftest with-file-lock-releases-on-the-unwind-path
  (reset-root!)
  (let [lock (str root "/scoped.lock")]
    (try
      (with-file-lock [h lock]
        (is (some? h))
        (throw (ex-info "boom" {})))
      (catch _ nil))
    ;; The unwind released it: a non-blocking retake succeeds.
    (let [h (flock lock {:block false})]
      (is (some? h) "lock released after the throwing body")
      (funlock h))))

(deftest flock-excludes-a-second-process
  ;; Two child processes contend for one exclusive lock. The holder
  ;; child takes it and signals readiness through a marker file; the
  ;; contender child then tries a non-blocking acquire and must lose;
  ;; once the holder releases, a fresh child must win.
  ;; POSIX-only: the orchestration shells POSIX-shaped children.
  (when-not windows?
    (reset-root!)
    (let [lock    (str root "/contended.lock")
          ready   (str root "/holder-ready")
          release (str root "/holder-release")
          probe   (str "(println (if (flock \"" lock "\" {:block false})"
                       " \"ACQUIRED\" \"LOCKED\"))")
          holder  (future
                    (sh "./mino" "-e"
                        (str "(let [h (flock \"" lock "\")]"
                             " (spit \"" ready "\" \"held\")"
                             " ((fn wait [n]"
                             "    (when (and (pos? n)"
                             "               (not (file-exists? \"" release "\")))"
                             "      (thread-sleep 50)"
                             "      (wait (dec n)))) 200)"
                             " (funlock h))")))]
      (is (await-file ready) "holder child signalled it holds the lock")
      (let [r (sh "./mino" "-e" probe)]
        (is (re-find #"LOCKED" (:out r))
            "contender loses while the holder is alive"))
      (spit release "go")
      @holder
      (let [r (sh "./mino" "-e" probe)]
        (is (re-find #"ACQUIRED" (:out r))
            "lock is free once the holder releases")))))

(run-tests-and-exit)
