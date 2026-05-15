/*
 * eval/bc/internal.h -- register-based bytecode VM internals.
 *
 * Layered on top of the tree-walker, not a replacement. mino_bc_compile_fn
 * attempts to compile a MINO_FN body; on success the resulting program is
 * cached in MINO_FN.bc and apply_callable dispatches through mino_bc_run.
 * On any unsupported shape the compiler returns MINO_BC_UNSUPPORTED and
 * the call falls back to the tree-walker.
 *
 * Var indirection discipline: every reference to a global name goes
 * through the var cell with the IC version snapshot. Compile time
 * never closes over a fn value.
 */

#ifndef MINO_EVAL_BC_INTERNAL_H
#define MINO_EVAL_BC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "mino_internal.h"

/* 32-bit fixed-width instruction word. ABC form is op|A|B|C (8/8/8/8);
 * ABx form is op|A|Bx (8/8/16); AsBx is the signed variant of ABx
 * biased by 0x8000 so a 0 offset encodes the no-op jump. */
typedef uint32_t mino_bc_insn_t;

typedef enum {
    OP_NOP = 0,
    OP_MOVE,           /* A=dst, B=src                                    */
    OP_LOAD_K,         /* A=dst, Bx=const index                           */
    OP_GETGLOBAL,      /* A=dst, Bx=symbol const index                    */
    OP_SETGLOBAL,      /* A=src, Bx=symbol const index                    */
    OP_JMP,            /* sBx=offset (added to pc after the jump fetch)   */
    OP_JMPIFNOT,       /* A=test, sBx=offset                              */
    OP_CALL,           /* A=fn, B=argc, C=ret_base; args at A+1..A+B      */
    OP_TAILCALL,       /* A=fn, B=argc                                    */
    OP_RETURN,         /* A=src                                           */
    OP_CLOSURE,        /* A=dst, Bx=child fn const index                  */
    OP_BINOP_INT,      /* A=dst, B=lhs, C=rhs; op nibble in instr top byte */
    /* Opcode IDs reserved for forms the compiler doesn't yet emit:
     * try/catch/throw, dynamic binding push/pop. Handlers land alongside
     * compile-time emission in a later release; reserving the IDs now
     * keeps the encoding stable. */
    OP_PUSHCATCH,      /* A=handler_pc_offset (sBx packed in Bx), B=reg_top_save */
    OP_POPCATCH,       /*                                                  */
    OP_THROW,          /* A=err                                            */
    OP_PUSHDYN,        /* A=var_k, B=val                                   */
    OP_POPDYN,         /* A=count                                          */
    OP_MAKE_LAZY,      /* A=dst, Bx=thunk const index                      */
    /* Specialization opcodes. OP_GETGLOBAL_CACHED + OP_CALL_CACHED
     * implement the same inline-cache discipline at two granularities:
     * GETGLOBAL_CACHED caches a name -> value resolution per ic_slot,
     * CALL_CACHED fuses the resolution and the dispatch so a hot
     * `(foo ...)` site keeps one slot lookup instead of two
     * (GETGLOBAL_CACHED + OP_CALL). Encoding: word-1 is A=arg_base,
     * B=argc, C=dst (args at regs[A..A+B-1], no fn-reg shift). Word-2
     * carries the slot index in Bx; the handler consumes it via
     * code[pc++] so the main dispatch never sees it as a free-standing
     * op. Per-op int specializations (OP_ADD_II etc.) are still emitted
     * directly at compile time. */
    OP_GETGLOBAL_CACHED, /* A=dst, Bx=ic slot index                        */
    OP_CALL_CACHED,      /* A=arg_base, B=argc, C=dst; word-2 carries slot */
    /* Protocol-method dispatch fast lane. Same two-word encoding as
     * OP_CALL_CACHED (ABC + slot index in word-2's Bx). At compile time,
     * the head of `(area c)` is recognized as a protocol method when its
     * resolved value is a fn whose body is the single form
     * `(protocol-dispatch <dispatch-atom> "<mname>" & params)` (the
     * macroexpansion of (defprotocol P (area [s])) in src/core.clj).
     * The IC slot stores the dispatch atom pointer fixed at compile;
     * the cached map/type/impl triple is filled on miss. The hot path
     * derefs the atom and pointer-compares (atom_val == cached_map &&
     * type_disc == cached_type); on hit it calls cached_impl directly
     * via apply_callable_argv with no cons-spine and no intermediate
     * dispatcher trampoline. */
    OP_PROTOCOL_CALL_CACHED, /* A=arg_base, B=argc, C=dst; word-2 carries slot */
    /* Tail-position twin. Same encoding (ABC + word-2 slot index),
     * but the handler hands the resolved impl + arg slice to the
     * MINO_TAIL_CALL sentinel so the apply_callable trampoline
     * continues dispatching without growing the C stack. C is unused
     * (the call's result never lands in a register; the trampoline
     * returns it from the enclosing fn). */
    OP_PROTOCOL_TAILCALL_CACHED, /* A=arg_base, B=argc; word-2 carries slot   */
    OP_ADD_II,           /* A=dst, B=lhs, C=rhs; int+int add              */
    OP_SUB_II,           /* "                                              */
    OP_MUL_II,           /* "                                              */
    OP_LT_II,            /* int < int                                      */
    OP_LE_II,            /* int <= int                                     */
    OP_GT_II,            /* int > int                                      */
    OP_GE_II,            /* int >= int                                     */
    OP_EQ_II,            /* int == int                                     */
    OP_INC_I,            /* int inc fast lane: A=dst, B=src                */
    OP_DEC_I,            /* int dec fast lane: A=dst, B=src                */
    OP_ZERO_INT_P,       /* int zero? fast lane: A=dst, B=src              */
    /* Extended int+int binary fast lanes. Same ABC shape as OP_ADD_II;
     * each has a dedicated fallback prim for the type-miss / div-zero /
     * shift-bounds path so the slow case still produces the correct
     * Clojure-semantics diagnostic. */
    OP_MOD_II,           /* int mod  int                                   */
    OP_QUOT_II,          /* int quot int                                   */
    OP_REM_II,           /* int rem  int                                   */
    OP_BAND_II,          /* int bit-and int                                */
    OP_BOR_II,           /* int bit-or  int                                */
    OP_BXOR_II,          /* int bit-xor int                                */
    OP_SHL_II,           /* int bit-shift-left   int                       */
    OP_SHR_II,           /* int bit-shift-right  int                       */
    OP_USHR_II,          /* int unsigned-bit-shift-right int               */
    /* Extended int unary fast lanes. Same shape as OP_INC_I. */
    OP_POS_P_I,          /* int pos?   fast lane                           */
    OP_NEG_P_I,          /* int neg?   fast lane                           */
    OP_EVEN_P_I,         /* int even?  fast lane                           */
    OP_ODD_P_I,          /* int odd?   fast lane                           */
    OP_BNOT_I,           /* int bit-not fast lane                          */
    /* Immediate-operand variants. ABC form: A=dst, B=lhs reg, C=signed
     * 8-bit immediate (in [-128, 127]). The compiler folds small int
     * literals into the C slot so a `(< i 10)` / `(+ i 2)` etc. avoids
     * the OP_LOAD_K + extra register slot for the literal. */
    OP_ADD_IK,           /* A=dst, B=lhs reg, sC=signed imm                */
    OP_SUB_IK,           /* "                                              */
    OP_LT_IK,            /* "                                              */
    OP_LE_IK,            /* "                                              */
    OP_EQ_IK,            /* "                                              */
    OP_GET_KW_MAP,       /* A=dst, B=map, C=kw                             */
    OP_NTH_VEC,          /* A=dst, B=vec, C=index                          */
    /* Write-side fast lanes. Mirrors of OP_NTH_VEC / OP_GET_KW_MAP.
     * OP_CONJ_VEC fires on `(conj v x)` when v is a vector at runtime;
     * misses fall back to prim_conj so list / set / record / sorted-coll
     * conj keep their full semantics. OP_ASSOC takes the 3-arg
     * `(assoc coll k v)` shape: A=dst, B=base; the call-site allocates
     * three consecutive regs for [coll, k, v] starting at base. Runtime
     * dispatches to vec_assoc1 (when coll is a vector and k is an
     * in-range int index) or mino_map_assoc1 (when coll is a map);
     * anything else (sorted-map, transient, record, variadic forms)
     * falls back to prim_assoc. The compiler only emits OP_ASSOC for
     * arity-3 calls; longer forms keep the OP_CALL path. */
    OP_CONJ_VEC,         /* A=dst, B=vec, C=item                           */
    OP_ASSOC,            /* A=dst, B=base; coll/k/v at regs[B..B+2]        */
    OP_DISSOC,           /* A=dst, B=map, C=key; arity-2 dissoc fast lane  */
    /* Read-side small-prim fast lanes. ABC form, A=dst, B=src_reg.
     * Fast path requires MINO_VECTOR; misses fall back to the
     * canonical prim so lazy seqs, chunked conses, strings, maps,
     * sets, sorted-colls, and host arrays keep their full semantics. */
    OP_FIRST_VEC,        /* A=dst, B=vec; nil on empty, vec[0] otherwise   */
    OP_COUNT_VEC,        /* A=dst, B=vec; tagged-int .len                  */
    OP_EMPTY_VEC,        /* A=dst, B=vec; true when .len==0, else false    */
    /* Fused counted-loop opcodes. Emitted at the (loop ...) entry pc
     * when the body matches a shape the compiler can specialize:
     *   (loop [i 0] (if (zero? i) <exit> (recur (dec i))))
     *   (loop [i 0 j N]
     *     (if (zero? j) <exit> (recur (inc i) (dec j))))
     * The op IS the loop entry; on each iteration it tests the
     * decrementing register, decrements (and optionally increments
     * the other register), and back-jumps to itself with offset -1.
     * On zero, it falls through to the compiled exit branch. All ops
     * preserve Clojure-correct semantics: tagged int discipline plus
     * overflow check on the increment side. The compiler declines to
     * emit these opcodes if the inputs aren't statically guaranteed to
     * stay int-typed, so the runtime cost stays one decode + two
     * tagged-int checks per iteration. */
    OP_LOOP_INT_DEC,     /* A=test_reg (decremented). One-binding form.  */
    OP_LOOP_INT_DEC_INC, /* A=test_reg (decremented), B=inc_reg (incremented). */
    /* Forward-counted variants. Recognise the dominant Clojure-canon
     * loop shape:
     *   (loop [i 0] (if (< i N) (recur (inc i)) <exit>))
     *   (loop [i 0 j 0] (if (< i N) (recur (inc i) (inc j)) <exit>))
     * The op IS the loop entry; on each iteration it tests A < B (or
     * an equivalent rearrangement), increments A (and optionally C),
     * and back-jumps. On exit (test false) it falls through to the
     * compiled exit branch. Slow paths delegate to prim_lt / prim_inc
     * so the canonical diagnostic still fires on non-int / overflow. */
    OP_LOOP_INT_LT,      /* A=counter, B=limit. One-binding form.       */
    OP_LOOP_INT_LT_INC,  /* A=counter, B=limit, C=inc-carry. Two-binding. */
    /* Lexical-env management for compiled closures. OP_PUSH_ENV and
     * OP_POP_ENV bracket let scopes when the enclosing fn captures
     * (its body contains an inner fn literal); OP_ENV_BIND publishes
     * a let-binding or fn-param into the current env so OP_CLOSURE
     * picks it up via the env chain. Fns without inner fns skip
     * these ops entirely and keep their bindings register-only. */
    OP_PUSH_ENV,         /*                                                  */
    OP_POP_ENV,          /*                                                  */
    OP_ENV_BIND,         /* A=src reg, Bx=name symbol const idx              */
    OP__COUNT
} mino_bc_op_t;

