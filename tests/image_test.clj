(require "tests/test")

(deftest image-save-load-simple-value
  ;; Save a state with a simple var, load into fresh state, verify.
  (rm-rf "/tmp/mino-image-test")
  (mkdir-p "/tmp/mino-image-test")
  (let [path "/tmp/mino-image-test/simple.img"]
    (eval "(do (def saved-val 42) (def saved-str \"hello\"))")
    (save-image path)
    (is (file-exists? path) "image file created")
    (load-image-into path)
    (is (= 42 saved-val) "integer var restored")
    (is (= "hello" saved-str) "string var restored"))
  (rm-rf "/tmp/mino-image-test"))

(deftest image-save-load-collection
  ;; Collections (vectors, maps, sets) survive save/load.
  (rm-rf "/tmp/mino-image-test")
  (mkdir-p "/tmp/mino-image-test")
  (let [path "/tmp/mino-image-test/coll.img"]
    (eval "(def coll-vec [1 2 3])")
    (eval "(def coll-map {:a 1 :b 2})")
    (eval "(def coll-set #{1 2 3})")
    (save-image path)
    (load-image-into path)
    (is (= [1 2 3] coll-vec) "vector restored")
    (is (= {:a 1 :b 2} coll-map) "map restored")
    (is (= #{1 2 3} coll-set) "set restored"))
  (rm-rf "/tmp/mino-image-test"))

(deftest image-save-load-fn
  ;; User-defined functions survive save/load and are callable.
  (rm-rf "/tmp/mino-image-test")
  (mkdir-p "/tmp/mino-image-test")
  (let [path "/tmp/mino-image-test/fn.img"]
    (eval "(defn add-one [x] (+ x 1))")
    (save-image path)
    (load-image-into path)
    (is (= 43 (add-one 42)) "restored fn is callable"))
  (rm-rf "/tmp/mino-image-test"))

(deftest image-save-load-atom
  ;; Atoms preserve their value across save/load.
  (rm-rf "/tmp/mino-image-test")
  (mkdir-p "/tmp/mino-image-test")
  (let [path "/tmp/mino-image-test/atom.img"]
    (eval "(def counter (atom 100))")
    (save-image path)
    (load-image-into path)
    (is (= 100 @counter) "atom value restored"))
  (rm-rf "/tmp/mino-image-test"))

(run-tests-and-exit)
