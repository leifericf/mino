# Changelog

## Unreleased

- GC: Trace fn.wraps_prim in MINO_FN/MINO_MACRO GC walker
- GC: Guard float/double fill pointer across alloc_val in mino_host_array_new
- GC: Guard fields_vec pointer across alloc_val in mino_defrecord
- GC: Pin val and key across the mino_symbol intern call in dyn_binding_make
- GC: Harden the binding snapshot with checked capacity growth, pinned roots across allocations, and a heap buffer for long qualified names
- GC: Pre-allocated OOM exception singleton so (catch e ...) receives a {:mino/kind :internal :mino/code "MIN001" :mino/message "out of memory"} map rather than nil when the allocator is exhausted
- GC: fix MSVC build break — thread_count read now uses tc_load() shim instead of __atomic_load_n directly
- GC: fix MSVC build break — __builtin_return_address(0) guarded with __GNUC__/__clang__ check, NULL fallback on MSVC
- GC: fix out-of-bounds finalizer dispatch — type_tag checked against GC_T__COUNT before indexing gc_finalizers[]
- GC: fix alloc-profile counter overflow on Windows — unsigned long fields replaced with uint64_t
- Security: Add size_t overflow guard and malloc NULL check in mino_keyword_ns_n
- Security: Add size_t overflow guard and malloc NULL check in mino_symbol_ns_n
- Security: Add size_t overflow guards in intern table entries-array and ht doubling
- Values: extract eq_map_same_type and eq_set_same_type helpers to bring eq_step under 250 LOC
- Values: Guard mino_host_array_new against len*sizeof overflow (security-values-002)
- Values: Pin head across allocating GC calls in mino_host_array_from_coll generic seq path (memory-values-001)
- Values: Guard eq_stack_push capacity doubling against SIZE_MAX overflow (security-values-001)
- Core: clojure.core gains seventeen vars: `line-seq`, `seque`, `sync`, `xml-seq`, `read+string`, `test`, `Throwable->map`, `print-simple`, `->Eduction`, the `Inst` protocol with `inst-ms*`, the `char-escape-string` and `char-name-string` tables, `default-data-readers` (with 'inst and 'uuid readers), `*repl*` (default false), and the `unquote` / `unquote-splicing` placeholders.
- Core: Add \delete to char-name-string and add \delete reader literal support
- Core: Register uuid reader in *data-readers* alongside inst reader
- Core: Add :type and :at keys to Throwable->map :via entries for canon conformance
- Core: Update var-get docstring to reflect thread-local binding lookup precedence
- Lib: `clojure.data/diff` now trims trailing nils from the both slot, so `(diff [1 2 3] [1 2 4])` reports both `[1 2]` instead of `[1 2 nil]`. Shared map keys whose values are nil on both sides land in both: `(diff {:a nil :b 1} {:a nil})` returns `[{:b 1} nil {:a nil}]`.
- Lib: `clojure.data` gains the `EqualityPartition` and `Diff` protocols with `equality-partition` and `diff-similar` methods; `diff` dispatches through them, so user types can extend how they partition and diff.
- Lib: `clojure.pprint/pprint` pretty-prints with margin-aware wrapping and logical-block indentation instead of echoing `pr-str`; collections that overflow `*print-right-margin*` (default 72) break across indented lines and maps break between key/value pairs.
- Lib: `clojure.pprint/write` takes the canon keyword-argument interface (`:stream`, `:pretty`, `:base`, `:radix`, `:right-margin`, `:miser-width`, `:length`, `:suppress-namespaces`); `:stream nil` returns the formatted string.
- Lib: `clojure.pprint` exposes the dispatch-writing surface -- `pprint-logical-block`, `pprint-newline`, `pprint-indent`, `print-length-loop`, `write-out`, `simple-dispatch`, `with-pprint-dispatch`, `set-pprint-dispatch`, `fresh-line`, `get-pretty-writer`, `pp` -- and the printer control vars (`*print-right-margin*`, `*print-miser-width*`, `*print-base*`, `*print-radix*`, `*print-pretty*`, `*print-suppress-namespaces*`, `*print-pprint-dispatch*`).
- Lib: integer printing under `clojure.pprint` honors `*print-base*` and `*print-radix*`, emitting `#x`, `#o`, `#b`, or `#NNr` radix prefixes.
- Lib: clojure.core.reducers is complete: the CollFold protocol with coll-fold, the reducer and folder wrappers, and the cat / append! / ->Cat / foldcat catenation machinery are now available; fold honors the seed-and-combine contract in one sequential pass.
- Lib: Folding a map feeds reducef each key and value separately as (reducef ret k v) instead of whole map entries, and reducers/reduce now seeds its no-init arity from (f) and reduces maps through reduce-kv.
- Lib: `clojure.walk/postwalk-demo` and `clojure.walk/prewalk-demo` print every visited form as `Walked: <form>` in traversal order and return the walked form.
- Lib: `clojure.math/random` returns a pseudorandom double in [0.0, 1.0).
- Lib: `clojure.core.protocols` declares the `InternalReduce` protocol; `internal-reduce` takes a seq, a function, and a start value, and its default implementation performs the built-in seq reduction.
- Lib: clojure.spec.alpha gains every, every-kv, keys*, merge, and multi-spec
- Lib: clojure.test gains thirty-one vars: `*load-tests*`, `*stack-trace-depth*`, `*initial-report-counters*`, `*testing-vars*`, `*test-out*`, `*current-test*`, `with-test-out`, `inc-report-counter`, `report` (dynamic dispatch fn), `do-report`, `assert-expr` (multimethod), `test-var`, `test-vars`, `test-all-vars`, `test-ns`, `run-test`, `run-tests`, `run-all-tests`, `join-fixtures`, `with-test`, `set-test`, `get-current-methods`, `test-ns-hook`, `successful?`, `file-position`, `testing-file`, `testing-vars-str`, `deftest-`, and `compose-fixtures` converged to canon 2-arg public arity with `join-fixtures` added for seq composition.
- Lib: clojure.pprint now provides cl-format, a directive-driven format function, plus the formatter and formatter-out compilers that turn a control string into a reusable function. Supported directives include ~a ~s, the integer directives ~d ~x ~o ~b and ~r (radix, cardinal, and ordinal english), the float directives ~f ~e ~$, ~% ~& ~~ ~c, iteration ~{ ~} with ~^ and sublists, the conditional ~[ ~], plural ~p, argument navigation ~* ~?, and column tabulation ~t. Destinations follow the canon: nil returns a string, true writes to *out*, and a writer receives the output.
- Lib: clojure.pprint now provides code-dispatch, a pretty-print dispatch function that lays out code forms (defn, def, let, loop, binding, doseq, when, if, fn) with the body indented under the head and let-style bindings kept paired. Select it with with-pprint-dispatch or set-pprint-dispatch.
- Lib: clojure.spec.alpha gains int-in, double-in and inst-in range specs (with in-range generators), int-in-range? and inst-in-range? predicates, fspec and fspec-impl for generative function specs, exercise-fn, explain-data*, explain-printer, explain-out and the *explain-out*, *fspec-iterations*, *coll-check-limit*, *coll-error-limit*, *recursion-limit* and *compile-asserts* dynamic vars; assert* throws ex-info carrying explain-data with ::failure :assertion-failed. double-in defaults :infinite? and :NaN? to false so a plain range spec rejects non-finite values unless opted in.
- Fix: def now evaluates metadata-map values at definition time, so ^{:k (+ 1 2)} stores 3 and ^{:test (fn [] ...)} stores a callable, matching canon; reader flags (^:dynamic, ^:private), docstrings, and ^Tag type hints keep working unchanged
- Fix: `binding` rebinds the var itself: a binding established under any spelling (bare, namespace-qualified, or alias-qualified) is now visible to every read of that var from any namespace, including qualified reads, `deref`/`var-get`, and compiled code, and restores correctly on unwind. Previously the dynamic-binding stack matched literal symbol text, so qualified bindings were invisible to bare reads and vice versa.
- Fix: get-thread-bindings keys var-backed entries by their fully qualified symbol, so replaying a snapshot with with-bindings* or conveying it to another thread installs the binding on the exact same var regardless of the namespace the replay runs in.
- Fix: Forcing a lazy-seq now coerces the body's value through `seq`, so `(first (lazy-seq [1 2]))` returns `1` and `(seq (lazy-seq [1 2]))` returns the seq instead of the raw vector. Map, set, and string bodies behave like their seqs the same way; a non-seqable body throws seq's type error.
- Fix: Macro vars now carry :macro true in their metadata, so (meta (resolve 'when)) reports the flag for core macros, bundled-lib macros, and user defmacro definitions alike
- Fix: require and the ns form's :refer now bind the source namespace's var rather than a copy of its value, so dynamic bindings, alter-var-root, and redefinitions are visible through referred names across namespaces
- Fix: math-round returns a long instead of a double, matching the canon rounding contract; integer inputs pass through unchanged
- Fix: Worker binding conveyance distinguishes an invalid key type from OOM, so a NULL from dyn_binding_make is no longer silently swallowed
- Fix: (nth s i) on a string returns a character, consistent with (first s) and (get s i), so char-literal comparisons like (= (nth s i) \~) match. Indexing is codepoint-counted, so multi-byte characters count as one position.
- Docs: Correct clojure.math exact-arithmetic docstrings to document bignum promotion instead of overflow throwing
- Test: (run-tests) with no arguments now runs only the current namespace's tests, matching clojure.test; pass namespace symbols to run a wider set.
- Pprint: print-table now renders padded, pipe-delimited columns with a separator row sized to the widest cell, instead of tab-separated values.
- BC: Add regression tests pinning queue/into correctness under BC with apply-= trigger shape
- API: Consolidate embedder config knobs into mino_set_option / mino_get_option (step/heap limits, thread limit, thread stack bytes, JIT mode, JIT hot threshold); setter returns 0/-1 like mino_gc_set_param and now rejects invalid JIT modes instead of ignoring them
- API: Remove mino_set_limit, MINO_LIMIT_STEPS/MINO_LIMIT_HEAP, mino_set_thread_limit, mino_get_thread_limit, mino_state_set_jit_mode, mino_state_jit_mode, mino_state_set_jit_hot_threshold, mino_state_jit_hot_threshold, and mino_set_thread_stack_size (alpha surface, no shims)
- Async: fix GC hazard when xform/ex_handler stored before alloc_val in chan_new (memory-async-001)
- Async: fix gc_pin/gc_unpin mismatch and sched_entry leak on throw path in sched_drain (memory-async-002, memory-async-003)
- Async: guard gc_save overflow in sched_drain to prevent user-triggerable abort in sanitizer builds (security-async-001)
- Collections: Fix OOM-path buffer leak in mino_bytes (calloc now follows gc_alloc_typed)
- Collections: Add write-barrier coverage for GC_T_VALARR stores in mino_bytes_seq
- Collections: Propagate :meta through sorted-map and sorted-set structural mutations
- Collections: Pin GC values across allocating calls in sorted_map_assoc1, sorted_map_dissoc1, sorted_set_disj1, and clone.c ratio branch
- Collections: Replace bare __atomic builtins in transient.c with portable MSVC/GCC shim
- Collections: Pin new_rec and new_ext across allocating calls in prim_assoc and prim_dissoc record branches
- Collections: Suppress GC in prim_assoc/prim_dissoc while malloc slots[] holds live GC pointers
- Diag: Guard against snprintf pos overflow in diag_render_pretty (security-diag-001)
- Diag: Clamp %.*s int precision to INT_MAX in diag_render_pretty (security-diag-002)
- Diag: Guard notes_cap doubling against size_t overflow in diag_add_note (memory-diag-006)
- Diag: Pin GC staging arrays in span_to_map, frame_to_map, diag_to_map (memory-diag-001/002/003)
- Diag: Pin note_items and frame_items GC buffers across allocating loops (memory-diag-004/005)
- Diag: Guard notes/frames array size multiplication against overflow (security-diag-003)
- Compiler: Guard GC liveness for scratch VALARR buffers and fresh cons in bc compile pass
- VM: Bounds-check 16-bit Bx step-register index in OP_LOOP_INT_LT_ACC and OP_LOOP_INT_DEC_ACC
- VM: Guard __builtin_expect at clause-match fast path with __GNUC__/__clang__ portability check
- JIT: Guard size-pass arithmetic and table allocations against integer overflow (emit.c)
- JIT: Pin GC-live locals across allocating calls in nth-vec, closure, make-lazy, and tailcall helpers
- JIT: Fix signed/unsigned protocol slot bounds check to block wrap-around bypass
- Eval: Fix snprintf over-read in gensym (security-eval-001)
- Eval: Fix write-barrier bypass in vec_destructure_args (memory-eval-003)
- Eval: Pin GC arrays in qq_expand_vector across quasiquote_expand loop (memory-eval-002)
- Eval: Pin ks/vs arrays in qq_expand_map before second allocation (memory-eval-001)
- Eval: Guard __atomic_* builtins with compiler check; add MSVC fallback (portability-eval-001)
- Eval: Document catch-all deviation in try/catch (conformance-eval-001, conformance-eval-002)
- Eval: Document lazy-seq retry deviation after thunk throw (conformance-eval-004)
- Eval: Cast stack guard pointer comparison through uintptr_t (portability-eval-003)
- Eval: Guard __attribute__((always_inline)) with compiler check (portability-eval-002)
- Eval: Guard argv buffer growth against integer overflow (security-eval-002)
- Interop: Guard size_t overflow in host registry capacity doubling (memory-interop-001, memory-interop-002)
- Interop: Reject negative arity in host_member_find to prevent variadic bypass (security-interop-001)
- IO: Guard getcwd behind _WIN32 for Windows portability
- Numerics: guard bigdec exponent and mc_precision integer overflow
- Numerics: guard bits-get offset+size and prim_bits total_bits overflow
- Numerics: pin GC values across allocating calls in bigdec, numeric, ratio
- Numerics: exact bigint/bigint comparison in compare (no double precision loss)
- Numerics: free qz_heap on bigint_wrap NULL path in bigdec rounding
- Primitives: file-mtime now returns sub-second precision on Linux and macOS
- Primitives: thread-sleep compiles and runs on Windows (Sleep fallback)
- Primitives: getcwd and chdir use _getcwd/_chdir on Windows
- Primitives: Fix GC-window, overflow, and write-barrier ordering bugs across prim-state and prim-misc modules (async, io, stateful, meta, ns, module, install, reflection, regex)
- Sequences: Guard rangev length arithmetic against signed overflow for extreme start/end values
- Sequences: Suppress GC in prim_comp while C-heap items[] array holds live GC pointers
- Sequences: Guard prim_sort alloc-size multiply against SIZE_MAX overflow
- Strings: Guard cap-doubling loops in prim_pr_str, prim_join, prim_str, and prim_split against size_t overflow
- Public API: fix GC window in mino_throw when printing non-string exceptions (pin ex across mino_print_to_buf)
- Public API: fix data race in mino_sampler_dump and mino_alloc_sampler_dump when multiple mino_state instances run concurrently (replace static aggregation tables with per-call malloc)
- Regex: Fix three process-global statics (re_flags, re_anchor_end, re_g_state) that produced wrong answers under concurrent mino_state use; they are now thread-local (conformance-misc-003, portability-misc-001)
- Regex: Fix matchrange signed-char comparison so byte ranges with endpoints >= 0x80 work correctly (conformance-misc-002)
- Regex: Cap matchgroup_loop recursion at 10 000 iterations to prevent stack exhaustion on long inputs (security-regex-001)
- Runtime: Fix GC window in set_eval_diag_with_data — pin keys/vals arrays across sequential allocating calls
- Runtime: Replace bare abort() with gc_oom_throw in ns_env OOM paths so (ns ...) OOM is catchable
- Runtime: Pin sym across alloc_val in ns_symbol_with_meta to close GC window
- Runtime: Guard __builtin_expect in mino_eval_stack_guard_fast behind GCC/Clang check for MSVC portability
- Runtime: Guard __atomic_compare_exchange_n in mino_lazy_inflight_unwind with InterlockedCompareExchange fallback for MSVC
- Stencils: JIT loop stencils now exit the region with a real return NULL on safepoint cancel and cons OOM instead of chaining NULL to the next stencil (memory-stencils-001)
- Stencils: non-loop stencils guard against NULL regs from slow helpers before chaining to the next stencil (memory-stencils-002)

## v0.423.5 — Security Fixes

- Security: `format "%Nd"` / `"%Nf"` with width or precision beyond the
  stack buffer no longer reads past the buffer (snprintf return-value misuse).
- Security: `mino_string_n` holds a gc_depth guard across dup_n→alloc_val,
  preventing a dangling pointer after minor collection.
- Security: `rm-rf` and `file-seq` use lstat instead of stat, so symlinks
  inside a removal tree are unlinked without traversal (CWE-59).

## v0.423.4 — Windows Timing and Bigint Fixes

- Windows: `time-ms` / `nano-time` no longer overflow after ~15 minutes
  uptime; intermediate multiply stays within 64 bits.
- Windows: `(long x)` and ratio extraction work across the full long range;
  values past 2^31 previously truncated through a 32-bit mp_small.

## v0.423.3 — Windows Teardown Fix

- Windows: State teardown uses a join-free path for future cells, preventing
  an abort in MinGW-built binaries at exit.

## v0.423.2 — Teardown and JIT-Eager Fixes

- VM: State teardown now runs per-type finalizers, releasing external payloads
  on values still live at exit (fixes LeakSanitizer reports on Linux).
- Numerics: `parse-long`, `parse-double`, and bigdec exponent parsing reject
  a bare sign with no digits on every libc, fixing musl and MinGW misparses.

## v0.423.1 — CI Matrix Portability Fixes

- Numerics: Hex, octal, radix, and decimal bigint overflow literals read
  correctly on musl (musl's strtoll reports overflow without consuming digits).
- Compiler: `recur` into a loop with destructuring bindings works again;
  exact-arity check applies to top-level slot count only.
- VM: Protected-call entry captures ns/loader snapshot under state lock,
  closing a ThreadSanitizer data race.
- Windows: Recursion guard is sized from the thread's real stack bounds
  instead of a fixed assumption.

## v0.423.0 — Runtime Hardening Sweep

- Reader: `#inst` literals print back as `#inst "..."` so instants round-trip
  through pr-str / read-string.
- Printer: Characters above the BMP print as the raw glyph and round-trip
  through pr-str / read-string.
- Stdlib: `spit` accepts `:append true` and `:encoding` option pairs; unknown
  options raise clear errors.
- Stdlib: `(str regex)` returns the bare pattern source; pr-str keeps `#"..."`.
- CLI: `mino -e` no longer prints a trailing nil when the form returns nil.
- CLI: Scripts starting with `#!` load; the shebang line is blanked so
  line numbers stay accurate.
- CLI: A FILE argument of `-` reads the script from stdin via a chunked
  reader for non-seekable inputs.
- CLI: `mino -e EXPR FILE args` evaluates the expression then runs the file.
- Stdlib: `locking` evaluates its body holding a reentrant per-object monitor,
  released on throw, held across blocking channel ops inside the body.
- Stdlib: A delay whose body throws caches the failure; every later `force`
  rethrows; `realized?` returns true for a failed delay.
- Async: Timeout channels wake blocking `<!!` / `>!!` / `alts!!` takes at
  deadline instead of deadlocking when host threads are granted.
- VM: A yielding worker thread no longer leaks its callee's namespace to the
  resuming thread, fixing broken alias resolution after a blocking future.
- Errors: A throw inside a lazy thunk after an earlier caught error now
  reports its own source location.
- Errors: `ex-info` throws a type error when its data argument is not a map.
- Printer: `print` / `println` apply unreadable printing to the whole value,
  so strings inside collections emit raw content; pr/prn/str keep readable forms.
- Collections: `compare` on vectors walks nested pairs iteratively, preventing
  C-stack exhaustion on deeply nested vectors.
- Eval: Structural equality walks element nesting through a worklist, preventing
  C-stack exhaustion on arbitrarily deep values.
- VM: Runaway non-tail recursion raises a catchable "stack overflow: script
  recursion too deep" error; worker threads get an 8 MiB stack by default.
- Async: Parks nested in function-call arguments inside `(go ...)` are lifted
  into let bindings; `(go (inc (<! ch)))` now works.
- API: `mino_atom_reset` routes through the GC write barrier, preventing a
  dangling atom reference after minor collection.
- Eval: `recur` across a `try` boundary raises "cannot recur across try".
- Eval: `recur` with the wrong argument count throws an arity error naming
  expected and actual counts.
- Stdlib: `extend` added for function-level protocol registration.
- Stdlib: `extends?` and `extenders` added for protocol introspection.
- Stdlib: `defmulti` honors the `:hierarchy` option; the dispatch cache
  invalidates when the supplied hierarchy changes.
- Stdlib: Protocol methods with multiple arities work end to end in
  `defprotocol`, `defrecord`, `extend-type`, and `extend-protocol`.
- Namespaces: `ns-imports` added, completing the namespace introspection set.
- Macros: Auto-gensym (`x#`) inside syntax-quote resolves at read time with
  one mapping per syntax-quote form, matching canonical reader behavior.
- Vars: `(meta #'x)` carries the full def-site metadata map and docstring.
- Regex: Backreferences `\1`..`\9` supported; a non-participating group
  fails the match; `(?i)` makes comparison case-insensitive.
- Regex: Lazy quantifiers `*?`, `+?`, `??`, `{n,m}?` supported on atoms
  and groups.
- Regex: Non-capturing groups `(?:...)` and named capture groups
  `(?<name>...)` supported.
- Regex: Word-boundary assertions `\b` and `\B` supported.
- Regex: `.` matches one character (not one byte), so multibyte UTF-8 works
  with wildcard patterns.
- Regex: Patterns that can match the empty string now match at end of input;
  `re-seq` and stateful matchers advance by real match position.
- Regex: `?` quantifier is now greedy, matching canonical engines.
- Stdlib: `clojure.string/split` follows canonical piece semantics: negative
  limit keeps trailing empty pieces, zero-width separator splits without
  dropping characters, multibyte UTF-8 sequences respected.
- Stdlib: `clojure.string/split-lines` splits on `\r\n` as well as `\n`.
- Regex: Control-character escapes `\n`, `\r`, `\t`, `\f`, `\a`, `\e`, `\0`
  recognized inside patterns and character classes.
- Stdlib: `clojure.string/re-quote-replacement` now escapes `$` and `\`.
- Stdlib: `clojure.string/replace-first` supports regex patterns, `$N`
  template replacements, and function replacements.
- Sequences: `doall` / `dorun` accept the two-argument bounded form.
- Sequences: `doall` realizes the whole seq when the source is chunked.
- Sequences: A zero-step `range` repeats `start` forever; `(range 5 5 0)`
  yields `()`.
- Sequences: `range` accepts the full numeric tower (floats, ratios, bigints,
  bigdecs); integer ranges keep the chunked fast path.
- Sequences: `map-indexed`, `keep-indexed`, and `keep` no longer process
  dropped elements when applied after `drop`/`drop-while` on chunked sources.
- Sequences: Forcing a lazy seq whose thunk previously threw no longer hangs;
  the thunk is retried on each force and rethrows each time.
- Collections: `assoc` on map entries works, treating them as 2-element vectors.
- Collections: `compare` and `sort` work on UUIDs using canonical compareTo order.
- Collections: Sorted maps/sets built with a custom comparator compare `=`
  to plain maps/sets with the same content.
- Collections: `conj` and `merge` accept whole maps into sorted maps and
  sorted maps into plain maps.
- Collections: Queues satisfy `coll?`, `counted?`, `sequential?`; `=`
  compares to lists/vectors; `into` accepts a queue target; popping an empty
  queue returns the empty queue.
- Collections: Every seq view of a map yields real map entries; sorted-map
  `seq`/`rseq`/`subseq`/`rsubseq` previously produced bare `[k v]` vectors.
- Stdlib: `(char n)` rejects the UTF-16 surrogate range `0xD800`-`0xDFFF`.
- Numerics: `bigint` / `biginteger` accept ratios, truncating toward zero.
- Numerics: `bigdec` coerces exactly: doubles convert via shortest round-trip
  form; non-terminating ratio expansions raise without `with-precision`.
- Stdlib: `clojure.math` gains `floor-div`, `floor-mod`, `rint`, `ulp`,
  `scalb`, `get-exponent`, `next-after`, and overflow-checked long ops.
- Reader: `#()` literals support positional args past `%9` up to `%20`;
  `%21` and beyond is a reader error.
- Reader: Unknown `##` token and odd-form reader conditional report specific
  error messages.
- Reader: Unicode character literals in the surrogate range are now reader errors.
- Reader: Unknown tagged literals in evaluated source self-evaluate to the
  tagged-literal record instead of failing with "unbound symbol".
- Reader: Nested `#()` literals are a reader error.
- Reader: Integer literals with a leading zero read as octal; non-octal digits
  after leading zero are an invalid-number error.
- Reader: Map and set literals with duplicate keys are reader errors.
- Reader: String literals support `\uXXXX`, `\b`, `\f`, and `\NNN` octal
  escapes; unrecognized escapes are reader errors.
- Reader: A token starting with a digit that fails to parse as a number is a
  reader error instead of silently reading as a symbol.
- Reader: Hex and radix integer literals past the long-long range promote to
  bigint; hex literals accept the `N` suffix.
- Printer: Depth ceiling matches the reader's 1024, so `(read-string (pr-str x))`
  round-trips deeply-nested data.
- JIT: An error raised inside JIT'd code on a worker thread surfaces as a
  future failure instead of crashing the process.
- JIT: Generic JIT'd loops poll the safepoint on every backward jump;
  previously a spinning future wedged the whole state.
- JIT: Safepoint auto-yield accounts for JIT poll amortization, yielding
  every ~64K jumps across interpreted, fused, and generic-JIT paths.
- Numerics: `Long/MAX_VALUE` and `Long/MIN_VALUE` are bound as `:int` so
  `(inc Long/MAX_VALUE)` throws overflow instead of promoting to bigint.
- Numerics: `=` between `:float32` and `:float` widens both to double before
  comparing, satisfying equal-implies-equal-hash.
- Stdlib: `repeat` throws on a boolean count instead of silently coercing.
- Namespaces: `:require` recognizes `:refer-macros` as a synonym for `:refer`;
  both vectors are concatenated when both appear in one spec.
- Tests: `clojure.test/is` isolates per-assertion errors; an unexpected throw
  inside an assertion no longer halts the surrounding deftest.
- Macros: `extend-protocol` rejects forms with no type marker before method
  specs, reporting a clear error at macro-expansion time.
- Reader: `(meta list-literal)` returns `{:line N :column M}` source position,
  matching JVM Clojure; synthesized on demand with no per-list allocation.
- CLI: `*print-namespace-maps*` defaults to true on startup, so qualified-key
  maps print as `#:ns{:k v}` in scripts and at the REPL.
- Collections: `pop` on a single-element list returns `()`; `(pop '())` throws
  an empty-list error.
- Stdlib: `Integer/toBinaryString`, `Integer/toHexString`, `Integer/toOctalString`
  and their `Long/*` counterparts use unsigned 64-bit two's-complement layout.
- Printer: Float printing uses uppercase `E` for scientific notation; `:float32`
  values print with shortest 32-bit-round-trippable form.
- Namespaces: `:refer` accepts a parenthesised list as well as a vector.
- Interop: Bare JVM class symbols (`String`, `Boolean`, `Long`, `Double`,
  `Character`, `Object`) resolve to mino type-marker keywords.

## v0.422.4 — `*math-context*` Threads Through `+`, `-`, `*`

- Numerics: `with-precision` now rounds bigdec `+`, `-`, and `*` in addition
  to `/`; previously those three returned exact results ignoring `*math-context*`.
- Numerics: `+`, `*`, and multi-arg `-` seed the fold from the first operand,
  eliminating a spurious extra rounding step from the synthetic-zero/one seed.

## v0.422.3 — `with-precision` Accepts JVM Rounding Symbols

- Numerics: `with-precision` accepts JVM `RoundingMode` enum symbols (`UP`,
  `DOWN`, `CEILING`, `FLOOR`, `HALF_UP`, etc.) in addition to keyword forms.

## v0.422.2 — `long` Coercion Stays Fixnum

- Numerics: `(long x)` no longer promotes to `:bigint` for values in the
  61-bit inline-tag range; uses `mino_int_wrap` so overflow is detectable.

## v0.422.1 — Amalgamate Symbol Collision Fix

- Build: `kw_match` in `src/prim/bits.c` renamed to `bits_kw_match` to avoid
  collision with an identically-named static in `src/prim/module.c` in the
  amalgamated `dist/mino.c` translation unit.

## v0.422.0 — Post-Canon-Gaps Close-Out

- API: Three sections of `src/mino.h` explicitly marked `MINO_UNSTABLE_*`
  through the alpha series; the rest of the header is the stable surface.

## v0.421.0 — C++ RAII Wrapper + API Audit

- API: New optional header `src/mino.hpp` provides C++14 RAII wrappers:
  `mino::state`, `mino::env`, `mino::pin`, plus `mino::eval_string` /
  `mino::load_file` / `mino::print_to_string` that throw `mino::error`.
- API: `src/mino.h` top-of-file disclaimer updated with post-1.0
  strict-SemVer policy and UNSTABLE section labels.

## v0.420.0 — Documentation Refresh

- Docs: mino-site and compatibility matrix updated for the v0.409–v0.419
  series (lazy-seq ns scoping, print dynvars, math-context rounding, JVM
  statics, inst/bytes/bit-syntax, AOT dynvars).

## v0.419.0 — Bit-Syntax Documentation

- Docs: mino-site adds "Bytes and Bit Syntax" page; mino-examples adds
  IPv4 header decoder and chess bitboard use-cases.

## v0.418.0 — let-bits Destructuring Macro

- Stdlib: `let-bits` binds named fields from a bytes value at running bit
  offsets; terminating `:type :bytes` segment without `:size` binds the tail.
- Sequences: `(apply hash-map (rest some-vec))` works; the prim walker now
  forces the lazy tail returned by `prim_rest_step` for vector seqs.

## v0.417.0 — Bit Syntax + Chunked Bytes Seq

- Stdlib: `bits` packs `[value & options]` segments into a `MINO_BYTES` value.
- Stdlib: `bits-get` reads a bit field by offset/size/type/endian.
- Stdlib: `subbits` returns a half-open bit-range slice.
- Sequences: `mino_bytes_seq` produces a chunked-cons spine of 32-element
  chunks, so `map`/`filter`/`reduce` propagate chunkedness on bytes values.
- Stdlib: `byte-array` accepts a lazy seq and materializes bytes on the fly.

## v0.416.0 — MINO_BYTES Sequence Surface

- Collections: `first`, `rest`, `last`, `nth`, `get`, `reduce`, `into`,
  `map`, `filter`, `vec` dispatch on bytes values.
- Collections: `(empty bytes-value)` returns a fresh empty bytes value.

## v0.415.0 — MINO_BYTES Value Type

- VM: New `MINO_BYTES` value tag for immutable binary data; `byte-array`
  returns a `MINO_BYTES` value; `aset` on it throws.
- Stdlib: `bytes?` and `bitstring?` predicates added.
- Stdlib: `aget` / `alength` read bytes as unsigned int / report byte length.
- Printer: Print form is `#bytes "HEX..."`.
- Reader: `#bytes "..."` reader literal accepts whitespace between hex pairs
  and optional `/0..7` trailing-bit suffix.

## v0.414.0 — inst? / inst-ms / #inst Reader Literal

- Reader: `#inst "..."` reads into a canonical component map with
  `{:mino/instant true}` metadata.
- Stdlib: `inst?` and `inst-ms` implemented (were always-false/always-throw
  stubs); `inst-ms` returns epoch millis from the component map.

## v0.413.0 — clojure-version + AOT-Compiler Dynvars

- Vars: `*compile-path*`, `*source-path*`, `*compile-files*`,
  `*warn-on-reflection*`, `*unchecked-math*` declared with JVM-canon defaults;
  bindable but have no effect (mino has no AOT compiler).

## v0.412.0 — JVM Statics + Embedded-Host Remap

- Interop: JVM value/math statics added: `Long/MAX_VALUE`, `Math/PI`,
  `Math/sqrt`, `Integer/parseInt`, `Double/parseDouble`, `String/valueOf`,
  `java.util.List/of`, etc.
- Interop: `System/currentTimeMillis`, `System/nanoTime`, `System/getenv`,
  `System/exit`, `Thread/sleep`, `java.util.UUID/randomUUID`,
  `java.util.UUID/fromString` route to mino-native equivalents.
- Stdlib: `(str uuid)` now emits the bare 36-char canonical form instead of
  `#uuid "..."`, matching JVM's `UUID.toString`.

## v0.411.0 — math-context Rounding Modes

- Numerics: `*math-context*` / `with-precision` now support all JVM rounding
  modes: `:down`, `:up`, `:floor`, `:ceiling`, `:half-down`, `:half-even`,
  `:unnecessary` in addition to the existing `:half-up`.

## v0.410.0 — Print Dynvars Completion

- Printer: `*print-readably*`, `*print-meta*`, `*print-dup*`,
  `*print-namespace-maps*`, and `*flush-on-newline*` all wired with JVM-canon
  defaults; cached on state at top of each pr/print call.

## v0.409.0 — Lazy-Seq Namespace Scoping Fix

- VM: Lazy-seq bodies now resolve unqualified symbols against the defining
  namespace, not the realizer's; `MINO_LAZY` stores a `const char *` ns slot
  set at construction across tree-walker, bytecode, and JIT paths.

## v0.408.0 — Canon-Parity Summary

- Stdlib: canon-parity summary for v0.401–v0.407:
  strict arity, `thrown-with-msg?`, `*print-length*`/`*print-level*`, `pcalls`
  / `pvalues` / `alt!`, `hash-combine` / `*math-context*` / `unchecked-long`,
  `test.check` rose-tree shrinking, parallel `r/fold`.

## v0.407.0 — Parallel `r/fold`

- Stdlib: `clojure.core.reducers/fold` runs parallel branch when thread
  budget > 1 and the collection is a vector larger than the chunk-size hint;
  chunk count capped at `thread-limit - 1`.
- Agents: `send-via` confirmed deferred; per-state eval-lock leaves no
  useful Executor surface to expose.

## v0.406.0 — `test.check` Rose-Tree Shrinking

- Tests: `clojure.test.check/quick-check` descends into rose trees to find
  the minimal counterexample; result map carries `:shrunk` with `:smallest`,
  `:depth`, `:total-nodes-visited`.
- Tests: Shrinkers implemented for int, nat, vector, list, set, map, tuple,
  string, boolean, such-that, bind, fmap.
- Compiler: Constructor-lane lowering uses `clojure.core/vector` etc. instead
  of bare symbols, so user namespaces excluding core names still get working
  collection literals.

## v0.405.0 — `hash-combine`, `*math-context*`, `unchecked-long` Wrap

- Stdlib: `hash-combine` ships as canonical 32-bit Boost-style
  `seed ^= hash + 0x9e3779b9 + (seed << 6) + (seed >> 2)`.
- Numerics: `*math-context*` and `with-precision` wire bigdec rounding via
  `mino_bigdec_div`; other rounding modes throw `MHO002` (deferred).
- Numerics: `unchecked-long` wraps bigint arguments outside the signed long
  range modulo 2^64 instead of clamping through double.

## v0.404.0 — `pcalls`, `pvalues`, and `alt!` Macro

- Stdlib: `pcalls` and `pvalues` added, falling through `pmap` with the same
  `mino-thread-limit <= 1` sequential fallback.
- Async: `clojure.core.async/alt!` added as a macro over `alts!`; supports
  plain-expression clauses and single/double binding vectors.

## v0.403.0 — `*print-length*` / `*print-level*` Dynvars

- Printer: `*print-length*` and `*print-level*` wired; truncation cached on
  state at top of each pr call; field placement preserves JIT-pinned offsets.

## v0.402.0 — `thrown-with-msg?` Assertion

- Tests: `clojure.test` dispatches on `(is (thrown-with-msg? <re> body))` and
  the JVM-shaped three-arg form; class symbol is documentation-only.

## v0.401.0 — Strict Arity Verified and Locked In

- Tests: New `tests/arity_strict_test.clj` locks strict-arity guarantees
  across fixed-arity, variadic, multi-arity, apply, macros, and destructuring;
  diagnostic carries `:mino/kind = :eval/arity` and `MAR001`/`MAR002`.

## v0.400.0 — `unchecked-*` Family Coerces the Full Numeric Tower

- Numerics: `unchecked-byte/-short/-int/-long/-char/-float/-double` and the
  seven `-int` arithmetic variants now accept the full numeric tower (floats,
  bigints, ratios, bigdecs); the long-domain family keeps integer-only contract.

## v0.399.0 — PersistentQueue (Two-List Persistent FIFO)

- Collections: New `MINO_QUEUE` value type backed by two cons-spine lists;
  `conj` and `pop` are amortised O(1); `clojure.lang.PersistentQueue/EMPTY`
  bound; print form is `#queue [a b c]`.

## v0.398.0 — Reader-Conditional `:preserve` Round-Trips Through `pr-str`

- Printer: `pr-str` on values carrying `:mino/reader-conditional` or
  `:mino/tagged-literal` metadata emits `#?(...)` / `#?@(...)` / `#tag form`.

## v0.397.0 — `clojure.core.reducers` (Sequential)

- Stdlib: `clojure.core.reducers` added behind `MINO_CAP_REDUCERS` (included
  in `MINO_CAP_DEFAULT`); `fold` is sequential (no fork/join yet).

## v0.396.0 — `spec.alpha`: `:via` Propagation, `explain-str` Format, `s/&`

- Stdlib: `explain-data` problems carry the registered spec name in `:via`.
- Stdlib: `explain-str` formats each problem as
  `"<val> - failed: <pred-abbrev> [in: ...] [at: ...] [spec: ...]"`.
- Stdlib: `s/&` added as a regex op wrapping a regex spec with additional preds.

## v0.395.0 — `spec.alpha`: `unform`, `conformer`, `with-gen`

- Stdlib: `unform` added as a multimethod inverse of `conform`.
- Stdlib: `conformer` builds a spec from an arbitrary conform fn and optional
  unform fn.
- Stdlib: `with-gen` attaches an alternative generator fn to a spec.

## v0.394.0 — `unchecked-*` Narrowing Casts and -Int Arithmetic

- Numerics: Seven narrowing casts added: `unchecked-long/-int/-short/-byte/
  -char/-float/-double`; each truncates toward zero at the target width.
- Numerics: Seven -int arithmetic variants added: `unchecked-add-int` etc.;
  each does 32-bit two's-complement wraparound.

## v0.393.0 — Regex Nested Alternation and Group Quantifiers

- Regex: `(foo|bar)`, `(foo|bar)+`, and `(foo)+` work; `matchgroup_loop`
  tries each branch with backtracking and repeats with greedy-then-backoff
  semantics.

## v0.392.0 — Binding / Thread-Binding Sweep

- Vars: `bound?` and `thread-bound?` are now variadic fns AND-ing over all
  var arguments.
- Vars: `with-bindings`, `push-thread-bindings`, `pop-thread-bindings` added;
  C `with-bindings*` and `push-thread-bindings*` accept maps keyed by vars.

## v0.391.0 — `with-redefs` Evaluates Temp-Values in Parallel

- Macros: `with-redefs` evaluates all temp-value expressions before any var
  is rebound, matching JVM Clojure's evaluation order.

## v0.390.0 — Public-Surface Audit and Docstring Sweep

- Docs: `MINO_UNSTABLE_*` blocks in `src/mino.h` state they stay unstable
  through the 0.x alpha series.
- Docs: `ARCHITECTURE_CONTRACT.md` and `INTERNAL_MODULE_MAP.md` corrected
  from old `_t`-suffixed to current unsuffixed public typedefs.

## v0.389.14 — `is-thrown` Correctly Checks the Body, Not the Type Symbol

- Tests: `(is (thrown? <type> body))` now skips the type symbol and checks
  only that the body throws; previously the unbound type symbol threw and
  caused a spurious pass.

## v0.389.13 — Lock-Invariant Assert on Regex Prim Entries

- Security: `MINO_ASSERT_STATE_SAFE` added at each regex prim call site to
  enforce that `state_lock` is held before accessing file-static regex globals.

## v0.389.12 — Agent Spawn-Rollback Hands Off Concurrent Producer's Queue

- Agents: Agent spawn-rollback checks for a concurrent producer's queued node
  and retries spawn once; prevents a queued action from being stranded with
  no worker when `pthread_create` fails.

## v0.389.11 — Restore `bc_top` on Catch Unwind

- GC: Catch landing pads in the bytecode VM and tree-walker restore `bc_top`
  to its pre-try value and zero freed slots, preventing stale GC roots from
  accumulating in a long-lived catching loop.

## v0.389.10 — Preserve Empty-String Namespace on Symbols and Keywords

- Stdlib: `(keyword "" "hi")` and `(symbol "" "hi")` preserve the empty-string
  namespace; `namespace` / `name` accessors recover it from the interned `/<name>`
  form.

## v0.389.9 — Lock-Invariant Asserts on Shared Tables

- Security: `MINO_ASSERT_STATE_SAFE` added at `intern_lookup_or_create_ns`
  and `mino_defrecord` to enforce state_lock; two genuine missing-lock sites
  fixed in the future worker and agent worker.

## v0.389.8 — `eval_try` Catch-Rethrow Protection Hygiene

- VM: Dead defensive gate on the catch-with-finally inner setjmp removed;
  the entry guard already prevents the unreachable fallback path.

## v0.389.7 — Reject `with-meta` / `vary-meta` on a Var

- Vars: `with-meta` and `vary-meta` now throw `eval/type` on a Var instead
  of shallow-copying it, which would decouple the copy's `@` from later `def`s.

## v0.389.6 — `swap!` Barrier Only on Successful CAS

- GC: `prim_swap_bang` fires the GC write barrier only after a successful CAS,
  not on every retry, preventing spurious remset entries from rejected candidates.

## v0.389.5 — Atomic Watch and Validator Installs

- Atoms: `add-watch`, `remove-watch`, and `set-validator!` use a CAS retry
  loop; concurrent installers no longer silently overwrite each other.

## v0.389.4 — `alter-meta!` Actually Atomic

- Atoms: `alter-meta!` now runs a CAS retry loop matching `prim_swap_bang`;
  barrier fires only after a successful publish.

## v0.389.3 — `aset` Barrier on Host Arrays

- GC: `aset` routes through `gc_write_barrier` so OLD-host-array→YOUNG slots
  are recorded in the remset; fixes use-after-free on next minor collection.

## v0.389.2 — Lazy-Force Exactly-Once Realization

- VM: `lazy_force` uses a tri-state CAS (`UNREALIZED`/`REALIZING`/`REALIZED`);
  only one thread evaluates the thunk, concurrent threads spin until the winner
  publishes; on throw, resets to unrealized for canonical retry behavior.

## v0.389.1 — Atomic Transient Owner Mint

- Collections: `mino_transient` mints owner IDs via atomic CAS loop instead
  of plain pre-increment, preventing two concurrent mints from publishing the
  same ID and mutating a persistent subtree.

## v0.389.0 — Native Channels

- Async: `MINO_CHAN` is a new C primitive value tag; `chan`, `offer!`, `poll!`,
  `put!`, `take!`, `close!`, `closed?`, `alts!` all backed by direct C slots
  instead of a swap!-on-state-map implementation.
- Performance: offer!/poll! throughput improved ~35–47% in the async bench
  suite; GC share of wall time drops from ~45% to ~34–40%.
- Async: `alts!` arbitration moved to C; each op carries a shared `:pending`
  atomic flag; `(type ch)` changes from `:atom` to `:chan`.

## v0.388.1 — Multi-Cycle Remset Filter

- GC: `gc_remset_reset` retains entries with at least one observed OLD→YOUNG
  edge across multiple minor cycles, preventing a YOUNG child from being freed
  while the parent slot still points to it (was a Heisenbug under tight nursery).

## v0.388.0 — Embedder UX Roundup

- API: roundup for v0.382–v0.387: dead `mino_install_core` references
  removed, 19 public typedefs dropped `_t` suffix, amalgamation distribution
  added, predicate/extractor grid complete, Clojure-canon C surface added,
  bulk `mino_register_fns` added, throw-payload preservation fixed at 3 sites.
- Docs: Cookbook adds hello-world and handle/record/atom decision tree;
  mino-site adds "Zero dependencies, vendored first" internals page.

## v0.387.1 — Embedder UX: Embedded-Source Canon Pass

- Docs: All embedded mino-source C-string literals in mino-examples cookbook
  use canonical vector-binding shape instead of legacy paren-binding shape.

## v0.387.0 — Embedder UX: API Ergonomics

- API: add `mino_register_fns(S, env, regs)` for bulk primitive
  registration via sentinel-terminated `mino_reg[]` array.
- API: add six `mino_args_parse` specifiers: `'B'` (bigint), `'r'`
  (ratio), `'d'` (bigdec), `'R'` (record), `'X'` (set/sorted-set),
  `'F'` (callable with typed error message).
- API: `mino_throw` and top-level eval handler now include non-string
  non-map throw payloads in human-readable diagnostics.
- API: `mino_clone` failure diagnostic names the specific
  non-transferable type instead of a generic list.
- API: `'M'` specifier in `mino_args_parse` now matches both `MINO_MAP`
  and `MINO_SORTED_MAP`.
- Tests: `embed_api_test` covers all new additions.

## v0.386.0 — Embedder UX: Clojure-Canon C Surface

- API: add `mino_meta` / `mino_with_meta` for meta read/write from C.
- API: add `mino_seq` / `mino_first` / `mino_rest` / `mino_next`
  universal seq quartet; routes through existing prim_seq internals.
- API: add `mino_compare(S, a, b)` three-way compare matching Clojure
  `compare`; throws on cross-type or orderless inputs.
- API: add `mino_hash(v)` returning canonical 32-bit Clojure hash.
- API: add `mino_push_bindings` / `mino_pop_bindings` peer to
  `(binding [...] ...)`; frame is cleared on script-side throw.
- API: add `mino_can_clone(v, out_reason)` transferability pre-flight
  that names the first non-transferable type without copying.
- Tests: `embed_api_test` covers all new entry points.

## v0.385.0 — Embedder UX: Predicate / Extractor Grid

- API: add `mino_is_float32`, `mino_is_sorted_map`, `mino_is_sorted_set`,
  `mino_is_map_entry`, `mino_is_host_array`, `mino_is_future`
  predicates; consolidate full `mino_is_*` grid in one header block.
- API: add extractors `mino_to_float32`, `mino_to_bigint_str`,
  `mino_to_ratio`, `mino_to_bigdec_str`, `mino_to_uuid_bytes`,
  `mino_to_regex_source`.
- Tests: `embed_api_test` `test_predicate_grid` block covers all new
  predicates and extractors including type-mismatch and buffer-too-small
  paths.

## v0.384.0 — Embedder UX: Amalgamation Distribution

- Build: `./mino task amalgamate` produces `dist/mino.c` + `dist/mino.h`
  single-file vendor distribution; `dist/` is gitignored and published
  as a release asset.
- Build: `./mino task clean-dist` removes the `dist/` tree.
- Build: `./mino task examples-amalgam` builds and runs all embed
  examples against the amalgamation; wired into `release-gate`.
- Build: rename three static symbols to remove name collisions in the
  unified TU: `kw_eq` → `ns_kw_eq`, two `count_args` →
  `host_count_args` / `bc_count_args`.

## v0.383.0 — Embedder UX: Type-Name Cleanup (Drop `_t`)

- API: drop `_t` suffix from all 18 public typedefs (e.g.
  `mino_val_t` → `mino_val`, `mino_state_t` → `mino_state`);
  `mino_gc_stats_t` renamed to `mino_gc_stats_out` to avoid
  collision with the `mino_gc_stats` function.
- API: rename `mino_ref(S, val)` → `mino_ref_new(S, val)` so `mino_ref`
  can occupy the ordinary identifier namespace as a struct name.
- Build: add `lib/clojure/math.clj` to the bundled-stdlib generator
  list so `./mino task clean` followed by a fresh build produces a
  working binary.

## v0.382.0 — Embedder UX: Cascade-Completion

- Build: add `./mino task examples` that builds and runs all
  `examples/embed_*.c`; wired into `release-gate`.
- Embedding: `tests/embed_api_test.c` no longer includes
  `mino_internal.h`; all reach-ins replaced with public API.
- Embedding: replace stale `mino_install_core(S, env)` calls in three
  example files with `mino_install(S, env, MINO_CAP_DEFAULT)`.
- Docs: update stale `mino_install_core` references in six source
  comments to current symbol names.

## v0.381.1 — Fix: agent dosync-send / await race on Linux

- Agents: fix `(dosync (send a fn))` + `(await a)` deadlock on Linux
  where a freshly-spawned worker could exit before the producer
  published its action, leaving `(await a)` waiting forever.
- Agents: merge worker spawn and enqueue into a single `agent_enqueue`
  critical section; action node is published before `pthread_create`.
- Tests: add `tests/migrated/dosync_send_repro.clj` reproducing the
  hang; passes in ~50 ms post-fix on Linux CI.

## v0.381.0 — C Micro-refactor: Hygiene, Identity-by-name, Dispatch Tables, God-function Splits

- Build: strip version stamps, Phase/Cycle breadcrumbs, and legacy
  annotations from source comments across 15+ sites.
- VM: switch prim identity checks from `as.prim.fn` pointer comparison
  to allocation-pointer equality or `strcmp` against registered name.
- VM: hoist per-element prim identity check in `reduce_step` into a
  caller-classified `reduce_int_kind_t` so the inner loop runs a
  switch.
- Compiler: `compile_call_impl` shrinks from 614 lines to a 77-line
  orchestrator with nine `try_emit_*` helpers and two dispatch tables.
- VM: extract `run_repl` from `main`; `main` shrinks from 488 to 296
  lines.
- Printer: `mino_print_to` refactored from 486-line switch to ~80-line
  driver with per-tag `print_*` helpers.
- Compiler: extract `vec_destructure_args` from `bind_vec_destructure`;
  shrinks from 193 to 80 lines.

## v0.380.0 — Splits in numeric / collections / sequences

- Build: extract `numeric_math.c` (203 lines), `numeric_bit.c` (150),
  `numeric_coerce.c` (310) from `prim/numeric.c`; file shrinks from
  2806 to 2191 lines.
- Build: extract `collections_transient.c` (174 lines) from
  `prim/collections.c`; file shrinks from 2312 to 2152 lines.
- Build: extract `sequences_seq.c` (255 lines) from `prim/sequences.c`;
  file shrinks from 3499 to 3257 lines.

## v0.379.0 — Complete mino_state Decomposition

- Build: extract all remaining `mino_state` field clusters into ten
  named sub-structs embedded at byte-stable positions; JIT-pinned
  offsets verified by existing `_Static_assert` guards updated to
  nested `offsetof` paths.
- Build: migrate ~600 field-access sites to nested paths
  (e.g. `S->gc_x` → `S->gc.x`, `S->ic_gen` → `S->ns_vars.ic_gen`).

## v0.378.0 — Mega-prim Splits

- Build: extract `prim/ratio.c` (510 lines) and `prim/bigdec.c` (625
  lines) from `prim/bignum.c`; bignum.c shrinks from 1797 to 702 lines.
- Build: introduce `prim/bignum_shared.h` for shared bigint helpers
  used by all three files.

## v0.377.0 — mino_state Decomposition

- Build: extract `gc_state_t` (`src/gc/state.h`, ~40 fields),
  `stm_subsystem_t` (`src/prim/stm_state.h`, 3 fields), and
  `async_state_t` (`src/async/state.h`, 3 fields) from `mino_state`;
  JIT-pinned byte offsets preserved via sub-struct embedding.
- Build: migrate ~341 field-access sites to nested paths
  (`S->gc_x` → `S->gc.x`, etc.); JIT offset asserts still pass.

## v0.376.0 — Cleanup + Graph Audit

- Build: delete stale `.d` dependency files for removed sources.
- Build: cross-component import edges down to 49 (from 117 at v0.369.0)
  after the refactor series; 43 components in the noumenon graph.

## v0.375.0 — Mega-prim Audit, Splits Deferred

- Build: audit four mega `prim/` files; splits deferred with a
  per-file sub-domain breakdown and extraction order.

## v0.374.0 — mino.h Audited for Embedder Fit

- API: demote `mino_sampler_dump` and `mino_alloc_sampler_dump` from
  `mino.h` to `mino_internal.h`; public header stays single-file.

## v0.373.0 — gc_state_t Type Alias

- Build: introduce `src/gc/state.h` declaring `gc_state_t` as a type
  alias for the GC subsection of `mino_state`; no byte layout change.

## v0.372.0 — gc/collections Coupling Broken

- GC: replace 280-line `gc_trace_children` switch with a
  `S->gc_tracers[GC_T__COUNT]` function-pointer table populated at
  state init; each component owns and registers its own tracers.
- GC: apply the same registration pattern to finalizers; `gc_minor_collect`
  and `gc_major_sweep_phase` dispatch through `S->gc_finalizers[tag]`.
- Build: introduce `src/gc/layout.h` isolating shared GC substrate so
  component-side tracer files need not include all of `gc/internal.h`.

## v0.371.0 — Values Component

- Build: move `struct mino_val`, tag-encoding macros, and `val.c` from
  `src/collections/` to new first-class `src/values/` component;
  umbrella headers re-include the new files so all consumers compile
  unchanged.

## v0.370.0 — Runtime Umbrella Header Split

- Build: split 1796-line `src/runtime/internal.h` umbrella into nine
  focused per-concern headers; reduce cross-component import edges from
  117 to a first batch of four dropped edges in initial consumer
  switches.

## v0.369.0 — Write Barrier + JIT Slab Packing

- GC: drop SATB push from write barrier; grow major remark with full
  `gc_mark_roots` re-walk replacing snapshot-at-begin invariant with
  incremental-update soundness; halves per-op barrier work in MAJOR_MARK.
- JIT: pack small fns (≤4 KB) into shared per-state slab pages instead
  of one mmap per fn; refcounted invalidation munmaps slab on last
  release.
- Compiler: template-aware bc recompile propagates fresh bc to all
  sibling closures via `template_fn` back-pointer instead of per-closure
  copies.
- GC: make symbol/keyword intern tables weak; unreachable entries
  tombstoned at end-of-mark and freed in OLD sweep; cached special-form
  symbols traced precisely.

## v0.368.0 — Weak intern table, skip MAJOR_MARK walk

- GC: intern tables are now weak; MAJOR_MARK skips the intern walk;
  `gc_intern_sweep_tombstones` reaps unreachable OLD entries before
  sweep.
- GC: cached special-form symbol pointers traced precisely from
  `gc_mark_envs_and_interns` so pointer-identity dispatch stays valid
  across major cycles.

## v0.367.0 — Template-aware bc recompile

- Compiler: add `template_fn` back-pointer to `mino_val.as.fn`; bc
  staleness recompile fires once on the shared template and broadcasts
  the fresh bc to sibling closures instead of producing per-closure
  copies.
- GC: `template_fn` traced in both production and `MINO_GC_VERIFY=1`
  walkers so closures cannot outlive their template.

## v0.366.0 — JIT slab pool: per-fn slot invalidate

- JIT: `mino_jit_invalidate` releases slab slots via `mino_jit_slab_release`;
  slab is munmap'd when `live_slots` reaches zero.
- JIT: slab `native_pc_offsets` tracked on `jit_regions` list with NULL
  region pointer so state teardown reaps the table even for unreleased
  slabs.

## v0.365.0 — JIT slab pool: infra + small-fn wire-up

- JIT: small fns (≤4 KB pre-rounding) now allocated from a per-state
  slab pool instead of one mmap page per fn; eliminates 92–99% per-page
  waste; slab sealed RX outside the per-compile fill window.
- JIT: add `native_slab` pointer to `mino_bc_fn_t` for per-fn
  invalidate (landed in v0.366).

## v0.364.0 — SATB drop, end-of-mark re-rooting

- GC: remove SATB (deletion) half of hybrid write barrier; `MAJOR_MARK`
  barrier now pushes only the Dijkstra (insertion) value, halving
  per-op barrier work.
- GC: `gc_major_remark` adds a full `gc_mark_roots` pass before the
  final stack scan, ensuring end-of-mark soundness without SATB.

## v0.363.0 — OLD-VALARR write-barrier retrofit + bc-regs remset pin

- GC: route four heap-resident argv spill sites in `src/eval/fn.c`
  through `gc_valarr_set` so promoted OLD VALARR buffers go through
  the write barrier.
- GC: `gc_remset_reset` re-adds each OLD bc-regs buffer to the remset
  after every clear, closing a remset-miss flagged by `MINO_GC_VERIFY=1`.

## v0.362.0 — SATB Restored; Faster JIT Loops

- GC: restore SATB push to pre-prototype shape after OLD-VALARR remset
  miss was found; SATB drop deferred.
- Performance: `(+ acc i)` accumulator loops 1.77–1.78x faster under JIT;
  `deref` path -6.3% from C-prim inlining; adaptive pause-budget
  controller added.

## v0.361.0 — SATB-drop audit + verification

- GC: add temporary `gc_barrier_clear_only` counter; audit all six
  clear-only write-barrier call sites; confirm SATB drop is sound;
  counter removed in v0.362.

## v0.360.0 — Adaptive major-slice budget

- GC: replace static 4096-header-per-slice budget with an adaptive
  controller targeting a configurable STW pause length (default 1 ms,
  `MINO_GC_PAUSE_TARGET_NS` env override); bounds `[256, 65536]`.

## v0.359.0 — Major-mark root pruning deferred (soundness gate)

- GC: defer MAJOR_MARK intern-walk skip; intern table is the canonical
  root for interned names; safe skip requires a weak-ref intern table.

## v0.358.0 — Anonymous-fn bc dedup deferred (diagnostic logged)

- Compiler: defer anonymous-fn bc dedup; root cause identified as
  per-closure recompile from stale `ic_gen` check firing on closures
  rather than their shared template; fix path documented.

## v0.357.0 — JIT per-fn slot invalidate (folded into v0.356 deferral)

- JIT: per-fn slot invalidate deferred together with the v0.356 JIT
  slab pool.

## v0.356.0 — JIT region sub-allocator deferred

- JIT: defer slab pool sub-allocator; 94–99% per-page waste documented;
  W^X page management and per-fn invalidate
  complexity exceed single-release scope.

## v0.355.0 — Fold delay realisation into C prim_deref

- Eval: `prim_deref` now realises delays directly in C on the `MINO_MAP`
  branch; removes the Clojure-side `deref` shadow at `src/core.clj:837`;
  `(deref atom)` tight loop -6.3% under JIT.

## v0.354.0 — Bench corpus expansion

- Tests: add `counter_only_loops.clj`, `deopt_trigger.clj`, and
  `alloc_site_saturation.clj` to mino-bench to cover previously dark
  sampler regions.

## v0.353.0 — Protocol-call cached fast lane deferred

- Performance: revert protocol-dispatch fast lane prototype; local
  measurement shows no win over current IC-stencil path; documented for
  reactivation if alloc pressure is reproduced on a future workload.

## v0.352.0 — JIT loop matcher: constant-step accumulator

- JIT: extend counted-loop matcher to accept literal-int accumulator
  steps (`(+ acc N)` and commuted form); emits `OP_LOAD_K` pre-load
  then reuses existing two-binding stencil; 1.78x speedup on 100k-iter
  constant-step loop.

## v0.351.0 — JIT loop matcher: arithmetic-step accumulator

- JIT: add `OP_LOOP_INT_LT_ACC` and `OP_LOOP_INT_DEC_ACC` opcodes with
  copy-and-patch stencils for forward/reverse counter + register-step
  accumulator loops; 1.77x speedup on 100k-iter `(+ acc i)` loop.

## v0.350.0 — GC Instrumentation Complete

- GC: 16-tag instrumentation complete; phase-timer sum tracks
  `total-gc-ns` within 1–3%.

## v0.349.0 — Synthesis dashboard

- Performance: measurement synthesis ranks JIT region waste (94–99%
  per-page) as the dominant lever; promotion-age tuning and SATB-only
  barrier work have zero corpus headroom.

## v0.348.2 — Allocation-site sampler (env-gated light)

- Performance: add `MINO_ALLOC_SAMPLE=1` allocation-site sampler; 4096-
  entry ring, rate tunable via `MINO_ALLOC_SAMPLE_RATE`; dumped at
  `mino_state_free`.
- API: add `mino_alloc_sampler_dump(state, FILE *)` to `mino.h`.

## v0.348.1 — Native-side sample tag

- Performance: `mino_sampler_fire` sets low bit of `flags` when sample
  fires from inside JIT'd native code; `mino_sampler_dump` reports
  `native=M` per `(bc, pc)` bucket.

## v0.348.0 — Safepoint-based CPU sampler

- Performance: add `MINO_SAMPLE=1` safepoint-based CPU sampler; 65536-
  entry ring, rate tunable via `MINO_SAMPLE_PERIOD`; dumped automatically
  at `mino_state_free`.
- API: add `mino_sampler_dump(state, FILE *)` to `mino.h`.

## v0.347.2 — Collection-size histogram (env-gated)

- Performance: add `MINO_COLL_SIZE_STATS=1` collection-size histogram
  on `mino_state`; ticked at `mino_persistent`; surfaced via `(gc-stats)`
  as `:coll-size-hist`.

## v0.347.1 — BC compile-decline histogram

- Compiler: add `bc_declines[16]` histogram tracking macro, special-form,
  bad-form, and catch-all decline categories per compile attempt.
- API: surface `:bc-declines` keyword→count map in `(gc-stats)`.

## v0.347.0 — Per-tag allocation histogram

- GC: add `gc_alloc_by_tag[16]` counter incremented on every typed alloc.
- API: surface `:alloc-by-tag` keyword→count map in `(gc-stats)`.

## v0.346.3 — JIT compile-time + code region usage

- JIT: add always-on `jit_compile_ns`, `jit_code_bytes`,
  `jit_code_region_dead` fields to `mino_bc_fn_t`.
- JIT: `MINO_CPJIT_STATS=tracing` dump prints compile ns, code bytes,
  region bytes, and dead bytes per compiled fn.

## v0.346.2 — Per-site IC stats (env-gated)

- JIT: `MINO_JIT_IC_STATS=1` lazily allocates per-site hit/miss/thrash
  counters; JIT inline-cached hits are not counted (lower bound only).
- JIT: `MINO_CPJIT_STATS=tracing` dump prints per-slot IC stats for fns
  with non-zero entries.

## v0.346.1 — Per-fn JIT wall-time (env-gated)

- JIT: `MINO_JIT_TIME_FNS=1` records cumulative and max wall-time per
  compiled fn; `MINO_CPJIT_STATS=tracing` dump adds `wall:` line per fn.

## v0.346.0 — Per-fn JIT invocation + deopt counters

- JIT: add `jit_invocations` and `jit_deopt_exits` counters to
  `mino_bc_fn_t`; reported in `MINO_CPJIT_STATS=tracing` per-fn rows.

## v0.345.3 — Pause-time distribution

- GC: record pause times in a 256-entry ring and 24-bucket log2 histogram.
- API: surface `:pause-p50-ns`, `:pause-p95-ns`, `:pause-p99-ns`,
  `:pause-hist` in `(gc-stats)`.

## v0.345.2 — Bytes promoted + young-survival age histogram

- GC: surface `:bytes-promoted-minor` (cumulative promotion bytes) and
  `:young-age-buckets` (8-bucket log2 age histogram) in `(gc-stats)`.

## v0.345.1 — GC barrier + overflow counters

- GC: surface `:barrier-satb-pushes`, `:barrier-dijkstra-pushes`, and
  `:mark-stack-overflows` cumulative counters in `(gc-stats)`.

## v0.345.0 — Per-phase GC timers

- GC: add five per-phase timer fields (minor-mark, minor-sweep,
  major-mark, major-sweep, root-scan) to state and `mino_gc_stats_t`.
- API: surface as `:minor-mark-ns`, `:minor-sweep-ns`, `:major-mark-ns`,
  `:major-sweep-ns`, `:root-scan-ns` in `(gc-stats)`.

## v0.344.0 — Measurement Notes

- Docs: performance measurement notes for v0.340–v0.343; no production
  code change.

## v0.343.0 — GC discovery, no production change

- Docs: GC fraction measured at 10–20% across workloads; finer per-phase
  instrumentation identified as prerequisite for actionable GC tuning.

## v0.342.0 — Predicate+branch fusion deferred

- Docs: predicate+branch JIT fusion deferred; `bc_has_branches` gate and
  multi-class chain-marker restructuring required before shipping.

## v0.341.0 — Workload corpus expansion (mino-bench)

- Tests: add `jit_blocker_workloads.clj`, `alloc_pressure_bench.clj`,
  and `protocol_hot_loop.clj` bench files to mino-bench.

## v0.340.0 — Loop matcher accepts (+ counter 1) as inc-equivalent

- Compiler: counted-loop matcher now accepts `(+ counter 1)` and
  `(+ 1 counter)` as inc-equivalent step shapes, enabling loop fusion
  for those forms.

## v0.339.0 — Measurement Notes

- Docs: forward stencil hooks deferred; workload corpus shows 0%
  `OK_WITH_DEOPT` fns.

## v0.338.0 — Bigram discovery instrumentation, no fusions shipped

- Build: add compile-gated `-DMINO_BC_OP_COUNTS=1` bigram frequency
  counter; zero runtime cost on default build.
- Docs: dominant bigrams measured; no fusion candidate cleared the 7%
  movement gate; prototype `OP_FUSED_MOVE_RETURN` reverted.

## v0.337.0 — Non-empty map / set literals BC-compile

- Compiler: lower non-empty map/set literals to `(hash-map ...)`/
  `(hash-set ...)` calls at compile time, keeping enclosing fns on the
  bytecode path.
- Compiler: fix `count_symbol_uses` to traverse map keys/values and set
  elements, preventing dead-binding elimination from dropping live refs.
- VM: fix `OP_THROW` to preserve `:mino/data` payload on no-try fallback,
  matching `prim_throw`'s extraction path.

## v0.336.0 — Lower non-const vector literals to `(vector ...)` calls

- Compiler: non-const vector literals (at least one non-self-evaluating
  element) are lowered to `(vector ...)` calls instead of falling back
  to tree-walk eval for the entire enclosing fn.

## v0.335.0 — Const-pool empty map / set literals

- Compiler: treat empty map/set literals as const-poolable, unblocking
  `(loop [acc {}] ...)` builder rewrites to reach bytecode and use
  transient paths; reduces alloc ~90% for affected patterns.

## v0.334.0 — OP_CONJ_BANG / OP_DISSOC_BANG / OP_DISJ_BANG complete the family

- VM: add `OP_CONJ_BANG`, `OP_DISSOC_BANG`, `OP_DISJ_BANG` opcodes with
  interpreter fast lanes and matching JIT stencils on all five targets.

## v0.333.0 — OP_ASSOC_BANG inline fast lane

- VM: add `OP_ASSOC_BANG` opcode; interpreter routes valid transients
  directly to `mino_assoc_bang`; JIT stencil and slow helper included.

## v0.332.0 — OP_GET_KW_MAP transient fast lane

- VM: `OP_GET_KW_MAP` (and its JIT slow helper) now handle `MINO_TRANSIENT`
  by unwrapping to the backing collection, cutting per-step alloc ~8%.

## v0.331.0 — Reduce slow-path uses argv directly

- VM: `reduce_step` generic dispatch passes args as a flat C array via
  `apply_callable_argv` instead of allocating a cons pair per step;
  reduces alloc ~6% and GC collections ~14% on bump-5k row.

## v0.330.0 — Reduce-pattern compile-time rewrite

- Compiler: `(reduce assoc/conj/dissoc/disj-shaped-fn seed coll)` is
  rewritten at compile time to a transient-wrapped equivalent, cutting
  bump-5k alloc ~10% and GC collections ~22%.

## v0.329.0 — Map / Collection Results

- Performance: net result for v0.327–v0.329: nested-vec row −29% (JIT
  off and on); alloc-bound rows within noise.

## v0.328.0 — Transient fast path in `into` for vectors

- Collections: `(into to from)` vector branch now wraps destination in
  a transient and conj-bangs each element; nested-vec row −30% time,
  −54% alloc.

## v0.327.0 — Builder-rewrite extension for non-empty seeds

- Compiler: `try_builder_rewrite` accepts non-empty seeds and allows
  `get`/`nth`/`count`/`get-in` reads on acc in step args; bump-loop
  5k map −45%.

## v0.326.0 — Interpreter Results

- Performance: headline win for v0.324–v0.326 is realize-10k-lazy
  ~1000x from chunked take/drop; recur move-coalescing −11 to −14%
  on interpreter-bound loops.

## v0.325.0 — Chunked-aware take and drop

- Sequences: `lazy-take` and `drop-seq` forward whole chunks from
  chunked sources; `(doall (take 10000 (range)))` drops from 4.43 ms
  to 4.85 µs (~1000x), chunk-cons allocs −97%.

## v0.324.0 — Move-coalescing peephole for recur

- Compiler: `compile_recur` writes directly into target loop binding
  when no later recur arg reads it, eliminating one `OP_MOVE` per
  independent binding; interpreter recur loops −11–14%.

## v0.323.0 — Side-Exit + Eligibility Results

- JIT: side-exit deopt stencil shipped, loop cancellability added,
  real-workload eligibility at 100% OK across v0.317–v0.323.

## v0.322.0 — Control-flow stencil measurement decision

- JIT: no new stencils shipped; tracing dashboard now splits bytes-blocked
  into `hard` vs `ok-with-deopt` columns.
- JIT: add `op_deopt_count` and `op_deopt_code_bytes` state counters.

## v0.321.0 — JIT loop cancellability

- JIT: fused-loop stencils poll `mino_bc_safepoint` every 256 iterations;
  a JIT'd infinite loop now responds to `(future-cancel f)` within
  bounded wall time.
- Tests: add two cancellability assertions to `jit_invalidation_test.clj`.

## v0.320.0 — Side-exit measurement

- JIT: `MINO_CPJIT_STATS=summary` counts `OK_WITH_DEOPT` fns toward the
  eligible total.
- Docs: side-exit round-trip cost ~100 ns per deopt measured; bench added
  to mino-bench as `benchmarks/side_exit_micro.clj`.

## v0.319.0 — Side-exit deopt stencil

- JIT: fns whose first unstenciled op is past PC 0 now compile to a
  native prefix plus a `deopt_to_interp` stencil; `mino_jit_invoke`
  resumes via `mino_bc_run_resume` on deopt sentinel.
- JIT: add `jit_deopt_pc`, `jit_deopt_pending`, and three resume-saved
  fields to `mino_state_t`; add `OP_DEOPT_TO_INTERP` synthetic opcode.

## v0.318.0 — Eligibility annotation for partial native coverage

- JIT: classifier distinguishes `CPJIT_REASON_UNKNOWN_OP` (first unknown
  op at PC 0, no prefix worth compiling) from new
  `CPJIT_REASON_OK_WITH_DEOPT` (first unknown op at PC > 0).
- JIT: `MINO_CPJIT_STATS=tracing` shows deopt PC in per-fn rows.

## v0.317.0 — Dispatch loop extraction

- VM: extract `mino_bc_run` dispatch loop body into static helper
  `bc_run_dispatch_from`, enabling future resume-at-arbitrary-PC entry
  for the JIT deopt path; no behavior change.

## v0.316.0 — Tiering + Loop Results

- JIT: headline results: `OP_LOOP_INT_LT` re-enabled (−35% JIT-on),
  adaptive tiering shipped, real-workload pipeline 1.24× JIT-on/off,
  invalidation torture suite green.

## v0.315.0 — Real-workload bench corpus

- Tests: add `real_workloads.clj` bench suite to mino-bench with CSV
  parse, transducer pipeline, protocol state machine, and nested-binding
  logger rows.

## v0.314.0 — JIT invalidation/deopt torture tests

- Tests: add `jit_invalidation_test.clj` with 8 test groups and 21
  assertions covering def rebind, var-set-root, protocol extend, redef
  chains, and argc-shift scenarios.

## v0.313.0 — Adaptive JIT tiering for callsite-aware promotion

- JIT: callees of JIT-compiled fns get effective hot-threshold of 1 in
  `MINO_JIT=auto` mode via a new `jit_invoke_depth` counter on the
  thread context.

## v0.312.0 — Safepoint cadence audit

- Docs: audit confirms interpreter per-backjump safepoint is already
  minimal; JIT loop stencils lack polls entirely (cancellability gap
  filed); no code change.

## v0.311.0 — Non-IC call routes through known-bc fast helper

- JIT: `mino_jit_call_slow` dispatches MINO_FN callees via
  `mino_apply_known_bc_fn_argv`, skipping one var-deref and one
  type-of dispatch per non-IC call.

## v0.310.0 — Side-Exit Design Deferred

- JIT: full side-exit implementation deferred; design document captured.

## v0.309.0 — `OP_BINOP_INT` reachability audit

- Docs: audit confirms compiler never emits `OP_BINOP_INT`; no stencil
  needed; line item closed without shipping dead code.

## v0.308.0 — `OP_LOOP_INT_LT` stencil re-enabled

- JIT: re-enable `OP_LOOP_INT_LT` in the entry table; lt-only 10M loop
  now runs ~35% faster JIT-on vs JIT-off (historical regression no
  longer reproduces).

## v0.307.0 — `task jit-blocker-report` dashboard

- CLI: add `task jit-blocker-report` that runs realistic_bench through
  `MINO_CPJIT_STATS=tracing` and writes bytes-blocked-by-op table to
  `.local/jit-blockers-latest.md`.

## v0.306.0 — `task perf-gate` chains to mino-bench

- CLI: add `task perf-gate` wrapping mino-bench's `perf_gate.clj`
  (15 benches × 3 runs); exits with warning when mino-bench is absent.

## v0.305.0 — `MINO_CPJIT_STATS=tracing` mode

- JIT: add `tracing` mode to `MINO_CPJIT_STATS`; appends bytes-blocked-
  by-op table at dump end, sorted descending by blocked bytes.

## v0.304.0 — GC/Alloc Results

- Performance: net wins vs v0.296: build-5k-map −10%, bump-5k-map −8%,
  nested-vec −22%, realize-10k-lazy −45%.

## v0.303.0 — Nursery default 4 MiB → 8 MiB

- GC: default nursery size raised from 4 MiB to 8 MiB; lazy-realize row
  −25%, nested-vec −6%; override via `MINO_GC_NURSERY_BYTES`.

## v0.302.0 — Builder-loop transient rewrite covers sets

- Compiler: `try_builder_rewrite` now accepts `#{}` acc init in addition
  to `[]` and `{}`; set builder loops compile to transient equivalents.

## v0.301.0 — Write-barrier MAJOR_MARK call-site dedup

- GC: `gc_write_barrier` consolidates to one `gc_phase` load; SATB/
  Dijkstra mark-push calls gated on `!h->mark`, skipping the function
  call when already marked.

## v0.300.0 — Bump allocator on by default

- GC: `MINO_BUMP_ALLOC` defaults to 1; slab bump path active on freelist
  miss; nested-vec −10%, alloc-only-100k −9%, nursery-pressure-50k −14%.

## v0.299.0 — Slab-backed bump allocator (opt-in)

- GC: add slab bump allocator in `gc_alloc_raw`; freelist misses carve
  from 64 KiB slabs; `gc_hdr_t` gains `bump` flag (no size change).
- API: surface `:alloc-bump-hits` and `:alloc-bump-slab-refills` in
  `(gc-stats)`.

## v0.298.0 — Alloc-source probe in `(gc-stats)`

- GC: surface `:alloc-freelist-hits`, `:alloc-calloc-class-miss`,
  `:alloc-calloc-no-class` counters in `(gc-stats)`.

## v0.297.0 — GC/alloc bench harness extensions

- Tests: add `gc_alloc_micro.clj` bench suite to mino-bench covering
  transient builder, persistent HAMT assoc, write-barrier, and nursery
  pressure rows.
- Tests: mino-bench reporter now prints `alloc-bytes/op` in human-readable
  output.

## v0.296.0 — `MINO_CPJIT_STATS=summary` one-line mode

- JIT: add `summary` value for `MINO_CPJIT_STATS`; emits one aggregate
  line at exit without per-fn ring allocation.

## v0.295.0 — JIT-call fast lane through `mino_bc_run_known_native`

- JIT: IC-hit call path enters streamlined `mino_bc_run_known_native`
  helper, skipping clause matcher, captures branch, and per-op PC write;
  fib(25) 2.07× JIT-on/off.

## v0.294.0 — Defer `mino_bc_run` try-state snapshot

- VM: `mino_bc_run` skips per-call `try_depth`/`bc_catch_depth` snapshot
  and cleanup for fns without try/catch/throw (`has_try` flag on
  `mino_bc_fn_t`).

## v0.293.0 — Cached-bc IC fast lane

- JIT: `OP_CALL_CACHED` IC slot caches `mino_bc_fn_t *`; single-arity,
  no-rest hits route to `mino_jit_call_known_native_slow` skipping
  staleness rechecks; fib(25) 1.98×, 1M-call loop 2.13×.
- JIT: IC slot grows from 56 to 64 bytes; stencil byte tables regenerated
  across all five host targets.

## v0.292.0 — `OP_DISSOC` stencil

- JIT: add `OP_DISSOC` stencil routing through `mino_jit_dissoc_slow`
  with MINO_MAP fast lane; improves fn-level JIT eligibility for bodies
  containing dissoc.

## v0.291.0 — Protocol-dispatch stencils

- JIT: add stencils for `OP_PROTOCOL_CALL_CACHED` and
  `OP_PROTOCOL_TAILCALL_CACHED`; protocol-dispatch loop 1.78× JIT-on/off.

## v0.290.0 — `OP_MAKE_LAZY` stencil unblocks lazy-using core helpers

- JIT: add `OP_MAKE_LAZY` stencil; drops eligibility blockers to zero
  on `realistic_bench`, unlocking `mapcat`, `repeat`, `iterate`, `range`.

## v0.289.0 — Stencils for unary int predicates + `bit-not`

- JIT: add stencils for `pos?`/`neg?`/`even?`/`odd?`/`bit-not` on
  tagged ints; up to 9.7× speedup on 1M-iter loops.

## v0.288.0 — Stencils for the bitwise int family

- JIT: add stencils for `band`/`bor`/`bxor`/`shl`/`shr`/`ushr` on
  tagged ints; 6–8× speedup on 1M-iter loops.

## v0.287.0 — Stencils for `mod` / `quot` / `rem`

- JIT: add stencils for `OP_MOD_II`/`OP_QUOT_II`/`OP_REM_II`; 3–6×
  speedup on 1M-iter loops.

## v0.285.0 — `OP_LOOP_INT_DEC_INC` stencil

- JIT: add `OP_LOOP_INT_DEC_INC` stencil; dec-inc 10M loop runs ~18 ms
  (was ~31 ms), 1.67×.

## v0.284.0 — `MINO_CPJIT_STATS` blocker breakdown self-describes opcode names

- JIT: blocker histogram now prints symbolic opcode names alongside
  numeric ids (e.g. `OP_MAKE_LAZY`) for easier diagnosis.

## v0.283.0 — `apply` passes lazy / chunked tails to fn rest-args

- Eval: `(apply f ... infinite-seq)` no longer hangs; lazy/chunked
  tails are spliced directly into variadic rest-args.
- Sequences: `val_to_seq` gains pass-throughs for `CHUNKED_CONS`/`CHUNK`
  so downstream consumers keep the chunked spine.

## v0.282.0 — `do` body iteration forces lazy cdrs

- Eval: `do` body walker forces lazy cdrs; macro-synthesized lazy
  bodies no longer silently drop trailing forms.

## v0.281.0 — Regex `|` top-level alternation + trailing-greedy fix

- Regex: `|` top-level alternation now compiles and matches correctly.
- Regex: fix leaked `matchlength` on early quantifier failure causing
  whole-match strings to be one byte too long.

## v0.280.0 — `eval` recurses through lazy / chunked call forms

- Eval: call forms that are `MINO_LAZY` or have lazy cdrs (produced by
  `concat`/`sequence` macro expansion) are now fully evaluated.

## v0.279.0 — `clojure.math` namespace

- Stdlib: ship `clojure.math` (PI, E, trig, log/exp, rounding, IEEE
  754 utils); gated on `MINO_CAP_MATH_LIB`.

## v0.278.0 — Macros receive `&env` (map of locals) and `&form`

- Macros: `&env` is now a map of lexical locals at the call site;
  `&form` is the full macro-call form (both were nil before).

## v0.277.0 — `rem` / `mod` on doubles match JVM byte-identically

- Numerics: `mod`/`rem` on doubles now match JVM results exactly by
  forcing separate IEEE 754 multiply and subtract ops via volatiles.

## v0.276.0 — Keywords / symbols carry ns and name as separate boundaries

- Interop: keywords/symbols store `ns_len` so `name`/`namespace`/`=`
  distinguish `(keyword "a/b" "c")` from `(keyword "a" "b/c")`.
- API: add `mino_keyword_ns_n` and `mino_symbol_ns_n` constructors.

## v0.275.0 — `last` survives `with-redefs` of `first`

- Stdlib: `last` captures `first`/`next` at boot time so `with-redefs`
  of `first` no longer causes infinite recursion.

## v0.274.0 — `letfn` supports mutual recursion via `letfn*` special form

- Eval: add `letfn*` special form; `letfn` now supports mutual recursion
  by pre-binding all names in a single env before evaluating bodies.

## v0.273.0 — `merge-with` returns a single non-map input as-is

- Collections: `(merge-with f x)` with a single non-map arg returns `x`
  unchanged, matching JVM (fixes `deep-merge` base-case).

## v0.272.0 — `walk` recurses into any `seq?`, not just `cons?`

- Stdlib: `postwalk`/`prewalk` now recurse into lazy seqs, not just
  cons lists.

## v0.271.0 — `reduce-kv` on vectors uses index-as-key

- Collections: `reduce-kv` on vectors calls `(f acc index element)` per
  slot, matching JVM.

## v0.270.0 — `partition-all` transducer emits vector groups

- Sequences: `partition-all` transducer now emits each group as a
  vector, matching JVM.

## v0.269.0 — Doubles print at the shortest round-trippable precision

- Printer: doubles now print at the shortest precision that round-trips,
  matching `Double.toString` semantics.

## v0.268.0 — `seq-to-map-for-destructuring` helper

- Stdlib: add `seq-to-map-for-destructuring` (JVM 1.11+ varargs kwargs
  helper).

## v0.267.0 — Nested patterns inside map destructuring

- Compiler: map destructuring now accepts vector/map patterns in the
  key position, e.g. `{[lhs rhs] :c}`.

## v0.266.0 — Symbols callable as keyword-style lookups

- Eval: `('sym m)` / `('sym m default)` look up the symbol-keyed entry
  in `m`, matching JVM.

## v0.265.0 — Map destructuring evaluates the key expression

- Compiler: `{sym k}` destructuring evaluates `k` in the surrounding
  scope rather than using it as a literal key.

## v0.264.0 — `keys` / `vals` accept seqs of map entries

- Collections: `keys`/`vals` now accept any seqable of `[k v]` pairs,
  not only maps.

## v0.263.0 — `keep-indexed` gains the 1-arg transducer arity

- Sequences: `(keep-indexed pred)` now returns a transducer.

## v0.262.0 — `name` / `namespace` split at the last slash

- Interop: `name`/`namespace` now scan for the last `/` so the
  2-arg keyword constructor round-trips correctly.

## v0.261.0 — `condp` recognizes the `:>>` result-fn arrow

- Macros: `condp` now supports the `:>> result-fn` form, passing the
  truthy pred result to the handler.

## v0.260.0 — `subseq` / `rsubseq` 5-arg form matches JVM Clojure semantics

- Collections: 5-arg `subseq`/`rsubseq` now accepts any test
  orientation in either position, matching JVM semantics.

## v0.259.0 — `mapv` gains the multi-collection arity

- Sequences: `(mapv f c1 c2 ...)` now supports N≥3 collections,
  stopping at the shortest.

## v0.258.0 — `sequence` gains the 1-arg coerce-to-seq arity

- Sequences: `(sequence coll)` now coerces its input to a seq (nil and
  empty collections yield `()`).

## v0.257.0 — `drop` always returns a seq, never the source collection

- Sequences: `(drop n coll)` for `n<=0` now routes through `prim_seq`
  so the result is always a seq type.

## v0.256.0 — `clojure.string/replace` accepts regex match and fn replacement

- Strings: `str/replace` now accepts a regex as the match argument, with
  string (`$N` group refs), callable, and char/string replacement shapes.

## v0.255.29 — Fix: BC catch lands restore `bc_current_bc`; safepoint sleeps for fair handoff

- VM: BC catch handler now restores `bc_current_bc` before
  `normalize_exception`, preventing heap-use-after-free under ASan.
- GC: `gc_mark_runtime_globals` marks `bc_current_bc` as a GC interior
  pointer for all live contexts.
- Async: BC safepoint auto-yield uses 100µs `nanosleep` instead of
  `sched_yield()` for fair lock handoff on ≤2-CPU hosts.

## v0.255.28 — Fix: tighten new async tests for tight-CPU CI runners

- Tests: clamp async test fanout to host thread grant and add 200ms
  cleanup delays to avoid `MTH001` on 3-CPU GHA runners.

## v0.255.27 — Bug-fix sweep: deref/regex/location/concurrency/cleanup

- Async: per-ctx BC register stack prevents concurrent fn calls from
  corrupting each other's argument slots.
- Async: BC safepoint poll enables `future-cancel` to interrupt
  CPU-bound workers and auto-yields for sibling scheduling.
- Async: 3-arg `(deref ref ms timeout-val)` timed deref implemented
  for futures and promises.
- Regex: `(?i)`/`(?s)`/`(?m)`/`(?x)` inline flags with negation and
  combination; `.` no longer matches `\n` by default.
- Errors: `eval_current_form` is restored after sub-eval so throw
  locations blame the correct file and form.
- Errors: user-throw catch values now carry `:mino/location`; BC fn
  body throws blame the throw site rather than the call site.
- Stdlib: `(char n)` constructor added to `clojure.core`.
- CI: update CI workflow comment to reflect actual release-gate steps.
- Embedding: `src/public` files now borrow internals only through
  `internal_bridge.h`.
- Stdlib: `pmap` shipped in `clojure.core` now that yield safety is in
  place.

## v0.255.26 — Fix: `(str/split "" re)` returns `[""]`, not `[]`

- Strings: `str/split` on empty input now returns `[""]`, matching JVM.

## v0.255.25 — Fix: `seq_iter_init` walks records as kv pairs

- Collections: `into`, `reduce`, `transduce` etc. now iterate records
  as `[k v]` pairs instead of terminating immediately.

## v0.255.24 — Fix: Vector destructuring of lazy / chunked seqs

- Compiler: vector destructuring now realizes lazy/chunked seqs
  incrementally; `(let [[a b c] (range 3)] ...)` works correctly.

## v0.255.23 — Fix: `set!` mutates dynamic-var bindings (was no-op)

- Vars: `set!` now mutates the topmost dynamic binding on the stack;
  throws when no binding frame is active, matching JVM.

## v0.255.22 — Fix: Regex engine now parses `{n}` / `{n,m}` / `{n,}` quantifiers

- Regex: bounded quantifiers `{n}`, `{n,m}`, `{n,}` now compile and
  match correctly; `{` followed by non-digit is treated as a literal.

## v0.255.21 — Fix: `time-ms` returns wall-clock, not process CPU time

- Stdlib: `time-ms` and the `(time expr)` macro now measure wall-clock
  time via `mino_monotonic_ns`.

## v0.255.20 — Fix: defrecord auto-binds fields in inline method bodies

- Interop: `defrecord` inline protocol method bodies now have declared
  fields bound as locals, matching JVM.

## v0.255.19 — Fix: Worker leaks last_diag on uncaught throw

- Async: `worker_run` now frees `ctx->last_diag` before freeing the
  ctx, eliminating a 160-byte leak per throwing future.

## v0.255.18 — Fix: Preserve ex-info data through futures + expose `>!` / `<!`

- Async: `ex-info` `:data` payload is now preserved when rethrown from
  a future's deref.
- Async: `>!` and `<!` parking ops are now var-bound and `:refer`-able.

## v0.255.17 — Fix: Disable ASan fake-stack for conservative scanner

- Build: `__asan_default_options` now disables `detect_stack_use_after_return`
  to prevent the conservative GC stack scanner from SEGVing under gcc
  ASan's fake-stack.

## v0.255.16 — Fix: Defer to libsanitizer + filter JIT note from parity diff

- Build: crash handler is skipped when any sanitizer is active so
  libasan/libubsan reports are not swallowed.
- CI: JIT-unavailable note is stripped from parity-diff output before
  byte-identity comparison.

## v0.255.15 — Fix: Free dyn_frame on eval_try unwind path

- GC: tree-walker `eval_try` unwind paths now free the `dyn_frame_t`
  after unbinding, eliminating a 16-byte leak per throwing `binding`.

## v0.255.14 — Fix: gc_scan_stack no_sanitize attribute on gcc-built ASan

- Build: `gc_scan_stack` `no_sanitize_address` attribute now also
  applies under gcc (`__SANITIZE_ADDRESS__`), not only clang.

## v0.255.13 — Fix: Drop byte-identity stencil checks from CI

- CI: `check-stencils-fresh` removed from release-gate and CI matrix;
  retained as a dev pre-commit task.

## v0.255.12 — Fix: Adapt closure-capture-macro-introduced fanout to thread-limit

- Tests: fanout in `closure-capture-macro-introduced` clamped to
  `(- (mino-thread-limit) 1)` so CI runners with ≤4 CPUs don't hit
  `MTH001`.

## v0.255.11 — Fix: Yield state_lock Around pthread_join in Future Sweep

- GC: `mino_future_gc_sweep` now yields `state_lock` around
  `pthread_join`, eliminating a deadlock when sweep runs with live
  workers.

## v0.255.10 — Diagnostic: SIGABRT Watchdog on CI Test Hang

- CI: test step now sends SIGABRT 30s before GHA timeout so
  `crash_handler` dumps a backtrace for hang diagnosis.

## v0.255.9 — Fix: `(gc!)` During In-Flight Major Mark Use-After-Free

- GC: `MINO_GC_FULL` path now finishes the in-flight major before
  running the minor, eliminating a use-after-free on the mark stack.

## v0.255.8 — Diagnostic: Per-Deftest Trace for CI Hang Investigation

- CI: `MINO_TEST_TRACE=1` prints `[trace] ns/deftest` to stderr before
  each deftest; trace artifact uploaded on failure.

## v0.255.7 — Portability: gcc Sanitizer Detection, POSIX strcasecmp, Empty TUs

- Build: fix gcc `__has_feature` preprocessor crash in `gc/internal.h`.
- Build: add `<strings.h>` for `strcasecmp`; alias to `_stricmp` on
  Windows.
- Build: add sentinel typedefs in JIT TUs that are empty without
  `MINO_CPJIT_HOST`.
- Build: fix `mino_jit_invoke` stub signature mismatch.

## v0.255.6 — Fix: BC Speculative Fold Longjmps Through Active Try-Frame

- Compiler: speculative constant-fold now zeroes `try_depth` before the
  test call so fold errors can't longjmp into the user's try-frame.

## v0.255.5 — Fix: BC Bitwise Fast Path No Longer Promotes to Bigint

- VM: bitwise BC fast-path results now use `mino_int_wrap` (no bigint
  promotion), matching prim semantics.

## v0.255.4 — Hygiene: I/O Buffer Overflow + Safepoint Comment

- Security: `append_byte` and `file_seq_recurse` growth loops now use
  `checked_double_sz`/`checked_mul_sz` overflow guards.
- Docs: `should_yield` field comment updated to name actual writers and
  readers.

## v0.255.3 — Fix: VM Arithmetic Fallback UB

- VM: non-GCC/Clang ADD/SUB/MUL overflow fallbacks now use
  well-defined unsigned arithmetic instead of signed UB.

## v0.255.2 — Fix: thread_count Atomic Accesses

- Async: all `thread_count` reads and writes now use `__atomic_*` with
  `ATOMIC_RELAXED`, eliminating the TSan data-race report.

## v0.255.1 — Fix: thread-sleep Yields state_lock

- Async: `thread-sleep` now yields `state_lock` during `nanosleep` so
  sibling futures can make progress.

## v0.255.0 — Fix: loop+recur Closures Inside defn Bodies

- Compiler: `compile_recur` emits `OP_POP_ENV`/`OP_PUSH_ENV` per
  iteration when the enclosing fn captures vars, so loop closures see
  per-iteration bindings instead of iter-0's values.

## v0.254.0 — Cross-Repo Release-Gate Hook

- CI: release-gate now chains into `mino-tests` satellite smoke suite
  when the sibling repo is checked out adjacent.

## v0.253.3 — Test-Suite Split: Borderline E2E Audit

- Tests: move `creative_test`, `doc_examples_test`, `bc_jit_deopt_test`,
  `ns_parity_run`, and `spawn_stress_regression` to `mino-tests`.

## v0.253.2 — Test-Suite Split: C-Side Embed Harnesses + Error-Message Normalization

- Tests: move multi-state/STM/capability C embed harnesses to
  `mino-tests`; single-state `embed_api_test.c` stays.
- Errors: top-level unprotected-throw path now emits "uncaught
  exception" consistently.

## v0.253.1 — Test-Suite Split: Fuzz / GC Stress / Fault Injection

- Tests: move fuzz, GC stress, fault-injection, and HAMT churn
  regression tests to `mino-tests`.
- CI: nightly GC-stress and fault-injection steps move to mino-tests
  nightly.

## v0.253.0 — Test-Suite Split: Concurrency-Heavy Migration

- Tests: move concurrency-heavy and async-soak tests to `mino-tests`;
  mino retains a minimal `async_smoke_test.clj`.

## v0.252.3 — Closure Capture Across Self-Tail-Call

- VM: fix closure capture across self-tail-call; closures in loop/recur
  or self-recursion now see the iteration's own param values, not the
  next iteration's
- VM: `apply_callable` and `eval_loop` recur trampolines allocate a
  fresh `env_child` on each iteration instead of reusing and mutating
  param slots in place
- Tests: add 6 deftests / 9 assertions covering closure-over-recur,
  dotimes, for comprehension, macro-introduced futures, and
  multi-arity self-call

## v0.252.2 — Runtime Hardening from Whitebox Review

- Reader: bound recursion depth at 1024 levels; deep nesting now
  returns MRE011 instead of stack-overflowing the embedder
- VM: gate `gc_pin`/`gc_unpin` overflow asserts on sanitizer builds
  only; scripts with many case/cond arms no longer abort release builds

## v0.252.1 — Developer UX Fixes from Whitebox Review

- Errors: reset `reader_col` at the start of each eval/load so
  line-1 errors report the correct column instead of the previous
  file's terminal column
- Errors: file-mode and `-e` error paths now preserve the inner
  exception kind, code, and message instead of degrading to MCT001
- REPL: multi-line error snippets now render actual user input
- CLI: `--jit=` flag is now case-insensitive, matching `MINO_JIT`
- CLI: `mino-lean` emits a stderr note when `--jit=on` is requested
  but the JIT is compiled out
- CLI: `mino-lean --help` annotates JIT flags as compiled-out
- CLI: `mino-lean --version` reports `mino-lean X.Y.Z (no-jit)`

## v0.252.0 — JIT Feature-Complete

- JIT: declare CPJIT feature-complete on the dev host; 39 stencils
  across 5 host arches, dual-binary build, 4-way parity green
- Docs: mino-site gains JIT status page documenting the runtime
  control surface and by-design omissions

## v0.251.0 — JIT Portability Matrix + On/Off A/B Evidence

- Docs: mino-site gains JIT support matrix page with on/off A/B
  numbers; fibonacci(25) shows 1.37x JIT win on the dev host

## v0.250.0 — Default Nursery 1 MiB -> 4 MiB

- GC: bump default nursery from 1 MiB to 4 MiB; allocation-heavy
  workloads run up to 1.42x faster with ~35-60% less total GC time
- GC: embedders can override via `MINO_GC_NURSERY_BYTES` or
  `mino_gc_set(state, MINO_GC_NURSERY_BYTES, n)`

## v0.249.0 — Measurement Baseline

- Performance: capture realistic_bench baseline; four of six rows are
  35-43% GC time, establishing allocation as dominant cost

## v0.248.0 — Nightly Matrix Workflow

- CI: add `ci-nightly.yml` cron workflow (04:00 UTC daily) running
  release-gate, GC stress, fault-inject, and embed stress on Linux +
  Darwin

## v0.247.0 — Cross-Compile Smoke Job + x86_64 Darwin Posture

- CI: add `cross-compile` GHA job on `macos-14` that regenerates all
  `gen-stencils-<arch>-<os>` targets and asserts headers match
  committed bytes via `git diff --exit-code`

## v0.246.0 — GHA Matrix Extension + Release-Gate Step

- CI: pin GHA matrix to `ubuntu-24.04`, `ubuntu-24.04-arm`, `macos-14`,
  `windows-2022`; add `release-gate` step on every non-Windows entry

## v0.245.0 — Docker Images + ci-matrix Task

- CI: add `docker/arm64-linux.Dockerfile` and
  `docker/x86_64-linux.Dockerfile` for local Linux CI mirrors
- Build: add `mino task ci-matrix` to build images, bind-mount repo,
  and run release-gate per target with aggregated pass/fail output

## v0.244.0 — Extractor Carve-Out: coff + Synthetic-Blob Selftests

- Toolchain: carve PE/COFF parser into `tools/stencil_extract/coff.{h,c}`
- Toolchain: add `tools/stencil_extract/selftest.{h,c}` with synthetic
  in-memory Mach-O / ELF / COFF blobs; parser regressions now surface
  from `--selftest` without a full gen-stencils pass

## v0.243.0 — Extractor Carve-Out: elf Module

- Toolchain: carve ELF64 parser into `tools/stencil_extract/elf.{h,c}`;
  functions take `elf_` prefix

## v0.242.0 — Extractor Carve-Out: macho Module

- Toolchain: carve Mach-O parser into `tools/stencil_extract/macho.{h,c}`;
  functions take `macho_` prefix

## v0.241.0 — Extractor Carve-Out: core Module

- Toolchain: carve format-agnostic stencil plumbing into
  `tools/stencil_extract/core.{h,c}`; binary renamed to
  `tools/stencil-extract` (hyphen) to free the directory for modules
- Build: `build-stencil-extract` now takes a source list; adding a
  format is a localised change

## v0.240.0 — 4-Way JIT Parity (AUTO / ON / OFF / lean)

- JIT: expand parity test to assert byte-identical stdout across
  `--jit=auto`, `--jit=on`, `--jit=off`, and `mino-lean`

## v0.239.0 — JIT Threshold Tuning + Capability Query API

- API: add `mino_state_set_jit_hot_threshold` / `mino_state_jit_hot_threshold`
  and `mino_state_jit_capability` returning `{available, mode, threshold,
  host_arch, host_os}`
- CLI: add `--jit-threshold=N` flag; reads `MINO_JIT_HOT_THRESHOLD` env var

## v0.238.0 — Runtime JIT Mode (AUTO / OFF / ON)

- API: add `mino_jit_mode_t` enum and `mino_state_set_jit_mode` /
  `mino_state_jit_mode`; each VM state has an independent JIT mode
- CLI: add `--jit=auto|off|on` flag; reads `MINO_JIT` env var

## v0.237.0 — Dual-Binary Build: mino + mino-lean

- Build: rename `mino_nojit` artifact to `mino-lean`; `mino-lean` is
  ~4% smaller static footprint than `mino`

## v0.236.0 — COFF Parser + VirtualAlloc Swap + Generated x86_64 Windows Header

- Toolchain: add PE/COFF amd64 parser to `stencil_extract` covering
  IMAGE_REL_AMD64_REL32/REL32_1/ADDR64 reloc kinds
- Windows: swap `mmap/mprotect/munmap` for `VirtualAlloc/VirtualProtect/
  VirtualFree` in `jit/emit.c` under `_WIN32`
- JIT: check in cross-compiled `stencils_x86_64_windows.h` for all 39
  stencils; x86_64 Windows toolchain path is now code-complete

## v0.235.0 — Mach-O x86_64 in Extractor + Generated x86_64 Darwin Header

- Toolchain: add x86_64 Mach-O reloc-kind map to extractor; dispatch
  by `cputype` between ARM64 and x86_64
- JIT: check in cross-compiled `stencils_x86_64_darwin.h` for all 39
  stencils

## v0.234.0 — x86_64 Patcher + Direct-emit + Trampoline + Arch Dispatch

- JIT: add x86_64 patchers (`patch_abs64`, `patch_pc32`,
  `patch_gotpcrel`, `patch_jmp32_to`, `patch_jcc32_to`) in
  `jit/patcher_x86_64.c`
- JIT: add arch dispatch in `emit.c`; reloc addend now flows through
  for x86_64 `S+A-P` arithmetic
- JIT: add 12-byte `movabs rax, target; jmp rax` trampoline writer
- Toolchain: extend x86_64 ELF reloc map with `R_X86_64_GOTPCRELX`
- JIT: check in cross-compiled `stencils_x86_64_linux.h` for all 39
  stencils

## v0.233.0 — Stencil ABI Overhaul: musttail Chain Marker

- JIT: replace ARM64-specific struct-return chain mechanism with a
  portable `__attribute__((musttail))` call to
  `mino_jit_chain_continue_marker`; chain ABI is now host-agnostic
- JIT: emit pass walks reloc table for chain marker relocations instead
  of scanning for `ret` opcodes; all non-final stencil chain sites are
  patched

## v0.232.0 — x86_64 ELF Reloc Map + Enum Mirror

- JIT: add `MINO_STENCIL_RELOC_X86_64_*` enum entries (ABS64, PC32,
  GOTPCREL) on both runtime and extractor sides
- Toolchain: add `reloc_x86_64_elf_kind_map`; `elf_extract_relocs`
  dispatches on `e_machine`

## v0.231.0 — Generated stencils_arm64_linux.h Committed

- JIT: check in cross-compiled `stencils_arm64_linux.h` for 39
  stencils; ARM64 Linux builds now JIT-eligible without a regenerate pass
- Build: refactor `gen-stencils` into parameterised `gen-stencils-for`;
  add `gen-stencils-arm64-linux` task

## v0.230.0 — ELF Parser In Stencil Extractor

- Toolchain: add ELF64 parser (`elf_open`, `elf_list_symbols`,
  `elf_find_symbol`, `elf_extract_relocs`) to `stencil_extract`
- Toolchain: add `reloc_arm64_elf_kind_map` covering all AArch64 reloc
  kinds used by clang stencil output

## v0.229.0 — JIT Stencil For OP_ASSOC

- JIT: add `OP_ASSOC` stencil with fast lanes for vector and map cases

## v0.228.0 — JIT Stencil For OP_CONJ_VEC

- JIT: add `OP_CONJ_VEC` stencil; vector case uses `vec_conj1`, other
  types fall through to `prim_conj`

## v0.227.0 — JIT Stencil For OP_GET_KW_MAP

- JIT: add `OP_GET_KW_MAP` stencil; map case uses `map_get_val`,
  record+keyword case uses `record_field_index`, others fall through to
  `prim_get`

## v0.226.0 — JIT Stencils For OP_COUNT_VEC + OP_EMPTY_VEC

- JIT: add `OP_COUNT_VEC` and `OP_EMPTY_VEC` stencils with vector fast
  lanes; misses fall through to `prim_count`/`prim_empty_p`

## v0.225.0 — JIT Stencils For OP_NTH_VEC + OP_FIRST_VEC

- JIT: add `OP_NTH_VEC` and `OP_FIRST_VEC` stencils; fns using vector
  indexing or first now pass JIT eligibility instead of forcing
  interpreter fallback

## v0.224.0 — apply_callable_argv Inlining Results

- JIT: apply_callable_argv inlining (v0.220-v0.224) lands the
  architecture, but the fib(25) 1.30x target was not met; `mino_bc_run`
  setup and re-entry overhead dominate

## v0.223.0 — PRIM_ARGV Fast Path In OP_CALL_CACHED

- JIT: add `MINO_IC_CALLABLE_PRIM_ARGV` branch in `OP_CALL_CACHED`
  stencil routing to `mino_jit_call_known_prim_slow`, skipping
  `apply_callable_argv` var-unwrap and type-of dispatch

## v0.222.0 — Single-Clause Dispatch Fast Path In mino_bc_run

- VM: add early-out in `mino_bc_run` arity dispatch for the common
  `n_clauses == 1` case, skipping two loop iterations per call

## v0.221.0 — Known-Callee Fast Path In OP_CALL_CACHED (Scaffolding)

- JIT: add `MINO_FN_BC_SINGLE` branch in `OP_CALL_CACHED` stencil
  routing to `mino_jit_call_known_fn_slow`, bypassing
  `apply_callable_argv` dispatch switch
- VM: extract `invoke_bc_fn_argv` as shared `always_inline` core
  called from both the original path and the new known-callee entry

## v0.220.0 — IC Slot Callable-Shape Cache (Setup)

- JIT: add `cached_callable_kind`, `cached_fn_has_rest`, and
  `cached_fn_n_params` fields to `mino_bc_ic_slot_t`; IC slot grows
  48->56 bytes; fields are filled on cache fill but not yet consumed

## v0.219.0 — Regex Engine + str/split With Regex Separators

- Regex: raise `MAX_REGEXP_OBJECTS` 30->256 and `MAX_CHAR_CLASS_LEN`
  40->256; overflow now returns an explicit compile failure instead of
  silently truncating
- Strings: `clojure.string/split` now honours regex separators;
  zero-width matches advance one codepoint to prevent infinite loops

## v0.218.0 — CI Guardrails For Stencil + Reloc Drift

- CI: add `check-stencils-fresh` (G1) asserting committed stencil
  headers match a fresh `gen-stencils` run
- CI: add `check-stencil-registry` (G2) cross-checking the stencil
  list against `src/eval/bc/stencils/*.c` in both directions
- CI: add `check-reloc-mirror` (G3) asserting `MINO_STENCIL_RELOC_*`
  values agree between runtime header and extractor source
- CI: add `release-gate` composite task running G1-G3, test suite,
  ASan suite, and JIT parity in fail-fast order

## v0.217.0 — Boundary Parity Tests Between JIT And Interpreter

- Tests: add `tests/jit_parity_test.clj` (47 deftests) covering range
  boundaries, tag-miss paths, and comparison identity for all 16
  inlined arith/cmp/unary stencils
- Build: add `build-nojit` task producing `./mino_nojit` and
  `test-jit-parity` task asserting byte-identical stdout between
  both binaries

## v0.216.0 — Split jit.c Into Five Translation Units

- Build: split `src/eval/bc/jit.c` (2012 lines) into
  `jit/entry.c`, `jit/stats.c`, `jit/helpers.c`, `jit/patcher.c`,
  `jit/emit.c`, and `jit/internal.h`; no behaviour change

## v0.215.0 — CPJIT Speedup Results

- JIT: fibonacci(25) reaches 1.19x JIT vs interpreter (v0.210-v0.214);
  other realistic_bench rows within noise
- JIT: multi-ret chain patching now rewrites every `ret` in a stencil
  span, not just the first
- JIT: publish `S->jit_invoke_ctx` so stencils read dynamic stack via
  a fixed-offset load, avoiding Darwin TLVP relocations

## v0.214.0 — Inline OP_CALL_CACHED Resolve Fast Path

- JIT: inline IC-slot hit check in `OP_CALL_CACHED` stencil; on hit,
  route to `mino_jit_call_resolved_slow` bypassing the IC cascade;
  fibonacci(25) reaches 1.19x JIT speedup

## v0.213.0 — Inline Arith II + IK Families

- JIT: inline tagged-int fast lanes in 13 arith/comparison stencils
  (ADD/SUB/MUL II, LT/LE/GT/GE/EQ II, ADD/SUB IK, LT/LE/EQ IK);
  misses fall through to existing slow helpers

## v0.212.0 — Inline INC_I / DEC_I / ZERO_INT_P Fast Lanes

- JIT: inline tagged-int fast lanes in `OP_INC_I`, `OP_DEC_I`, and
  `OP_ZERO_INT_P` stencils; `map/filter/map/reduce over 50k` improves
  ~1.12x vs v0.210 baseline

## v0.211.0 — Inline OP_GETGLOBAL_CACHED Hit Path

- JIT: OP_GETGLOBAL_CACHED stencil reads IC slot and writes regs[A]
  inline on hit, falling through to slow helper only on miss.
- JIT: Published calling thread's ctx on mino_state_t::jit_invoke_ctx
  so stencils can read dyn_stack without a TLS round-trip.
- JIT: Chain-patcher now rewrites every ret in a non-final stencil's
  span, not just the first, fixing silent mid-region early exit.
- Build: runtime_layout.h typedef guards widened to also recognise
  MINO_BC_STENCIL_ABI_H, eliminating -Wpedantic redefinition warning.

## v0.210.0 — JIT Stencil-Layer Runtime-Layout Header

- JIT: Adds src/eval/bc/stencils/runtime_layout.h exposing selected
  runtime struct fields to hermetic stencil compilation units.
- JIT: Layout-anchor offset constants and accessor macros let stencils
  read IC gen, dyn_stack, and bc ic_slots without including canonical
  headers.
- JIT: Build-time offsetof asserts in jit.c catch field-reorder
  regressions at compile time rather than stencil mis-reads at runtime.

## v0.209.0 — CPJIT Coverage Results

- JIT: CPJIT eligibility on tests/run.clj reaches 83.9% (up from
  18.8%); jit_bench reaches 94.9%.
- JIT: jit_bench perf gate deferred — per-row deltas remain within
  run-to-run noise; gate will land alongside a measurable speedup.
- JIT: LOOP_INT_LT direct-emit redo stays deferred; no new evidence
  justifies the work over existing fused stencils.
- Build: JIT builds are ARM64 Darwin only; Windows/x86_64/ARM64 Linux
  portability deferred.

## v0.208.0 — Closure / Env Stencils

- JIT: Stencilises OP_CLOSURE, OP_PUSH_ENV, OP_POP_ENV, and
  OP_ENV_BIND, dropping the captures eligibility blocker entirely.
- JIT: Eligibility on tests/run.clj rises to 83.9%; captures blockers
  fall from 15 to 0.

## v0.207.0 — Multi-Arity + Variadic Eligibility

- JIT: has_rest (variadic &) blocker removed; rest-collection is
  already in regs before the JIT region runs.
- JIT: n_clauses > 1 blocker removed for entry_pc == 0 clauses; other
  arities fall back to interpreter pending per-clause entry points.
- JIT: Eligibility on tests/run.clj rises to 76.2%.

## v0.206.0 — OP_CALL Uncached + OP_TAILCALL Stencils

- JIT: OP_CALL stencil added for uncached callee positions; routes
  through apply_callable_argv.
- JIT: OP_TAILCALL FINAL stencil added; publishes sentinel and returns
  to trampoline without growing the C stack.
- JIT: jit_bench eligibility reaches 94.9%; tests/run.clj reaches
  68.6%.

## v0.205.0 — OP_CALL_CACHED Stencil + Two-Word Op Handling

- JIT: OP_CALL_CACHED stencil added, covering global-head call sites;
  OP_CALL_CACHED rejections drop to zero.
- JIT: Compile walk and eligibility check grow op_extra_words helper
  to correctly skip two-word instructions.
- JIT: IMM_KIND_BX2 immediate kind carries slot index from the
  trailing instruction word into the stencil at compile time.

## v0.204.0 — OP_GETGLOBAL_CACHED Stencil

- JIT: OP_GETGLOBAL_CACHED stencil added; ic_slots_len > 0 eligibility
  blocker removed.
- JIT: jit_invoke ABI extended with jit_invoke_env field so the slow
  helper can perform env-lookup for closure-captured locals.
- JIT: IMM_KIND_BC immediate carries the bc pointer into the stencil
  literal pool.

## v0.203.0 — JIT Introspection + Eligibility Tracer

- JIT: MINO_CPJIT_STATS=1 enables per-fn compile tracking with
  eligibility reason histogram and unknown-op breakdown at exit.
- JIT: apply_callable_argv now bumps hot_counter and triggers JIT
  compile, fixing fns reached only via the bc call ABI never warming.
- JIT: Baseline eligibility: 18.8% on tests/run.clj, 43.6% on
  jit_bench; top blocker is ic-slots.

## v0.202.0 — CPJIT Stencil Coverage + Perf Measurement

- JIT: OP_LOOP_INT_LT removed from active stencil table after
  measuring a 17% regression vs interpreter inline fast path.
- JIT: sum-loop achieves 2.08x speedup; dec-loop 1.66x; count-loop
  stays at interpreter speed by design.
- JIT: Active stencil set is 22 ops after this release.

## v0.201.0 — Fused Counted-Loop Stencils

- JIT: Stencils added for OP_LOOP_INT_LT, OP_LOOP_INT_DEC, and
  OP_LOOP_INT_LT_INC; each embeds a native for(;;) loop avoiding
  per-iteration stencil re-entry.
- JIT: Slow helpers use low-bit tag on returned regs pointer to signal
  exit vs continue without an extra register.
- JIT: Back-jump marker mechanism wired into emit_stencil for future
  stencils that can't use a natural C loop.

## v0.200.0 — JIT Control Flow (OP_JMP / OP_JMPIFNOT) + Chain ABI Fix

- JIT: OP_JMP and OP_JMPIFNOT added as direct-emit templates; branch
  targets resolved in a post-layout patch pass.
- JIT: OP_JMPIFNOT inlines mino truthiness check using sub+b.ls to
  cover nil and false in two instructions.
- JIT: Non-final stencils now return mino_stencil_chain_t (regs,
  consts) so clang preserves consts across bl calls via AAPCS.
- JIT: MINO_STENCIL_CHAIN_RETURN macro pins S in x2 at every ret via
  named-register asm, fixing chain-ABI clobber bug.
- JIT: LOAD_K + RETURN fusion disabled when body contains branch ops
  to prevent jumps landing on the RETURN half of a fused stencil.

## v0.199.0 — Unary + Immediate-Arg Stencils

- JIT: Stencils added for OP_INC_I, OP_DEC_I, OP_ZERO_INT_P and for
  OP_ADD_IK, OP_SUB_IK, OP_LT_IK, OP_LE_IK, OP_EQ_IK.
- JIT: IMM_KIND_KIMM carries pre-tagged immediate literal into the
  stencil pool; stencil set grows to 20 ops.

## v0.198.0 — Comparison Stencils (LT / LE / GT / GE / EQ_II)

- JIT: Stencils added for LT, LE, GT, GE, EQ_II; tagged-int fast lane
  returns sentinels without allocation; stencil set grows to 10 ops.

## v0.197.0 — Stencil Call ABI + ADD_II / SUB_II / MUL_II

- JIT: ADD_II, SUB_II, MUL_II stencils added; first stencils to call
  host C helpers via bl.
- JIT: Stencil ABI settled at (regs, consts, S) in (x0, x1, x2);
  non-final stencils return regs to preserve the window across bl.
- JIT: Per-call trampoline (ldr/br + 8-byte target) appended between
  code and pool to sidestep bl's ±128 MB range limit.
- JIT: Trailing-ret trim heuristic replaced with full-body copy and
  first-ret scan; mmap layout becomes [code | trampolines | pool].
- Build: -fno-optimize-sibling-calls added to gen-stencils so clang
  does not tail-call slow helpers.

## v0.196.0 — Externalise Int-Arith Fast-Path Helpers

- JIT: binop_int_fast, unop_int_fast, and tag_or_box_int promoted from
  static to extern so upcoming stencils can call them via direct bl.

## v0.195.0 — Fused LOAD_K + RETURN Superinstruction

- JIT: Fused OP_FUSED_LOAD_K_RETURN stencil reduces constant-returning
  fn code size from 40 to 16 bytes and pool slots from 3 to 1.

## v0.194.0 — Saturating Counter on JIT-Ineligible Fns

- JIT: hot_counter saturates to UINT_MAX on first compile rejection;
  subsequent calls skip eligibility re-check entirely.
- JIT: Background compile worker deferred; synchronous compile latency
  is sub-millisecond with the current narrow stencil set.

## v0.193.0 — Single-Page JIT Layout for Small Fns

- JIT: Fns whose code plus pool fit in one host page are laid out in a
  single mmap region, halving resident memory for small fns.

## v0.192.0 — Windows COFF Detection Scaffolding

- Windows: stencil_extract learns to detect COFF amd64 objects and
  emits a placeholder error; COFF reloc constants declared for future
  parser.

## v0.191.0 — x86_64 Infrastructure Scaffolding

- Build: stencil_extract gains x86_64 ELF reloc constants; jit.c host
  detection adds MINO_CPJIT_X86_64_LINUX and _X86_64_DARWIN branches.

## v0.190.0 — ARM64 Linux Infrastructure Scaffolding

- Linux: stencil_extract sniffs ELF magic and emits placeholder error;
  ARM64 ELF reloc constants declared for future parser.
- Build: MINO_CPJIT_STENCILS_HEADER macro indirects the stencil header
  path so ARM64 Linux can slot in without source reshuffling.

## v0.189.0 — JIT Default On with Host-Aware Stubs

- Build: -DMINO_CPJIT=1 shipped by default in Makefile and
  builtin.clj; no extra flag needed for JIT builds.
- JIT: Hosts without a generated stencil header compile silent stubs
  that return failure, leaving the interpreter unaffected.

## v0.188.0 — Public Deopt Primitive

- JIT: mino_jit_invalidate(S, fn) drops native/native_size/offsets and
  rewinds hot_counter; apply_callable's ic_gen-mismatch path uses it.

## v0.187.0 — JIT Deopt-on-IC-Gen Mismatch Regression Suite

- Tests: tests/bc_jit_deopt_test.clj pins deopt contract for
  JIT'd fns across redefinition, batched defs, and warm/cool cycles.
- Docs: jit.h gains a Deopt-model block documenting the dispatch-entry
  check and mid-execution invalidation case.

## v0.186.0 — Per-PC Native-Offset Side Table

- JIT: native_pc_offsets side table on mino_bc_fn_t maps each bytecode
  pc to its stencil byte offset within the native region.
- JIT: mino_jit_offset_to_pc reverse lookup provided for stack-trace
  and debugger introspection.

## v0.185.0 — Runtime JIT Compile Path

- JIT: mino_jit_compile walks bytecode, mmaps RX pages, copies and
  patches stencils, and flushes I-cache; mino_jit_invoke dispatches.
- JIT: mino_jit_eligible gates on single arity, no captures, no IC
  slots, OP_RETURN terminator, and all ops covered by stencils.
- JIT: apply_callable tier-selects at MINO_JIT_THRESHOLD=100 calls;
  mino_bc_run routes to JIT or interpreter via bc->native check.
- JIT: jit_regions linked list on state owns all mmap regions; freed
  at mino_state_free.
- JIT: MINO_CPJIT_TRACE=1 emits one stderr line per successful compile.

## v0.184.0 — Stencil Immediate ABI and Relocation Pipeline

- JIT: abi.h defines extern char-array IMM symbols (A, B, C, BX, SBX);
  compiler emits relocations the runtime patches per stencil instance.
- JIT: stencil_extract walks Mach-O reloc table and emits per-stencil
  (offset, kind, sym, addend) records plus --append mode.
- JIT: move.c, load_k.c, and return.c stencils added; generated header
  regenerated with byte tables, symbol lists, and reloc tables.

## v0.183.0 — First Stencil Source

- JIT: src/eval/bc/stencils/return.c is the first stencil source
  (ldr/ret, two instructions); exercises full build pipeline end-to-end.
- Build: mino task gen-stencils compiles stencil .c files, runs
  stencil_extract, and writes stencils_arm64_darwin.h.

## v0.182.0 — Stencil Extractor Tool

- Build: tools/stencil_extract.c extracts function bytes from 64-bit
  Mach-O objects into C headers; --list and symbol-extract modes added.
- Build: mino tasks build-stencil-extract and test-stencil-extract
  added; selftest verifies Mach-O struct sizes match file format.

## v0.181.0 — Diagnostic Source-Span Coverage

- Errors: catchable diagnostics now include :mino/location from call
  form cons metadata or bc source map at current pc.
- Tests: tests/bc_error_quality_test.clj pins location-carrying
  contract for arith errors, divide-by-zero, and unresolved symbols.

## v0.180.0 — Var-Discipline Uniform Read Path

- JIT: mino_bc_ic_global_load C entry point exposes the
  dyn/lex/cache/resolve lookup for native tiers and embedders.
- Docs: IC slot contract documented in header: one slot per syntactic
  var ref; ic_gen invalidation forces re-resolve.

## v0.179.0 — Deopt Protocol Scaffolding

- JIT: mino_bc_fn_t gains native, native_size, native_gen, and
  hot_counter fields; apply_callable carries tier-selection branch.
- VM: bc cell sentinel mutability relaxed to allow hot_counter writes
  through the single fn->as.fn.bc pointer.
- Docs: Stencil ABI invariant documented in eval/bc/internal.h.

## v0.178.0 — Source-Map Scaffolding

- VM: Per-fn (line, column) side table added, indexed by pc, populated
  from cons metadata at compile time.
- VM: Bytecode dispatch loop publishes current-pc cursor on thread
  context; set_eval_diag falls back to it when form has no line info.

## v0.177.0 — Lazy-Cell Allocation Probe

- Performance: OP_MAKE_LAZY is 44.4% of dispatch on pure-mino
  lazy-seq path, running 70x slower than the C-backed equivalent.
- Performance: MINO_LAZY-specific freelist deferred; win is narrow
  to pure-mino lazy-seq and idiomatic users use map/filter instead.

## v0.176.0 — BigInt Fusion Bench

- Bigint: bigint_bench.clj added to mino-bench matrix; bigint ops
  measured at 120–235x slower per-op than tagged-int baseline.
- Bigint: fusion deferred; opportunity is real but narrow and requires
  a transient-bignum substrate no other code shares.

## v0.175.0 — Threaded Dispatch Probe

- VM: Top 4 opcodes carry 70.6% of dispatches; distribution is too
  concentrated for threaded dispatch to improve branch prediction.
- VM: Switch dispatch retained; MINO_BC_OP_COUNTS=1 instrumentation
  left in place for future workload re-evaluation.

## v0.174.0 — Type-Feedback IC Probe Re-Run

- VM: No hot monomorphic-int arith call sites found across test suite
  and bench matrix; type-feedback IC deferred.
- VM: MINO_CALL_SITE_SHAPES=1 instrumentation retained for future
  workloads.

## v0.173.0 — Range-Direct Pipeline_Walk

- Performance: pipeline_walk drives bounded int-range sources via
  inline for-loop, eliminating range_thunk chunk allocation; 5.09x
  on reduce+map+range 100k workload.

## v0.172.0 — Builder-Rewrite Coverage Probe

- Compiler: MINO_BUILDER_REWRITE_COUNTS=1 instrumentation measures
  try_builder_rewrite hit rate at 21.8% on the test corpus.
- Compiler: Decision to keep recogniser narrow; non-collection acc
  inits are correctly declined.

## v0.171.0 — User-Fn-Wrapping-Prim Recogniser

- Compiler: compile_fn_literal stamps wraps_prim on single-arg prim-
  forwarding fn templates.
- Performance: pipeline_fast_callable recognises wraps_prim, giving
  fn-wrapped prims the same inline fast lane as bare prim references;
  13% improvement on map-(fn [x] (inc x)) pipeline.

## v0.170.0 — In-Place Set Mutation And Set Into Fast Path

- Collections: set_conj1_owned and set_disj1_owned mutate owner-tagged
  HAMT nodes in place; into #{} drops 60% (2.52x) on 1000-vec source.
- Collections: prim_into for MINO_SET uses transient fast path with
  gc_pin across the loop.

## v0.169.0 — Hamt Owner Discipline And In-Place Map Mutation

- Collections: hamt_assoc_owned/hamt_dissoc_owned mutate owner-matching
  nodes in place; into {} drops 69% (3.24x) on 1000-pair vec source.
- Collections: prim_into for MINO_MAP routes through transient/assoc!/
  persistent with gc_pin across the loop.
- GC: vnode slot writes route through gc_write_barrier to keep remset
  consistent during long transient batches spanning a minor GC.

## v0.168.0 — Keyword-As-Fn Pipeline Fast Lane

- Performance: pipeline_fast_callable recognises MINO_KEYWORD as
  PIPELINE_FAST_KW; kw-fn-record-loop drops 59% (2.5x).
- Collections: record_field_index gains pointer-equality first pass
  for interned keywords, avoiding per-field memcmp on the hot path.

## v0.167.0 — Forward-Counted Recur-Shape Fusion

- Compiler: OP_LOOP_INT_LT and OP_LOOP_INT_LT_INC fused opcodes added
  for forward-counted loop shapes with (< i N)/(>= i N) tests; loop
  benchmarks drop 73–89%.

## v0.166.1 — Builder-Rewriter Safety Patch

- Compiler: try_builder_rewrite declines when acc appears outside the
  bare-exit branch, preventing transient exposure to seq/reduce/=/
  contains? operations that transients do not support.
- Tests: 10 new regression tests cover unsafe and safe rewrite shapes.

## v0.166.0 — Builder-Pattern Compile-Time Rewrite

- Compiler: compile_loop rewrites canonical (loop [... acc []] (if
  test (recur ... (conj acc x)) acc)) to transient form before
  bytecode emission; 71–74% reduction on builder loops.

## v0.165.0 — In-Place Transient Vector Mutation

- Collections: transient/conj!/assoc!/pop!/persistent! on vectors
  mutate owner-tagged trie nodes in place; into-vec-pipeline drops
  74%, mapv-pipeline drops 69%.
- GC: vnode_slot_set routes through gc_write_barrier to keep remset
  consistent when OLD owner-tagged nodes receive YOUNG values.

## v0.164.0 — Unboxed Int-Acc Reducer Fast Lane

- Performance: +, *, -, bit-and, bit-or, bit-xor reducers run in
  unboxed long long when acc and elements are tagged ints; reduce+vec
  100k drops 48%, reduce+set 100k drops 49%.
- Collections: Unboxed path plumbed through reduce_int_range,
  reduce_vec_apply, reduce_pipeline_walk, and prim_reduce fallback via
  shared reduce_ctx_t helpers.

## v0.163.0 — IC Resolve Path Consolidation

- VM: Consolidate IC-cache resolve logic for GLOBAL and PROTOCOL
  slot kinds into shared `ic_resolve_global` / `ic_resolve_protocol`
  helpers; opcode handlers now delegate instead of duplicating.
- GC: Centralize BC IC-slot tracing in `gc_mark_bc_ic_slots` helper
  to prevent GLOBAL/PROTOCOL field walks from drifting.

## v0.162.0 — Hot/Cold Bytecode Handler Partition

- VM: Move ~25 infrequently-dispatched opcodes into a `bc_cold_op`
  helper called from the `default:` arm, shrinking the dispatch
  switch from ~50 to ~25 case labels for tighter jump-table codegen.
- Performance: fn-call and let-binding benches improve up to -8%;
  fib(20) stays flat.

## v0.161.0 — Chunked-source Walk + Canonical-prim Stage Recognition

- Sequences: Iterate `MINO_CHUNKED_CONS` value array directly in
  `pipeline_walk`, skipping per-element `seq_iter` calls.
- Sequences: Inline `inc`/`dec`/`odd?`/`even?`/`pos?`/`neg?`/`zero?`
  on tagged-int elements in MAP/FILTER pipeline stages; falls back to
  `apply_callable_argv` on overflow or non-int.
- Performance: reduce+map-inc over 1M range improves -19%.

## v0.160.0 — Builder-pattern Recur Fusion: Investigation, Deferred

- Compiler: Defer compile-time transient rewrite for builder loops;
  mino's `conj!` copies rather than mutates, causing 2.5x slowdown.

## v0.159.0 — Pipeline Fusion For The Seq-consumer Surface

- Sequences: Extend reduce-pipeline fusion to `into` (vector target),
  `mapv`, `filterv`, and `dorun`; lazy-seq cells along the chain are
  not allocated on the fused path.
- Performance: `filterv` and `dorun` pipeline benches improve -93/94%;
  `into` and `mapv` improve -63/67%.

## v0.158.0 — Protocol-keyed Inline Cache

- VM: Add `OP_PROTOCOL_CALL_CACHED` / `OP_PROTOCOL_TAILCALL_CACHED`
  opcodes that cache dispatch by atom-map pointer + type discriminator,
  skipping the `protocol-dispatch` trampoline on hits.
- Compiler: Recognize `defprotocol`-emitted dispatcher fn shape at
  compile time and resolve the dispatch atom statically.
- GC: Add `kind` discriminator to `mino_bc_ic_slot_t`; trace PROTOCOL
  slot fields (`atom`, `cached_map`, `cached_type`) through GC.
- Performance: Monomorphic protocol-call bench improves -57% (~2.4x).

## v0.157.1 — Per-opcode Dispatch Counter Build Flag

- Build: Add `MINO_BC_OP_COUNTS=1` flag that wires per-opcode dispatch
  counters into `vm.c` and dumps sorted totals to stderr at exit;
  zero cost in production builds.

## v0.157.0 — Transducer Fusion For Reduce Pipelines

- Sequences: `prim_reduce` detects map/filter/take LAZY chains and
  walks the bottom source in a single fused loop without allocating
  intermediate lazy-seq cells; up to 8 stages per chain.
- Sequences: Add argv-ABI variants for `zero?`/`pos?`/`neg?`/`odd?`/
  `even?` so filter stages hit the cons-free fast path.
- Performance: `pipeline-sum` bench improves -77%; allocations drop
  -86%.

## v0.156.0 — Generic Get And Dissoc Fast Lanes

- VM: Extend `OP_GET_KW_MAP` map branch to accept any hashable key,
  not just keywords.
- VM: Add `OP_DISSOC` opcode for arity-2 `(dissoc m k)` with direct
  `mino_map_dissoc1` call; misses fall through to `prim_dissoc`.
- Performance: `get-str-map` bench improves -81%; `dissoc-map` -21%.

## v0.155.0 — Inline-Cached Call Sites

- VM: Add `OP_CALL_CACHED` opcode that fuses global-symbol resolution
  and dispatch into one inline-cached step, dropping the intermediate
  fn-reg from the register window.
- Performance: `fib-30` bench improves -13%; `loop-recur-1M` -9%.

## v0.154.0 — Record Fast Path And Keyword-As-Fn Inlining

- VM: Add record fixed-slot read to `OP_GET_KW_MAP`; field miss falls
  through to `prim_get` for ext-map lookup.
- Compiler: Rewrite literal `(:kw coll)` keyword-as-fn invocation to
  `OP_GET_KW_MAP` at emit time.
- Performance: `get-kw-record` -93%, `kw-fn-record` -84%,
  `kw-fn-map` -77%.

## v0.153.0 — Small-Prim Inlining For Vectors

- VM: Add `OP_FIRST_VEC`, `OP_COUNT_VEC`, `OP_EMPTY_VEC` opcodes for
  direct field reads on `MINO_VECTOR`; non-vector types fall through
  to canonical prims.
- Performance: `count-vec3` -94%, `first-vec` -93%.

## v0.152.0 — Write-Side Fast Lanes In The Bytecode VM

- VM: Add `OP_CONJ_VEC` opcode for `(conj v x)` on vectors with
  direct `vec_conj1` call; misses fall through to `prim_conj`.
- VM: Add `OP_ASSOC` opcode for arity-3 `(assoc coll k v)` dispatching
  to `vec_assoc1` or `mino_map_assoc1`; misses fall through to
  `prim_assoc`.
- Performance: `conj-vec` -34%, `assoc-vec` -56%.

## v0.151.1 — Embedding API Hardening

- API: Fix NULL-input crash in `mino_eval_string` / `mino_read`; both
  now validate args and return documented EOF/error shapes.
- API: Fix `mino_iter_next` skipping all entries on sorted-map and
  sorted-set due to wrong struct-field reads; now does in-order RB
  traversal.
- API: Fix `mino_eval_ex` / `mino_eval_string_ex` / `mino_load_file_ex`
  delivering raw thrown payload instead of re-stringified diagnostic.
- API: Fix `mino_to_int` accepting `MINO_BIGINT` values that fit in
  `long long`, closing the int-family round-trip.

## v0.151.0 — Embedding API Revamp And Stabilization

- API: Add `mino_typeof`, full predicate grid, symmetric extractors,
  structured error access (`mino_error_kind`/`_code`/`_clear`),
  `_ex` eval-family, collection builders, unified iterator
  (`mino_iter_init`/`_next`/`_done`), `mino_print_to_buf`,
  `mino_agent_deref`.
- API: Add `mino_install(S, env, caps)` data-driven capability install
  with named presets (`mino_install_minimal`, `_sandbox`, `_all`);
  22 per-capability entry points removed from public header.
- API: Rename `mino_new` to `mino_env_new_default`.
- API: Opaque `struct mino_val` body; tag macros, struct reach-in, and
  test helpers move to private `src/mino_internal.h`.
- API: Mark GC tuning, thread pool, and allocation-profiler sections
  explicitly unstable.
- Interop: Route host-sugar special forms through direct `clojure.core`
  lookup so `host/new`, `host/call`, etc. resolve from any namespace.
- Embedding: `mino_int` auto-promotes to `MINO_BIGINT` when
  `MINO_CAP_BIGNUM` is installed and value exceeds 61-bit tag range.

## v0.150.0 — Realloc Safety, Checked-Size Arithmetic, And Embed-Test Tagging Fixes

- Security: Fix nine `realloc`-overwrite leaks in `src/prim/string.c`
  using canonical temp-pointer pattern.
- Security: Add `checked_add_sz`/`checked_mul_sz`/`checked_double_sz`
  helpers; guard growth arithmetic in `env`, `state`, `module`,
  `eval/read`, and `prim/io`.
- Security: Guard anonymous-fn `#(...)` item-array sizing in reader
  against integer overflow via `checked_mul_sz`.
- Security: Fix `prim_sh` buffer-growth and stdout-read paths leaking
  on `realloc` failure.
- GC: `gc_pin` asserts on overflow in debug builds instead of silently
  dropping slots past 64.
- Embedding: Fix `embed_stm_test` crash reading inline-tagged int as
  boxed pointer; all six unsafe sites now use `mino_to_int`.
- Build: Convert `mino_safepoint_poll` from macro to `static inline`.
- Stdlib: Replace `strcpy` with `memcpy` in `add-load-path!`.

## v0.149.1 — Hash Contract, Sorted-Collection Counts, Error-Path Metadata, And OOM Cleanup

- Collections: Fix `hash` on sequential collections (`vector`, `cons`,
  `chunked-cons`, `empty-list`) to produce equal hashes for equal
  sequences via unified `hash_sequential` helper.
- Collections: Fix `hash` on `sorted-map`/`sorted-set` to use
  content-based hashing instead of pointer identity.
- Collections: Fix `dissoc`/`disj` of an absent key blindly
  decrementing sorted-collection count; now checks containment first.
- GC: Fix BC clause `params_vec` not traced by GC, causing use-after-
  free on destructured fn params after major collection.
- VM: Tighten `eval_apply_regular_call` to always bail on NULL from
  `eval_args`, synthesizing a diagnostic if none is latched.
- Errors: `name`/`namespace` prims now include the offending value in
  diagnostics; `extend-type` validates method shape at call site.
- Reader: Fix `:/` rejected as malformed keyword; slash is now a valid
  keyword name.
- Errors: Constructor-sugar `ClassName.` lookup now emits a dedicated
  diagnostic instead of `unbound symbol`.
- Errors: Arity-mismatch diagnostics now name the callee and expected
  count in both tree-walker and BC VM.
- Async: Fix `mino_state_free` hanging when workers block on
  undelivered promises; quiesce cancels all pending futures/promises
  before joining.
- Vars: Fix `(declare x) x` returning nil; now throws `Var is unbound`
  matching JVM Clojure.
- STM: `ref` now accepts `:validator`, `:meta`, `:min-history`,
  `:max-history` option keywords matching JVM Clojure signature.
- Vars: `binding` now rejects non-dynamic vars with a clear diagnostic.
- Namespaces: Fix `clojure.core/refer` direct call binding vars into
  env without auto-deref, causing `not a function (got var)` errors.
- Reader: Fix tagged literal with missing body surfacing as `unbound
  symbol: form`; now emits `tagged literal #foo: missing form`.
- Errors: Fix uncaught throws losing the original message in BC VM,
  tree-walker, and `future` worker path.
- Reader: Fix set and namespaced-map readers not skipping
  reader-conditional no-match forms.
- Reader: Fix wrap-one macros (`@`, `'`, `` ` ``, etc.) not naming
  empty reader-conditionals in their diagnostic.
- Macros: `defrecord`/`deftype` now reject non-vector fields at call
  site with a clear diagnostic.
- Build: Fix const-qualifier mismatch in arity-mismatch diagnostic
  helpers breaking `-Werror` bootstrap build.

## v0.149.0 — clojure-test-suite Conformance Pass: 220 / 220 Files, 5340 / 5340 Assertions

- Numerics: `(abs Long/MIN_VALUE)` returns `Long/MIN_VALUE` via
  `unchecked-negate`, matching JVM `Math/abs` 2's-complement behavior.
- Macros: `derive` validates tag, parent, and hierarchy shape at call
  site; rejects nil/invalid parents with named diagnostics.
- Sequences: `some`, `every?`, `zipmap` now call `prim_seq` on
  collection args and throw on non-seqable inputs instead of silently
  returning empty/true/{}.
- Stdlib: Remove JVM-class-name bridges
  (`clojure.lang.IPending`/`BigInt`/`MapEntry`) from `core.clj`.
- Stdlib: Add `thread-sleep` C primitive backed by `nanosleep` with
  `EINTR` restart.

## v0.148.0 — Move More Of clojure.core Into C: distinct?, merge-with, complement, comp, partial, juxt

- Stdlib: Move `distinct?`, `merge-with`, `complement`, `comp`,
  `partial`, `juxt` from `core.clj` to C primitives in
  `src/prim/sequences.c`.

## v0.147.0 — Move Seq Predicates And Map Builders Into C

- Stdlib: Move `every?`, `some`, `not-any?`, `not-every?`, `zipmap`,
  `frequencies`, `group-by` from `core.clj` to C primitives, reducing
  install-time eval cost.

## v0.146.0 — Capability-Gated Install API For Embedders

- Embedding: Add `mino_install_minimal` (sub-millisecond, no core.clj)
  and per-capability `mino_install_*` entry points; `mino_install_core`
  preserved as back-compat alias.
- API: Add `MINO_CAP_*` bitmask, `mino_capability_installed`,
  `mino_capabilities` query surface.
- Errors: Add `MNS002` capability-disabled diagnostic with structured
  `:mino/data` payload naming the disabled capability and enable path.
- REPL: Add `:capabilities` / `:caps` command and banner note for
  partial-install states.

## v0.145.1 — Task Runner Fix: Pre-Resolve Tasks Outside The BC Doseq Body

- CLI: Fix `mino task <name>` corrupting const-pool slots when a
  namespace is loaded for the first time inside a bc-compiled doseq
  body; task fns are now pre-resolved via a `mapv` pass before any
  task runs.

## v0.145.0 — Reduce Fast Paths

- Collections: `reduce` over map/set takes a direct-walk path, yielding
  `MINO_MAP_ENTRY` (single alloc) instead of a per-entry vector; 1.6× faster.
- Collections: `reduce` over vector uses a recursive trie walker instead of
  per-element `vec_nth`; 1.9× faster on 1M-element vectors.
- VM: `reduce` 2-arg form now passes the original collection to the C
  primitive instead of seq-decomposing it first; 180× faster on lazy ranges.
- Compiler: let-binding fold-through collapses `(let [x (+ 1 2)] (* x x))`
  to a single constant load at compile time.
- Compiler: dead let-bindings whose RHS is side-effect-free are eliminated
  at compile time.
- Collections: `assoc` and set `conj` short-circuit and return the input
  unchanged when the operation has no observable effect; 2× faster for noop
  assoc on HAMT-100.
- Strings: `(count s)` now returns codepoint count instead of byte length,
  matching the indexing model of `subs`/`nth`/`char-at`.

## v0.144.6 — Correctness Fixes: BC Closure Capture, Catch Unwind, And Loud Limit Errors

- VM: `OP_GETGLOBAL_CACHED` no longer writes free-var lookups into the
  shared bc slot, fixing incorrect values across closures built from the
  same fn template.
- VM: dyn frames are now unwound to the pre-try anchor on catch landing,
  fixing leaked bindings when `throw` fires inside a `binding` form.
- VM: multi-arity bc fn called with no matching clause now raises MAR002
  instead of returning NULL silently.
- VM: `OP_PUSHCATCH` hitting `MAX_TRY_DEPTH` now raises MLM002 instead of
  returning NULL silently.

## v0.144.5 — Correctness Fix: BC Re-Throw Through Nested Try / Finally

- VM: outer `try`/`finally` catch handler now sees the re-thrown exception
  value instead of the inner-caught one; fix reads `try_depth_at_push` from
  heap-backed `bc_catch_stack` instead of a gcc-reused stack local.

## v0.144.4 — Build Fix: Suppress gcc's `-Wclobbered` Instead Of `volatile`

- Build: replace `volatile` annotations on `mino_bc_run` locals with
  `-Wno-clobbered`; reverts the miscompile introduced in v0.144.2.

## v0.144.3 — Build Fix: Drop Local In `mino_current_ctx`

- Build: remove unused local `t` from `mino_current_ctx` inline to silence
  gcc `-Werror=clobbered` false positive.

## v0.144.2 — Build Fix: Mark setjmp-Adjacent Locals `volatile`

- Build: mark `env` and `rest` `volatile` in `mino_bc_run` to fix Linux gcc
  `-Werror=clobbered` failure on the CI build.

## v0.144.1 — GC Fix: Compiled Bytecode Children Traced Via Remset

- GC: introduce `GC_T_BC` tag so `gc_trace_children` pushes the bc record's
  `code`, `consts`, `clauses`, and `ic_slots` children; fixes
  heap-use-after-free when the bc record ages into old generation.
- Tests: add 25 regression tests covering small-persistent-map, `:strs`/
  `:syms` destructure, and hash-cache correctness (1596 tests / 7427 assertions).

## v0.144.0 — Cached Hash On Immutable Collection Headers

- Collections: vector, map, and set headers gain a lazily-populated
  `cached_hash` field; repeated `hash` calls return the memo.
- Collections: `mino_eq` short-circuits structural comparison when both
  operands already have differing cached hashes.

## v0.143.0 — Tree-Walker `:strs` / `:syms` Destructure And Forcing Map Equality

- Eval: tree-walker `bind_map_destructure` now handles `:strs` and `:syms`
  patterns, matching the BC-side expansion.
- Eval: `mino_eq_force` now walks into map values and set elements when
  forcing lazy sequences, so `=` on maps with lazy-seq values works correctly.

## v0.142.0 — Flatmap For Small Persistent Maps

- Collections: maps with ≤ 8 entries use a flatmap representation (linear
  scan, no per-entry hash or HAMT node allocation); promotes to HAMT lazily
  on the first `assoc` past the threshold, never demotes.

## v0.141.2 — Fast-Lane Emission Honours User Shadows

- Compiler: speculative `OP_*_II`/`OP_*_IK`/unary fast-lane opcodes and the
  fused counted-loop detector now gate on canonical-prim identity; user
  shadows of `+`, `dec`, etc. correctly fall through to `OP_CALL`.

## v0.141.1 — Fused Counted-Loop: Proper Diagnostics on Miss

- VM: `OP_LOOP_INT_DEC` miss path now raises the correct diagnostic via
  `prim_zero_p` / `prim_dec` instead of returning NULL silently.

## v0.141.0 — Measurement Gate

- Performance: bytecode VM reaches 10.5× faster than v0.128.0 on
  tight-loop-10M; beats Lua 5.5 by 4.5× on that benchmark.

## v0.140.0 — Direct Compile for when / and / or

- Compiler: `when`, `and`, and `or` now compile to direct `OP_JMPIFNOT`
  short-circuit chains instead of falling back to the tree-walker.

## v0.139.0 — Collection Fast Lanes: nth-vec and get-kw-map

- VM: `OP_NTH_VEC` and `OP_GET_KW_MAP` specialised opcodes for
  `(nth vector int-index)` and `(get map :keyword)` hot paths; misses fall
  back to `prim_nth`/`prim_get`.

## v0.138.0 — Broader Int Fast Lane Inside Reduce

- VM: `prim_reduce` inner loop extends int+int shortcut to `-`, `bit-and`,
  `bit-or`, and `bit-xor`, skipping per-step cons allocation for those ops.

## v0.137.0 — Fused Counted-Loop Opcodes

- VM: `OP_LOOP_INT_DEC` and `OP_LOOP_INT_DEC_INC` fuse common counted-loop
  shapes into a single decode+step+jump; tight-loop-10M drops ~9× to ~15 ms.

## v0.136.0 — Drop Redundant Register Zeroing on Push

- VM: `bc_push_window` no longer zeroes register slots on entry; slots are
  already cleared by `bc_pop_window` and growth-path allocation. ~7% win on
  fib-30.

## v0.135.0 — Inline Cache for Global Symbol Resolution

- VM: `OP_GETGLOBAL_CACHED` caches resolved global values per-fn with an
  `ic_gen` epoch check; skips full var-resolution cascade on cache hit.
  fib-30 ~2×, call-noop-1M ~2× faster.

## v0.134.0 — argv ABI for BC Calls

- VM: `OP_CALL` passes register slice directly as `argv`/`argc` to
  `apply_callable_argv`, eliminating N cons-cell allocations per call.
  call-noop-1M ~3×, fib-30 ~1.9× faster.

## v0.133.0 — N-Arity Arithmetic Expansion

- Compiler: variadic `+`, `-`, `*` calls expand to left-associative binary
  chains at compile time, routing each step through the `OP_*_II` fast lane
  with correct overflow semantics. arith-chain-1M ~17× faster.

## v0.132.0 — Literal-Arg Pure-Fn Fold

- Compiler: calls to pure core prims with all-literal arguments are folded
  to a constant at compile time; recompiles when a redef invalidates
  `compile_ic_gen`.

## v0.131.0 — Immediate-Operand Fast Lanes

- VM: `OP_ADD_IK`, `OP_SUB_IK`, `OP_LT_IK`, `OP_LE_IK`, `OP_EQ_IK` encode
  a signed 8-bit literal in the instruction operand, saving one opcode
  dispatch and one register per occurrence.

## v0.130.0 — Extended Int Fast-Lane Breadth

- VM: dedicated `OP_*_II`/`OP_*_I` opcodes for `mod`, `quot`, `rem`,
  `bit-and`, `bit-or`, `bit-xor`, `bit-shift-left`, `bit-shift-right`,
  `unsigned-bit-shift-right`, `pos?`, `neg?`, `even?`, `odd?`, `bit-not`.
  cond-branch-1M drops from 1340 ms to 31 ms (~43×).

## v0.129.0 — Drop Arith Hot-Path Instrumentation

- VM: `bc_int_make_count`/`bc_int_alloc_avoided` counter increments moved
  behind `MINO_BC_PROFILE_COUNTS` compile-time flag; always-on writes
  removed from the tagged-int hot path. ~10% win on tight-loop microbench.

## v0.128.0 — Destructure Bytecode Compilation

- Compiler: `let`/`loop` bindings and fn-params with destructuring patterns
  now compile to bytecode via `prim_destructure` expansion; only
  `(loop [[a b] ...])` still falls back to the tree-walker.

## v0.127.0 — Binding (Dynamic Vars) Bytecode Compilation

- VM: `OP_PUSHDYN`/`OP_POPDYN` handlers manage dyn-frame lifecycle; bc
  compiler emits these for `binding` forms; `mino_bc_run` unwinds leaked
  frames at `bc_done`.

## v0.126.0 — Try/Catch/Throw Bytecode Compilation

- VM: `OP_PUSHCATCH`/`OP_POPCATCH`/`OP_THROW` handle try/catch/finally/throw
  on the BC path; compiler covers try-no-handler, try+catch, and
  try+finally shapes; `bc_done` rolls back stale setjmp landing pads.

## v0.125.0 — Arith Fast-Lane Direct Tag Extraction

- VM: `binop_int_fast` and `unop_int_fast` extract tagged ints inline via
  `MINO_IS_INT`/`MINO_INT_VAL` instead of the helper chain; `tag_or_box_int`
  encodes results inline for the 61-bit range.

## v0.124.0 — GC IC-Marking Audit and Stress Gate

- GC: audit confirms all IC-marking sites safely route tagged values through
  `gc_mark_interior`/`gc_mark_child_push`; one clarifying comment added, no
  code change.

## v0.123.0 — Inline Tags for BOOL, NIL, CHAR

- VM: `mino_true`, `mino_false`, `mino_nil`, and `mino_char` return
  inline-encoded values; per-state singleton fields become dead storage.
- API: `MINO_IS_BOOL`, `MINO_IS_NIL`, `MINO_IS_CHAR`, `MINO_MAKE_BOOL`,
  `MINO_MAKE_NIL`, `MINO_MAKE_CHAR`, `MINO_BOOL_VAL`, `MINO_CHAR_VAL`
  macros added; `mino_type_of` dispatches on all 5 tag values.

## v0.122.0 — Constructor Flip: Inline-Tagged Integers

- VM: `mino_int(S, n)` returns an inline-tagged value for the 61-bit signed
  range, eliminating heap allocation for almost all integer construction;
  small-int cache removed.

## v0.121.0 — Generic-Deref Audit for Tagged-Int Safety

- VM: remaining `X->type` sites used as function arguments, in cross-type
  comparisons, and in defensive checks migrated to `mino_type_of(X)` to be
  safe when `X` is inline-tagged.

## v0.120.0 — Tag-Safe Type Discrimination at Call Sites

- VM: all `X->type == MINO_Y`, `switch(X->type)`, and `X->as.i` reads
  migrated to `mino_type_of(X)` / `mino_val_int_get(X)` across 48 files.
- GC: `gc_write_barrier` early-returns on non-zero tag bits in preparation
  for the constructor flip.

## v0.119.0 — Pointer-Tagged Value Representation: Infrastructure

- API: `mino_val_int_p`/`mino_val_int_get` unified accessors handle both
  inline-tagged and boxed integers.
- GC: `gc_mark_child_push` and `gc_mark_interior_push` skip pointers with
  non-zero low three bits; alloc paths assert 8-byte alignment.

## v0.118.0 — Pointer-Tagged Value Representation: Layout Contract

- API: `mino.h` gains the 3-bit tag scheme with macros for PTR, INT, BOOL,
  NIL, CHAR tags and encode/decode helpers; 64-bit hosts only.

## v0.117.0 — Bytecode Constant-If Fold And Tail-MOVE Peephole

- Compiler: `compile_if` folds constant conditions at compile time, emitting
  only the chosen branch.
- Compiler: tail-MOVE peephole eliminates a redundant `OP_MOVE` when the
  last instruction writes a binding that is immediately returned. ~2–4% win
  across eval-floor benchmarks.

## v0.116.0 — Bytecode Operand-Inplace For Fast Lanes

- Compiler: `compile_operand_inplace` reuses a local's existing register for
  binop/unary fast-lane operands, eliminating `OP_MOVE` temps; fns like
  `(fn [x j] (+ x j))` drop from 5 to 3 registers. ~5–8% win across
  eval-floor benchmarks.

## v0.115.0 — Bytecode Tail-Position Propagation And Unary Int Fast Lanes

- Compiler: tail position propagates through `if`, `do`, `let`, and `loop`;
  tail calls emit `OP_TAILCALL` and reuse the trampoline.
- Compiler: fix bias-encoded jump-offset bounds in `patch_jmp` and recur
  back-jump; previously every conditional branch declined bytecode
  compilation silently.
- VM: `OP_INC_I`, `OP_DEC_I`, `OP_ZERO_INT_P` unary int fast-lane opcodes
  added. tight-loop-10M drops from ~4.7s to ~1.2s.

## v0.114.0 — Bytecode Speculative Int+Int Fast Lanes

- VM: `OP_ADD_II`, `OP_SUB_II`, `OP_MUL_II`, `OP_LT_II`, `OP_LE_II`,
  `OP_GT_II`, `OP_GE_II`, `OP_EQ_II` specialised opcodes for binary
  arith/compare with int+int operands; misses fall back to the prim.

## v0.113.0 — Bytecode Multi-Arity Dispatch

- Compiler: multi-arity fns compile each clause into a `mino_bc_clause_t`
  entry; runtime dispatches by arity at fn entry with a two-pass scan.

## v0.112.0 — Bytecode Loop/Recur and Lazy-Seq

- Compiler: `loop`/`recur` compiles to a binding scope with a recur target
  at the loop entry pc; `recur` moves args into loop registers and
  back-jumps.
- Compiler: `lazy-seq` compiles to `OP_MAKE_LAZY`, capturing the form list
  and live lexical env in the constant pool.

## v0.111.0 — Bytecode &-Rest and Constant Vectors

- Compiler: single-arity fns with `& rest` now bc-compile; overflow args
  are collected into a list in the register after fixed params.
- Compiler: vector literals with all self-evaluating elements are stashed in
  the constant pool and loaded with a single `OP_LOAD_K`.

## v0.110.0 — Bytecode Closures

- Compiler: inner `fn` literals compile to `OP_CLOSURE`; `OP_PUSH_ENV`,
  `OP_POP_ENV`, `OP_ENV_BIND` manage the lexical-env chain for closures that
  capture let-bindings.

## v0.109.0 — Bytecode Macro-Aware Emit

- Compiler: macro probe at emit time walks lexical env then defining-ns env
  with alias resolution; unblocks bc dispatch for ordinary calls that were
  previously declined due to the lexical-only check.

## v0.108.0 — Specialization Opcode Reservation

- VM: eleven specialization opcode enum entries reserved (`OP_GETGLOBAL_CACHED`,
  `OP_CALL_CACHED`, eight per-op int+int variants, `OP_GET_KW_MAP`,
  `OP_NTH_VEC`) to stabilise encoding before handlers land.

## v0.107.0 — Bytecode Require Mode

- CLI: `MINO_BC_REQUIRE=1` env var turns silent tree-walker fallback into a
  hard abort, surfacing compiler gaps during development.

## v0.106.0 — Bytecode Tail-Call Trampoline

- VM: `OP_TAILCALL` returns a `MINO_TAIL_CALL` sentinel; `apply_callable_argv`
  loops on the sentinel, switching active fn and argv without growing the C
  stack.

## v0.105.0 — Bytecode VM Foundation

- VM: register-based bytecode interpreter added behind the tree-walker; lazy
  per-fn compilation caches compiled programs on the fn; falls back to
  tree-walker on unsupported forms.
- VM: 11 core opcodes implemented: `OP_MOVE`, `OP_LOAD_K`,
  `OP_GETGLOBAL`, `OP_SETGLOBAL`, `OP_JMP`, `OP_JMPIFNOT`, `OP_CALL`,
  `OP_TAILCALL`, `OP_RETURN`, `OP_CLOSURE`, `OP_BINOP_INT`.
- VM: `MINO_FN.bc` field, register stack on `mino_state_t`, and GC root scan
  of live register slots added.
- Compiler: covers literals, local/global refs, `if`, `do`,
  plain-symbol `let`, `quote`, and top-level `def`.

## v0.104.0 — Eval-Floor Performance

- Performance: tight integer loop/recur cut from 941 ms to 375 ms; `(reduce + (range 1M))` from ~870 ms to ~514 ms (~24%
  avg reduction across 15 benches).
- Toolchain: add compile-time allocation profiler (`-DMINO_ALLOC_PROFILE=1`)
  with `alloc-profile-enabled?`, `alloc-profile-reset!`,
  `alloc-profile-dump!` prims.
- Vars: open-addressing hash indices for var registry and interned-string
  table replace linear scan; `var_intern`/`var_find` hit pointer-pair
  equality on hot path.
- Compiler: build-time pre-parsed core.clj not shipped; cold-start parse
  share too small to justify maintenance cost.
- Sequences: `prim_reduce` fast path for range source + int+int accumulator
  skips chunk materialization; `(reduce + (range 1M))` ~514 ms.
- VM: binary call fast lane for `+ - * = < <= > >=` when both args are
  `MINO_INT`; uses overflow-safe builtins, skips cons spine and
  `tower_reduce`.
- VM: monomorphic inline call cache keyed on form pointer + head-symbol
  tag; `ic_gen` invalidation covers `var_set_root`, `env_unbind`,
  `var_unintern`.
- Vars: `ns-unmap` now correctly bumps `ic_gen`, fixing stale cache
  entries after `env_unbind`/`var_unintern`.
- Vars: per-var `version` counter incremented by `var_set_root`; used
  as inline-cache invalidation signal.
- VM: `MINO_FN.shape` cache avoids destructure dispatch on every call
  for simple-param fns; routes through `bind_simple_params`.
- VM: `mino_prim_fn2` argv ABI for hot prims; `inc dec count first rest
  cons`, all 22 type predicates, and variadic arith/compare prims
  skip per-call cons spine.
- VM: multi-arity recur reuses existing env slots when dispatched clause
  is unchanged; avoids one allocation per same-clause recur iteration.
- VM: symbol `hash` field cached on interned val; `env_find_here_hashed`
  skips FNV rehash per probed frame.
- VM: `mino_is_truthy_inline` used in `eval_if`, `eval_when`, `eval_and`,
  `eval_or`, `prim_not`, and lazy predicate loops.
- VM: `mino_env_get_sym` carries cached symbol length; avoids `strlen`
  per parent frame on lexical walk.
- GC: in-process run-over-run drift on `(reduce + (range 1M))` is GC
  settling over ~89 MB bootstrapped old-gen; no leak; use median-of-5
  after 3-run warmup.

## v0.103.0 — Worker-List Lock Split

- Async: new per-state `worker_list_lock` (inner to `state_lock`) guards
  worker linked-list and `thread_count`; eliminates contention between
  tight `dosync` loops and future/agent worker entry/exit.
- Agents: agent worker entry-link, exit-detach, and `agent_worker_ensure`
  gate moved off `state_lock` onto `worker_list_lock`.
- GC: `gc_mark_thread_state` holds `worker_list_lock` across
  `worker_ctxs_head` walks instead of relying on `state_lock`.
- Tests: `future-thread-count-not-stuck-under-tight-loop` regression
  test in `tests/host_threads_test.clj`.

## v0.102.1 — Adversarial-test pass: doc accuracy + qa-arch hygiene

- Docs: fix misleading `thread_limit` wording in `agent_worker_ensure`
  and all mirrored docs; embedder thread does not count against limit.
- Docs: correct Coming-from-Clojure: `agent` works at `thread_limit=0`;
  `send`/`send-off` throw MTH001 when pool worker can't spawn.
- Docs: update Compatibility Matrix `send-via` row to reflect POOLED/SOLO
  pool split from v0.102.0.
- Docs: update STM page intro to reflect both agent pools.
- Tests: add adversarial probes for agent surfaces.
- Build: 11 `abort()` sites now carry rationale comments; `task qa-arch`
  now passes; fix `check-large-fn` calling unqualified `includes?`.

## v0.102.0 — Agents finish MVP: async dispatch + pool split + C-API

- Agents: `send`/`send-off` enqueue onto per-state async run-queue;
  worker thread lazy-spawned on first send; `await`/`await-for` block
  on `agent_cv` until `in_flight` reaches zero.
- Agents: POOLED/SOLO split — `send` uses POOLED pool, `send-off` uses
  SOLO pool; independent queues prevent long send-off from stalling sends.
- Agents: `shutdown-agents` joins the worker thread; `restart-agent`
  accepts `:clear-actions true` to splice queued actions for failed agent.
- STM: `dosync` post-commit drain enqueues pending sends onto POOLED worker
  instead of running synchronously on embedder thread.
- API: `mino_send`, `mino_send_off`, `mino_await`, `mino_await_for`,
  `mino_agent_error`, `mino_restart_agent` C-API entries; cross-state
  misuse throws MST007.
- VM: `mino_pcall` now restores `lock_depth` after longjmp; prevents
  deadlock when agent/future workers catch throws via pcall.
- GC: GC root walk covers both pools' run-queues so queued actions stay
  live until popped.

## v0.101.1 — STM and agent hardening pass

- STM: commit restructured as two-pass (stage all new values + validate,
  then apply); late validator throw no longer leaves earlier refs
  partially committed.
- STM: commute log replay routes through `mino_pcall`; throwing commute
  fn no longer leaks global commit lock.
- STM: `tx_state_t.in_commit` flag rejects re-entrant `alter`/`ref-set`/
  `commute` from validator or commute-replay callbacks.
- STM: `send`/`send-off` inside `dosync` queue and dispatch only on
  successful commit; cleared on retry/abort; `release-pending-sends`
  counts and clears the queue.
- STM: `alter`/`ref-set` after `commute` on same ref now throws "Can't
  set after commute" matching JVM canon.
- STM: `set-validator!` no longer validates current value at install time,
  matching JVM Clojure post-A.1 semantics.
- Agents: cross-state defense via `owning_state`; all agent prims throw
  MST007 on mismatch; ref check moved into shared cores.
- Agents: constructor accepts `:validator`, `:error-handler`, `:error-mode`,
  `:meta`; unknown options throw.
- Agents: `error-handler` invoked on action and validator failure;
  `restart-agent` runs validator on new state.
- Agents: `*agent*` bound to dispatching agent across action/validator/watch
  bodies.
- Agents: `shutdown-agents` flips `agents_shutdown`; subsequent sends throw
  MST008; `send-via` throws MST008 (not silently aliased).
- Agents: watch dispatch uses `mino_pcall`; first thrown exception captured,
  remaining watches still fire, then exception re-thrown.
- Atoms: `with-meta`/`vary-meta` on atom/agent now throw MTY001; `alter-meta!`
  adds missing `gc_write_barrier`.
- Agents: `#agent[ID VAL]` print form with monotonic `agent_id` matching
  `#ref[ID VAL]`.
- STM: pending-sends drain checks failed-`:fail` state before dispatching
  action; pending sends run before watch dispatch in `tx_outer_run`.
- STM: removed dead `tx_state_t.retry_signal` field.
- Vars: `add-watch`/`set-validator!` validate callability of fn argument
  at install time across atom/ref/var/agent; `set-error-handler!` and
  `set-error-mode!` likewise reject invalid arguments immediately.

## v0.101.0

- STM: full STM surface lands — `ref`, `ref?`, `dosync`, `alter`,
  `ref-set`, `commute`, `ensure`, `io!`, `in-transaction?`; single-version
  optimistic locking with global commit lock.
- STM: `ref-min-history`/`ref-max-history`/`ref-history-count` are no-op
  stubs (0/10/0); no MVCC history.
- API: `mino_tx_ref`, `mino_tx_ref_deref`, `mino_tx_ref_set`,
  `mino_tx_alter_c`, `mino_tx_commute_c`, `mino_tx_ensure`, `mino_tx_run`
  C-API entries share cores with Clojure-side prims.
- STM: ref `owning_state` back-pointer; all public C entries throw MST007
  on cross-state ref use.
- Vars: `add-watch`/`remove-watch`/`set-validator!`/`get-validator` extended
  to accept vars; `var_set_root` runs validator and dispatches watches.
- VM: `mino_pcall` catch arm no longer publishes to `last_error`; prevents
  longjmp hijack of outer try frames (was deadlocking STM commit lock).
- API: `mino_pcall` gains `out_ex` parameter exposing raw thrown value;
  agent `agent_try_call` workaround removed.
- STM: validator throw now propagates original exception; falsy-reject
  raises MCT001 as before.
- Agents: MVP agents land — `agent`, `agent?`, `send`, `send-off`, `await`,
  `await-for`, `agent-error`, `restart-agent`, error-mode/handler prims;
  sends run synchronously on calling thread in MVP.
- Sequences: fix `(= (filter pred []) (filter pred []))` returning false;
  both-LAZY equality now routes through `eq_seq_like`.
- Tests: `tests/stm_test.clj`, `tests/stm_concurrent_test.clj`,
  `tests/embed_stm_test.c`, `tests/agent_test.clj` added.
- Build: `mino_install_stm` wired into `mino_install_all`; embedders still
  opt in via explicit call.

## v0.100.34

- Interop: `aset` mutates `MINO_HOST_ARRAY` vals in place.
- Sequences: `seq_iter_init`/`seq_iter_done`/`seq_iter_val` handle
  `MINO_HOST_ARRAY` and `MINO_MAP_ENTRY`; `into`/`mapv` iterate them
  uniformly.
- Collections: `vec` rejects bad shapes (numbers, booleans, etc.) up
  front with a typed error.

## v0.100.33

- Numerics: add `MINO_FLOAT32` tag; `float?` matches both tiers,
  `double?` matches only `MINO_FLOAT`; `(type x)` returns `:float32`
  for 32-bit.
- Numerics: arithmetic always promotes `MINO_FLOAT32` to `MINO_FLOAT`
  matching JVM where Float arithmetic yields Double.
- Numerics: `(= 5.0 (float 5))` is now false; `double?`/`float?` split
  matches JVM contract.

## v0.100.32

- Collections: add distinct `MINO_MAP_ENTRY` type; `key`/`val` accept
  only MAP_ENTRY and throw on plain 2-vector.
- Collections: `seq` of map/sorted-map/record produces MAP_ENTRY values;
  equality with `[k v]` is element-wise.
- Interop: `clojure.lang.MapEntry/create` constructs a MAP_ENTRY.

## v0.100.31

- Numerics: `(float x)` range-checks against `[-FLT_MAX, FLT_MAX]` and
  narrows precision via `(double)(float)d`; overflow throws MTY001.

## v0.100.30

- Reader: out-of-long-range integer literals promoted to bigint via
  `mino_bigint_from_string_n` instead of silently saturating.

## v0.100.29

- Interop: add `MINO_HOST_ARRAY` value type; `object-array`/`int-array`/
  `long-array` etc. are now C primitives that zero-fill or copy from
  collections.
- Interop: `coll?`/`vector?`/`counted?`/`associative?`/`sequential?`/
  `reversible?` all return false on host arrays; equality is identity.

## v0.100.28

- Eval: fixed-arity fn calls with excess args now throw `eval/arity`
  MAR001; `let`/`loop`/`for`/`doseq` destructuring remains lenient.

## v0.100.27

- Collections: `(cons x y)` result sets `not_list` flag; `list?`/`peek`/
  `pop` reject it, matching JVM `clojure.lang.Cons` semantics.

## v0.100.26

- Numerics: bigdec/ratio division now widens to bigdec via
  `mino_bigdec_div` instead of collapsing to float.

## v0.100.25

- Numerics: unprimed `+`/`-`/`*`/`inc`/`dec` now throw on long overflow;
  primed `+'`/`-'`/`*'`/`inc'`/`dec'` auto-promote.
- Numerics: `prim_short`/`prim_byte` range-check via `narrow_cast` before
  truncation.

## v0.100.24

- Interop: `Foo.` trailing-dot constructor syntax invokes the defrecord
  `->Foo` factory.

## v0.100.23

- Numerics: `(int x)` range-checks against int32; `(long x)` is a new
  C primitive with int64 range check; both throw on out-of-range.

## v0.100.22

- Numerics: add `(short x)` and `(byte x)` with range checks; return
  `MINO_INT` (no narrow-int tier).

## v0.100.21

- Interop: define `clojure.lang.MapEntry` namespace so
  `(clojure.lang.MapEntry/create k v)` works in cross-dialect tests.

## v0.100.20

- Build: fix two `-Werror` regressions on gcc-11 (Ubuntu 22.04):
  guard `-Wdangling-pointer` pragma behind `__GNUC__ >= 12`; replace
  `snprintf` truncation-warning pattern with return-value check.

## v0.100.19

- Async: `(future ...)` now snapshots caller's dynamic bindings and
  reinstalls them on the worker thread via `mino_snapshot_thread_bindings`.

## v0.100.18

- Sequences: `(seq sorted-map/-set)` now returns `MINO_CHUNKED_CONS`
  so `list?` returns false on sorted-collection seqs.

## v0.100.17

- Numerics: `(bigint d)` converts via shortest-decimal round-trip string,
  yielding full integer for large doubles instead of `Long/MIN_VALUE`.
- Numerics: `(rationalize d)` converts via shortest-decimal + BigDecimal;
  `(rationalize 1.1)` is `11/10`.
- Interop: `clojure.lang.BigInt` bridge for `(instance? clojure.lang.BigInt x)`.
- Numerics: `+'`/`-'`/`*'`/`inc'`/`dec'` defined as aliases for unprimed
  forms (unprimed already auto-promoted at this point; superseded by
  v0.100.25).

## v0.100.16

- Sequences: `(repeat true :a)` returns `[:a]`; `(repeat false :a)`
  returns `[]`; non-numeric/non-boolean counts still throw.

## v0.100.15

- Collections: `subvec` coerces any numeric index to long via
  `subvec_to_long` helper; NaN -> 0; non-numeric still throws.

## v0.100.14

- Collections: `(nth nil i)` returns nil; `(nth nil i default)` returns
  default; matches JVM nil-as-empty-seq treatment.

## v0.100.13

- Atoms: watch exceptions now propagate out of `swap!`/`reset!`/
  `compare-and-set!`; previously swallowed silently.

## v0.100.12

- Atoms: `atom` tolerates extra positional args and accepts
  `MINO_SORTED_MAP` as `:meta` value.
- Atoms: validator returning nil now rejects new state (was only `false`).

## v0.100.11

- Async: `(promise)` no longer blocks process exit; quiesce loop skips
  promises with no backing thunk.
- Interop: `(ifn? (promise))` returns true; `clojure.lang.IPending`
  bridge for `instance?` checks.

## v0.100.10

- Eval: `let` uses sequential-binding semantics; each init expression
  sees only prior bindings in the same `let`, fixing closure capture
  and eliminating a segfault under nested `let` shadowing.
- Stdlib: `run!`, `tree-seq`, `interleave`, `shuffle`, `take-last`,
  `trampoline`, `condp`/`case` build helpers converted from
  mutable-env self-reference to named `fn` forms.

## v0.100.9

- Modules: add `add-load-path!` to append runtime search paths; enables
  external test suite cross-file `require` without per-file preloading.

## v0.100.8

- Regex: `MINO_REGEX` is now a distinct first-class value type; equality
  is identity; `#"..."` literal builds it directly; `re-pattern` is a C
  primitive; `re-find`/`re-matches`/`clojure.string/split` accept regex
  or string.

## v0.100.7

- Collections: `MINO_UUID` first-class value type; `#uuid "..."` literal,
  `parse-uuid`, `random-uuid`, `uuid?` all operate on the type; equality
  is byte-wise; `(str u)` and `(pr-str u)` round-trip exactly.

## v0.100.6

- Numerics: `(/ bigdec bigdec)` now exact via `mino_bigdec_div`; throws
  on non-terminating decimal expansion matching JVM `ArithmeticException`.
- Numerics: `(= 1.0M 1.00M)` is now true; hash strips trailing zeros to
  preserve equal-implies-equal-hash.

## v0.100.5

- Collections: `sort` with no comparator throws on incomparable elements
  via `prim_compare`; was silently using type-tag fallback.
- Numerics: `min-key`/`max-key` variadic case uses `(<= kw kv)` NaN-safe
  loop matching JVM behavior.
- Macros: `if-let`/`when-let`/`if-some`/`when-some` validate binding
  vector is exactly two elements at expansion time.

## v0.100.4

- Numerics: `rationalize` accepts BigDecimals; converts `unscaled *
  10^-scale` to ratio or integer.

## v0.100.3

- Collections: `nthnext` — `(nthnext nil _)` returns nil; non-integer `n`
  throws typed error.
- Collections: `rand-nth` — `(rand-nth nil)` returns nil; non-collection
  throws typed error.
- Sequences: `mino_eq_force` routes same-tag chunked-cons through
  `eq_seq_like_force`; fixes `(= (range 1000) (filter (fn [_] true)
  (range 1000)))` returning false.

## v0.100.2

- Collections: transient supports read-only interface (`nth`, `get`, `count`,
  `contains?`, direct invocation) matching persistent contract.

## v0.100.1

- Atoms: `(deref delay)` forces and returns the delay's value.

## v0.100.0

- Reader: drop `:clj` branch from reader conditionals; match only
  `:mino` and `:default`.

## v0.99.4

- CI: upload build-log artifact from `release-build.yml` on failure.

## v0.99.3

- Build: capture `getcwd` return value in `main.c` to satisfy
  `-Werror=unused-result` on glibc.

## v0.99.2

- CI: upload build log as downloadable artifact when bootstrap fails.
- Build: initialise `r_at`/`b_at` to `NULL` in `mqr_ratio` to silence
  gcc-11 `-Wmaybe-uninitialized`.

## v0.99.1

- CI: print gcc version list and surface build log in job summary on
  failure.
- Reader: drop dead `buf_capacity` variable; move `*err = 0` reset to
  top of `try_parse_numeric`.

## v0.99.0

- Strings: `(get string i)` returns `\char` codepoint, not substring.
- Numerics: `numerator`/`denominator` require Ratio; integer input throws.
- Namespaces: `intern` requires the namespace to already exist.
- Sequences: symbol/keyword `compare` sorts unqualified before qualified.
- Sequences: `(symbol "" "name")` preserves the empty-string namespace.
- Sequences: `repeat` rejects non-numeric count.
- Sequences: `select-keys` calls `seq` on `ks`; single keyword throws.
- Numerics: `NaN?` rejects non-numeric values.
- Numerics: `pos-int?`/`neg-int?`/`nat-int?` remain Long-only (reject bigint).
- Sequences: `counted?` no longer reports strings as counted.
- Macros: `use-fixtures` is now a macro capturing the caller's namespace
  at expansion time.
- Strings: `subs` indexes by codepoint, not byte offset.
- Sequences: `sort` orders `MINO_CHAR` by codepoint; `(sort nil)` returns `()`.
- Sequences: `(sort 1)` and `(set 1)` throw on non-seqable input.
- Sequences: `list?` accepts only proper cons chains and `()`; excludes
  chunked-cons and lazy-seq.
- Numerics: `(rational? 1.5M)` returns `true`.
- Reader: `try_parse_numeric` heap-allocates for tokens exceeding 63 bytes.
- Sequences: `cycle`, `mapcat`, and `reverse` validate collection argument
  eagerly.
- Collections: sorted collections with predicate comparator now resolve
  equality correctly via `rb_compare` falsy probe.
- Collections: sorted collections with different comparators compare equal
  when content matches.
- Strings: `(str 1N)` omits `N` suffix; `(str 1.0M)` omits `M` suffix.
- Numerics: `<`/`<=`/`>`/`>=` reject non-numeric operands; NaN short-circuits
  to `false`.
- Eval: `special-symbol?` recognises Clojure reserved special-form names.
- Numerics: `mod`/`rem`/`quot` preserve operand type across full numeric tower.
- Numerics: `integer?` returns true for `MINO_BIGINT`.
- Sequences: `doseq` supports `:let`, `:when`, and `:while` modifier clauses.
- Sequences: `realized?` throws on non-pending input.
- Collections: keyword invocation against a set performs membership probe.
- Sequences: `repeat` truncates non-integer count toward zero.
- Sequences: `(reverse nil)` returns `()`.
- Printer: `print`/`println` emit char codepoint as UTF-8, not `\name`.
- Sequences: `empty` on seq types returns `()` singleton.
- Strings: `first`/`rest`/`cons`/iterators return `MINO_CHAR` when walking
  strings codepoint by codepoint.
- Strings: `(seq string)` yields `MINO_CHAR` codepoints per Clojure.
- Sequences: `parse-boolean` throws on non-string input.
- Collections: `keys`/`vals` accept empty-list singleton `()`.
- Stdlib: `clojure.test/use-fixtures` added with `:once`/`:each` support.
- Numerics: `zero?`/`pos?`/`neg?`/`even?`/`odd?` accept full numeric tower.
- Sequences: `compare` handles nil, cross-tower numbers, chars, and vectors.
- Sequences: `(symbol var)` returns the var's fully-qualified name.
- Collections: `merge` accepts MapEntry and 2-element vector args.
- Collections: `(conj map nil)` is a no-op.
- Collections: `peek` on empty-list singleton returns `nil`.
- Collections: `assoc!`/`dissoc!`/`disj!`/`conj!` accept variadic arities.
- Collections: `(dissoc m)` returns map unchanged.
- Atoms: `atom` accepts `:meta` and `:validator` options.
- Macros: syntax-quote inside macro-generated closures qualifies against
  the macro's defining namespace.
- Macros: `require`/`load-file` clears `fn_ambient_ns` for the file load.
- Tests: add `tests/clojure_test_suite.clj` driver for jank-lang suite.
- Build: compile warning-free with `-Wall -Wpedantic -Wextra -Werror`;
  fix 14 pre-existing warnings.

## v0.98.6 — Bump MINO_VERSION_* Constants

- Build: bump `MINO_VERSION_MINOR`/`PATCH` constants to match v0.98.6 tag.

## v0.98.5 — Seedable PRNG + Minimal clojure.test.check Port

- Stdlib: add `random-seed!` to seed the per-state PRNG for reproducible
  `rand`/`rand-int`/`rand-nth` output.
- Stdlib: add `clojure.test.check` with `generators`, `properties`, and
  `quick-check`; shrinking not implemented.
- Stdlib: `clojure.spec.alpha/gen` and `exercise` now generate values
  instead of throwing `:mino/unsupported`.

## v0.98.3 — Auto-Chunking Sources

- Sequences: `(seq vector)` and `(range ...)` emit `MINO_CHUNKED_CONS`
  spines of 32-element chunks.
- Sequences: `prim_count`, `prim_empty?`, `prim_nth`, `cons?`, `seq?`,
  `sequential?`, and unquote-splicing handle `MINO_CHUNKED_CONS`.

## v0.98.2 — clojure.string/split 3-Arg Limit

- Strings: `(split s sep limit)` returns at most `limit` substrings when
  `limit > 0`.

## v0.98.1 — compare Cross-Type Total Order

- Sequences: `compare` returns canon total order across type tiers
  (`nil < false < true < numbers < strings < symbols < keywords`).

## v0.98.0 — Macro Hygiene For Cross-NS :refer :all

- Macros: `qq_qualify_symbol` consults `S->fn_ambient_ns` so syntax-quote
  qualifies against the macro's defining namespace.
- Stdlib: `clojure.test` internal helpers made public to match hygiene fix.

## v0.97.5 — clojure.spec.alpha Introspection Utilities

- Stdlib: add `abbrev` and `describe` to `clojure.spec.alpha`.

## v0.97.4 — Lift defn So Top-Of-File Predicates Use It

- Stdlib: move `defn`/`defn-`/`defonce` above early type predicates in
  `src/core.clj`; convert bootstrap `def`+`fn` sites to `defn`.

## v0.97.3 — clojure.core.async Canon Combinators

- Async: add `reduce`, `transduce`, `split`, and `partition-by` to
  `clojure.core.async`.

## v0.97.2 — src/core.clj Code-Quality Sweep

- Stdlib: wrap 157 long lines to 80-char limit in `src/core.clj`.

## v0.97.1 — Sort-By and Reductions Arities

- Sequences: `sort-by` exposes `[keyfn coll]` and `[keyfn cmp coll]`
  arities; bad arity now throws.
- Sequences: `reductions` exposes `[f coll]` and `[f init coll]` arities.

## v0.97.0 — Kwargs Destructuring

- Eval: `& {:keys [...]}` destructuring accepts inline pairs, trailing map,
  and mixed forms; `:or` defaults evaluated in binding env.
- Stdlib: `iteration` signature matches canon kwargs shape.

## v0.96.9

- CI: add `workflow_dispatch` trigger to `release-build.yml`.

## v0.96.8 — Chunked-Seq Family

- Sequences: add `chunk-buffer`, `chunk-append`, `chunk`, `chunk-cons`,
  `chunk-first`, `chunk-rest`, `chunk-next`, `chunked-seq?` with
  `MINO_CHUNK` and `MINO_CHUNKED_CONS` C types.
- Sequences: `map`, `filter`, `take`, `keep`, `keep-indexed`,
  `map-indexed` propagate chunkedness end-to-end.

## v0.96.7 — `:refer :all` Drops Transitive Refers; Macros Get Vars

- Namespaces: `:refer :all` now binds only the source ns's owned publics,
  not transitive refers from `clojure.core`.
- Namespaces: `defmacro` interns a var so macros appear in `ns-publics`.

## v0.96.6 — Wrap `clojure.core.async`; Rename `merge-chans`/`async-into`

- Async: merge `lib/core/channel.clj` and `lib/core/async.clj` into
  `lib/clojure/core/async.clj` under `clojure.core.async` namespace.
- Async: rename `merge-chans`→`merge` and `async-into`→`into`.

## v0.96.5 — `iteration` (Clojure 1.11)

- Stdlib: add `iteration` for consuming paginated/batch sources.

## v0.96.4 — Small Canon-Parity Additions

- Sequences: `comp` and `partial` adopt hand-unrolled fast-path arities.
- Sequences: `some-fn` and `every-pred` unrolled per-arity for 1–3 preds.
- Sequences: `into` gains 0-arg and 1-arg forms.
- Stdlib: `unchecked-divide-int` aliased to `quot`.

## v0.96.3 — Transients in `frequencies`/`group-by`; `unreduced` Cleanups

- Collections: `frequencies` and `group-by` use transient accumulator;
  one allocation per distinct key.
- Collections: `get` treats transient associative as transparent.

## v0.96.2 — Lazy-Seq `recur`-On-Skip Rewrites

- Sequences: `distinct`, `drop-while`, `keep-indexed`, `dedupe` allocate
  one lazy-seq cell per emitted value instead of per visited element.

## v0.96.1 — Stateful Transducers Use Real `volatile!`

- Sequences: ten transducer state slots switch from `atom` to `volatile!`,
  reducing per-step overhead on stateful pipelines.

## v0.96.0 — `volatile!` Becomes a Real Type

- VM: add `MINO_VOLATILE` type with `volatile!`, `volatile?`, `vreset!`,
  `vswap!` as C primitives; `deref` recognises volatile.

## v0.95.5 — `src/core.clj` Hygiene Sweep

- Stdlib: private helpers renamed from trailing-underscore to `defn-`
  convention; ~20 names changed.
- Stdlib: ~120 `(def name doc (fn ...))` forms converted to `defn`.

## v0.95.4 — `mino.tasks.builtin` and `clojure.string` Hygiene

- Build: deduplicate C-string-literal escape logic into shared helper.
- Stdlib: `gen-stdlib-headers` and `qa-arch` use `reduce` instead of
  mutable atoms.
- Strings: rename `index-of-from_` to `index-of-from`; simplify
  `re-quote-replacement` via `clojure.string/escape`.

## v0.95.3 — `core.async` Canon Parity

- Async: rename `onto-chan`→`onto-chan!` and `to-chan`→`to-chan!`.
- Async: `pipeline` gains 6-arg form with `ex-handler`.
- Async: `alts!` accepts trailing kwargs in addition to single-map form.

## v0.95.2 — Decomposed `clojure.instant/parse-timestamp`

- Stdlib: `parse-timestamp` refactored into per-segment helpers;
  public surface unchanged.

## v0.95.1 — Dynamic-Var `clojure.test` Internals

- Tests: `clojure.test` counters and context stack use real `^:dynamic`
  vars with `binding` instead of atoms.
- Tests: `run-tests` returns summary map; process exit moved to wrapper.

## v0.95.0 — Reduce-Based `clojure.data/diff`

- Stdlib: `clojure.data/diff-map` and `diff-sequential` rewritten as
  pure `reduce` over three-element accumulator; no atoms.

## v0.94.5 — Static-Link Windows Binary

- Windows: pass `-static` to linker so mingw runtime is baked into
  `mino.exe`, fixing `STATUS_DLL_NOT_FOUND` on clean installs.

## v0.94.4 — Force Line-Buffered Stdout on Windows

- Windows: call `setvbuf` at startup to force line-buffered stdout and
  unbuffered stderr, fixing missing output under PowerShell.

## v0.94.3 — bundle.awk Sidesteps MSYS Path Translation

- Build: move bundle escape script to `src/bundle.awk`; invoke with
  `-f` to avoid MSYS path-translation mangling inline regex.
- Windows: Windows artifact rejoins release matrix.

## v0.94.2 — Portable Bootstrap, Windows Rejoins Releases

- Build: bootstrap Makefile uses `awk` instead of `sed` for portable
  header generation.
- CI: remove `continue-on-error` guards now that bootstrap is portable.

## v0.94.1 — Release-Build Windows Guard

- CI: add `fail-fast: false` and Windows job-level `continue-on-error`
  to `release-build.yml`.

## v0.94.0 — Empty-List Canon Parity

- VM: `()` is a distinct `MINO_EMPTY_LIST` singleton; `(= '() nil)` is
  now `false`; `(seq? '())` is `true`.
- Sequences: all empty-seq-result branches return `()` instead of `nil`.
- Build: add 75-line bootstrap `Makefile`.
- Modules: disk `lib/<ns>.clj` takes priority over bundled copy.

## v0.93.0 — C Refactoring Pass

- VM: add trust-model and lock-contract banner comments to key subsystems.
- VM: split 8 god functions (`prim_require`, `eval_try`, `apply_callable`,
  `gc_mark_roots`, `gc_alloc_typed`, `read_atom`, `quasiquote_expand`,
  `tower_reduce`) into named helpers.
- VM: extract per-pattern helpers across 6 files to flatten duplicate code.
- VM: `runtime_module_add_alias` returns int; OOM surfaces as catchable
  exception.
- Security: `prim_random_uuid` uses `snprintf`; `ns_process_require_spec_ex`
  emits `MSY001` on oversized name.
- GC: add overflow guards to 5 buffer-grow paths.
- VM: remove dead `diag_add_note_at` and `diag_set_cause`.
- Docs: `src/mino.h` doc-only sweep; add seam-map banners to `mino_state`.
- Embedding: bundle `lib/mino/deps.clj`, `tasks.clj`, `tasks/builtin.clj`
  into binary via `mino_install_mino_tooling`.
- VM: add `MINO_EMPTY_LIST` scaffolding (not yet user-visible).

## v0.92.1 — CI And Linux Build Fixes

- Linux: define `_XOPEN_SOURCE 600` in `runtime/internal.h` to expose
  `PTHREAD_MUTEX_RECURSIVE`.
- CI: fix bootstrap to generate all bundled-stdlib headers.
- CI: invoke `./mino tests/run.clj` directly; cap step at 8 minutes.
- Tests: cap concurrent-test fan-out at `(dec (mino-thread-limit))`.
- Async: `close!` drains run queue after scheduling wake-callbacks.
- Windows: mark proc-test step `continue-on-error` for cmd.exe quirk.

## v0.92.0 — Audit and Doc Realignment

- Tests: full suite passes ASan, UBSan, and TSan.
- Async: `close!` drains run queue to prevent blocking `<!!`/`>!!`
  deadlock.
- Docs: site and performance page refreshed to reflect shipped runtime.

## v0.91.0 — Embed-Distinctive Thread API

- Embedding: add `mino_set_thread_pool` for host-managed worker pools.
- Embedding: add `mino_set_thread_factory` for per-worker lifecycle hooks.
- Embedding: add `mino_set_thread_stack_size` for spawn-per-future path.
- Async: `mino_host_threads_quiesce` yields GIL before joining to prevent
  deadlock from recursive callers.

## v0.90.0 — Blocking Channel Ops Park Across Threads

- Async: `<!!`, `>!!`, `alts!!` park the OS thread when `thread-limit > 1`,
  enabling true cross-thread channel coordination.
- Async: `thread` is a stable alias for `future-call`.
- VM: `S->thread_count` decrements on worker exit, not only on quiesce.
- GC: `mino_future_gc_sweep` unlinks impl from `future_list_head` before
  freeing, preventing use-after-free on quiesce.

## v0.89.0 — Real Host Threads

- VM: add `MINO_FUTURE` type backed by pthread (CreateThread on Windows)
  with full future/promise/thread API.
- VM: TLS-backed `mino_current_ctx` accessor; ~415 sites migrated.
- VM: per-state recursive mutex serialises `mino_eval`/`mino_call`.
- GC: suppress collection while worker threads are alive.
- Embedding: `mino_quiesce_threads` joins all workers; called from
  `mino_state_free` and `(exit ...)`.

## v0.88.0 — Safepoint Poll And STW Request For Major GC

- GC: mutators poll `should_yield` at eval entry, alloc prologue, and
  loop/recur back-edges to support stop-the-world major GC.
- GC: `gc_request_stw` / `gc_release_stw` wrap the major sweep; single-
  threaded path is O(1) flag toggle on `main_ctx`.
- Performance: fib(30) and reduce-over-million-range within noise vs v0.87.0.

## v0.87.0 — Per-Thread Context And Atom CAS

- VM: per-thread mutable fields moved from `mino_state_t` into a new
  `mino_thread_ctx_t`; `S->ctx` points at the embedded `main_ctx`.
- Atoms: `swap!` and `compare-and-set!` use `__atomic_compare_exchange_n`
  when `S->multi_threaded` is set; single-threaded fast path unchanged.
- Atoms: `compare-and-set!` switches to pointer-identity comparison,
  matching JVM `AtomicReference` semantics.

## v0.86.1 — Audit Fixes

- Linux: fix CPU-count detection by dropping dead `#ifdef` guard and
  calling `sysconf(_SC_NPROCESSORS_ONLN)` unconditionally.
- Tests: add missing `(run-tests)` call to two new test files so they
  produce output when invoked standalone.
- REPL: fix stray leading newline in `doc` output for capability-tagged
  bindings that have an empty docstring.

## v0.86.0 — Test Harness Suite Mode

- Tests: add `clojure.test/*suite-mode*` to suppress per-file
  `(run-tests)` calls; suite driver accumulates and runs once,
  recovering 246 previously skipped tests across 11 files.
- Tests: fix six `go-try-*` catch assertions to compare via `(ex-data e)`
  instead of comparing the diagnostic record directly to a string.
- Tests: replace deleted `Makefile` reference in `fs_test.clj` with
  `CHANGELOG.md`.
- Stdlib: promote `validate-dep-spec` from `defn-` to `defn` so tests
  can call it directly.

## v0.85.0 — Capability Metadata As Documentation

- API: each non-core install group tags its primitives with a capability
  keyword (`:io`, `:fs`, `:proc`, `:host`, `:async`).
- API: new `(mino-capability 'sym)` primitive returns the capability
  keyword or `nil`.
- REPL: `(clojure.repl/doc sym)` appends a "Capability: :group" line
  when the binding carries a label.

## v0.84.0 — Host Threads — Foundation Slice

- API: add `mino_set_thread_limit` / `mino_get_thread_limit` /
  `mino_thread_count` / `mino_quiesce_threads` C surface for
  host-grant-gated threads.
- VM: `future`, `thread`, `promise`, `deliver`, `realized?`,
  `future-cancel`, `future-done?`, `future-cancelled?` defined; throw
  `:mino/unsupported` with messages distinguishing "not granted" vs
  "in flight".
- CLI: standalone sets thread limit from host CPU count at startup.
- API: `(mino-thread-limit)` and `(mino-thread-count)` expose per-state
  knobs to script side.

## v0.83.0 — Clojure.spec.alpha And Clojure.core.specs.alpha

- Stdlib: port `clojure.spec.alpha` with `s/def`, `s/valid?`,
  `s/conform`, `s/explain`, `s/and`, `s/or`, `s/keys`, `s/coll-of`,
  `s/map-of`, `s/tuple`, `s/nilable`, `s/cat`, `s/*`, `s/+`, `s/?`,
  `s/alt`, `s/fdef`, `s/instrument`, `s/unstrument`, `s/registry`,
  `s/get-spec`, `s/form`, `s/assert`; shipped under
  `mino_install_clojure_spec`.
- Stdlib: port `clojure.core.specs.alpha` destructure-form specs for
  `defn`, `fn`, `let`, `binding`.
- Macros: `defmacro` records defining namespace so macro-body symbols
  resolve against the macro's own namespace.
- Macros: macro invocation sets `fn_ambient_ns` only, leaving `*ns*`
  as the caller's namespace inside macro bodies.

## v0.82.0 — Clojure.instant, Clojure.template, And Tagged-Literal Reader Hook

- Reader: `#tag form` resolved at read time via `*data-readers*`,
  `*default-data-reader-fn*`, then `tagged-literal` record fallback;
  tag emitted as symbol per canonical Clojure.
- Stdlib: add `clojure.template` (`apply-template`, `do-template`)
  under `mino_install_clojure_test`.
- Stdlib: add `clojure.instant` parsing ISO 8601 strings to a
  component map under its own `mino_install_clojure_instant` hook.

## v0.81.0 — Bundled Stdlib And Per-Group Install Hooks

- Embedding: `clojure.string`, `clojure.set`, `clojure.walk`,
  `clojure.edn`, `clojure.pprint`, `clojure.zip`, `clojure.data`,
  `clojure.test`, `clojure.repl`, `clojure.datafy` baked into the
  binary; each gets a per-state install hook.
- API: `mino_install_all` convenience installs core + I/O groups +
  all bundled namespaces.
- API: `mino_register_bundled_lib` exposes the registry so hosts can
  bundle their own namespaces.
- Build: `gen-stdlib-headers` task escapes each bundled `.clj` into a
  per-namespace header regenerated on every build.

## v0.80.0 — Real Records And Embed-Distinctive Type Construction

- VM: `defrecord` defines real value types with slot storage (not
  tagged maps); `->Type` and `map->Type` constructors generated.
- VM: record equality requires type-pointer identity + per-field
  equality + extension map equality; `(= record map)` is false.
- VM: `deftype` is an alias for `defrecord`; `reify` creates an
  anonymous type and returns a single instance.
- VM: `instance?` compares type-pointer or keyword identity; throw
  stub removed.
- VM: protocol dispatch atoms accept mixed keyword and type-pointer
  keys; `extend-type` / `extend-protocol` resolve type symbols at
  runtime.
- API: `mino_defrecord`, `mino_record`, `mino_record_field`,
  `mino_is_record`, `mino_is_record_type` added to `src/mino.h`.

## v0.79.0 — Auto-Promoting Arithmetic And `unchecked-*` Opt-In

- Numerics: `+`, `-`, `*`, `inc`, `dec` auto-promote to bigint on long
  overflow instead of throwing `:eval/overflow`.
- Numerics: add `unchecked-add`, `unchecked-subtract`,
  `unchecked-multiply`, `unchecked-inc`, `unchecked-dec`,
  `unchecked-negate` for explicit two's-complement wraparound.
- Numerics: remove `+'`, `-'`, `*'`, `inc'`, `dec'`; plain forms now
  auto-promote with the same semantics.
- Errors: retire `:eval/overflow` MOV001; out-of-range `(int bigint)`
  now reports `:eval/type` MTY001.

## v0.78.0 — `clojure.core.protocols` And Cross-Namespace Protocol Extension

- VM: `CollReduce`, `IKVReduce`, `Datafiable`, `Navigable` are first-
  class protocols; `reduce`, `reduce-kv`, `datafy`, `nav` consult
  protocol dispatch on every call.
- Stdlib: `clojure.datafy` surfaces `datafy` and `nav` at the canonical
  namespace.
- Macros: `extend-type` / `extend-protocol` preserve namespace prefix
  on protocol symbol, fixing cross-namespace extension.

## v0.77.0 — REPL Specials And `clojure.repl` / `clojure.stacktrace`

- REPL: bind `*1`, `*2`, `*3`, `*e`, `*command-line-args*`, `*file*`
  after each form; vars interned from `main.c` so embedders pay
  nothing.
- Stdlib: add `clojure.repl` with `doc`, `source`, `dir`, `find-doc`,
  `pst` macros/fns.
- Stdlib: add `clojure.stacktrace` with `print-throwable`,
  `print-stack-trace`, `print-cause-trace`, `root-cause`.
- Modules: fix require skipping `.clj` load for namespaces with pre-
  installed C primitives by checking `module_cache` instead of var
  registry.
- API: remove `doc`, `source`, `apropos` from `clojure.core`; now live
  in `clojure.repl`.

## v0.76.2 — Insertion Barrier For Incremental Major

- GC: write barrier now pushes both the old value (Yuasa SATB) and the
  new value (Dijkstra insertion) onto the major mark stack during
  MAJOR_MARK, closing a liveness window for OLDs reachable only via
  the new edge.

## v0.76.1 — GC Defensive Fixes On Alloc-Pair Patterns

- GC: `intern_lookup_or_create` raises `gc_depth` across the
  `alloc_val` call to protect the freshly dup'd character buffer from
  being swept.
- GC: `vec_from_array` keeps `gc_depth` raised through `vec_assemble`
  in both tail-only and full-trie paths.

## v0.76.0 — Print Pipeline And `*out*` / `*err*` / `*in*`

- VM: `*out*`, `*err*`, `*in*` interned as dynamic vars; binding to a
  string-collecting atom captures output or feeds input from a string.
- VM: print family (`println`, `prn`, `print`, `pr`, `newline`) routes
  through `*out*` before deciding the sink.
- Stdlib: add `with-out-str`, `with-in-str`, `print-str`, `prn-str`,
  `println-str`, `printf`, `flush`, `read-line`, `read*`.
- Embedding: print primitives moved to `k_prims_io_core` so sandboxed
  embedders using only `mino_install_core` get the print family.

## v0.75.0 — Surface Honesty

- Reader: `#"..."` regex literals now pass body bytes verbatim to the
  engine; backslashes are no longer consumed by the string-escape pass.
- CLI: expose `load-string` and `load-file` as primitives, surfacing the
  existing `mino_eval_string` / `mino_load_file` C functions.
- Docs: remove regex-escape entry from Intentional Divergences page;
  mark `#"regex"` as Same in Coming-from-Clojure table.

## v0.74.3 — One-Shot Expression CLI

- CLI: positional argument starting with `(`, `[`, `{`, `#`, `@`, `'`
  is evaluated as an inline expression; `--` forces file interpretation.
- CLI: `--help` documents the new expression-mode shape.

## v0.74.2 — Heap-Allocated Dynamic Binding Frames

- VM: `binding` and `with-bindings*` heap-allocate dynamic frames so
  `longjmp` unwinding does not read stale stack memory; fixes Windows
  SIGSEGV in `tests/run.clj`.
- CI: Windows matrix job restored to the blocking matrix.

## v0.74.1 — CI Hygiene

- CI: mark Windows matrix job `continue-on-error: true` pending root-
  cause investigation of post-v0.73.0 SIGSEGV.
- CI: mark `perf-gate` job informational (`continue-on-error: true`)
  due to shared-runner noise and v0.73.0 namespace-lookup cost.
- Build: fix `mino-bench` bundled-task module to qualify `clojure.string`
  calls broken by the v0.73.0 namespace move.
- CI: update `mino-site` deploy to bootstrap from `src/core.clj` and
  refresh `mino-examples` submodule pin.

## v0.74.0 — Deferred Core Surface

- VM: `*ns*` interned as a dynamic var; `in-ns`, `ns`, `require`
  publish and restore it correctly.
- VM: `bound-fn`, `bound-fn*`, `get-thread-bindings`,
  `with-bindings*` capture and replay dynamic bindings.
- Reader: `read-string` accepts optional opts-map with `:read-cond`
  (`:allow`, `:preserve`, `:disallow`); `clojure.edn/read` forces
  `:preserve`.
- Compiler: `destructure` exposed as a callable primitive.
- Regex: parenthesised capture groups supported; `re-find` /
  `re-matches` return `[whole g1 ...]` vectors when groups present.
- VM: `re-matcher` returns an atom-backed iterator; `re-groups` reads
  last recorded match result.

## v0.73.0 — First-Class Namespaces

- Namespaces: each namespace has its own root binding table; `clojure.core`
  is the parent of all user namespaces.
- Namespaces: `(ns ...)` accepts `:require`, `:use`, `:refer-clojure`
  with full modifier set (`:as`, `:refer`, `:rename`, prefix lists).
- Vars: `def` returns a first-class var; `intern`, `find-var`,
  `var-get`, `var-set`, `alter-var-root`, `with-redefs` all work.
- Namespaces: auto-resolved keywords (`::foo`, `::alias/foo`) and
  namespaced map literals (`#:foo{...}`, `#::{...}`) supported.
- Errors: cyclic `require` throws with load chain in message; namespace
  mismatch between file name and `(ns ...)` is rejected.
- Namespaces: full introspection surface: `in-ns`, `find-ns`, `the-ns`,
  `create-ns`, `remove-ns`, `ns-name`, `ns-publics`, `ns-interns`,
  `ns-refers`, `ns-aliases`, `ns-map`, `ns-unmap`, `ns-unalias`,
  `alias`, `all-ns`, `loaded-libs`, `ns-resolve`, `requiring-resolve`.
- Macros: syntax-quote expands alias prefixes on namespaced symbols;
  refer'd entries keep source-namespace identity.
- VM: `defn` honors `{:pre [...] :post [...]}` maps; `*assert*` bound
  true; `find` accepts transient associatives.
- VM: C primitives interned as vars in their install-time namespace.
- Stdlib: `clojure.core` 405/413 portable names (98%); `clojure.string`,
  `clojure.set`, `clojure.walk`, `clojure.zip` at 100%.
- Stdlib: `clojure.string` adds `index-of`, `last-index-of`,
  `re-quote-replacement`, `replace-first`; `clojure.zip` adds
  `leftmost`, `rightmost`; `compare-and-set!` added to `clojure.core`.
- VM: `agent`, `send-to`, `agent-error`, `defrecord`, `deftype`,
  `reify`, `proxy`, `gen-class`, `definterface`, `import`,
  `instance?` throw `:mino/unsupported`.
- Reader: `:clj` treated as active dialect in reader conditionals
  alongside `:mino`.
- Build: source files renamed from `.mino` to `.clj`; require resolver
  searches `.cljc`, `.clj`, `.cljs` in that order.
- Modules: `mino.deps` auto-detects Maven `src/main/clojure/` source
  root alongside plain `src/`.

## v0.72.0 — Release Pipeline & Build Polish

- CI: tag-triggered builds produce a draft GitHub Release with platform
  archives for linux/darwin amd64+arm64 and windows amd64, plus
  `checksums.txt`.
- CI: `promote-packages` workflow verifies SHA-256s and opens a PR
  against the Homebrew tap or Scoop bucket; auto-merge stays off.
- Build: fix `-Wcast-qual` warning in `apply_non_fn_callable` by
  making `form` parameter `const`.
- Build: fix `-Wcomment` warning in host-interop dispatch doc comment.
- Docs: fix README bootstrap snippet to include the `printf`/`sed`
  prelude that generates `src/core_mino.h`.

## v0.71.0 — Standalone CLI Polished

- CLI: add `-h`/`--help`, `-V`/`--version`, `-e`/`--eval EXPR`, and
  `--` separator; help/version exit 0, usage errors exit 2.
- CLI: `mino repl` alias; `mino nrepl` / `mino lsp` exec companion
  binary from PATH or exit 127 with a clear message.
- REPL: banner hint added; prompt changed to `mino=>`; bare `:help`
  and `:quit` meta-commands intercepted before eval.

## v0.70.0 — C-Core Refactored

- Docs: file-level headers updated to describe current content; stale
  provenance lines and old filename references removed.
- Docs: `INTERNAL_MODULE_MAP.md` and `ARCHITECTURE_CONTRACT.md`
  updated for special-form data-table dispatch and fast-path entries.
- API: `src/mino.h` drops stale claim that the transient API is
  not shipped.

## v0.68.0

- VM: `eval_impl` split into `eval_check_limits`, `eval_try_host_syntax`,
  `eval_try_special_form`, `eval_apply_regular_call`; special forms
  moved to a static `k_special_forms[]` table in
  `src/eval/special_registry.c`.

## v0.67.0

- Reader: `read_form` decomposed into `read_dispatch`, `read_wrap_one`,
  `read_char_literal` helpers; `ADVANCE`/`ADVANCE_N` macros replaced
  with type-checked static inline functions.

## v0.66.0

- VM: `hash_val` decomposed into named byte-loop helpers; numeric tier
  collapse funneled through one helper enforcing equal-implies-equal-
  hash invariant.
- VM: `mino_eq` grouped helpers renamed to `eq_*_like` family to pair
  with the hash side; contract documented above `mino_eq`.

## v0.65.0

- Build: regex engine in `src/regex/` isolated to a single `re.h`
  header consumed only from `src/prim/regex.c`; `-Isrc/regex` flag
  removed; `re.c` depends only on the C standard library.

## v0.64.0

- Tests: perf gate benchmark suite expanded from 5 to 15 micros covering
  reader, eval-special, allocation, host-call, and regex paths; alloc
  gate uses zero-tolerance for zero-baseline entries.

## v0.63.0

- Build: replace `DEF_PRIM` macro with static `mino_prim_def` tables
  per TU; `mino_install_core` becomes one nested loop over
  `k_core_domains[]`.
- Errors: add `src/diag/diag_contract.h` three-class severity taxonomy
  (`RECOVERABLE`, `HOST`, `CORRUPT`); each subsystem internal header
  documents error classes emitted.

## v0.62.2

- Build: rename source files under `src/` to drop redundant subsystem
  prefixes (e.g. `runtime_gc.c` → `gc/driver.c`,
  `prim_*.c` → `prim/*.c`); includes updated to path-qualified form.

## v0.62.1

- Build: decompose `src/mino_internal.h` into per-subsystem internal
  headers (`runtime/`, `gc/`, `collections/`, `eval/`, `interop/`,
  `async/`); old monolithic header deleted.

## v0.62.0

- Build: split `mino_state_free` into per-subsystem teardown helpers
  called in fixed order from a thin orchestrator.
- Build: replace `FMT_ENSURE`, `MINO_GC_VERIFY_CHECK`, `MATH_UNARY`
  behavioral macros with static inline functions.
- Build: add `src/runtime/path_buf.{c,h}` centralizing `PATH_BUF_CAP`
  with explicit truncation reporting.

## v0.61.0

- Build: reorganize `src/` into per-subsystem subdirectories (`public/`,
  `runtime/`, `gc/`, `eval/`, `collections/`, `prim/`, `async/`,
  `interop/`, `regex/`, `diag/`, `vendor/imath/`); CI bootstrap and
  README updated to per-subdirectory globs.

## v0.60.0 — Dialect Complete

- VM: transients (`transient`, `persistent!`, `assoc!`, `conj!`,
  `dissoc!`, `disj!`, `pop!`) over vec/map/set as public API.
- Collections: `sorted-map-by`, `sorted-set-by` accept custom
  comparator; `subseq`, `rsubseq` walk rbtree with bounded keys.
- VM: `print-method` multimethod routes C-level printer through a
  late-binding mino-level hook.
- Numerics: full tower with `MINO_BIGINT`, `MINO_RATIO`, `MINO_BIGDEC`
  backed by vendored imath; `1N`, `1/2`, `1M` literals; auto-promoting
  `+'`/`-'`/`*'`/`inc'`/`dec'`.
- VM: hierarchy version counter invalidates stale multimethod dispatch
  caches on `derive`/`underive`.
- Docs: new Compatibility Matrix and Intentional Divergences pages on
  mino-site; Coming from Clojure refreshed.

## v0.56.0 — Dialect-Semantics Audit

- VM: `(derive child parent)` 2-arity form now returns `nil` per
  Clojure contract.
- VM: multimethod caches now invalidated on `derive`/`underive` via
  hierarchy version counter.
- VM: `prefer-method` resolution follows hierarchy parents
  recursively, matching Clojure's `prefers?` behavior.
- VM: add `prefers` and `remove-all-methods` to multimethod surface.

## v0.55.0 — Numeric Tower Complete

- Numerics: add `MINO_RATIO` type; always reduced, denominator
  positive; collapses to int when denominator is 1.
- Numerics: add `MINO_BIGDEC` type (unscaled bigint + scale); `M`
  literal suffix; `=` is representation-strict, `==` collapses scale.
- Numerics: `+`, `-`, `*`, `/` perform full five-tier tower dispatch
  (int → bigint → ratio → bigdec, float collapses all).
- Numerics: `<`, `<=`, `>`, `>=` compare across all tiers without
  coercion artifacts.
- Numerics: add `==` numeric-equality primitive for cross-tier
  comparison.
- Numerics: add `numerator`, `denominator`, `rationalize`, `bigdec`,
  `decimal?`, `ratio?`, `rational?`.
- Numerics: `(= 1 1.0)` is now false, matching Clojure type-strict
  equality; `(/ 1 2)` returns ratio `1/2`.

## v0.54.0 — Auto-Promoting Arithmetic

- Numerics: add `+'`, `-'`, `*'`, `inc'`, `dec'` promoting to bigint
  on long overflow; homogeneous long operands keep the fast
  `__builtin_*_overflow` path.
- Bigint: add internal helpers `mino_bigint_add`, `mino_bigint_sub`,
  `mino_bigint_mul`, `mino_bigint_neg` for bigint arithmetic.
- Bigint: fix vendored imath UB at `MP_SMALL_MIN` where signed
  `LONG_MIN` negation before unsigned cast was undefined behavior.

## v0.53.0 — Bigint Foundation

- Bigint: add `MINO_BIGINT` value type backed by vendored imath; GC
  frees per-cell `mpz_t` digit storage during sweep.
- Reader: `1N` literal suffix produces real bigint of any magnitude.
- Bigint: add `bigint`, `biginteger`, `bigint?` primitives.
- Numerics: cross-tier `=` and `hash` for int/bigint;
  `(= 1 1N)` is true, shared hash bucket.
- Build: vendor imath under `src/vendor/` (MIT); document in
  `THIRD_PARTY_LICENSES.md`.

## v0.52.0 — Extensible Printer

- Printer: add `print-method` multimethod dispatched on `(type x)` for
  user-extensible readable printing
- Printer: add `pr-builtin` primitive that bypasses the dispatch hook
- Printer: install late-binding hook in `prim_pr`/`prim_prn`; safe
  fallback when hook is absent
- Eval: `type` honors `:type` metadata before the value's type tag

## v0.51.0 — Transients, Sorted-By, Subseq, Pr/Print/Newline

- Collections: add public transient API (`transient`, `persistent!`,
  `assoc!`, `conj!`, `dissoc!`, `disj!`, `pop!`, `transient?`)
- Collections: add `sorted-map-by` and `sorted-set-by` for custom
  comparators
- Sequences: add `subseq` and `rsubseq` range queries on sorted
  collections backed by pruning `rb_bounded_seq` walker
- Printer: add `pr`, `print`, and `newline` (no-trailing-newline
  siblings of `prn`/`println`)

## v0.50.0 — C Core Complete and Polished

- Build: tag-only release; no code changes since v0.49.1
- Tests: full ASAN/UBSAN/TSAN matrix clean across test suite and GC
  stress shards

## v0.49.1 — Callable and Module-Resolution Dedup

- VM: unify non-fn callable dispatch into `apply_non_fn_callable`;
  error codes now canonical from both call paths
- Modules: merge duplicate `dotted_to_path` / `dots_to_slashes` into
  `src/runtime_module.c`; fix alias duplicate-detection and OOM safety

## v0.49.0 — Docs and Hygiene

- Docs: correct `INCREMENTAL_BUDGET` default comment in `mino.h`
  (1024 → 4096)
- Build: add `src/public_embed.c` to task-runner source list
- Docs: remove dated `docs/architecture/baseline-2026-04-21.md`
- Build: tighten `.gitignore` rule to `docs/**/*.md`

## v0.48.0 — Embedder Polish

- API: add `MINO_VERSION_MAJOR/MINOR/PATCH` constants and
  `mino_version_string()`; REPL prints version at startup
- Build: add `mino task build-asan/build-ubsan/build-tsan` sanitizer
  targets
- API: add `mino_throw(S, payload)` to raise mino exceptions from C
- API: add `mino_args_parse(S, name, args, fmt, ...)` for typed
  argument destructuring at primitive entry points
- Tests: add `tests/embed_api_test.c` C-level smoke test
- Docs: add SemVer policy section to README

## v0.47.0 — Release Gates

- CI: add perf regression gate (`perf_gate.mino`) with ±15%/30%
  threshold; wired into CI as `perf-gate` job
- Tests: expand reader fuzz corpus from 4 to 22 seeds; add
  `fuzz-smoke` task and libFuzzer nightly CI job
- CLI: add native crash handler (SIGSEGV/SIGABRT/SIGBUS) printing GC
  stats and backtrace via async-signal-safe writes
- GC: add write-barrier site matrix in `runtime_gc_barrier.c` header;
  debug-time assert traps bogus container pointers
- GC: fix write-barrier gap in C transient API; route all `current`
  slot stores through `transient_set_current`

## v0.46.0 — Dialect C Groundwork

- Reader: add first-class `MINO_CHAR` type for character literals;
  `char?`, `int`, `str`, `pr-str` all support chars
- API: add public C transient API (`mino_transient`, `mino_persistent`,
  `mino_assoc_bang`, etc.) for batch mutation from C
- Sequences: add multi-collection `sequence` with matching `map`
  transducer multi-input arity
- Numerics: `+`, `-`, `*`, `inc`, `dec` now throw `eval/overflow`
  (MOV001) on integer overflow instead of wrapping
- Tests: add `tests/transient_test.c` and `tests/multimethod_test.c`
- Tests: re-enable four previously-disabled clj-compat assertions

## v0.45.0 — Correctness Closure

- GC: add lazy-seq cache-barrier regression test with generational
  promotion scenario
- Numerics: `bit-shift-left/right`, `unsigned-bit-shift-right`
  bounds-check shift amount; out-of-`[0,63]` raises `eval/bounds`
- Modules: `ns :require` now propagates load failures instead of
  silently swallowing them

## v0.44.0 — GC Observability and Spawn-Path Perf

- GC: add `:remset-cap`, `:remset-high-water`, `:mark-stack-cap`,
  `:mark-stack-high-water` to `gc-stats` and `mino_gc_stats_t`
- GC: add `:nursery-bytes` key to `gc-stats` map
- GC: intern-table marking bypasses interior-pointer resolver; minor
  per-intern cost drops from 24.7 ms to 1.8 ms on 190k-symbol
  workloads (-18% bot-fleet time)
- GC: gensym output bypasses intern table; resident heap at N=10k
  drops from 119 MiB to 108 MiB

## v0.43.1 — Nested-Minor UAF Fix, GC Event Ring, Multi-State Stress

- GC: fix use-after-free when nursery overflow fires during
  `MAJOR_MARK`; force in-flight major to completion before overflow
  minor
- GC: fix `MINO_GC_VERIFY=1` false positive on unreachable OLD
  containers; filter to reachable-OLD before scanning
- GC: add `MINO_GC_EVT=1` event ring with reachability classifier for
  post-mortem debugging
- Tests: add `tests/embed_multi_state.c` + `mino task test-embed` for
  16-thread multi-state isolation
- GC: add `MINO_GC_NURSERY_BYTES` env var override at state init
- VM: add `(gc!)` primitive for explicit GC trigger from mino code
- VM: move filename/var-string intern tables and PRNG per-state;
  eliminate cross-state sharing
- GC: split young/old allocation lists; minor collection skips OLD
  list
- GC: pin `mapv`/`filterv` accumulators on GC save stack across
  iterations
- GC: suppress GC around malloc'd C-heap accumulators in collection
  and sequence primitives
- VM: remove `lib/core/actor.mino`; channels cover all use cases

## v0.43.0 — Pure-mino Channels and Actors

- Async: demote channel implementation from C to `lib/core/channel.mino`;
  public surface unchanged
- Async: demote actor system from C to `lib/core/actor.mino`
- Async: replace `timeout*` with `async-schedule-timer*` callback
  primitive
- Async: rename `async/merge` to `async/merge-chans` to avoid
  shadowing `clojure.core/merge`
- Async: add `async-sched-enqueue*` bridge primitive for mino-level
  scheduler access
- Async: fix nil-callback crash on 2-arg `put!` / 1-arg `take!` path
- Build: remove `src/async_buffer.c/h`, `src/async_channel.c/h`,
  `src/async_handler.c/h`, `src/async_select.c/h` (~1331 LOC)

## v0.42.0 — Generational + Incremental Garbage Collector

- GC: replace stop-the-world mark-and-sweep with two-generation
  non-moving collector; max pause drops from ~110 ms to under 60 ms
- GC: add `mino_gc_collect`, `mino_gc_set_param`, `mino_gc_stats` to
  public API
- GC: add `:phase` key to `gc-stats` map
- GC: add `MINO_NURSERY`, `MINO_GC_MAJOR_GROWTH`, `MINO_GC_BUDGET`,
  `MINO_GC_QUANTUM` env vars for standalone CLI tuning
- GC: split `runtime_gc.c` into five TUs for readability
- GC: fix write-barrier missing on literal-builder scratch buffers
  (`eval_vector_literal`, `eval_map_literal`, `eval_set_literal`,
  both quasiquote branches)
- GC: raise default incremental slice budget from 1024 to 4096 headers

## v0.41.0 — GC Timing Instrumentation

- GC: add `:total-gc-ns` and `:max-gc-ns` keys to `gc-stats` map
- GC: deduplicate clock read into shared `mino_monotonic_ns` helper

## v0.40.0 — Interpreter Performance Pass

- VM: add `nano-time` and `gc-stats` primitives for benchmark
  instrumentation
- Sequences: add `range`, `lazy-map-1`, `lazy-filter`, `lazy-take`,
  `drop-seq`, `doall`, `dorun` as C c_thunks
- VM: add `inc`, `dec`, `not`, `empty?`, `some?`, `zero?`, `pos?`,
  `neg?`, `odd?`, `even?` and full type-predicate family as C
  primitives
- VM: switch symbol/keyword intern tables to open-addressing hash
- GC: make mark phase iterative with explicit stack
- GC: add per-class free lists for 16/24/48/64-byte blocks
- VM: add hash-index for large env frames; O(1) name resolution
- VM: cache interned symbol pointers for special-form dispatch;
  inline `when`/`and`/`or`
- VM: reuse `MINO_RECUR`/`MINO_TAIL_CALL` singleton cells
- VM: reuse local env frame for single-arity self-tail-calls
- VM: add small integer cache for −128..127
- VM: reduce fn frame initial capacity from 16 to 4 slots
- VM: return self-evaluating literal collections directly from AST
- VM: single-pass arithmetic for `+`, `-`, `*`
- VM: fix `core.mino` shadowing faster C primitives (`mapv`, `filterv`,
  `atom?`, `swap!`)

## v0.39.1 — Cross-Platform Portability Fixes

- Linux: add `_POSIX_C_SOURCE 200809L` to fix `strdup` implicit
  declaration and 64-bit truncation under `-std=c99`
- Windows: fix `sh!` to use `cmd.exe`-compatible quoting
- Windows: check both `mino` and `mino.exe` in build relink check
- Windows: use `TMPDIR`/`TEMP`/`TMP` in I/O tests instead of `/tmp/`
- VM: mark `setjmp`/`longjmp`-crossing variables `volatile` to fix
  GCC `-Wclobbered`

## v0.39.0 — Task Runner and Self-Hosting Build

- CLI: add `mino task <name>` task runner with dependency resolution
  backed by `mino.edn`
- Build: define `build`, `clean`, `test`, `test-external`,
  `gen-core-header`, `qa-arch` tasks replacing Makefile
- VM: add `file-mtime` primitive for incremental build support
- Strings: add C `str-replace` primitive (O(n) single-pass)
- CI: add Windows build and test on `windows-latest`
- Build: remove Makefile; self-host via `mino task build`

## v0.38.0 — Project Manifest and Dependency Management

- Modules: add `mino.edn` project manifest with `:paths` and `:deps`
- Modules: add `mino deps` subcommand for git dependency fetching into
  `.mino/deps/`
- Modules: auto-wire `:paths` and dependency dirs into module resolver
- API: add `file-exists?`, `directory?`, `mkdir-p`, `rm-rf` filesystem
  primitives
- API: add `sh` and `sh!` process execution primitives
- Modules: add binary-dir resolver fallback for bundled `lib/` modules
- Modules: implement deps logic in `lib/mino/deps.mino`

## v0.37.0 — Compatibility and Stdlib

- VM: add `defmulti`/`defmethod` multimethods with value dispatch and
  caching
- Macros: add `letfn`, `defonce`, `defn-`, multi-arity `defmacro`
- Reader: add `#"..."` regex literals, `#?@` splice in maps, octal/
  unicode char literals, `#_` discard fix
- VM: add `random-uuid`, `file-seq`, `getenv`, `getcwd`, `chdir`
  primitives
- Modules: add `.clj`/`.cljs` resolution and hyphen-to-underscore
  conversion
- Namespaces: add `:use` with `:only`; silently accept
  `:refer-clojure :exclude`
- Stdlib: add `clojure.core`, `clojure.data`, `clojure.zip`,
  `clojure.test`, `clojure.walk`, `clojure.edn`, `clojure.pprint`
- VM: add `extend-type` multi-protocol, docstring and keyword-option
  stripping in `defprotocol`
- Interop: add JVM stub forms (`defrecord`, `deftype`, `reify`,
  `proxy`, `gen-class`, `import`) that throw clear messages
- Tests: add 50-repo compatibility runner with failure categorization
- Errors: diagnostics inside `try` now longjmp to catch handler;
  cascading require errors include file path

## v0.36.0 — Error Diagnostics

- Errors: all errors represented as `mino_diag_t` with kind, code,
  phase, message, source span, notes, and stack frames
- Errors: assign stable error codes (MRE/MSY/MNS/MAR/MTY/MBD/MCT/
  MHO/MLM/MUS/MIN)
- Reader: add column tracking to all parsed forms
- REPL: pretty error rendering with source snippet and caret pointer
- API: add `mino_last_diag()`, `mino_last_error_map()`,
  `diag_to_map()` for structured error access from C
- REPL: add `(last-error)` and `(error?)` primitives
- Errors: `catch` always receives a diagnostic map; original value via
  `(ex-data e)`
- Errors: fix `prim_throw_error` infinite recursion outside try block

## v0.35.0 — core.async and Conformance

- Async: add full CSP channel implementation with `go` macro and 17 C
  primitives
- Async: add `alts!`/`alts!!` with `:priority` and `:default` options
- Async: add `go`/`go-loop` state machine transform supporting parks
  in let/if/cond/loop/try
- Async: add blocking bridge `<!!`, `>!!`, `alts!!`
- Async: add combinators `pipe`, `mult`/`tap`, `pub`/`sub`, `mix`,
  `pipeline`, `pipeline-async`, `pipeline-blocking`
- Stdlib: add `clojure.set` namespace
- Strings: add `escape`, `re-quote-replacement`, `capitalize`,
  `upper-case`, `lower-case`
- Macros: add `comment` and `when-first`
- Collections: add `make-array`, `aset`, `aget`, `alength`, `aclone`
- Macros: rewrite `case` with proper constant quoting and multi-value
  match
- Numerics: move `>`, `<=`, `>=` to C primitives with NaN handling
- Sequences: `conj` supports maps and lazy-seqs; `cons`/`seq` work on
  sorted collections
- Errors: fix reader `#?`/`#?@` handling; fix go macro park transform
  in multiple positions

## v0.34.0 — Further Conformance Hardening

- Reader: add radix integer literals (`2r…`, `8r…`, `16r…`)
- Reader: add tagged literal handling for unknown dispatch macros
- Reader: add `tagged-literal` function
- Collections: add `array-map` alias for `hash-map`
- Sequences: add `rseq` for sorted maps and sets
- Docs: add C, C++, Java language binding examples and eight use-case
  example programs
- Strings: `str` prints `Infinity`/`-Infinity`/`NaN` for special
  floats
- Numerics: `even?`/`odd?` throw on non-integer; `zero?` throws on
  non-number; `NaN?`/`infinite?` throw on nil
- Sequences: `mapcat` and `mapv` support multiple collections
- GC: fix use-after-free in `mino_map`, `mino_set`, `mino_sorted_map`,
  `mino_sorted_set` during construction

## v0.33.0 — Conformance Hardening

- VM: add `double?`, `volatile!`/`vreset!`/`vswap!`, `delay`/`force`/
  `delay?`
- Collections: add vector element comparison in `val_compare` for
  sorting vectors and map entries
- Strings: add index-based `contains?` for strings
- Tests: add external conformance test runner and `CONFORMANCE.md`
- Errors: all C primitive type/arity errors now use `prim_throw_error`
  making them catchable
- Sequences: `cons` throws on non-seqable second argument
- Reader: fix `#?@` splice in list context dropping elements

## v0.32.0 — Host Interop

- Interop: add capability registry with `mino_host_register_*` C API
  (default-deny)
- Interop: add `host/new`, `host/call`, `host/static-call`, `host/get`
  primitives
- Eval: recognize dot-method, field access, constructor, and static
  method call syntax; desugar to host primitives

## v0.31.0 — clojure.string Namespace

- Stdlib: add `clojure.string` namespace with `blank?`, `capitalize`,
  `starts-with?`, `ends-with?`, `escape`, `lower-case`, `upper-case`,
  `reverse`
- Modules: `require` saves and restores current namespace to prevent
  `ns` leakage

## v0.30.0 — Hierarchies and Dispatch Essentials

- VM: add `make-hierarchy`, `derive`, `underive`, `parents`,
  `ancestors`, `descendants`, `isa?` with global hierarchy atom

## v0.29.0 — Stateful Operations and Watches

- Atoms: add `add-watch`, `remove-watch`, `set-validator!`,
  `get-validator`, `swap-vals!`, `reset-vals!`
- Atoms: `reset!` and `swap!` now invoke validators and notify watches

## v0.28.0 — Core Collections Semantics

- Collections: add `subvec` (O(1) slice sharing trie), `seqable?`,
  `indexed?`
- Collections: `ifn?` returns true for keywords, maps, vectors, sets
- Collections: `empty` preserves metadata from input collection

## v0.27.0 — Numeric Tower Behavior

- Numerics: add `unsigned-bit-shift-right`, `parse-long`, `parse-double`
- Numerics: add `pos-int?`, `neg-int?`, `nat-int?` range predicates
- Numerics: add `ratio?`, `decimal?` stubs; `rational?` true for ints
- Numerics: add `long`/`double` coercion aliases, `num` validator

## v0.26.0 — Reader Literal Parity

- Reader: add `##Inf`, `##-Inf`, `##NaN` special float tokens
- Reader: add character literals (named and single-char) as
  single-character strings
- Reader: add hex integer literals, ratio literals, bigint/bigdec
  suffixes
- Numerics: add `NaN?`, `infinite?`; float division by zero returns
  IEEE infinity/NaN

## v0.25.0 — Test Framework Compatibility

- Tests: add `are` macro for parameterized assertions
- Tests: `is` macro recognizes namespace-qualified `thrown?` forms
- Modules: resolver searches `lib/` prefix and accepts `.cljc`
  extension
- Tests: add `lib/clojure/test.mino` shim and portability helpers
- VM: `resolve` auto-interns vars for C primitives

## v0.24.0 — Namespace and Var Semantics

- Vars: add `MINO_VAR` type with per-state registry; `def` creates
  vars, `var`/`#'` return them
- Namespaces: add qualified symbol resolution through alias table
- Namespaces: add `:refer` support in `require`
- Vars: add `var?`, `resolve`, `namespace`, 2-arity `symbol`
- Vars: add `qualified-keyword?`, `qualified-symbol?`,
  `simple-keyword?`, `simple-symbol?`

## v0.23.0 — Reader and Loadability Baseline

- Namespaces: add `ns` special form with `:require` (`:as`, `:refer`)
- Reader: add `#?`/`#?@` reader conditionals with `"mino"` dialect key
- Reader: add `#'` var-quote reader macro
- Modules: add vector syntax for `(require '[x.y :as z])`
- VM: add `current_ns` and `reader_dialect` fields to `mino_state_t`

## v0.22.0 — Collection and Sequence Conformance

- Collections: maps, vectors, sets callable as functions in direct and
  higher-order contexts; maps/keywords accept optional default arg
- Collections: add `peek`, `pop`, `find` (C primitive), `empty` (C
  primitive), `rseq`, `take-nth`, `lazy-cat`
- Collections: add sorted collections (`sorted-map`, `sorted-set`)
  backed by persistent LLRB tree
- Macros: add `some->`, `some->>`, `update-vals`, `update-keys`,
  `min-key`, `max-key`, `random-sample`, `halt-when`, `while`,
  `distinct?`, `bounded-count`, `counted?`, `ensure-reduced`
- Sequences: `rest` on vectors/maps/sets/strings returns a lazy cons
  chain via C thunk

## v0.21.0 — Architecture Hardening

- Build: split `eval_special.c` and `prim.c` into focused TUs;
  `qa-arch` passes with zero allowlists, TU limit tightened to 1100
  LOC
- VM: remove all state-field alias macros; use explicit `S->field`
  access throughout
- API: add ownership annotations to function declarations in
  `mino_internal.h` and `prim_internal.h`
- GC: increase `gc_save` array from 32 to 64 slots; assert on underflow
  in debug builds
- API: add `mino_set_fail_raw_at` fault injection API
- VM: fix `mino_pcall` to establish try frame before calling; fix
  `gc_pin`/`gc_unpin` counter imbalance in error path

## v0.20.0 — Dialect Alignment

- VM: add multi-arity `fn`/`defn` dispatch with clear arity-mismatch
  errors
- VM: add vector binding form for `let`, `fn`, `loop`, and all
  destructuring contexts
- VM: add positional and map destructuring (`:keys`, `:strs`, `:or`,
  `:as`) at any depth
- VM: add named `fn` for self-reference
- VM: add `defprotocol`, `extend-type`, `extend-protocol`, `satisfies?`
- Sequences: add `transduce`, `into` with xform, `sequence`,
  `eduction`, `completing`, `cat`; transducer arities for `map`,
  `filter`, `remove`, `take`, `drop`, `take-while`, `drop-while`,
  `keep`, `keep-indexed`, `map-indexed`, `dedupe`, `partition-by`,
  `partition-all`, `distinct`, `interpose`
- VM: add value metadata (`meta`, `with-meta`, `vary-meta`,
  `alter-meta!`); reader `^` syntax
- Macros: add `#(...)` anonymous fn shorthand, `#_` discard
- VM: add callable keywords, `ex-info`/`ex-data`/`ex-message`,
  `try`/`finally`, `with-open`, `identical?`, `reduced`, `declare`
- VM: add multi-binding `for`/`doseq` with `:when`/`:while`/`:let`
- Sequences: add multi-collection `map`
- Tests: grow suite to 552 tests / 2039 assertions

## v0.19.0 — Explicit Runtime State

- API: `mino_prim_fn` gains `mino_state_t *S` first parameter;
  `mino_current_state()` removed
- API: remove implicit default global state; host must call
  `mino_state_new()`
- Macros: `spawn` becomes a macro taking unquoted forms; `spawn*` is
  the primitive
- Performance: add `rangev`, `mapv`, `filterv` eager vector builders
  in C; `rangev` 60-70x faster than lazy `range`
- VM: cache parsed `core.mino` forms per state

## v0.18.0 — Runtime State, GC Hardening, and Repo Reorganization

- API: all public functions take explicit `mino_state_t *S`; add
  `mino_state_new`/`mino_state_free`
- GC: add `gc_pin`/`gc_unpin` save stack for borrowed values across
  allocation boundaries
- API: add `mino_clone` for cross-state value transfer
- Async: add thread-safe `mino_mailbox_t` and actor primitives
  (`spawn`, `send!`, `receive`)
- API: add `mino_env_clone`, `mino_interrupt`, `mino_ref`/`mino_deref`/
  `mino_unref`
- VM: add `binding` special form for dynamic variables
- VM: add `swap!` primitive; add regex `re-find`/`re-matches`
- Build: move library source to `src/`; split monolithic `mino.c` into
  9 TUs
- GC: fix borrowed function pointer UAF under `MINO_GC_STRESS=1`
- Errors: division by zero, bounds errors, format mismatches now throw
  catchable exceptions
- Performance: replace `tmpfile()` mailbox serialization with direct
  buffer printer; actor send+recv 50k drops from 6 s to 156 ms

## v0.17.0 — Proper Tail Calls and Core Library

- VM: add proper tail calls via `MINO_TAIL_CALL` trampoline; all tail
  positions run in constant stack space
- VM: add type predicates, sequence navigation helpers (`next`,
  `nfirst`, `fnext`, `nnext`), map entry accessors (`key`, `val`)
- Macros: add `if-not`, `when-not`, `if-let`, `when-let`, `if-some`,
  `when-some`, `as->`, `cond->`, `cond->>`
- Sequences: add `last`, `butlast`, `nthrest`, `nthnext`, `take-last`,
  `drop-last`, `split-at`, `split-with`, `sort-by`, `keep`,
  `keep-indexed`, `map-indexed`, `partition-all`, `reductions`,
  `dedupe`
- Collections: add `get-in`, `assoc-in`, `update-in`, `merge-with`,
  `reduce-kv`, `replace`, `str-replace`
- VM: add `every-pred`, `some-fn`, `fnil`, `memoize`, `trampoline`,
  `doto`, `dotimes`, `doseq`, `condp`, `case`, `for`
- Tests: grow suite to 300 tests / 664 assertions

## v0.16.0 — Complete C Primitive Layer

- Numerics: add math primitives (`math-floor`, `math-ceil`,
  `math-round`, `math-sqrt`, `math-pow`, `math-log`, `math-exp`,
  trig functions, `math-pi`)
- Collections: add `hash`, `compare`, `sort` with comparator
- VM: add `symbol`/`keyword` constructors, `eval`, `rand`, `time-ms`
- Regex: add `re-find`, `re-matches` via bundled tiny-regex-c (ANSI C)

## v0.15.0 — Test Framework and Dogfooding

- CLI: add file argument support; `./mino script.mino` evaluates and
  exits with code 1 on failure
- Modules: add CWD-relative module resolver via `mino_set_resolver`
- CLI: add `exit` primitive
- Tests: add `test.mino` framework (`deftest`, `is`, `testing`,
  `run-tests`); replace smoke.sh with 203 mino tests / 427 assertions
- Reader: `read-string` now throws catchable exceptions on parse errors

## v0.14.0 — Lazy Sequences, Complete C Core, core.mino Expansion

- Sequences: add `MINO_LAZY` type with `lazy-seq`; forces on first
  access, releases thunk after forcing
- Sequences: add `seq`, `realized?`, `dissoc`
- Numerics: add `mod`, `rem`, `quot`, bitwise operations, `int`,
  `float`, `char-at`
- Printer: add `pr-str`, `read-string`, `format`
- Stdlib: ~40 new `core.mino` definitions including `second`, `mapcat`,
  `cycle`, `partition`, `doall`, `dorun`
- VM: move `map`, `filter`, `take`, `drop`, `concat`, `range`,
  `repeat`, `update`, `some`, `every?` from C to lazy mino
  implementations
- Build: rename `stdlib.mino` to `core.mino`

## v0.13.0 — Atoms, Spit, Stdlib Architecture

- Atoms: add `MINO_ATOM` type with `atom`, `deref`, `reset!`, `swap!`,
  `atom?`; reader `@form` shorthand
- VM: add `spit` I/O primitive
- VM: add `defn` stdlib macro
- Build: move stdlib to standalone `core.mino` file compiled into
  binary; migrate 15 definitions from C to mino

## v0.12.0 — Release Candidate (Alpha)

- Errors: `let`/`loop` errors include source file and line; type
  errors report actual received type
- Docs: add embedding cookbook (`cookbook/`) with six worked examples
- Tests: add libFuzzer reader harness and 57-case adversarial crash
  test suite
- Tests: add map and sequence benchmarks

## v0.11.0 — Sequences and Remainder of Stdlib

- Collections: add `MINO_SET` type with HAMT backing; `hash-set`,
  `set?`, `contains?`, `disj`, `get`, `conj` on sets
- Sequences: add `map`, `filter`, `reduce`, `take`, `drop`, `range`,
  `repeat`, `concat`, `into`, `apply`, `reverse`, `sort` (strict,
  return lists)
- Strings: add `subs`, `split`, `join`, `starts-with?`, `ends-with?`,
  `includes?`, `upper-case`, `lower-case`, `trim`
- Stdlib: add `comp`, `partial`, `complement`, `not`, `not=`,
  `empty?`, `some`, `every?`, `identity`

## v0.10.0 — Interactive Development

- Printer: add cycle-safe printing; emits `#<...>` at depth > 128
- REPL: add `doc`, `source`, `apropos` reflection primitives
- VM: `def`/`defmacro` record docstring metadata
- API: add `mino_repl_t` in-process REPL handle for host-driven
  read-eval-print without a thread

## v0.9.0 — Sandbox, Modules, Diagnostics

- Reader: track file name and line number; eval errors include
  `file:line:` prefix and stack trace
- VM: add `try`/`catch`/`throw` via `setjmp`/`longjmp`
- API: move I/O primitives to `mino_install_io`; core no longer
  installs I/O by default
- Modules: add `require` with host resolver and result cache via
  `mino_set_resolver`

## v0.8.0 — Host C API

- API: add `mino_new`, `mino_eval_string`, `mino_load_file`,
  `mino_register_fn`, `mino_call`, `mino_pcall`
- API: add `MINO_HANDLE` opaque host-object type
- API: add `mino_to_int`, `mino_to_float`, `mino_to_string`,
  `mino_to_bool` type-safe extraction
- API: add `mino_set_limit` with `MINO_LIMIT_STEPS` and
  `MINO_LIMIT_HEAP`
- VM: add `string?`, `number?`, `keyword?`, `symbol?`, `vector?`,
  `map?`, `fn?` type predicates; `type` primitive; `str`, `println`,
  `prn`
- Docs: add `examples/embed.c` 50-line embedding example

## v0.7.0 — Tracing Garbage Collection

- GC: replace per-allocation malloc/free with stop-the-world
  mark-and-sweep collector tracking all heap objects
- GC: conservative stack scan with `setjmp` register flush and
  interior pointer support
- GC: adaptive collection threshold starting at 1 MiB, grows to 2×
  live-bytes after each sweep
- GC: add `MINO_GC_STRESS=1` mode forcing collection on every
  allocation

## v0.6.0 — Macros

- Macros: add `MINO_MACRO` type sharing closure layout; `defmacro`
  special form
- Reader: add `` ` ``, `~`, `~@` quasiquote shorthands;
  `quasiquote` special form
- VM: add variadic `& rest` parameter lists for `fn`, `defmacro`,
  `loop`
- VM: add `macroexpand-1`, `macroexpand`, `gensym`
- Macros: add `when`, `cond`, `and`, `or`, `->`, `->>` in-language
  stdlib macros

## v0.5.0 — Persistent Maps

- Collections: replace map layout with 32-wide HAMT plus
  insertion-order key vector; O(log₃₂ n) lookup, stable iteration
  order
- Collections: hash function consistent with `=`; integral floats hash
  as equivalent int
- Collections: add collision handling via linear bucket promoted to
  bitmap node

## v0.4.0 — Persistent Vectors

- Collections: replace vector layout with persistent 32-way trie with
  tail buffer; O(1) amortized `conj`, O(log₃₂ n) `nth`
- Collections: add `vec_from_array` O(n) bulk build path

## v0.3.0 — Literal Vectors, Maps, and Keywords

- Collections: add `MINO_KEYWORD`, `MINO_VECTOR`, `MINO_MAP` types
  with reader/printer support; symbols and keywords interned
- Collections: add `count`, `nth`, `first`, `rest`, `vector`,
  `hash-map`, `assoc`, `get`, `conj`, `update`, `keys`, `vals`
- VM: factor `apply_callable` out of evaluator for primitives

## v0.2.0 — Core Special Forms and Closures

- VM: add chained environments; `if`, `do`, `let`, `fn`, `loop`,
  `recur` special forms
- VM: add `MINO_FN` closure type capturing definition-time environment
- VM: add `loop`/`recur` trampoline for bounded-stack tail recursion
- VM: add `<=`, `>`, `>=` comparison primitives

## v0.1.0 — Walking Skeleton

- VM: add tagged value representation covering nil, bool, int, float,
  string, symbol, cons, primitive fn
- Reader: recursive-descent reader for atoms, lists, strings, numerics,
  `'` quote shorthand
- Printer: round-trip printer with re-escaped strings and float decimal
  point
- VM: tree-walking evaluator with `quote`, `def`, and arithmetic/list
  primitives
- REPL: standalone interactive REPL with multi-line input and clean
  stdout
- CI: GitHub Actions matrix for ubuntu and macos; MIT license, README
