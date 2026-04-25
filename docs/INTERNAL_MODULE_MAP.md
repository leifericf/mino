# Internal Module Map

Source layout under `src/`. Each module has a single responsibility.
State access is explicit (`S->field`).

## Directory Layout

```
src/
├── mino.h                         # public embedding API (stable surface)
├── core.mino                      # bundled mino-side core library
├── core_mino.h                    # generated from core.mino
│
├── public/                        # host-facing C API
├── runtime/                       # state, env, vars, errors, modules (+ internal.h)
├── gc/                            # generational + incremental collector (+ internal.h)
├── eval/                          # evaluator + reader + printer (+ internal.h)
├── collections/                   # val, vec, map, rbtree, transient, clone (+ internal.h)
├── prim/                          # primitive registration tables (+ internal.h)
├── async/                         # scheduler + timers (+ internal.h)
├── interop/                       # host interop syntax (+ internal.h)
├── regex/                         # self-contained regex engine
├── diag/                          # diagnostic kinds + reporting
└── vendor/imath/                  # MIT-licensed bignum (vendored)
```

## Runtime Core

| File | Responsibility |
|------|----------------|
| `src/eval/eval.c` | Evaluator front door: `mino_eval`, `mino_eval_string`, `mino_load_file`, `mino_call`, `mino_pcall`, REPL, core eval helpers (`eval_value`, `eval_implicit_do`, `eval_args`, `macroexpand*`, `quasiquote_expand`, `lazy_force`) |
| `src/eval/special.c` | Eval dispatch (`eval_impl`, `eval`), literal evaluation (symbol/vector/map/set), `ns`, `var` |
| `src/eval/defs.c` | Definition special forms: `def`, `defmacro`, `declare` |
| `src/eval/bindings.c` | Destructuring (`bind_form`, `bind_params`), `let`, `loop`, `binding` |
| `src/eval/control.c` | `try`/`catch`/`finally` |
| `src/eval/fn.c` | `fn`, `apply_callable`, multi-arity dispatch |
| `src/runtime/state.c` | State lifecycle (`mino_state_new`, `mino_state_free`), limits, interrupt, env public API (`mino_env_new`, `mino_env_free`, `mino_env_clone`, `mino_new`), refs, fault injection setup |
| `src/gc/driver.c` | Allocation driver (`gc_alloc_typed`, `alloc_val`, `dup_n`), shared mark-stack primitives, driver tick that picks minor vs. major vs. incremental-major-slice, STW orchestrator (`gc_major_collect`), force-finish helper |
| `src/gc/roots.c` | Root enumeration, conservative stack scan (`gc_scan_stack`, `gc_mark_roots`), range index over live headers for interior-pointer resolution |
| `src/gc/minor.c` | Minor (nursery) collector: marks YOUNG-reachable via roots+remset, sweeps dead YOUNG, promotes surviving YOUNG to OLD, nests safely inside an active major |
| `src/gc/major.c` | Major collector state machine: `gc_major_begin`, `gc_major_step`, `gc_major_remark`, `gc_major_sweep_phase`; OLD sweep that preserves YOUNG survivors |
| `src/gc/barrier.c` | Write barrier fast/slow path, SATB old-value push during MAJOR_MARK, remembered set (add + purge dead), singleton pointer filter |
| `src/gc/trace.c` | GC tracing hooks for diagnostics |
| `src/public/gc.c` | Host-facing GC API: `mino_gc_collect` (MINOR/MAJOR/FULL), `mino_gc_set_param` (range-validated tuning knobs), `mino_gc_stats` (out-struct populate) |
| `src/public/embed.c` | Host-facing embedding API surface |
| `src/runtime/env.c` | Internal environment ops (`env_alloc`, `env_child`, `env_root`, `env_bind`, `env_find_here`), root-env registry, dynamic binding lookup |
| `src/runtime/error.c` | Error reporting (`set_error`, `set_error_at`, `clear_error`), call stack (`push_frame`, `pop_frame`, `append_trace`), `type_tag_str`, metadata table |
| `src/runtime/var.c` | Var registry (`var_intern`, `var_find`, `var_set_root`) |
| `src/runtime/module.c` | Module/namespace machinery |

## Primitives

