(require "tests/test")

;; STM concurrency tests. Skipped under thread_limit=1 since the
;; retry path never lights up there. The standalone `./mino` binary
;; raises the limit to cpu_count after install, so these run by
;; default; embedders that explicitly set thread_limit=1 see the
;; tests as passing-with-no-asserts.

(deftest concurrent-alter-increment
  (when (> (mino-thread-limit) 1)
    (let [r (ref 0)
          n 200
          workers (min 4 (max 2 (dec (mino-thread-limit))))
          futs (doall (for [_ (range workers)]
                        (future
                          (dotimes [_ n]
                            (dosync (alter r inc))))))]
      (doseq [f futs] @f)
      (is (= (* workers n) @r)))))

(deftest concurrent-commute-increment
  ;; Commute should never lose updates either, despite not
  ;; participating in read-set validation.
  (when (> (mino-thread-limit) 1)
    (let [r (ref 0)
          n 200
          workers (min 4 (max 2 (dec (mino-thread-limit))))
          futs (doall (for [_ (range workers)]
                        (future
                          (dotimes [_ n]
                            (dosync (commute r inc))))))]
      (doseq [f futs] @f)
      (is (= (* workers n) @r)))))

(deftest concurrent-watch-fires-once-per-commit
  (when (> (mino-thread-limit) 1)
    (let [r (ref 0)
          counter (atom 0)
          n 100
          workers (min 4 (max 2 (dec (mino-thread-limit))))
          _ (add-watch r :w (fn [_ _ _ _] (swap! counter inc)))
          futs (doall (for [_ (range workers)]
                        (future
                          (dotimes [_ n]
                            (dosync (alter r inc))))))]
      (doseq [f futs] @f)
      ;; Each successful commit fires the watch once. Retries don't.
      (is (= (* workers n) @counter)))))
