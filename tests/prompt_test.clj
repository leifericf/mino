(require "tests/test")
(require '[clojure.string :as str])
(require 'mino.term)

;; mino.term/prompt and mino.term/confirm: interactive line-read helpers.
;;
;; The pure core (parse-yes-no, render-prompt-line, prompt-result) takes
;; data and returns data; the effectful shells (prompt, confirm) inject
;; the read source via :read-fn so every loop can be tested without a tty.
;;
;; All top-level names in this file carry the pt- prefix to avoid
;; collisions in the shared single-process suite.

;;;; helpers

(defn- pt-bad
  "The :mino/kind of the throw from calling f, or :no-throw on success."
  [f]
  (try (f) :no-throw (catch e (:mino/kind e))))

(defn- pt-lines-reader
  "Returns a 0-arg fn that pops lines from coll in order, then returns nil."
  [coll]
  (let [a (atom (seq coll))]
    (fn []
      (let [[h & t] @a]
        (reset! a t)
        h))))

;;;; parse-yes-no: pure fn mapping a raw input string to true/false/nil

(deftest pt-parse-yes-no-accepts-y-forms
  ;; y and yes in any case map to true
  (is (true?  (mino.term/parse-yes-no "y")))
  (is (true?  (mino.term/parse-yes-no "Y")))
  (is (true?  (mino.term/parse-yes-no "yes")))
  (is (true?  (mino.term/parse-yes-no "YES")))
  (is (true?  (mino.term/parse-yes-no "Yes")))
  (is (true?  (mino.term/parse-yes-no "yEs"))))

(deftest pt-parse-yes-no-accepts-n-forms
  ;; n and no in any case map to false
  (is (false? (mino.term/parse-yes-no "n")))
  (is (false? (mino.term/parse-yes-no "N")))
  (is (false? (mino.term/parse-yes-no "no")))
  (is (false? (mino.term/parse-yes-no "NO")))
  (is (false? (mino.term/parse-yes-no "No")))
  (is (false? (mino.term/parse-yes-no "nO"))))

(deftest pt-parse-yes-no-returns-nil-for-blank
  ;; blank input signals "use default"; nil is the sentinel
  (is (nil? (mino.term/parse-yes-no "")))
  (is (nil? (mino.term/parse-yes-no "  "))))

(deftest pt-parse-yes-no-returns-nil-for-unrecognized
  ;; anything else is not a valid answer; nil triggers a re-prompt
  (is (nil? (mino.term/parse-yes-no "maybe")))
  (is (nil? (mino.term/parse-yes-no "yep")))
  (is (nil? (mino.term/parse-yes-no "nope")))
  (is (nil? (mino.term/parse-yes-no "1"))))

;;;; prompt-result: pure fn deciding the outcome of one raw input line

(deftest pt-prompt-result-returns-trimmed-line-on-valid-input
  ;; without a validator every non-blank line is accepted and returned trimmed
  (is (= "hello" (mino.term/prompt-result "hello" {})))
  ;; whitespace-only input is blank; non-blank input is trimmed
  (is (= "ok" (mino.term/prompt-result "  ok  " {}))))

(deftest pt-prompt-result-returns-default-on-blank
  ;; blank input uses :default when supplied
  (is (= "fallback" (mino.term/prompt-result "" {:default "fallback"})))
  (is (= "fallback" (mino.term/prompt-result "  " {:default "fallback"}))))

(deftest pt-prompt-result-returns-nil-when-no-default-and-blank
  ;; blank with no :default returns nil (caller re-prompts)
  (is (nil? (mino.term/prompt-result "" {})))
  (is (nil? (mino.term/prompt-result "  " {}))))

(deftest pt-prompt-result-applies-validator
  ;; a truthy validator result accepts the line; falsy rejects (returns nil)
  (let [nonempty? #(not (str/blank? %))]
    (is (= "ok"  (mino.term/prompt-result "ok"  {:validate nonempty?})))
    (is (nil?    (mino.term/prompt-result ""    {:validate nonempty?})))
    (is (nil?    (mino.term/prompt-result "   " {:validate nonempty?})))))

(deftest pt-prompt-result-validator-respects-default
  ;; blank with :default short-circuits before :validate runs
  (let [always-reject (constantly false)]
    (is (= "dflt" (mino.term/prompt-result "" {:default "dflt"
                                               :validate always-reject})))))

;;;; confirm loop: re-prompt on invalid, cap-exhaustion throws

(deftest pt-confirm-accepts-y
  (is (true? (mino.term/confirm "Continue?" {:read-fn (pt-lines-reader ["y"])}))))

(deftest pt-confirm-accepts-yes-case-insensitive
  (is (true? (mino.term/confirm "ok?" {:read-fn (pt-lines-reader ["YES"])}))))

(deftest pt-confirm-accepts-n
  (is (false? (mino.term/confirm "ok?" {:read-fn (pt-lines-reader ["n"])}))))

(deftest pt-confirm-accepts-no-case-insensitive
  (is (false? (mino.term/confirm "ok?" {:read-fn (pt-lines-reader ["No"])}))))

(deftest pt-confirm-blank-uses-default-false
  ;; blank with no :default opts defaults to false (the safe no-op side)
  (is (false? (mino.term/confirm "ok?" {:read-fn (pt-lines-reader [""])}))))

(deftest pt-confirm-blank-uses-explicit-default-true
  (is (true? (mino.term/confirm "ok?" {:default true
                                       :read-fn (pt-lines-reader [""])}))))

(deftest pt-confirm-re-prompts-on-invalid-then-takes-valid
  ;; "maybe" is invalid; the second answer "y" is taken
  (is (true? (mino.term/confirm "ok?"
                                {:read-fn (pt-lines-reader ["maybe" "y"])}))))

(deftest pt-confirm-re-prompts-multiple-invalid-then-valid
  ;; two invalid answers followed by a valid one
  (is (false? (mino.term/confirm "ok?"
                                 {:read-fn (pt-lines-reader ["junk" "nope" "n"])}))))

(deftest pt-confirm-cap-exhaustion-throws-term-prompt
  ;; exceeding :max-tries throws :term/prompt
  (is (= :term/prompt
         (pt-bad (fn []
                   (mino.term/confirm "ok?"
                                      {:max-tries 2
                                       :read-fn (pt-lines-reader ["junk" "junk"])}))))))

(deftest pt-confirm-cap-exhaustion-kind-is-pinned
  ;; :mino/kind is exactly :term/prompt — no drift
  (let [kind (try
               (mino.term/confirm "ok?" {:max-tries 1
                                         :read-fn (pt-lines-reader ["bad"])})
               :no-throw
               (catch e (:mino/kind e)))]
    (is (= :term/prompt kind)
        ":term/prompt is the pinned kind for cap-exhaustion")))

(deftest pt-confirm-default-cap-is-three
  ;; the default attempt cap is 3; three invalid inputs exhaust it
  (is (= :term/prompt
         (pt-bad (fn []
                   (mino.term/confirm "ok?"
                                      {:read-fn (pt-lines-reader ["x" "x" "x"])}))))))

(deftest pt-confirm-cap-of-four-does-not-throw-on-third-bad
  ;; with :max-tries 4, three invalid answers do not yet exhaust the cap
  (is (false? (mino.term/confirm "ok?"
                                 {:max-tries 4
                                  :read-fn (pt-lines-reader ["x" "x" "x" "n"])}))))

;;;; prompt loop: re-prompt on invalid, cap-exhaustion throws

(deftest pt-prompt-returns-entered-line
  (is (= "hello"
         (mino.term/prompt "Enter:" {:read-fn (pt-lines-reader ["hello"])}))))

(deftest pt-prompt-blank-uses-default
  (is (= "fallback"
         (mino.term/prompt "Enter:" {:default "fallback"
                                     :read-fn (pt-lines-reader [""])}))))

(deftest pt-prompt-blank-with-no-default-re-prompts
  ;; blank with no :default is invalid; second answer "ok" is taken
  (is (= "ok"
         (mino.term/prompt "Enter:" {:read-fn (pt-lines-reader ["" "ok"])}))))

(deftest pt-prompt-validate-reject-then-accept
  (let [nonempty? (fn [s] (not (str/blank? s)))]
    (is (= "good"
           (mino.term/prompt "Enter:" {:validate nonempty?
                                       :read-fn (pt-lines-reader ["" "good"])})))))

(deftest pt-prompt-validate-cap-exhaustion
  (let [always-no (constantly false)]
    (is (= :term/prompt
           (pt-bad (fn []
                     (mino.term/prompt "Enter:" {:validate always-no
                                                  :max-tries 2
                                                  :read-fn (pt-lines-reader ["a" "b"])})))))))

(deftest pt-prompt-cap-exhaustion-kind-is-pinned
  ;; :term/prompt is the cap-exhaustion kind for prompt too
  (let [always-no (constantly false)]
    (is (= :term/prompt
           (pt-bad (fn []
                     (mino.term/prompt "Enter:" {:validate always-no
                                                  :max-tries 1
                                                  :read-fn (pt-lines-reader ["x"])})))))))

(deftest pt-prompt-default-cap-is-three
  (let [always-no (constantly false)]
    (is (= :term/prompt
           (pt-bad (fn []
                     (mino.term/prompt "Enter:" {:validate always-no
                                                  :read-fn (pt-lines-reader ["a" "b" "c"])})))))))

(deftest pt-prompt-password-routes-through-injected-read-fn
  ;; :password true normally switches to read-password; with :read-fn
  ;; injected, the loop must still call the injected fn regardless of
  ;; the :password flag (the test proves routing without a tty)
  (is (= "s3cr3t"
         (mino.term/prompt "Password:" {:password true
                                         :read-fn (pt-lines-reader ["s3cr3t"])}))))

(deftest pt-prompt-password-blank-returns-default
  ;; blank password input returns :default (not blank string)
  (is (= "hunter2"
         (mino.term/prompt "Password:" {:password true
                                         :default "hunter2"
                                         :read-fn (pt-lines-reader [""])}))))

(run-tests-and-exit)