/* Sub-op enum for the generic OP_BINOP_INT opcode. The compiler emits
 * the per-op OP_*_II variants directly today; this enum is retained
 * so the dispatch helpers in vm.c can share one switch across both
 * the generic and specialized lanes. */
typedef enum {
    BINOP_ADD = 0,
    BINOP_SUB,
    BINOP_MUL,
    BINOP_LT,
    BINOP_LE,
    BINOP_GT,
    BINOP_GE,
    BINOP_EQ,
    BINOP_MOD,
    BINOP_QUOT,
    BINOP_REM,
    BINOP_BAND,
    BINOP_BOR,
    BINOP_BXOR,
    BINOP_SHL,
    BINOP_SHR,
    BINOP_USHR,
    BINOP__COUNT
} mino_bc_binop_t;

typedef enum {
    UNOP_INC = 0,
    UNOP_DEC,
    UNOP_ZERO_P,
    UNOP_POS_P,
    UNOP_NEG_P,
    UNOP_EVEN_P,
    UNOP_ODD_P,
    UNOP_BNOT,
    UNOP__COUNT
} mino_bc_unop_t;

/* Encoding helpers. The base ABC form occupies 24 bits; the high 8 bits
 * are zero for ABC ops and used for the BINOP sub-op when OP_BINOP_INT.
 * Bx is unsigned 16-bit, sBx is signed via 0x8000 bias. */