| File | Responsibility |
|------|----------------|
| `src/prim/prim.c` | Shared helpers (`prim_throw_error`, `print_to_string`, seq iterator), `install_core_mino`, `mino_install_core`, `mino_install_io` |
| `src/prim/numeric.c` | Arithmetic (`+`, `-`, `*`, `/`, `mod`, `rem`, `quot`), coercion (`int`, `float`), bitwise ops, math functions, comparison (`=`, `<`, `compare`, `identical?`) |
| `src/prim/bignum.c` | Arbitrary-precision integer/ratio/decimal primitives backed by vendored imath |
| `src/prim/collections.c` | List/vector/map/set primitives (`car`, `cdr`, `cons`, `count`, `nth`, `first`, `rest`, `assoc`, `get`, `conj`, `keys`, `vals`, `hash-set`, `contains?`, `disj`, `dissoc`, `seq`, `realized?`) |
| `src/prim/sequences.c` | Sequence operations (`reduce`, `into`, `apply`, `reverse`, `sort`, `rangev`, `mapv`, `filterv`, `reduced`, `reduced?`) |
| `src/prim/string.c` | String primitives (`str`, `pr-str`, `format`, `read-string`, `subs`, `split`, `join`, `starts-with?`, `ends-with?`, `includes?`, `upper-case`, `lower-case`, `trim`, `char-at`) |
| `src/prim/io.c` | I/O primitives (`println`, `prn`, `slurp`, `spit`, `exit`, `time-ms`, `getenv`) |
| `src/prim/lazy.c` | Lazy sequence primitives implemented as C thunks (`range`, `lazy-map-1`, `lazy-filter`, `lazy-take`, `drop-seq`, `doall`, `dorun`) |
| `src/prim/reflection.c` | Reflection/utility (`type`, `name`, `eval`, `symbol`, `keyword`, `hash`, `gensym`, `macroexpand`, `throw`, `rand`, `resolve`, `namespace`, `var?`), `gc-stats`, `nano-time` |
| `src/prim/meta.c` | Metadata (`meta`, `with-meta`, `vary-meta`, `alter-meta!`) |
| `src/prim/regex.c` | Regex (`re-find`, `re-matches`) |
| `src/prim/stateful.c` | Atoms (`atom`, `deref`, `reset!`, `swap!`, `atom?`, `add-watch`, `remove-watch`, `set-validator!`, `get-validator`, `swap-vals!`, `reset-vals!`) |
| `src/prim/module.c` | Module system (`require`, `doc`, `source`, `apropos`, `mino_set_resolver`) |
| `src/prim/fs.c` | Filesystem primitives |
| `src/prim/proc.c` | Process / subprocess primitives |
| `src/prim/host.c` | Host interop primitives (`host/new`, `host/call`, `host/static-call`, `host/get`), capability registry |
| `src/interop/syntax.c` | Interop syntax desugaring (dot-method, field access, constructor, static calls) |

## Data Structures

| File | Responsibility |
|------|----------------|
| `src/collections/val.c` | Value constructors, interning, hashing, equality |
| `src/collections/vec.c` | Persistent vector (32-way trie) |
| `src/collections/map.c` | HAMT for maps and sets |
| `src/collections/rbtree.c` | Persistent left-leaning red-black tree for sorted maps and sets |
| `src/collections/transient.c` | Transient kernel for vec/map/set |

## Reader/Printer/Regex

| File | Responsibility |
|------|----------------|
| `src/eval/read.c` | Reader (tokenizer + form parser, reader conditionals, radix literals, tagged literals) |
| `src/eval/print.c` | Printer (value -> text, one switch dispatch) |
| `src/regex/re.c` | Regex engine (self-contained, thread-safe) |

## Cross-State

| File | Responsibility |
|------|----------------|
| `src/collections/clone.c` | Value cloning across mino_state_t instances (nil/bool/int/float/string/symbol/keyword/cons/vector/map/set). Host-facing `mino_clone` only. |

## Async

Channels, buffers, and alts arbitration live in `lib/core/channel.mino`.
The C surface is limited to the scheduler run queue, deadline timers,
and a four-primitive bridge.

| File | Responsibility |
|------|----------------|
| `src/async/scheduler.c` | Scheduler run queue and drain loop |
| `src/async/timer.c` | Deadline priority queue; enqueues callbacks on expiry |
| `src/prim/async.c` | Bridge primitives: `async-sched-enqueue*`, `async-schedule-timer*`, `drain!`, `drain-loop!` |

## Diagnostics

| File | Responsibility |
|------|----------------|
| `src/diag/diag.c` | Diagnostic kind taxonomy and reporting |
| `src/diag/diag.h` | Diagnostic kind enum + reporting API |

## Vendored Libraries

| File | Responsibility |
|------|----------------|
| `src/vendor/imath/imath.c` | MIT-licensed arbitrary-precision integer library |
| `src/vendor/imath/imath.h` | imath public header |

## Headers

