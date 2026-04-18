# Internal Module Map

Source layout after the readability/architecture work. Each module
has a single responsibility. State access is explicit (`S->field`).

## Runtime Core

| File | LOC | Responsibility |
|------|-----|----------------|
| `src/mino.c` | 279 | Evaluator front door: `mino_eval`, `mino_eval_string`, `mino_load_file`, `mino_call`, `mino_pcall`, REPL, core eval helpers (`eval_value`, `eval_implicit_do`, `eval_args`, `macroexpand*`, `quasiquote_expand`, `lazy_force`) |
| `src/eval_special.c` | 754 | Eval dispatch (`eval_impl`, `eval`), literal evaluation (symbol/vector/map/set), `ns`, `var` |
| `src/eval_special_defs.c` | 398 | Definition special forms: `def`, `defmacro`, `declare` |
| `src/eval_special_bindings.c` | 530 | Destructuring (`bind_form`, `bind_params`), `let`, `loop`, `binding` |
| `src/eval_special_control.c` | 195 | `try`/`catch`/`finally` |
| `src/eval_special_fn.c` | 432 | `fn`, `apply_callable`, multi-arity dispatch |
| `src/runtime_state.c` | 528 | State lifecycle (`mino_state_new`, `mino_state_free`), limits, interrupt, env public API (`mino_env_new`, `mino_env_free`, `mino_env_clone`, `mino_new`), refs, fault injection setup |
| `src/runtime_gc.c` | 621 | GC allocation (`gc_alloc_typed`, `alloc_val`, `dup_n`), mark-and-sweep collector, conservative stack scan, range index |
| `src/runtime_env.c` | 171 | Internal environment ops (`env_alloc`, `env_child`, `env_root`, `env_bind`, `env_find_here`), root-env registry, dynamic binding lookup |
| `src/runtime_error.c` | 178 | Error reporting (`set_error`, `set_error_at`, `clear_error`), call stack (`push_frame`, `pop_frame`, `append_trace`), `type_tag_str`, metadata table |
| `src/runtime_var.c` | 88 | Var registry (`var_intern`, `var_find`, `var_set_root`) |

## Primitives

| File | LOC | Responsibility |
|------|-----|----------------|
| `src/prim.c` | 368 | Shared helpers (`prim_throw_error`, `print_to_string`, seq iterator), `install_core_mino`, `mino_install_core`, `mino_install_io` |
| `src/prim_numeric.c` | 640 | Arithmetic (`+`, `-`, `*`, `/`, `mod`, `rem`, `quot`), coercion (`int`, `float`), bitwise ops, math functions, comparison (`=`, `<`, `compare`, `identical?`) |
| `src/prim_collections.c` | 1082 | List/vector/map/set primitives (`car`, `cdr`, `cons`, `count`, `nth`, `first`, `rest`, `assoc`, `get`, `conj`, `keys`, `vals`, `hash-set`, `contains?`, `disj`, `dissoc`, `seq`, `realized?`) |
| `src/prim_sequences.c` | 887 | Sequence operations (`reduce`, `into`, `apply`, `reverse`, `sort`, `rangev`, `mapv`, `filterv`, `reduced`, `reduced?`) |
| `src/prim_string.c` | 690 | String primitives (`str`, `pr-str`, `format`, `read-string`, `subs`, `split`, `join`, `starts-with?`, `ends-with?`, `includes?`, `upper-case`, `lower-case`, `trim`, `char-at`) |
| `src/prim_io.c` | 153 | I/O primitives (`println`, `prn`, `slurp`, `spit`, `exit`, `time-ms`) |
| `src/prim_reflection.c` | 351 | Reflection/utility (`type`, `name`, `eval`, `symbol`, `keyword`, `hash`, `gensym`, `macroexpand`, `throw`, `rand`, `resolve`, `namespace`, `var?`) |
| `src/prim_meta.c` | 108 | Metadata (`meta`, `with-meta`, `vary-meta`, `alter-meta!`) |
| `src/prim_regex.c` | 61 | Regex (`re-find`, `re-matches`) |
| `src/prim_stateful.c` | 359 | Atoms (`atom`, `deref`, `reset!`, `swap!`, `atom?`, `add-watch`, `remove-watch`, `set-validator!`, `get-validator`, `swap-vals!`, `reset-vals!`) |
| `src/prim_module.c` | 281 | Module system (`require`, `doc`, `source`, `apropos`, `mino_set_resolver`) |
| `src/prim_host.c` | 252 | Host interop primitives (`host/new`, `host/call`, `host/static-call`, `host/get`), capability registry |
| `src/host_interop.c` | 128 | Interop syntax desugaring (dot-method, field access, constructor, static calls) |