#define MK_ABC(op, a, b, c)                                                 \
    ((mino_bc_insn_t)((mino_bc_op_t)(op) & 0xFFu)                           \
     | ((mino_bc_insn_t)((a) & 0xFFu) << 8)                                 \
     | ((mino_bc_insn_t)((b) & 0xFFu) << 16)                                \
     | ((mino_bc_insn_t)((c) & 0xFFu) << 24))

#define MK_ABx(op, a, bx)                                                   \
    ((mino_bc_insn_t)((mino_bc_op_t)(op) & 0xFFu)                           \
     | ((mino_bc_insn_t)((a) & 0xFFu) << 8)                                 \
     | ((mino_bc_insn_t)((bx) & 0xFFFFu) << 16))

#define MK_AsBx(op, a, sbx)  MK_ABx((op), (a), (unsigned)((sbx) + 0x8000))

#define MK_BINOP_INT(a, b, c, subop)                                        \
    (MK_ABC(OP_BINOP_INT, (a), (b), (c))                                    \
     | ((mino_bc_insn_t)((subop) & 0xFu) << 4))

#define OP_OF(i)    ((unsigned)((i) & 0xFFu))
#define A_OF(i)     ((unsigned)(((i) >> 8) & 0xFFu))
#define B_OF(i)     ((unsigned)(((i) >> 16) & 0xFFu))
#define C_OF(i)     ((unsigned)(((i) >> 24) & 0xFFu))
#define Bx_OF(i)    ((unsigned)(((i) >> 16) & 0xFFFFu))
#define sBx_OF(i)   ((int)((int)Bx_OF(i) - 0x8000))
#define BINOP_OF(i) ((unsigned)(((i) >> 4) & 0xFu))

