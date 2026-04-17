# Internal Module Map

Current source layout and target decomposition for the readability/architecture
work. Each row shows the current file, what it contains today, and what it
should contain after extraction.

## Current State

| File | LOC | Responsibility |
|------|-----|----------------|
| `src/mino.c` | 3207 | State lifecycle, GC (alloc/mark/sweep/range-index), environment, error reporting, metadata, evaluator (dispatch + all special forms), quasiquote, destructuring, apply/call, REPL, file loader |
| `src/prim.c` | 4091 | All C primitives (~90 functions), `mino_install_core`, `mino_install_io`, core.mino bootstrap |
| `src/val.c` | 499 | Value constructors, interning, hashing, equality |
| `src/vec.c` | 311 | Persistent vector (32-way trie) operations |
| `src/map.c` | 642 | HAMT operations for maps and sets |
| `src/read.c` | 795 | Reader (tokenizer + form parser) |
| `src/print.c` | 221 | Printer (value -> text) |
| `src/clone.c` | 643 | Value cloning, serialization, mailbox, actors, mutex shims |
| `src/re.c` | 544 | Regex engine (self-contained) |
| `src/mino_internal.h` | 440 | All internal types, state struct, ~50 state alias macros, shared declarations |
| `src/mino.h` | 562 | Public API |
| `src/re.h` | 69 | Regex public header |
| `main.c` | 168 | CLI entry point |

## Target State (after P1 + P2)

### From `src/mino.c` (target: < 900 LOC orchestration)

| New file | Extracted from | Contents |
|----------|---------------|----------|
| `src/runtime_state.c` | mino.c:7-76 | `state_init`, `mino_state_new`, `mino_state_free`, ref management |
| `src/runtime_error.c` | mino.c:~340-410 | `set_error`, `set_error_at`, `clear_error`, `append_trace`, `push_frame`, `pop_frame`, `type_tag_str` |
| `src/runtime_gc.c` | mino.c:~200-1050 | `gc_alloc_typed`, `alloc_val`, `dup_n`, `gc_collect`, `gc_mark_*`, `gc_sweep`, `gc_scan_stack`, range index, `gc_note_host_frame` |
| `src/runtime_env.c` | mino.c:~415-560 | `env_alloc`, `env_child`, `env_root`, `env_find_here`, `env_bind`, `mino_env_new`, `mino_env_free`, `mino_env_clone`, `mino_env_set`, `mino_env_get`, `dyn_lookup` |
| `src/eval_core.c` | mino.c:~2410-2635,~2632-2900 | `eval_impl` (dispatch + trampoline), `eval`, `eval_value`, `eval_implicit_do`, `apply_callable`, `eval_args`, `macroexpand*`, `lazy_force` |
| `src/eval_special_defs.c` | mino.c:~1687-1850 | `eval_def`, `eval_defmacro`, `eval_declare` |
| `src/eval_special_bindings.c` | mino.c:~1846-1580,~1327-1580 | `eval_let`, `eval_binding`, `bind_form`, `bind_params`, destructuring helpers |
| `src/eval_special_control.c` | mino.c:~2106-2408 | `eval_try`, `eval_binding` (dynamic), `eval_loop` |
| `src/eval_special_fn.c` | mino.c:~1905-2105 | `eval_fn`, `find_arity_clause`, `list_len`, `param_arity` |
| `src/runtime_repl.c` | mino.c:~3120-3200 | `mino_repl_new`, `mino_repl_feed`, `mino_repl_free` |
| `src/runtime_loader.c` | mino.c:~3050-3120 | `mino_load_file`, `mino_eval_string`, module require support |

### From `src/prim.c` (target: < 700 LOC composition)

| New file | Extracted from | Contents |
|----------|---------------|----------|
| `src/prim_numeric.c` | prim.c:17-475 | Arithmetic, bitwise, coercion, math functions |
| `src/prim_meta.c` | prim.c:~962-1090 | `meta`, `with-meta`, `vary-meta`, `alter-meta!` |
| `src/prim_collections.c` | prim.c:~1134-2030 | List, vector, map, set primitives (car, cdr, cons, count, nth, first, rest, assoc, get, conj, keys, vals, hash-set, contains?, disj, dissoc) |
| `src/prim_sequences.c` | prim.c:~2080-2560 | Sequence iterator, reduce, into, apply, reverse, sort, rangev, mapv, filterv, reduced |
| `src/prim_string.c` | prim.c:~510-710,~2693-3025 | str, pr-str, format, read-string, subs, split, join, starts-with?, ends-with?, includes?, upper-case, lower-case, trim, char-at |
| `src/prim_regex.c` | prim.c:~3407-3470 | re-find, re-matches |
| `src/prim_io.c` | prim.c:~3175-3400,~3466-3478 | println, prn, slurp, spit, exit, time-ms |
| `src/prim_stateful.c` | prim.c:~3851-3947 | atom, deref, reset!, swap!, atom? |
| `src/prim_reflection.c` | prim.c:~3025-3065,~3214-3273,~3479-3670 | type, name, doc, source, apropos, macroexpand, gensym, eval, symbol, keyword, hash, compare, rand, throw, require |
| `src/prim_install.c` | prim.c:~3675-4091 | `install_core_mino`, `mino_install_core`, `mino_install_io`, registration tables |

### From `src/mino_internal.h`

| New file | Contents |
|----------|----------|
| `src/runtime_state.h` | `mino_state_t` struct definition, GC_SAVE_MAX, gc_pin/gc_unpin |
| `src/runtime_error.h` | Error function declarations |
| `src/runtime_gc.h` | GC function declarations, gc_hdr_t, gc_range_t |
| `src/runtime_env.h` | env_binding_t, mino_env struct, env function declarations |
| `src/eval_core.h` | Evaluator function declarations |

The monolithic `mino_internal.h` remains as a convenience include that
pulls in all internal headers. State alias macros will be gradually removed
as modules switch to explicit `S->field` access.

### Unchanged files

These files are already well-scoped and need no extraction:

- `src/val.c` (499 LOC) -- constructors, interning, hashing
- `src/vec.c` (311 LOC) -- persistent vector
- `src/map.c` (642 LOC) -- HAMT
- `src/read.c` (795 LOC) -- reader (P4 will split internally)
- `src/print.c` (221 LOC) -- printer (P4 will split internally)
- `src/clone.c` (643 LOC) -- clone/mailbox/actors (P5 will extract mutex shims)
- `src/re.c` (544 LOC) -- regex engine
- `src/re.h` (69 LOC) -- regex header
- `src/mino.h` (562 LOC) -- public API
- `main.c` (168 LOC) -- CLI

## Dependency Rules

After decomposition, the include graph should be:

```
main.c -> mino.h
src/*.c -> mino_internal.h -> mino.h
                           -> runtime_state.h
                           -> runtime_gc.h
                           -> runtime_env.h
                           -> runtime_error.h
                           -> eval_core.h
```

**No circular includes.** Each internal header is self-contained with
forward declarations where needed.

**No cross-domain static calls.** If a prim_* file needs evaluator
functionality, it calls through the declared interface in eval_core.h,
not through a static function.

## Execution Notes

- P1 moves code mechanically first (no behavior change), then rewires
  internal call sites, then optionally cleans up locally.
- P2 follows the same pattern for primitives.
- Each extraction step must pass `make test`, `make test-fault-inject`,
  `make test-regex-thread` before committing.
