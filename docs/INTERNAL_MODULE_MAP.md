# Internal Module Map

Source layout after the P0-P6 readability/architecture work. Each module
has a single responsibility. State access is explicit (`S->field`).

## Runtime Core

| File | LOC | Responsibility |
|------|-----|----------------|
| `src/mino.c` | 275 | Evaluator front door: `mino_eval`, `mino_eval_string`, `mino_load_file`, `mino_call`, `mino_pcall`, REPL, core eval helpers (`eval_value`, `eval_implicit_do`, `eval_args`, `macroexpand*`, `quasiquote_expand`, `lazy_force`) |
| `src/eval_special.c` | 1517 | Special form dispatch (`eval_impl`), all special forms (`def`, `let`, `fn`, `if`, `do`, `loop/recur`, `try/catch/finally`, `binding`), destructuring, `apply_callable` |
| `src/runtime_state.c` | 519 | State lifecycle (`mino_state_new`, `mino_state_free`), limits, interrupt, env public API (`mino_env_new`, `mino_env_free`, `mino_env_clone`, `mino_new`), refs, fault injection setup |
| `src/runtime_gc.c` | 596 | GC allocation (`gc_alloc_typed`, `alloc_val`, `dup_n`), mark-and-sweep collector, conservative stack scan, range index |
| `src/runtime_env.c` | 171 | Internal environment ops (`env_alloc`, `env_child`, `env_root`, `env_bind`, `env_find_here`), root-env registry, dynamic binding lookup |
| `src/runtime_error.c` | 175 | Error reporting (`set_error`, `set_error_at`, `clear_error`), call stack (`push_frame`, `pop_frame`, `append_trace`), `type_tag_str`, metadata table |

## Primitives

| File | LOC | Responsibility |
|------|-----|----------------|
| `src/prim.c` | 1005 | Shared helpers (`prim_throw_error`, `print_to_string`, seq iterator), metadata prims, reflection prims, macroexpand/gensym, throw, regex prims, module/require, atoms, `install_core_mino`, `mino_install_core`, `mino_install_io` |
| `src/prim_numeric.c` | 583 | Arithmetic (`+`, `-`, `*`, `/`, `mod`, `rem`, `quot`), coercion (`int`, `float`), bitwise ops, math functions, comparison (`=`, `<`, `compare`, `identical?`) |
| `src/prim_collections.c` | 1051 | List/vector/map/set primitives (`car`, `cdr`, `cons`, `count`, `nth`, `first`, `rest`, `assoc`, `get`, `conj`, `keys`, `vals`, `hash-set`, `contains?`, `disj`, `dissoc`, `seq`, `realized?`) |
| `src/prim_sequences.c` | 604 | Sequence operations (`reduce`, `into`, `apply`, `reverse`, `sort`, `rangev`, `mapv`, `filterv`, `reduced`, `reduced?`) |
| `src/prim_string.c` | 711 | String primitives (`str`, `pr-str`, `format`, `read-string`, `subs`, `split`, `join`, `starts-with?`, `ends-with?`, `includes?`, `upper-case`, `lower-case`, `trim`, `char-at`) |
| `src/prim_io.c` | 162 | I/O primitives (`println`, `prn`, `slurp`, `spit`, `exit`, `time-ms`) |

## Data Structures

| File | LOC | Responsibility |
|------|-----|----------------|
| `src/val.c` | 499 | Value constructors, interning, hashing, equality |
| `src/vec.c` | 311 | Persistent vector (32-way trie) |
| `src/map.c` | 642 | HAMT for maps and sets |

## Reader/Printer/Regex

| File | LOC | Responsibility |
|------|-----|----------------|
| `src/read.c` | 802 | Reader (tokenizer + form parser, per-form helpers) |
| `src/print.c` | 221 | Printer (value -> text, one switch dispatch) |
| `src/re.c` | 544 | Regex engine (self-contained, thread-safe) |

## Cross-State / Concurrency

| File | LOC | Responsibility |
|------|-----|----------------|
| `src/clone.c` | 643 | Value cloning, serialization, mailbox (mutex-protected FIFO), actors |

## Headers

| File | Contents |
|------|----------|
| `src/mino.h` | Public embedding API (stable surface) |
| `src/mino_internal.h` | Internal types (`gc_hdr_t`, `try_frame_t`, `mino_state` struct), `gc_pin`/`gc_unpin` macros, shared function declarations with ownership annotations |
| `src/prim_internal.h` | Shared primitive helpers, `seq_iter_t`, per-domain primitive declarations |
| `src/re.h` | Regex public header |

## Entry Point

| File | LOC | Responsibility |
|------|-----|----------------|
| `main.c` | 168 | CLI: argument parsing, REPL, file execution |

## Include Graph

```
main.c -> mino.h
src/prim_*.c -> prim_internal.h -> mino_internal.h -> mino.h
src/*.c -> mino_internal.h -> mino.h
src/re.c -> re.h (standalone, no mino headers)
```

No circular includes. Primitive files access shared helpers through
`prim_internal.h`. All other runtime files use `mino_internal.h`.

## How to Add a Primitive

1. Choose the domain file (`prim_numeric.c`, `prim_collections.c`, etc.).
2. Write the function with signature `mino_val_t *prim_name(mino_state_t *S, mino_val_t *args, mino_env_t *env)`.
3. Declare it in `prim_internal.h`.
4. Register it in `mino_install_core()` or `mino_install_io()` in `prim.c`.
5. Add tests in `tests/` and run `make test`.

## How to Add a Special Form

1. Add the handler in `eval_special.c`.
2. Add the keyword check in `eval_impl`'s dispatch chain.
3. Add tests. Special forms are recognized by the evaluator, not installed.