/* Per-pc source position. One slot per instruction word; populated by
 * the compiler from each form's cons metadata. Used by error reporting
 * to report the exact instruction's source line (more precise than the
 * call-site line in the call frame), and consulted by future native
 * tiers that need PC -> source line mapping for stack traces in JIT'd
 * code. line == 0 means "unknown / no source"; column == 0 likewise. */
typedef struct mino_bc_source_pos {
    int line;
    int column;
} mino_bc_source_pos_t;

/* Per-fn source-map side table. positions is allocated GC_T_RAW (POD,
 * no pointer scan); len matches code_len after compile. file is a
 * borrowed pointer into the runtime's interned-filename pool (lifetime
 * matches the state's lifetime, so no GC tracking needed). */
typedef struct mino_bc_source_map {
    mino_bc_source_pos_t *positions;
    size_t                len;
    const char           *file;
} mino_bc_source_map_t;

/* Compiled function record. Owned by its parent MINO_FN value. Lifetime
 * matches the fn: the GC walks consts as a root via the parent fn's
 * mark pass. code is a GC_T_RAW buffer of uint32_t; consts is a
 * GC_T_PTRARR of mino_val_t pointers. */
/* Inline-cache slot for OP_GETGLOBAL_CACHED / OP_CALL_CACHED, repurposed
 * for OP_PROTOCOL_CALL_CACHED through a `kind` discriminator. Per-fn
 * array indexed by the Bx field of the cached opcode. For the GLOBAL
 * kind: `sym` is the unqualified or qualified MINO_SYMBOL whose
 * resolution this slot stands for, `cached` is the last resolved value
 * (NULL = uninitialized / invalidated), and `gen` is the S->ic_gen
 * snapshot at fill -- when ic_gen advances (def / ns-unmap /
 * var_set_root / var_unintern) the cache misses on its next read. For
 * the PROTOCOL kind: `atom` is the dispatch atom captured at compile
 * time, `cached_map` is the @atom map pointer at fill, `cached_type`
 * is the type-disc pointer (MINO_TYPE for records, interned keyword
 * for built-ins), `cached` is the impl fn, `sym` is the method-name
 * string used in the diagnostic on a missing impl, and `gen` is
 * repurposed as the miss counter (megamorphic at >= 16 misses, after
 * which the slot bails to the slow dispatcher). The fn-value owns the
 * slots array (one bc per fn-value), so env stays constant across
 * calls and does not need to be part of the cache key. */
