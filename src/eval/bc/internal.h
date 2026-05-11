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

#include "mino.h"

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
    /* Specialization opcodes. The intent was that the dispatch loop
     * would rewrite generic OP_GETGLOBAL / OP_CALL / OP_BINOP_INT
     * sites to these after observing stable runtime shapes; in
     * practice the compiler emits the per-op int specializations
     * directly when the head matches a known prim, so the
     * _CACHED variants are reserved for future use. */
    OP_GETGLOBAL_CACHED, /* A=dst, Bx=ic slot index                        */
    OP_CALL_CACHED,      /* A=fn, B=argc, C=ret_base; cached callable      */
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
    OP_GET_KW_MAP,       /* A=dst, B=map, C=kw                             */
    OP_NTH_VEC,          /* A=dst, B=vec, C=index                          */
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
    BINOP__COUNT
} mino_bc_binop_t;

typedef enum {
    UNOP_INC = 0,
    UNOP_DEC,
    UNOP_ZERO_P,
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

/* Compiled function record. Owned by its parent MINO_FN value. Lifetime
 * matches the fn: the GC walks consts as a root via the parent fn's
 * mark pass. code is a GC_T_RAW buffer of uint32_t; consts is a
 * GC_T_PTRARR of mino_val_t pointers. */
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
