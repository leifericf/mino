(require "tests/test")

;; Filesystem primitives: file-exists?, directory?, mkdir-p, rm-rf.

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

(run-tests)
