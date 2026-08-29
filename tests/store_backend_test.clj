(require "tests/test")
(require '[mino.store :as sstore])
(require '[clojure.string :as str])

;; mino.store backend seam (ADR 35), third-party side. A backend that
;; is neither :memory nor :file rides the whole lifecycle through the
;; seam: open registers it, transact appends before publish through its
;; :commit, checkpoint and close own their own bytes, reopen replays
;; its segments with the db logic staying in mino.store. Malformed
;; custom backends are rejected with classified errors.

(def backend-test-dir "/tmp/mino-store-backend-test")

(defn atom-byte-store
  "A third-party byte store: a snapshot slot plus a WAL vector, both
  plain data."
  []
  (atom {:snapshot nil :wal [] :db-at-append nil}))

(defn atom-backend
  "A backend over an atom byte store, tagged with a fresh :kind. Each
  op touches only its own bytes: :commit appends the tx before the
  publish (recording the still-unpublished conn as the ordering
  witness), :checkpoint stores the current db and clears the WAL,
  :close final-checkpoints then releases the handle. The publish and
  the handle release ride the same prims every backend rides."
  [store]
  {:kind :atom-store
   :initial (fn [] (:snapshot @store))
   :wal-entries (fn [] (let [w (:wal @store)] (when (seq w) w)))
   :commit (fn [conn new-db tx-info]
             (swap! store assoc :db-at-append @conn)
             (swap! store update :wal conj tx-info)
             (store-commit* conn new-db tx-info))
   :checkpoint (fn [conn]
                 (swap! store assoc :snapshot @conn :wal [])
                 nil)
   :close (fn [conn]
            (swap! store assoc :snapshot @conn :wal [])
            (store-close* conn))})

(deftest custom-backend-round-trips-through-the-seam
  (let [store (atom-byte-store)
        backend (atom-backend store)
        conn (sstore/open nil {:backend backend})]
    (is (= backend (sstore/backend-for conn))
        "the prebuilt third-party map registers as-is")
    (is (= :atom-store (:kind (sstore/backend-for conn)))
        "neither built-in kind")
    (sstore/transact conn {1 {:name "Alice" :age 30}})
    (is (= 0 (:tx (:db-at-append @store)))
        "the backend saw the conn before the publish: append first")
    (is (= 1 (:tx (sstore/db conn))) "the publish took effect")
    ;; Crash without checkpoint or close: only the WAL survives.
    (sstore/dissoc-on-close conn)
    (let [conn2 (sstore/open nil {:backend backend})
          db2 (sstore/db conn2)]
      (is (= "Alice" (sstore/read db2 1 :name)) "WAL replay recovered the tx")
      (is (= 30 (sstore/read db2 1 :age)))
      (is (= 1 (:tx db2)) "the tx counter survived the replay")
      (sstore/checkpoint conn2)
      (is (= db2 (:snapshot @store)) "checkpoint stored the db value")
      (is (= [] (:wal @store)) "checkpoint cleared the WAL")
      (sstore/transact conn2 {2 {:name "Bob"}})
      (is (= 1 (count (:wal @store))) "a fresh WAL started after checkpoint")
      (sstore/close conn2)
      (is (nil? (sstore/backend-for conn2)) "close deregistered the backend")
      (is (= 2 (:tx (:snapshot @store))) "close final-checkpointed")
      (let [conn3 (sstore/open nil {:backend backend})
            db3 (sstore/db conn3)]
        (is (= "Alice" (sstore/read db3 1 :name)) "from the snapshot segment")
        (is (= "Bob" (sstore/read db3 2 :name))
            "close's final checkpoint captured the later tx")
        (is (= 2 (:tx db3)))
        (sstore/close conn3)))))

(deftest custom-backend-keeps-db-logic-in-the-store
  (let [store (atom-byte-store)
        backend (atom-backend store)
        conn (sstore/open nil {:backend backend
                               :schema {:name {:type :string}}
                               :closed true
                               :history {:keep-last 1}})]
    (let [e (try
              (sstore/transact conn {1 {:nickname "A"}})
              nil
              (catch Throwable e e))]
      (is (some? e) "an attribute outside the closed schema is rejected")
      (is (= [] (:wal @store))
          "the rejected tx never reached the backend's append"))
    (sstore/transact conn {1 {:name "A"} 2 {:name "B"}})
    (sstore/transact conn {1 {:name "A2"} 2 {:name "B2"}})
    (let [live (sstore/db conn)]
      (is (= 1 (count (:log live)))
          "the history policy compacted the log inside mino.store")
      (is (= "A2" (sstore/read live 1 :name)) "the entities view survived")
      (sstore/checkpoint conn)
      (sstore/close conn)
      (let [conn2 (sstore/open nil {:backend backend})]
        (is (= live (sstore/db conn2))
            "schema, history, and the compacted log rode the db value")
        (let [e (try
                  (sstore/transact conn2 {3 {:nickname "C"}})
                  nil
                  (catch Throwable e e))]
          (is (some? e) "the closed schema survived the reopen"))
        (sstore/close conn2)))))

(deftest open-rejects-malformed-custom-backends
  ;; A fresh :kind keyword is valid (that acceptance is the third-party
  ;; proof above); what throws is a structurally broken map.
  (let [good (atom-backend (atom-byte-store))
        rejected (fn [backend]
                   (try
                     (sstore/open nil {:backend backend})
                     nil
                     (catch Throwable e e)))
        e1 (rejected (dissoc good :commit))
        e2 (rejected (assoc good :kind "atom-store"))
        e3 (rejected 42)]
    (is (true? (sstore/backend? good))
        "the well-formed custom backend passes validation")
    (is (every? some? [e1 e2 e3]) "each malformed shape throws")
    (doseq [e [e1 e2 e3]]
      (is (some? (re-find #"invalid-backend" (pr-str (ex-data e))))
          "ex-data carries the ::invalid-backend tag"))
    (is (some? (re-find #":commit" (pr-str (ex-data e1))))
        "the missing-op error names the op")))

(run-tests-and-exit)