typedef enum {
    MINO_BC_IC_GLOBAL   = 0,
    MINO_BC_IC_PROTOCOL = 1
} mino_bc_ic_kind_t;

typedef struct mino_bc_ic_slot {
    mino_val_t   *sym;
    mino_val_t   *cached;
    unsigned      gen;
    unsigned char kind;
    /* PROTOCOL-only fields. Zero / NULL when kind == MINO_BC_IC_GLOBAL. */
    mino_val_t   *atom;
    mino_val_t   *cached_map;
    mino_val_t   *cached_type;
} mino_bc_ic_slot_t;

/* One arity clause. Multi-arity fns carry an array of these, one per
 * (params body...) clause; single-arity fns degenerate to a one-entry
 * array with entry_pc = 0. The compiler emits each clause's body
 * sequentially into the shared code stream; the runtime picks the
 * matching clause at fn entry and copies argv into the first
 * `n_params` registers (plus a collected rest list when has_rest). */
typedef struct mino_bc_clause {
    int          n_params;        /* fixed args this clause accepts */
    int          has_rest;        /* 1 iff trailing `& rest` binding */
    int          entry_pc;        /* code offset to start running from */
    mino_val_t  *params_vec;      /* MINO_VECTOR of param syms (incl. & and
                                   * the rest sym when has_rest); used by
                                   * the runtime to env_bind names when the
                                   * clause's fn captures */
} mino_bc_clause_t;

