(ns mino.shell
  "Shell quoting and word-splitting helpers: the safety companions to sh
  and sh!. All functions are pure data-in data-out with no process I/O.

  (require '[mino.shell :as shell])
  (shell/quote \"hello world\")        ; \"'hello world'\"
  (shell/quote \"it's fine\")          ; \"'it'\\''s fine'\"
  (shell/quote \"\")                   ; \"''\"
  (shell/split \"echo 'hello world'\") ; [\"echo\" \"hello world\"]

  POSIX single-quote wrapping: every character inside single quotes is
  literal, so the only escape needed is to end the quote, inject a
  literal single-quote via '\\'' (end quote, backslash-escaped single
  quote, reopen quote), then reopen the quote. The output of (quote s)
  is safe to pass through the sh primitive without shell-injection risk
  regardless of the content of s.

  (split s) performs POSIX word-splitting on the string s, handling
  single quotes, double quotes, and backslash escapes. The :comments
  option (default false) instructs the splitter to stop at a #
  character that is not inside a quote. Returns a vector of strings;
  empty string splits to []. Throws :mino/kind :shell/parse on
  unterminated quotes.

  Security note: (quote s) is injection-safe by construction; (split s)
  is for parsing shell-shaped config lines, not for evaluating
  untrusted input — use sh directly for the latter.")

(require '[clojure.string :as str])

;;;; Quoting

(defn quote
  "Wraps s in POSIX single quotes so the result is safe as a shell word.
  Each embedded single quote becomes '\\''."
  [s]
  (str "'" (str/replace s "'" "'\\''") "'"))

;;;; Splitting

(defn- split-fail
  [msg data]
  (throw {:mino/kind :shell/parse
          :mino/code "MSP001"
          :mino/message msg
          :mino/data data}))

(defn split
  "Splits the string s into a vector of words using POSIX quoting rules.
  Handles single quotes, double quotes, and backslash escapes. With
  {:comments true} a bare # ends the input. Throws :mino/kind
  :shell/parse on an unterminated quote."
  ([s] (split s {}))
  ([s opts]
   (let [comments? (:comments opts false)
         chars (vec s)
         n (count chars)]
     (loop [i 0
            words []
            cur nil   ; nil = no current word; string = in-progress word
            state :bare]  ; :bare :single :double :dquote-esc
       (if (= i n)
         ;; end of input
         (case state
           :single (split-fail "unterminated single quote" {:input s})
           :double (split-fail "unterminated double quote" {:input s})
           :dquote-esc (split-fail "unterminated double-quote escape" {:input s})
           ;; :bare :bare-esc -> flush
           (if (some? cur)
             (conj words cur)
             words))
         (let [c (chars i)]
           (case state
             :bare
             (cond
               (and comments? (= c \#) (nil? cur))
               ;; bare # at word boundary with comments: stop (comment to EOL)
               words

               (= c \')
               (recur (inc i) words (or cur "") :single)

               (= c \")
               (recur (inc i) words (or cur "") :double)

               (= c \\)
               ;; bare backslash: next char is literal; \newline is
               ;; a line-continuation (both chars dropped, POSIX §2.2.1)
               (if (= (inc i) n)
                 (split-fail "trailing backslash" {:input s})
                 (let [nc (chars (inc i))]
                   (if (= nc \newline)
                     (recur (+ i 2) words cur :bare)
                     (recur (+ i 2) words (str (or cur "") nc) :bare))))

               (or (= c \space) (= c \tab) (= c \newline))
               ;; word boundary
               (if (some? cur)
                 (recur (inc i) (conj words cur) nil :bare)
                 (recur (inc i) words nil :bare))

               :else
               (recur (inc i) words (str (or cur "") c) :bare))

             :single
             (if (= c \')
               (recur (inc i) words cur :bare)
               (recur (inc i) words (str cur c) :single))

             :double
             (cond
               (= c \") (recur (inc i) words cur :bare)
               (= c \\) (recur (inc i) words cur :dquote-esc)
               :else    (recur (inc i) words (str cur c) :double))

             :dquote-esc
             ;; In double quotes: \\ \" \$ \` \newline are special;
             ;; anything else preserves the backslash (POSIX §2.2.3).
             (if (#{\" \\ \$ \` \newline} c)
               (recur (inc i) words (str cur c) :double)
               (recur (inc i) words (str cur \\ c) :double)))))))))