| File | Contents |
|------|----------|
| `src/mino.h` | Public embedding API (stable surface) |
| `src/runtime/internal.h` | `mino_state` and `mino_env` structs, runtime-support types (`module_entry_t`, `meta_entry_t`, `call_frame_t`, `root_env_t`, `mino_ref`, `dyn_frame_t`, `dyn_binding_t`, `ns_alias_t`, `var_entry_t`, `env_binding_t`), runtime function declarations, ownership annotations. Includes the per-subsystem internal headers below. |
| `src/gc/internal.h` | GC types (`gc_hdr_t`, `gc_evt_t`, `gc_range_t`), enums (`GC_T_*`, `GC_GEN_*`, `GC_PHASE_*`, `GC_EVT_*`), `gc_pin`/`gc_unpin` macros, GC function declarations |
| `src/collections/internal.h` | Persistent collection types (`mino_vec_node`, `mino_hamt_node`, `mino_rb_node`, `hamt_entry_t`), `intern_table_t`, val.c constructors and equality, vec/HAMT/rbtree declarations, bignum/ratio/bigdec value support |
| `src/eval/internal.h` | `try_frame_t` + `MAX_TRY_DEPTH`, evaluator core helpers, macroexpand, quasiquote, `print_val`, `intern_filename` |
| `src/eval/special_internal.h` | Cross-domain declarations for evaluator special-form files |
| `src/interop/internal.h` | Host-interop capability registry types (`host_member_t`, `host_type_t`), `HOST_*` enum, lookup helpers |
| `src/async/internal.h` | Async umbrella (includes `async/scheduler.h` + `async/timer.h`) |
| `src/async/scheduler.h` | Scheduler types and public surface |
| `src/async/timer.h` | Timer types and public surface |
| `src/prim/internal.h` | Shared primitive helpers, `seq_iter_t`, per-domain primitive declarations |
| `src/regex/re.h` | Regex public header |

## Entry Point

| File | Responsibility |
|------|----------------|
| `main.c` | CLI: argument parsing, REPL, file execution |

## Dependency Directions

Allowed include directions (no cycles):

```
main.c -> runtime/internal.h -> mino.h

runtime/internal.h -> { gc/internal.h, collections/internal.h,
                        eval/internal.h, interop/internal.h,
                        async/internal.h, diag/diag.h, mino.h }

eval/{bindings,control,defs,fn,special}.c
                       -> eval/special_internal.h -> runtime/internal.h
prim/*.c               -> prim/internal.h         -> runtime/internal.h
prim/regex.c           -> prim/internal.h + regex/re.h
prim/bignum.c          -> prim/internal.h + vendor/imath/imath.h

runtime/*.c, gc/*.c, public/*.c, async/*.c, prim/async.c,
eval/eval.c, eval/read.c, eval/print.c,
collections/*.c, interop/syntax.c
                       -> runtime/internal.h

regex/re.c             -> regex/re.h (standalone, no mino headers)
vendor/imath/imath.c   -> vendor/imath/imath.h (standalone)
```

Includes are path-qualified (`#include "runtime/internal.h"` rather
than bare `#include "internal.h"`) because each subsystem owns its own
`internal.h`; bare includes would resolve based on `-I` flag order.

Rules:
- No circular includes.
- Primitive files access shared helpers through `prim/internal.h`.
- Evaluator special-form files access cross-domain helpers through `eval/special_internal.h`.
- Runtime core files include `runtime/internal.h`, which in turn includes the per-subsystem internal headers needed to define `mino_state`. Files that only touch a single subsystem can include just that subsystem's header.
- Only `prim/regex.c` includes `regex/re.h` (regex is isolated from the rest of the primitive layer).
- Only `prim/bignum.c` includes `vendor/imath/imath.h`.
- `prim/prim.c` includes `core_mino.h` (generated) for the core library bootstrap.

## How to Add a Primitive

1. Choose the domain file (`src/prim/numeric.c`, `src/prim/collections.c`, etc.).
2. Write the function with signature `mino_val_t *prim_name(mino_state_t *S, mino_val_t *args, mino_env_t *env)`.
3. Declare it in `src/prim/internal.h` under the appropriate domain section.
4. Register it in `mino_install_core()` or `mino_install_io()` in `src/prim/prim.c`.
5. Add tests in `tests/` and run `./mino task test`.

## How to Add a Special Form

1. Add the handler in the appropriate `src/eval/{bindings,control,defs,fn,special}.c` file by domain.
2. Declare it in `src/eval/special_internal.h` if it needs to be called from the dispatch.
3. Add the keyword check in `eval_impl`'s dispatch chain in `src/eval/special.c`.
4. Add tests. Special forms are recognized by the evaluator, not installed.
