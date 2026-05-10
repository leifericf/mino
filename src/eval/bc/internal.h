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
    /* Phase 2 scaffolding: full handlers land alongside compile-time
     * emission once macro detection and tail-call discipline are
     * tightened up. The opcode IDs are reserved now so the encoding
     * is stable for the cycle. */
    OP_PUSHCATCH,      /* A=handler_pc_offset (sBx packed in Bx), B=reg_top_save */
    OP_POPCATCH,       /*                                                  */
    OP_THROW,          /* A=err                                            */
    OP_PUSHDYN,        /* A=var_k, B=val                                   */
    OP_POPDYN,         /* A=count                                          */
    OP_MAKE_LAZY,      /* A=dst, Bx=thunk const index                      */
    OP__COUNT
} mino_bc_op_t;

/* Sub-op nibble for OP_BINOP_INT (encoded in the C operand high bits is
 * not portable across 8-bit C; we reuse the 8-bit C slot directly when
 * sub-op fits in 4 bits and the operand range is small). Phase 1 stores
 * the sub-op in a dedicated 8-bit byte by re-purposing the C operand's
 * upper nibble. To keep things simple we use a separate field: pack the
 * sub-op into the instruction's top 8 bits by extending the encoding —
 * the C operand stays in the conventional position. The encoder helper
 * MK_BINOP_INT below hides this. */
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
typedef struct mino_bc_fn {
    mino_bc_insn_t  *code;        /* instruction stream */
    size_t           code_len;
    mino_val_t     **consts;      /* const pool: nil/true/false/symbols/literals/child fns */
    size_t           consts_len;
    int              n_regs;      /* number of register slots the body needs */
    int              n_params;    /* fixed param count (Phase 1 only) */
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

/* True iff the val's bc slot was populated with a real compiled program
 * (as opposed to NULL = not yet tried, or &mino_bc_declined = declined).
 * The macro parameter is named `v` so it does not collide with the
 * `.fn.bc` field access in callers that have a local named `fn`. */
#define MINO_BC_RUNNABLE(v) \
    ((v)->as.fn.bc != NULL && (v)->as.fn.bc != &mino_bc_declined)

#endif /* MINO_EVAL_BC_INTERNAL_H */