## Data Structures

| File | LOC | Responsibility |
|------|-----|----------------|
| `src/val.c` | 590 | Value constructors, interning, hashing, equality |
| `src/vec.c` | 448 | Persistent vector (32-way trie) |
| `src/map.c` | 643 | HAMT for maps and sets |
| `src/rbtree.c` | 450 | Persistent left-leaning red-black tree for sorted maps and sets |

## Reader/Printer/Regex

| File | LOC | Responsibility |
|------|-----|----------------|
| `src/read.c` | 1197 | Reader (tokenizer + form parser, reader conditionals, radix literals, tagged literals) |
| `src/print.c` | 260 | Printer (value -> text, one switch dispatch) |
| `src/re.c` | 544 | Regex engine (self-contained, thread-safe) |

## Cross-State / Concurrency

| File | LOC | Responsibility |
|------|-----|----------------|
| `src/clone.c` | 660 | Value cloning, serialization, mailbox (mutex-protected FIFO), actors |

## Headers

| File | Contents |
|------|----------|
| `src/mino.h` | Public embedding API (stable surface) |
| `src/mino_internal.h` | Internal types (`gc_hdr_t`, `try_frame_t`, `mino_state` struct), `gc_pin`/`gc_unpin` macros, shared function declarations with ownership annotations |
| `src/eval_special_internal.h` | Cross-domain declarations for evaluator special-form files |
| `src/prim_internal.h` | Shared primitive helpers, `seq_iter_t`, per-domain primitive declarations |
| `src/re.h` | Regex public header |

## Entry Point

| File | LOC | Responsibility |
|------|-----|----------------|
| `main.c` | 193 | CLI: argument parsing, REPL, file execution |

## Dependency Directions

Allowed include directions (no cycles):

```
main.c -> mino.h

eval_special_*.c -> eval_special_internal.h -> mino_internal.h -> mino.h
prim_*.c         -> prim_internal.h         -> mino_internal.h -> mino.h
prim_regex.c     -> prim_internal.h + re.h

runtime_*.c, mino.c, val.c, vec.c, map.c, rbtree.c,
read.c, print.c, clone.c, host_interop.c -> mino_internal.h -> mino.h

re.c -> re.h (standalone, no mino headers)
```

Rules:
- No circular includes.
- Primitive files access shared helpers through `prim_internal.h`.
- Evaluator special-form files access cross-domain helpers through `eval_special_internal.h`.
- Runtime core files use `mino_internal.h` directly.
- Only `prim_regex.c` includes `re.h` (regex is isolated from the rest of the primitive layer).
- `prim.c` includes `core_mino.h` (generated) for the core library bootstrap.

## How to Add a Primitive

1. Choose the domain file (`prim_numeric.c`, `prim_collections.c`, etc.).
2. Write the function with signature `mino_val_t *prim_name(mino_state_t *S, mino_val_t *args, mino_env_t *env)`.
3. Declare it in `prim_internal.h` under the appropriate domain section.
4. Register it in `mino_install_core()` or `mino_install_io()` in `prim.c`.
5. Add tests in `tests/` and run `make test`.

## How to Add a Special Form

1. Add the handler in the appropriate `eval_special_*.c` file by domain.
2. Declare it in `eval_special_internal.h` if it needs to be called from the dispatch.
3. Add the keyword check in `eval_impl`'s dispatch chain in `eval_special.c`.
4. Add tests. Special forms are recognized by the evaluator, not installed.
