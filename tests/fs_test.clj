(require "tests/test")

;; Filesystem primitives: file-exists?, directory?, mkdir-p, rm-rf.

(def ^:private windows? (some? (getenv "OS")))

(def test-dir "/tmp/mino-fs-test")

(deftest file-exists?-known-file
  (is (file-exists? "CHANGELOG.md"))
  (is (file-exists? "src"))
  (is (not (file-exists? "nonexistent-path-xyz"))))

(deftest directory?-basics
  (is (directory? "src"))
  (is (directory? "tests"))
  (is (not (directory? "CHANGELOG.md")))
  (is (not (directory? "nonexistent-path-xyz"))))

(deftest mkdir-p-and-rm-rf
  ;; Clean up from any previous run.
  (rm-rf test-dir)
  (is (not (file-exists? test-dir)))

  ;; Create nested directories.
  (mkdir-p (str test-dir "/a/b/c"))
  (is (directory? test-dir))
  (is (directory? (str test-dir "/a")))
  (is (directory? (str test-dir "/a/b")))
  (is (directory? (str test-dir "/a/b/c")))

  ;; Create a file inside.
  (spit (str test-dir "/a/b/file.txt") "hello")
  (is (file-exists? (str test-dir "/a/b/file.txt")))
  (is (not (directory? (str test-dir "/a/b/file.txt"))))

  ;; rm-rf removes the entire tree.
  (rm-rf test-dir)
  (is (not (file-exists? test-dir))))

(deftest mkdir-p-idempotent
  (rm-rf test-dir)
  (mkdir-p test-dir)
  (mkdir-p test-dir) ;; should not error
  (is (directory? test-dir))
  (rm-rf test-dir))

(deftest type-errors
  (is (thrown? (file-exists? 42)))
  (is (thrown? (directory? nil)))
  (is (thrown? (mkdir-p :foo)))
  (is (thrown? (rm-rf 123))))

(deftest rm-rf-does-not-follow-symlinks
  ;; rmrf() used stat() which follows symlinks; a symlink-to-directory
  ;; planted inside the removal tree caused rmrf to recurse through it
  ;; and delete the *target* directory's contents (CWE-59).
  ;; The fix changes stat() to lstat() so symlinks are unlinked directly.
  ;;
  ;; POSIX-only: builds the symlink with `ln -s`, which Windows has no
  ;; equivalent for (mklink needs a privileged shell), and tests
  ;; lstat-based unlink semantics that do not apply to Windows
  ;; reparse-point handling. Skipped there; full coverage stays on POSIX.
  (when-not windows?
  (let [sentinel "/tmp/mino-fs-symlink-sentinel"]
    (try (rm-rf test-dir)   (catch _ nil))
    (try (rm-rf sentinel)   (catch _ nil))
    (mkdir-p sentinel)
    (spit (str sentinel "/victim.txt") "must survive")
    (mkdir-p test-dir)
    ;; Plant a symlink-to-dir inside the tree we are about to remove.
    (sh! "ln" "-s" sentinel (str test-dir "/evil-link"))
    ;; rm-rf must remove test-dir (including the symlink) but must NOT
    ;; descend through the symlink into sentinel.
    (try (rm-rf test-dir) (catch _ nil))
    (is (file-exists? (str sentinel "/victim.txt"))
        "victim file inside symlink target was deleted")
    (try (rm-rf test-dir)   (catch _ nil))
    (try (rm-rf sentinel)   (catch _ nil)))))

(run-tests-and-exit)