typedef struct mino_bc_fn {
    mino_bc_insn_t  *code;        /* instruction stream */
    size_t           code_len;
    mino_val_t     **consts;      /* const pool: nil/true/false/symbols/literals/child fns */
    size_t           consts_len;
    int              n_regs;      /* number of register slots the body needs */
    int              n_params;    /* fixed param count of clauses[0]; the
                                   * single-arity fast path in mino_bc_run
                                   * reads this directly without indexing
                                   * the clauses array */
    int              has_rest;    /* 1 iff clauses[0] ends in `& rest` --
                                   * argv overflow past n_params is
                                   * collected into a list and placed in
                                   * regs[n_params] at entry */
    int              n_clauses;   /* >= 1; 1 for single-arity, N for multi */
    mino_bc_clause_t *clauses;    /* n_clauses entries; clauses[0] mirrors
                                   * n_params / has_rest for the single-
                                   * arity fast path */
    int              captures;    /* 1 iff body contains inner fn literal --
                                   * forces env_child + OP_ENV_BIND of params
                                   * at entry, and let scopes bracket their
                                   * bindings with OP_PUSH_ENV / OP_POP_ENV */
    int              has_folds;   /* 1 iff the compile resolved a
                                   * literal-arg call against a core
                                   * pure prim and emitted OP_LOAD_K
                                   * with the folded result. Soundness
                                   * tracker: the fold is valid only
                                   * while the dependent var bindings
                                   * stay the way they were at fold
                                   * time, observed via compile_ic_gen
                                   * below. */
    unsigned         compile_ic_gen; /* S->ic_gen at end of compile.
                                      * apply_callable invalidates the
                                      * bc and reruns the compile if a
                                      * mismatch is observed at call
                                      * time AND has_folds is set. */
    mino_bc_ic_slot_t *ic_slots;  /* OP_GETGLOBAL_CACHED slot array;
                                   * Bx of each cached op indexes here.
                                   * Allocated as GC_T_RAW (POD slots
                                   * plus value pointers walked by the
                                   * MINO_FN GC pass). */
    int              ic_slots_len;
    int              ic_slots_cap;
    mino_bc_source_map_t source_map; /* per-pc source positions; len
                                      * matches code_len when present */
} mino_bc_fn_t;

/* Compile / run status. Returned from mino_bc_compile_fn and consulted
 * by apply_callable to decide between the bc dispatch and the
 * tree-walker fallback. */
typedef enum {
    MINO_BC_OK          = 0,
    MINO_BC_UNSUPPORTED = -1,    /* Form shape not yet handled by compiler. */
    MINO_BC_ERROR       = -2     /* Compile failed unexpectedly; same fallback. */
} mino_bc_status_t;

/* Public entry points. */
int mino_bc_compile_fn(struct mino_state *S, mino_val_t *fn);
mino_val_t *mino_bc_run(struct mino_state *S, mino_val_t *fn,
                        mino_val_t **argv, int argc, mino_env_t *env);

/* GC hook: walk a single mino_bc_fn_t's consts and child fns. Called from
 * gc_mark_runtime_globals (indirectly via the MINO_FN walker) and from
 * the closure-build path so a partially-constructed bc fn stays rooted
 * across allocations. */
void mino_bc_fn_mark(struct mino_state *S, const mino_bc_fn_t *bc);

/* Look up the source position recorded for a given pc. Returns 1 with
 * the position filled in if available, 0 if no source info is recorded
 * (uncompiled fn, pc out of range, or position with line == 0). */
int mino_bc_source_lookup(const mino_bc_fn_t *bc, size_t pc,
                          const char **out_file, int *out_line,
                          int *out_column);

/* Sentinel placed in MINO_FN.bc after a failed compile attempt so the
 * next call doesn't re-attempt compilation. apply_callable sees this
 * pointer and routes straight to the tree-walker. The fields are zero
 * (code == NULL, code_len == 0, consts == NULL, ...). */
extern const mino_bc_fn_t mino_bc_declined;

/* Debug knob: set non-zero (e.g., via the MINO_BC_REQUIRE env var or a
 * future Clojure-level setter) to abort on any tree-walker fallback.
 * Useful for VM development: an unintended decline is loud instead of
 * silently degrading. Defaults to 0; production builds leave it off. */
extern int mino_bc_require_flag;
void mino_bc_check_require(struct mino_state *S, mino_val_t *fn);

/* True iff the val's bc slot was populated with a real compiled program
 * (as opposed to NULL = not yet tried, or &mino_bc_declined = declined).
 * The macro parameter is named `v` so it does not collide with the
 * `.fn.bc` field access in callers that have a local named `fn`. */
#define MINO_BC_RUNNABLE(v) \
    ((v)->as.fn.bc != NULL && (v)->as.fn.bc != &mino_bc_declined)

#endif /* MINO_EVAL_BC_INTERNAL_H */
