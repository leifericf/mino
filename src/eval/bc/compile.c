/* ALLOW: src/eval/bc/compile.c 4783 LOC — single-pass bytecode compiler; the compilation context (compile_state_t) and all emission helpers are tightly coupled through the context pointer; decomposition would require threading the full context through every helper and does not reduce complexity */
/*
 * eval/bc/compile.c -- AST-to-bytecode compiler.
 *
 * Coverage:
 *   - literals (nil / bool / int / float / string / keyword / char)
 *   - constant vector literals (all-self-evaluating elements)
 *   - bare symbol refs (locals via register, globals via OP_GETGLOBAL)
 *   - (if c t e), (if c t)
 *   - (do e1 e2 ...)
 *   - (let [b v ...] body...) with plain-symbol bindings only
 *   - (loop [b v ...] body...) + (recur ...) with plain-symbol bindings
 *   - (quote x)
 *   - (lazy-seq body...) -- body realised on the tree-walker
 *   - function application: multi-arity, optional & rest, no destructure
 *   - inner (fn ...) / (fn* ...) literals via OP_CLOSURE
 *   - (def name) / (def name expr) without metadata
 *   - tail position propagated through if / do / let / loop so
 *     recursive arms emit OP_TAILCALL (constant C stack via trampoline)
 *   - speculative int fast lanes:
 *       binary: + - * < <= > >= = (OP_*_II)
 *       unary:  inc, dec, zero?   (OP_INC_I / OP_DEC_I / OP_ZERO_INT_P)
 *
 * Decline conditions (any of these -> MINO_BC_UNSUPPORTED, fn falls back
 * to the tree-walker):
 *   - params with destructuring / :as / map shape (& rest is accepted)
 *   - body uses (try/catch/finally), (throw), (binding), (set!)
 *   - body uses other not-yet-handled special forms (when, and, or,
 *     quasiquote, case, cond, defrecord, ...): they're typically
 *     macroexpanded before the compiler sees them, but raw occurrences
 *     decline so the tree-walker handles them rather than emitting a
 *     bogus regular call
 *   - call head resolves to a MINO_MACRO (macros expand on the eval
 *     path before reaching the compiler when applicable; a residual
 *     macro-head form declines)
 *   - jump offset out of 16-bit signed range
 *
 * Register allocation: single-pass stack-discipline plus
 * compile_operand_inplace for fast-lane operand positions. Locals are
 * read straight from their binding register without an extra MOVE; the
 * unary and binary fast lanes use independent B / C operand slots so
 * (op local local) compiles to a single instruction. Temps for sub-
 * expressions are allocated above the current high-water mark and
 * released by next_reg restore at scope end.
 *
 * Var-indirection discipline: every reference to a global name compiles
 * to OP_GETGLOBAL with the symbol in the const pool, never a baked
 * value. OP_SETGLOBAL bumps S->ns_vars.ic_gen so cached call sites invalidate.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mino.h"
#include "runtime/internal.h"
#include "eval/internal.h"          /* prim_destructure, eval_impl, apply_callable */
#include "eval/bc/internal.h"       /* build_multi_arity_clauses, prim_* fwd decls */
#include "collections/internal.h"   /* make_fn */

extern mino_val *mino_nil(mino_state *S);

/* Sentinel for failed compiles. apply_callable checks against this
 * pointer to skip the retry on subsequent calls. */
mino_bc_fn_t mino_bc_declined = {0};

/* Look up the source position recorded for a given pc. Returns 1 with
 * out_file / out_line / out_column filled in if a meaningful position
 * is recorded, 0 otherwise. NULL out_* args are tolerated. */
int mino_bc_source_lookup(const mino_bc_fn_t *bc, size_t pc,
                          const char **out_file, int *out_line,
                          int *out_column)
{
    if (bc == NULL || bc == &mino_bc_declined) return 0;
    if (bc->source_map.positions == NULL) return 0;
    if (pc >= bc->source_map.len) return 0;
    mino_bc_source_pos_t p = bc->source_map.positions[pc];
    if (p.line <= 0) return 0;
    if (out_file != NULL)   *out_file   = bc->source_map.file;
    if (out_line != NULL)   *out_line   = p.line;
    if (out_column != NULL) *out_column = p.column;
    return 1;
}

/* Bytecode-required mode. When non-zero, the tree-walker fallback at
 * apply_callable's bc-entry path is treated as a fatal error so VM
 * regressions surface loudly instead of degrading silently. Set from
 * the embedder (e.g., by reading the MINO_BC_REQUIRE env var at
 * startup). Defaults to zero in production builds. */
int mino_bc_require_flag = 0;

void mino_bc_check_require(mino_state *S, mino_val *fn)
{
    if (!mino_bc_require_flag) return;
    if (fn == NULL || mino_type_of(fn) != MINO_FN) return;
    if (MINO_BC_RUNNABLE(fn)) return;
    /* Decline mode: fn was not compiled. Whoever set require_flag should
     * run with an expanded compiler.  Surface as a catchable mino-level
     * error so embedders can handle it; gate the hard abort behind
     * MINO_BC_REQUIRE_ABORT for deployments that truly want to crash. */
    fprintf(stderr, "MINO_BC_REQUIRE: fn declined by compiler\n");
#ifdef MINO_BC_REQUIRE_ABORT
    abort(); /* Explicit opt-in: treat decline as unrecoverable. */
#else
    /* Surface as a catchable error rather than process termination. */
    prim_throw_classified(S, "user", "MBC001",
        "MINO_BC_REQUIRE: fn declined by compiler");
#endif
}

#define BC_MAX_LOCALS 256
#define BC_MAX_REGS   255            /* operand width fits in 8 bits */

typedef struct local {
    const char *name;   /* interned symbol data; pointer-stable */
    int         reg;
    /* Compile-time-known value, or NULL. Set when a let-binding's
     * right-hand side folds to a constable value (a literal, or a
     * pure-prim call over folded args). Treated as if the local
     * were self-evaluating when it appears as a head-call arg, so
     * (let [x (+ 1 2)] (* x x)) collapses to 9 at compile time.
     * Cleared on rebind and at let scope exit (locals are
     * restored from saved_n_locals; the value just isn't visible
     * past the lifetime of its slot). */
    mino_val *folded;
} bc_local_t;

/* The innermost active loop, if any. `entry_pc` is the bytecode offset
 * to jump back to on (recur ...); `bind_regs[i]` is the register slot
 * that binding `i` of (loop [b1 v1 ... bN vN]) lives in. n_bindings is
 * the arity recur must match. The compiler stacks these so a nested
 * loop's recur targets only the innermost one. */
typedef struct loop_target {
    int  entry_pc;
    int  bind_regs[BC_MAX_LOCALS];
    int  bind_const_idx[BC_MAX_LOCALS]; /* const-pool index for each
                                          binding's name symbol, used by
                                          compile_recur to re-publish the
                                          env entry on each iteration so
                                          a closure built in iter N
                                          captures iter N's value, not
                                          iter 0's. -1 when there's no
                                          captured env (loop body has no
                                          closures that escape). */
    int  n_bindings;
    int  captures;                     /* mirrors bc->captures at loop
                                          entry: were we wrapped in an
                                          OP_PUSH_ENV / OP_POP_ENV pair? */
} loop_target_t;

typedef struct compiler {
    mino_state      *S;
    mino_env        *env;          /* fn's captured env, for macroexpand1 */
    const char        *defining_ns;  /* fn->as.fn.defining_ns, for ns-env probes */
    mino_bc_fn_t      *bc;           /* attached to fn from the start so the
                                      * GC can find the in-progress code and
                                      * consts buffers through it */
    size_t             consts_cap;
    size_t             code_cap;
    size_t             src_cap;      /* allocated length of source_map.positions */
    bc_local_t         locals[BC_MAX_LOCALS];
    int                n_locals;
    int                n_regs;       /* high-water mark */
    int                next_reg;     /* next register to allocate */
    int                n_params;
    int                ok;            /* 0 means decline */
    int                has_folds;     /* 1 iff any literal pure-fn fold
                                       * fired during this compile.
                                       * propagates to bc->has_folds so
                                       * apply_callable knows to check
                                       * compile_ic_gen at dispatch. */
    int                has_try;       /* 1 iff body emits OP_PUSHCATCH /
                                       * OP_POPCATCH / OP_THROW. Gates
                                       * mino_bc_run's per-call try-state
                                       * snapshot + cleanup (saved in
                                       * bc->has_try). */
    loop_target_t     *loop;          /* innermost loop or NULL */
    /* The form being compiled right now; emit_* reads (line, column)
     * out of its cons metadata and stores the values per-pc in the
     * source map. compile_expr saves/restores this in nested call
     * sites so source positions track the innermost form. The file
     * slot lives on the source_map itself (compile records it once
     * from the first cons it sees). */
    int                cur_line;
    int                cur_column;
    /* AST nesting depth. Incremented on compile_expr entry, decremented
     * on exit. Capped at BC_MAX_COMPILE_DEPTH: when exceeded the
     * compiler raises a recoverable error so deeply-nested user forms
     * cannot exhaust the C stack. */
    int                depth;
} compiler_t;

#define BC_MAX_COMPILE_DEPTH 1000

/* ------------------------------------------------------------------- */
/* Buffer growth                                                       */
/* ------------------------------------------------------------------- */

/* All buffer growth updates the bc record directly. Because bc is
 * attached to fn->as.fn.bc before any compile-time allocation, the GC
 * keeps the buffers alive across any collection triggered by the
 * compiler itself. */
static int ensure_consts(compiler_t *c, size_t need)
{
    if (need <= c->consts_cap) return 1;
    size_t cap = c->consts_cap == 0 ? 8 : c->consts_cap;
    while (cap < need) cap *= 2;
    mino_val **grown = (mino_val **)gc_alloc_typed(
        c->S, GC_T_VALARR, cap * sizeof(*grown));
    if (grown == NULL) { c->ok = 0; return 0; }
    if (c->bc->consts != NULL && c->bc->consts_len > 0) {
        memcpy(grown, c->bc->consts, c->bc->consts_len * sizeof(*grown));
    }
    for (size_t i = c->bc->consts_len; i < cap; i++) grown[i] = NULL;
    /* The bc record may already be OLD after surviving a minor during
     * compile; reassigning bc->consts to a fresh YOUNG buffer requires
     * the write barrier so the next minor's remset finds the new
     * buffer through the fn -> bc -> consts chain. */
    gc_write_barrier(c->S, c->bc, c->bc->consts, grown);
    c->bc->consts = grown;
    c->consts_cap = cap;
    return 1;
}

static int ensure_code(compiler_t *c, size_t need)
{
    if (need <= c->code_cap) return 1;
    size_t cap = c->code_cap == 0 ? 16 : c->code_cap;
    while (cap < need) cap *= 2;
    mino_bc_insn_t *grown = (mino_bc_insn_t *)gc_alloc_typed(
        c->S, GC_T_RAW, cap * sizeof(*grown));
    if (grown == NULL) { c->ok = 0; return 0; }
    if (c->bc->code != NULL && c->bc->code_len > 0) {
        memcpy(grown, c->bc->code, c->bc->code_len * sizeof(*grown));
    }
    gc_write_barrier(c->S, c->bc, c->bc->code, grown);
    c->bc->code = grown;
    c->code_cap = cap;
    return 1;
}

/* Grow the source_map positions buffer so positions[pc] is in-range.
 * Each emit calls this just before bumping code_len, so the position
 * slot for the new instruction is always reachable. Returns 0 on OOM. */
static int ensure_source_map(compiler_t *c, size_t need)
{
    if (need <= c->src_cap) return 1;
    size_t cap = c->src_cap == 0 ? 16 : c->src_cap;
    while (cap < need) cap *= 2;
    mino_bc_source_pos_t *grown = (mino_bc_source_pos_t *)gc_alloc_typed(
        c->S, GC_T_RAW, cap * sizeof(*grown));
    if (grown == NULL) { c->ok = 0; return 0; }
    if (c->bc->source_map.positions != NULL && c->bc->source_map.len > 0) {
        memcpy(grown, c->bc->source_map.positions,
               c->bc->source_map.len * sizeof(*grown));
    }
    for (size_t i = c->bc->source_map.len; i < cap; i++) {
        grown[i].line   = 0;
        grown[i].column = 0;
    }
    gc_write_barrier(c->S, c->bc, c->bc->source_map.positions, grown);
    c->bc->source_map.positions = grown;
    c->src_cap = cap;
    return 1;
}

/* Record the compiler's current (cur_line, cur_column) for the
 * instruction slot at c->bc->code_len. Called from each emit_* before
 * writing the instruction, so source positions are densely populated. */
static void record_source_pc(compiler_t *c)
{
    size_t pc = c->bc->code_len;
    if (!ensure_source_map(c, pc + 1)) return;
    c->bc->source_map.positions[pc].line   = c->cur_line;
    c->bc->source_map.positions[pc].column = c->cur_column;
    if (pc + 1 > c->bc->source_map.len) {
        c->bc->source_map.len = pc + 1;
    }
}

/* Grow the slot buffer if needed and return the index of a freshly
 * zero-initialized slot. Callers fill kind / sym / atom / etc. */
static int reserve_ic_slot(compiler_t *c)
{
    if (c->bc->ic_slots_len >= 0xFFFF) { c->ok = 0; return -1; }
    if ((int)c->bc->ic_slots_len + 1 > c->bc->ic_slots_cap) {
        int cap = c->bc->ic_slots_cap == 0 ? 8 : c->bc->ic_slots_cap * 2;
        mino_bc_ic_slot_t *grown = (mino_bc_ic_slot_t *)gc_alloc_typed(
            c->S, GC_T_RAW, (size_t)cap * sizeof(*grown));
        if (grown == NULL) { c->ok = 0; return -1; }
        if (c->bc->ic_slots != NULL && c->bc->ic_slots_len > 0) {
            memcpy(grown, c->bc->ic_slots,
                   (size_t)c->bc->ic_slots_len * sizeof(*grown));
        }
        for (int i = c->bc->ic_slots_len; i < cap; i++) {
            grown[i].sym                  = NULL;
            grown[i].cached               = NULL;
            grown[i].gen                  = 0;
            grown[i].kind                 = MINO_BC_IC_GLOBAL;
            grown[i].cached_callable_kind = MINO_IC_CALLABLE_NONE;
            grown[i].cached_fn_has_rest   = 0;
            grown[i]._pad_ic0             = 0;
            grown[i].atom                 = NULL;
            grown[i].cached_map           = NULL;
            grown[i].cached_type          = NULL;
            grown[i].cached_fn_n_params   = 0;
            grown[i]._pad_ic1             = 0;
            grown[i]._pad_ic2             = 0;
            grown[i].cached_bc            = NULL;
        }
        gc_write_barrier(c->S, c->bc, c->bc->ic_slots, grown);
        c->bc->ic_slots = grown;
        c->bc->ic_slots_cap = cap;
    }
    int idx = c->bc->ic_slots_len++;
    c->bc->ic_slots[idx].sym                  = NULL;
    c->bc->ic_slots[idx].cached               = NULL;
    c->bc->ic_slots[idx].gen                  = 0;
    c->bc->ic_slots[idx].kind                 = MINO_BC_IC_GLOBAL;
    c->bc->ic_slots[idx].cached_callable_kind = MINO_IC_CALLABLE_NONE;
    c->bc->ic_slots[idx].cached_fn_has_rest   = 0;
    c->bc->ic_slots[idx]._pad_ic0             = 0;
    c->bc->ic_slots[idx].atom                 = NULL;
    c->bc->ic_slots[idx].cached_map           = NULL;
    c->bc->ic_slots[idx].cached_type          = NULL;
    c->bc->ic_slots[idx].cached_fn_n_params   = 0;
    c->bc->ic_slots[idx]._pad_ic1             = 0;
    c->bc->ic_slots[idx]._pad_ic2             = 0;
    c->bc->ic_slots[idx].cached_bc            = NULL;
    return idx;
}

/* Allocate a fresh IC slot for OP_GETGLOBAL_CACHED / OP_CALL_CACHED.
 * Grows the slots buffer geometrically (8 -> 16 -> 32 ...); writes the
 * symbol the slot stands for so the runtime miss path can re-resolve.
 * Returns the slot index (fits in 16-bit Bx) or -1 on overflow / OOM. */
static int alloc_ic_slot(compiler_t *c, mino_val *sym)
{
    int idx = reserve_ic_slot(c);
    if (idx < 0) return -1;
    gc_write_barrier(c->S, c->bc->ic_slots, NULL, sym);
    c->bc->ic_slots[idx].sym = sym;
    return idx;
}

/* Allocate a fresh IC slot for OP_PROTOCOL_CALL_CACHED. `mname` is the
 * method-name string (used in the no-impl diagnostic); `atom` is the
 * dispatch atom, captured at compile time and pinned in the slot so the
 * hot path can deref without symbol resolution. */
static int alloc_protocol_ic_slot(compiler_t *c, mino_val *mname,
                                  mino_val *atom)
{
    int idx = reserve_ic_slot(c);
    if (idx < 0) return -1;
    gc_write_barrier(c->S, c->bc->ic_slots, NULL, mname);
    gc_write_barrier(c->S, c->bc->ic_slots, NULL, atom);
    c->bc->ic_slots[idx].sym  = mname;
    c->bc->ic_slots[idx].atom = atom;
    c->bc->ic_slots[idx].kind = MINO_BC_IC_PROTOCOL;
    return idx;
}

static int add_const(compiler_t *c, mino_val *v)
{
    /* No dedup: the const pool is per-fn and typical bodies have small
     * pools, so a duplicate symbol const is cheap. */
    if (!ensure_consts(c, c->bc->consts_len + 1)) return -1;
    if (c->bc->consts_len > 0xFFFFu) { c->ok = 0; return -1; }
    int k = (int)c->bc->consts_len;
    /* Write barrier: the consts buffer may have been promoted to OLD
     * by a minor cycle between allocation and now, and `v` may be a
     * just-allocated symbol or literal. */
    gc_write_barrier(c->S, c->bc->consts, NULL, v);
    c->bc->consts[c->bc->consts_len++] = v;
    return k;
}

/* ------------------------------------------------------------------- */
/* Emission                                                            */
/* ------------------------------------------------------------------- */

static void emit_abc(compiler_t *c, mino_bc_op_t op,
                     unsigned a, unsigned b, unsigned cc)
{
    if (!ensure_code(c, c->bc->code_len + 1)) return;
    record_source_pc(c);
    c->bc->code[c->bc->code_len++] = MK_ABC(op, a, b, cc);
}

static void emit_abx(compiler_t *c, mino_bc_op_t op,
                     unsigned a, unsigned bx)
{
    if (!ensure_code(c, c->bc->code_len + 1)) return;
    record_source_pc(c);
    c->bc->code[c->bc->code_len++] = MK_ABx(op, a, bx);
}

static int emit_jmp_placeholder(compiler_t *c, mino_bc_op_t op, unsigned a)
{
    if (!ensure_code(c, c->bc->code_len + 1)) return -1;
    record_source_pc(c);
    int pos = (int)c->bc->code_len;
    c->bc->code[c->bc->code_len++] = MK_AsBx(op, a, 0);
    return pos;
}

static void patch_jmp(compiler_t *c, int pos)
{
    if (pos < 0 || (size_t)pos >= c->bc->code_len) { c->ok = 0; return; }
    /* The jump offset is added to pc AFTER the instruction is fetched,
     * so the natural "fall-through here" offset is code_len - (pos+1).
     * Bias-encoded as 16-bit unsigned (+0x8000): valid -0x8000..0x7FFF. */
    int off = (int)c->bc->code_len - (pos + 1);
    if (off < -0x8000 || off > 0x7FFF) {
        c->ok = 0;
        return;
    }
    mino_bc_insn_t ins = c->bc->code[pos];
    unsigned op = OP_OF(ins);
    unsigned a  = A_OF(ins);
    c->bc->code[pos] = MK_AsBx(op, a, off);
}

/* ------------------------------------------------------------------- */
/* Registers                                                           */
/* ------------------------------------------------------------------- */

static int alloc_reg(compiler_t *c)
{
    if (c->next_reg >= BC_MAX_REGS) { c->ok = 0; return -1; }
    int r = c->next_reg++;
    if (r + 1 > c->n_regs) c->n_regs = r + 1;
    return r;
}

static int bind_local(compiler_t *c, const char *name, int reg)
{
    if (c->n_locals >= BC_MAX_LOCALS) { c->ok = 0; return 0; }
    c->locals[c->n_locals].name   = name;
    c->locals[c->n_locals].reg    = reg;
    c->locals[c->n_locals].folded = NULL;
    c->n_locals++;
    return 1;
}

/* Returns the compile-time-known value bound to `name`, or NULL when
 * the name isn't a local or its binding's right-hand side did not
 * fold. Scans innermost-out so an inner let-binding shadows an
 * outer one. */
static mino_val *local_folded_value(compiler_t *c, const char *name)
{
    for (int i = c->n_locals - 1; i >= 0; i--) {
        if (c->locals[i].name == name
            || (c->locals[i].name != NULL
                && strcmp(c->locals[i].name, name) == 0)) {
            return c->locals[i].folded;
        }
    }
    return NULL;
}

static int find_local(compiler_t *c, const char *name)
{
    for (int i = c->n_locals - 1; i >= 0; i--) {
        if (c->locals[i].name == name) return c->locals[i].reg;
    }
    /* Fall back to strcmp for non-interned-pointer-matching cases
     * (shouldn't happen for symbols read from parsed source, but
     * paranoia). */
    for (int i = c->n_locals - 1; i >= 0; i--) {
        if (c->locals[i].name != NULL
            && strcmp(c->locals[i].name, name) == 0) {
            return c->locals[i].reg;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------- */
/* Tail-position peephole                                              */
/* ------------------------------------------------------------------- */

/* Replace the A operand of an instruction. A occupies bits 8..15 in
 * every encoding form (ABC, ABx, AsBx), so the rewrite is a uniform
 * bit-twiddle. */
static mino_bc_insn_t set_A_field(mino_bc_insn_t ins, unsigned new_a)
{
    return (ins & ~((mino_bc_insn_t)0xFFu << 8))
         | ((mino_bc_insn_t)(new_a & 0xFFu) << 8);
}

/* True iff `op` writes a value to its A operand. The peephole only
 * folds these producers; OP_CALL (result goes to C), OP_SETGLOBAL
 * (writes to a global, not a register), OP_RETURN / OP_TAILCALL
 * (don't produce a value-in-register at all) are not folded. */
static int producer_writes_to_A_dst(unsigned op)
{
    switch (op) {
    case OP_MOVE:
    case OP_LOAD_K:
    case OP_GETGLOBAL:
    case OP_GETGLOBAL_CACHED:
    case OP_CLOSURE:
    case OP_MAKE_LAZY:
    case OP_ADD_II:  case OP_SUB_II:  case OP_MUL_II:
    case OP_LT_II:   case OP_LE_II:   case OP_GT_II:
    case OP_GE_II:   case OP_EQ_II:
    case OP_MOD_II:  case OP_QUOT_II: case OP_REM_II:
    case OP_BAND_II: case OP_BOR_II:  case OP_BXOR_II:
    case OP_SHL_II:  case OP_SHR_II:  case OP_USHR_II:
    case OP_ADD_IK:  case OP_SUB_IK:
    case OP_LT_IK:   case OP_LE_IK:   case OP_EQ_IK:
    case OP_NTH_VEC: case OP_GET_KW_MAP:
    case OP_INC_I:   case OP_DEC_I:   case OP_ZERO_INT_P:
    case OP_POS_P_I: case OP_NEG_P_I:
    case OP_EVEN_P_I: case OP_ODD_P_I: case OP_BNOT_I:
    case OP_BINOP_INT:
        return 1;
    default:
        return 0;
    }
}

/* True iff any OP_JMP / OP_JMPIFNOT in code[0..code_len) targets pc. */
static int pc_is_jump_target(const mino_bc_insn_t *code, size_t code_len,
                             size_t target_pc)
{
    for (size_t pc = 0; pc < code_len; pc++) {
        mino_bc_insn_t ins = code[pc];
        unsigned op = OP_OF(ins);
        if (op != OP_JMP && op != OP_JMPIFNOT) continue;
        long off = sBx_OF(ins);
        ptrdiff_t target = (ptrdiff_t)pc + 1 + off;
        if (target >= 0 && (size_t)target == target_pc) return 1;
    }
    return 0;
}

/* Fold the tail-position MOVE that closes a clause's body, when one
 * is present. Pattern: the body's last emitted instruction is
 * `OP_MOVE A B`, the immediately preceding instruction is a foldable
 * producer that wrote to B, neither the producer nor the MOVE is a
 * jump target. In that case we rewrite the producer's A operand to
 * the MOVE's A and drop the MOVE entirely. After the fold the
 * surrounding OP_RETURN reads A directly from the producer's output.
 *
 * Only the body's terminal MOVE is folded -- the producer is at
 * pc code_len - 2, the MOVE at code_len - 1. The MOVE's removal
 * shrinks code_len by 1, which is safe because (1) we just verified
 * no jump targets it, and (2) the OP_RETURN about to be emitted
 * takes the slot the MOVE vacated. */
static void peephole_tail_move(compiler_t *c)
{
    if (!c->ok) return;
    if (c->bc->code_len < 2) return;
    size_t move_pc = c->bc->code_len - 1;
    mino_bc_insn_t last = c->bc->code[move_pc];
    if (OP_OF(last) != OP_MOVE) return;
    unsigned A = A_OF(last);
    unsigned B = B_OF(last);
    if (A == B) {
        /* A == B is a true no-op. Drop it. */
        c->bc->code_len--;
        return;
    }
    size_t prod_pc = move_pc - 1;
    mino_bc_insn_t prev = c->bc->code[prod_pc];
    unsigned prev_op = OP_OF(prev);
    if (!producer_writes_to_A_dst(prev_op)) return;
    if (A_OF(prev) != B) return;
    if (pc_is_jump_target(c->bc->code, c->bc->code_len, move_pc)) return;
    if (pc_is_jump_target(c->bc->code, c->bc->code_len, prod_pc)) return;
    c->bc->code[prod_pc] = set_A_field(prev, A);
    c->bc->code_len--;
}

/* ------------------------------------------------------------------- */
/* AST helpers                                                         */
/* ------------------------------------------------------------------- */

static int is_self_evaluating(const mino_val *v)
{
    if (v == NULL) return 1;
    switch (mino_type_of(v)) {
    case MINO_NIL: case MINO_BOOL: case MINO_INT: case MINO_FLOAT:
    case MINO_FLOAT32: case MINO_STRING: case MINO_KEYWORD: case MINO_CHAR:
        return 1;
    default:
        return 0;
    }
}

static int sym_is(const mino_val *v, const char *name)
{
    if (v == NULL || mino_type_of(v) != MINO_SYMBOL) return 0;
    return strcmp(v->as.s.data, name) == 0;
}

/* The eleven macro-family forms that canon exposes as clojure.core macros.
 * Kept in sync with k_public_form_docs in src/eval/special_registry.c.
 * A clojure.core/-qualified head naming one of these (produced by
 * syntax-quote, or written directly) dispatches the same compile handler
 * as the bare form; the true special forms stay bare. */
static int is_core_macro_form(const char *name)
{
    return strcmp(name, "fn") == 0
        || strcmp(name, "let") == 0
        || strcmp(name, "loop") == 0
        || strcmp(name, "lazy-seq") == 0
        || strcmp(name, "binding") == 0
        || strcmp(name, "declare") == 0
        || strcmp(name, "defmacro") == 0
        || strcmp(name, "ns") == 0
        || strcmp(name, "when") == 0
        || strcmp(name, "and") == 0
        || strcmp(name, "or") == 0;
}

/* Names of every special form mino's eval recognizes, plus the syntactic
 * sub-forms that only appear as heads inside their parent (catch,
 * finally) but would otherwise look like regular calls. Kept in sync
 * with k_special_forms in src/eval/special_registry.c.
 *
 * DUPLICATION NOTE (factoring-eval-bc-r2-002): this table duplicates the
 * canonical special-form name list in src/eval/special_registry.c
 * (k_special_forms). The registry is the authority; update BOTH when
 * adding or removing a special form name. Deduplication is blocked because
 * the registry's lookup interface requires a cons-form argument (not a
 * const char *), so a direct call from here would require touching an
 * out-of-scope module. */
static int is_special_form_name(const char *name)
{
    /* Recognized by the registry (eval_try_special_form). */
    if (strcmp(name, "quote") == 0) return 1;
    if (strcmp(name, "quote*") == 0) return 1;
    if (strcmp(name, "quasiquote") == 0) return 1;
    if (strcmp(name, "unquote") == 0) return 1;
    if (strcmp(name, "unquote-splicing") == 0) return 1;
    if (strcmp(name, "defmacro") == 0) return 1;
    if (strcmp(name, "declare") == 0) return 1;
    if (strcmp(name, "ns") == 0) return 1;
    if (strcmp(name, "var") == 0) return 1;
    if (strcmp(name, "def") == 0) return 1;
    if (strcmp(name, "if") == 0) return 1;
    if (strcmp(name, "do") == 0) return 1;
    if (strcmp(name, "let") == 0 || strcmp(name, "let*") == 0) return 1;
    if (strcmp(name, "fn") == 0 || strcmp(name, "fn*") == 0) return 1;
    if (strcmp(name, "recur") == 0) return 1;
    if (strcmp(name, "loop") == 0 || strcmp(name, "loop*") == 0) return 1;
    if (strcmp(name, "try") == 0) return 1;
    if (strcmp(name, "binding") == 0) return 1;
    if (strcmp(name, "lazy-seq") == 0) return 1;
    if (strcmp(name, "when") == 0) return 1;
    if (strcmp(name, "and") == 0) return 1;
    if (strcmp(name, "or") == 0) return 1;
    /* Sub-forms (only valid inside try) but still not regular calls. */
    if (strcmp(name, "catch") == 0) return 1;
    if (strcmp(name, "finally") == 0) return 1;
    if (strcmp(name, "throw") == 0) return 1;
    if (strcmp(name, "set!") == 0) return 1;
    /* Other forms mino dispatches at eval time. defrecord/deftype/
     * defprotocol/reify/case/cond/new/. are macros expanded before
     * reaching here, but match the eye -- exclude defensively in case
     * a future reader inserts them as raw forms. */
    if (strcmp(name, "defrecord") == 0) return 1;
    if (strcmp(name, "deftype") == 0) return 1;
    if (strcmp(name, "defprotocol") == 0) return 1;
    if (strcmp(name, "reify") == 0) return 1;
    if (strcmp(name, "case") == 0) return 1;
    if (strcmp(name, "cond") == 0) return 1;
    if (strcmp(name, "new") == 0) return 1;
    if (strcmp(name, ".") == 0) return 1;
    /* Host-syntax sugar: (.method target ...), (.-field target),
     * TypeName/staticMethod. These rewrite to host/... primitive calls
     * in eval_try_host_syntax, which only runs on the tree-walker
     * path. Send them there so a BC-compiled fn body can use them. */
    if (name[0] == '.' && name[1] != '\0') return 1;
    {
        const char *slash = strchr(name, '/');
        if (slash != NULL && slash != name && slash[1] != '\0') {
            /* Reject the bare division op `/` (one-char name) and
             * already-namespaced symbols like clojure.core/+ where the
             * head is a valid env binding; here we only steer host
             * static-method calls to the tree-walker.  Heuristic:
             * Capital-letter type names. */
            const unsigned char c0 = (unsigned char)name[0];
            if (c0 >= 'A' && c0 <= 'Z') return 1;
        }
    }
    return 0;
}

/* Forward declarations. `tail` is 1 when the result of this expression
 * is the fn's return value; control forms (if/do/let) propagate it to
 * their inner tail position so a nested call there emits OP_TAILCALL
 * and goes through the trampoline (constant C stack). */
static int compile_expr(compiler_t *c, mino_val *form, int dst, int tail);
static int compile_expr_dispatch(compiler_t *c, mino_val *form,
                                 int dst, int tail);
static int compile_body(compiler_t *c, mino_val *body, int dst, int tail);
static mino_val *probe_head_value(compiler_t *c, mino_val *head);
/* Forward decl: defined alongside try_fold_call further down. Used by
 * compile_let to compute the compile-time-known value of a binding's
 * right-hand side, if any. */
typedef struct pure_prim pure_prim_t;
static int          try_fold_arg(compiler_t *c, mino_val *v,
                                 mino_val **out);
static int          fold_result_constable(mino_val *v);
static const pure_prim_t *should_fold_call(compiler_t *c, mino_val *head);

/* Pre-scan: does the form tree contain a form that captures the
 * current lexical env -- an inner (fn ...) literal or a (lazy-seq ...)
 * that will realise its body in the env later? When yes, the enclosing
 * fn sets bc->captures = 1, which forces env-binding of params and
 * bracketing of let scopes with OP_PUSH_ENV / OP_POP_ENV so any
 * captured shape sees the right lexical chain. Quote-forms are data,
 * not code, and don't count. */
static int contains_env_capture(mino_val *form)
{
    if (form == NULL) return 0;
    if (mino_type_of(form) == MINO_VECTOR) {
        for (size_t i = 0; i < form->as.vec.len; i++) {
            if (contains_env_capture(vec_nth(form, i))) return 1;
        }
        return 0;
    }
    if (mino_type_of(form) == MINO_MAP || mino_type_of(form) == MINO_SET) {
        return 0;
    }
    if (!mino_is_cons(form)) return 0;
    mino_val *head = form->as.cons.car;
    if (head != NULL && mino_type_of(head) == MINO_SYMBOL) {
        /* Recognize the clojure.core/-qualified spelling of the
         * capturing forms (syntax-quote qualifies fn / lazy-seq to
         * clojure.core/...), so a nested syntax-quoted closure still
         * forces the enclosing fn to publish its let locals into the
         * env -- otherwise the closure cannot resolve them at runtime. */
        const char *hn = head->as.s.data;
        if (strncmp(hn, "clojure.core/", 13) == 0) hn += 13;
        if (strcmp(hn, "fn") == 0 || strcmp(hn, "fn*") == 0) return 1;
        if (strcmp(hn, "lazy-seq") == 0) return 1;
        if (strcmp(hn, "quote") == 0 || strcmp(hn, "quote*") == 0) return 0;
    }
    while (mino_is_cons(form)) {
        if (contains_env_capture(form->as.cons.car)) return 1;
        form = form->as.cons.cdr;
    }
    return 0;
}

/* ------------------------------------------------------------------- */
/* Special-form compilers                                              */
/* ------------------------------------------------------------------- */

static int compile_if(compiler_t *c, mino_val *form, int dst, int tail)
{
    /* (if cond then) or (if cond then else). When `tail` is set, the
     * value of the chosen branch is the fn's result, so each branch
     * inherits the tail position from us. */
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }
    mino_val *cond_form = args->as.cons.car;
    mino_val *rest1 = args->as.cons.cdr;
    if (!mino_is_cons(rest1)) { c->ok = 0; return -1; }
    mino_val *then_form = rest1->as.cons.car;
    mino_val *rest2 = rest1->as.cons.cdr;
    mino_val *else_form = NULL;
    if (mino_is_cons(rest2)) {
        else_form = rest2->as.cons.car;
        if (mino_is_cons(rest2->as.cons.cdr)) { c->ok = 0; return -1; }
    }

    /* Constant-condition fold. When cond is a literal whose truthiness
     * is known at compile time, we know exactly which branch will run
     * and can skip the JMPIFNOT / JMP scaffolding entirely. nil and
     * false are falsy; everything else (including 0, "", :keyword, the
     * empty list) is truthy. The fold also covers (if true ...) which
     * shows up in the eval-floor benches as a way to keep call shape
     * stable. */
    if (is_self_evaluating(cond_form)) {
        if (mino_is_truthy_inline(cond_form)) {
            return compile_expr(c, then_form, dst, tail);
        }
        if (else_form != NULL) {
            return compile_expr(c, else_form, dst, tail);
        }
        int k = add_const(c, mino_nil(c->S));
        if (k < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
        return 0;
    }

    int saved_next = c->next_reg;
    int cond_reg = alloc_reg(c);
    if (cond_reg < 0) return -1;
    if (compile_expr(c, cond_form, cond_reg, 0) < 0) return -1;
    c->next_reg = saved_next;

    int jmp_to_else = emit_jmp_placeholder(c, OP_JMPIFNOT, (unsigned)cond_reg);
    if (jmp_to_else < 0) return -1;

    if (compile_expr(c, then_form, dst, tail) < 0) return -1;

    int jmp_to_end = emit_jmp_placeholder(c, OP_JMP, 0);
    if (jmp_to_end < 0) return -1;

    patch_jmp(c, jmp_to_else);

    if (else_form != NULL) {
        if (compile_expr(c, else_form, dst, tail) < 0) return -1;
    } else {
        /* (if cond then) without else: result is nil. */
        int k = add_const(c, mino_nil(c->S));
        if (k < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
    }

    patch_jmp(c, jmp_to_end);
    return 0;
}

static int compile_do(compiler_t *c, mino_val *form, int dst, int tail)
{
    /* (do e1 e2 ... eN) -> eval each, result is eN. Empty (do) -> nil.
     * Tail position propagates to the last expression. */
    mino_val *body = form->as.cons.cdr;
    return compile_body(c, body, dst, tail);
}

/* Walk a cons chain and return its length. Used by bc_count_args and
 * cons_count below; extracting the common loop avoids three copies of
 * the same traversal. */
static int bc_cons_len(const mino_val *list)
{
    int n = 0;
    while (mino_is_cons(list)) { n++; list = list->as.cons.cdr; }
    return n;
}

/* Count cons args starting at `args` (a cons-list). */
static int bc_count_args(mino_val *args)
{
    return bc_cons_len(args);
}

static int compile_when(compiler_t *c, mino_val *form, int dst, int tail)
{
    /* (when test body...) == (if test (do body...) nil). The body is
     * implicit-do; falsy test stores nil in dst and skips the body. */
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }
    mino_val *test_form = args->as.cons.car;
    mino_val *body      = args->as.cons.cdr;

    int saved_next = c->next_reg;
    int test_reg = alloc_reg(c);
    if (test_reg < 0) return -1;
    if (compile_expr(c, test_form, test_reg, 0) < 0) return -1;
    c->next_reg = saved_next;

    int jmp_to_else = emit_jmp_placeholder(c, OP_JMPIFNOT, (unsigned)test_reg);
    if (jmp_to_else < 0) return -1;

    if (compile_body(c, body, dst, tail) < 0) return -1;

    int jmp_to_end = emit_jmp_placeholder(c, OP_JMP, 0);
    if (jmp_to_end < 0) return -1;

    patch_jmp(c, jmp_to_else);
    /* Falsy test path: result is nil. */
    int k = add_const(c, mino_nil(c->S));
    if (k < 0) return -1;
    emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);

    patch_jmp(c, jmp_to_end);
    return 0;
}

static int compile_and(compiler_t *c, mino_val *form, int dst, int tail)
{
    /* (and) -> true. (and a) -> a. (and a b ...) short-circuits on the
     * first falsy arg, returning its value; otherwise the last arg's
     * value is the result (in tail position). */
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) {
        int k = add_const(c, mino_true(c->S));
        if (k < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
        return 0;
    }
    int n = bc_count_args(args);
    if (n == 1) {
        return compile_expr(c, args->as.cons.car, dst, tail);
    }
    if (n > 64) { c->ok = 0; return -1; }
    int end_jumps[64];
    int n_end_jumps = 0;
    mino_val *cur = args;
    for (int i = 0; i < n - 1; i++) {
        if (compile_expr(c, cur->as.cons.car, dst, 0) < 0) return -1;
        int j = emit_jmp_placeholder(c, OP_JMPIFNOT, (unsigned)dst);
        if (j < 0) return -1;
        end_jumps[n_end_jumps++] = j;
        cur = cur->as.cons.cdr;
    }
    /* Last arg evaluated in tail position. */
    if (compile_expr(c, cur->as.cons.car, dst, tail) < 0) return -1;
    for (int i = 0; i < n_end_jumps; i++) patch_jmp(c, end_jumps[i]);
    return 0;
}

static int compile_or(compiler_t *c, mino_val *form, int dst, int tail)
{
    /* (or) -> nil. (or a) -> a. (or a b ...) returns the first truthy
     * arg or the last arg's value otherwise. */
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) {
        int k = add_const(c, mino_nil(c->S));
        if (k < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
        return 0;
    }
    int n = bc_count_args(args);
    if (n == 1) {
        return compile_expr(c, args->as.cons.car, dst, tail);
    }
    if (n > 64) { c->ok = 0; return -1; }
    int end_jumps[64];
    int n_end_jumps = 0;
    mino_val *cur = args;
    for (int i = 0; i < n - 1; i++) {
        if (compile_expr(c, cur->as.cons.car, dst, 0) < 0) return -1;
        /* Truthy: keep dst's value and jump to end.  Synthesize
         * "jump if truthy" via JMPIFNOT skip; JMP end; skip:. */
        int skip = emit_jmp_placeholder(c, OP_JMPIFNOT, (unsigned)dst);
        if (skip < 0) return -1;
        int end = emit_jmp_placeholder(c, OP_JMP, 0);
        if (end < 0) return -1;
        end_jumps[n_end_jumps++] = end;
        patch_jmp(c, skip);
        cur = cur->as.cons.cdr;
    }
    if (compile_expr(c, cur->as.cons.car, dst, tail) < 0) return -1;
    for (int i = 0; i < n_end_jumps; i++) patch_jmp(c, end_jumps[i]);
    return 0;
}

/* Walk a let / loop binding vector and return 1 iff every LHS is a
 * plain MINO_SYMBOL. When this returns 0 the caller can run
 * prim_destructure on the vector to expand the destructure patterns
 * into a flat plain-symbol sequence. */
static int bindings_are_plain(mino_val *bindings)
{
    if (bindings == NULL || mino_type_of(bindings) != MINO_VECTOR) return 0;
    size_t n = bindings->as.vec.len;
    if ((n & 1) != 0) return 0;
    for (size_t i = 0; i < n; i += 2) {
        mino_val *lhs = vec_nth(bindings, i);
        if (lhs == NULL || mino_type_of(lhs) != MINO_SYMBOL) return 0;
    }
    return 1;
}

/* Run prim_destructure on a binding vector to flatten any vector /
 * map destructure patterns into plain-symbol bindings backed by
 * synthesized `nth` / `get` calls + gensym intermediates. Returns
 * NULL on a destructure error (with c->ok cleared so the caller
 * declines into the tree-walker). */
static mino_val *expand_destructure_bindings(compiler_t *c,
                                                mino_val *bindings)
{
    mino_val *args = mino_cons(c->S, bindings, mino_nil(c->S));
    if (args == NULL) { c->ok = 0; return NULL; }
    mino_val *expanded = prim_destructure(c->S, args, c->env);
    if (expanded == NULL || mino_type_of(expanded) != MINO_VECTOR) {
        c->ok = 0;
        return NULL;
    }
    return expanded;
}

/* Count syntactic references to the symbol with interned name `name`
 * in `form`. Conservative: counts any occurrence in the AST without
 * tracking inner shadows or quote contexts. A 0 result is safe to
 * trust (the binding's name appears nowhere in the body), which is
 * all dead-binding elimination needs; over-counting just declines
 * the optimisation. */
static int count_symbol_uses(mino_val *form, const char *name)
{
    if (form == NULL) return 0;
    if (mino_type_of(form) == MINO_SYMBOL) {
        return (form->as.s.data == name
                || (form->as.s.data != NULL
                    && strcmp(form->as.s.data, name) == 0)) ? 1 : 0;
    }
    if (mino_type_of(form) == MINO_CONS) {
        return count_symbol_uses(form->as.cons.car, name)
             + count_symbol_uses(form->as.cons.cdr, name);
    }
    if (mino_type_of(form) == MINO_VECTOR) {
        size_t n = form->as.vec.len;
        int    total = 0;
        for (size_t i = 0; i < n; i++) {
            total += count_symbol_uses(vec_nth(form, i), name);
        }
        return total;
    }
    if (mino_type_of(form) == MINO_MAP) {
        /* A map literal as a body form is itself an expression: walk
         * its keys and values so dead-binding elimination doesn't
         * drop a let-binding whose only use sits inside the map. */
        size_t n = form->as.map.len;
        int    total = 0;
        for (size_t i = 0; i < n; i++) {
            mino_val *k = vec_nth(form->as.map.key_order, i);
            mino_val *v = (form->as.map.val_order != NULL)
                            ? vec_nth(form->as.map.val_order, i)
                            : map_get_val(form, k);
            total += count_symbol_uses(k, name);
            total += count_symbol_uses(v, name);
        }
        return total;
    }
    if (mino_type_of(form) == MINO_SET) {
        size_t n = form->as.set.len;
        int    total = 0;
        for (size_t i = 0; i < n; i++) {
            total += count_symbol_uses(vec_nth(form->as.set.key_order, i),
                                       name);
        }
        return total;
    }
    return 0;
}

/* True when `val_form` is observably side-effect-free: a literal, a
 * symbol reference, or a pure-prim call whose args are themselves
 * side-effect-free. Side-effecting forms (println, def, atom-deref,
 * IO, user fns we can't see through) must be kept for their effect
 * even when their binding is unused. */
static int is_side_effect_free(compiler_t *c, mino_val *form);

static int is_side_effect_free(compiler_t *c, mino_val *form)
{
    if (form == NULL) return 1;
    if (is_self_evaluating(form)) return 1;
    if (mino_type_of(form) == MINO_SYMBOL) return 1;
    if (mino_type_of(form) == MINO_CONS) {
        mino_val        *head = form->as.cons.car;
        mino_val        *p;
        const pure_prim_t *pp;
        if (head == NULL || mino_type_of(head) != MINO_SYMBOL) return 0;
        pp = should_fold_call(c, head);
        if (pp == NULL) return 0;
        p = form->as.cons.cdr;
        while (mino_is_cons(p)) {
            if (!is_side_effect_free(c, p->as.cons.car)) return 0;
            p = p->as.cons.cdr;
        }
        return 1;
    }
    return 0;
}

static int compile_let(compiler_t *c, mino_val *form, int dst, int tail)
{
    /* (let [b1 v1 b2 v2 ...] body...) -- plain-symbol bindings, or
     * vector / map destructure patterns which we flatten via
     * prim_destructure at compile time. Tail position propagates to
     * the body's last expression. */
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }
    mino_val *bindings = args->as.cons.car;
    mino_val *body     = args->as.cons.cdr;
    if (bindings == NULL || mino_type_of(bindings) != MINO_VECTOR) {
        c->ok = 0; return -1;
    }
    if (!bindings_are_plain(bindings)) {
        bindings = expand_destructure_bindings(c, bindings);
        if (bindings == NULL) return -1;
    }
    size_t blen = bindings->as.vec.len;
    if ((blen & 1) != 0) { c->ok = 0; return -1; }

    int saved_n_locals = c->n_locals;
    int saved_next_reg = c->next_reg;
    int pushed_env = 0;
    /* When the enclosing fn captures, every let scope publishes its
     * bindings into a fresh env_child so inner closures see exactly
     * the let-scoped names that were in scope at OP_CLOSURE time --
     * and stop seeing them after the let body returns. Fns that
     * don't capture skip this and stay register-only. */
    if (c->bc->captures) {
        emit_abc(c, OP_PUSH_ENV, 0, 0, 0);
        pushed_env = 1;
    }

    for (size_t i = 0; i < blen; i += 2) {
        mino_val *name_form = vec_nth(bindings, i);
        mino_val *val_form  = vec_nth(bindings, i + 1);
        if (name_form == NULL || mino_type_of(name_form) != MINO_SYMBOL) {
            c->ok = 0; goto out;
        }
        /* Phase E2 dead-binding elimination: a binding whose name
         * appears nowhere in (the rest of the bindings + the body)
         * and whose value expression is observably side-effect-free
         * can be dropped entirely -- no register, no value emission,
         * no env publish. Skipped for capturing lets since env_bind
         * has its own observable effect (the name becomes visible
         * to inner closures regardless of register usage). */
        if (!pushed_env) {
            int uses = 0;
            for (size_t j = i + 2; j < blen; j += 2) {
                uses += count_symbol_uses(vec_nth(bindings, j + 1),
                                          name_form->as.s.data);
            }
            uses += count_symbol_uses(body, name_form->as.s.data);
            if (uses == 0 && is_side_effect_free(c, val_form)) {
                c->has_folds = 1;  /* gates redef-invalidate, same as folds */
                continue;
            }
        }
        int reg = alloc_reg(c);
        if (reg < 0) goto out;
        if (compile_expr(c, val_form, reg, 0) < 0) goto out;
        if (!bind_local(c, name_form->as.s.data, reg)) goto out;
        /* Phase E1 fold-through: when the binding's right-hand side
         * folded to a constable literal at compile time -- either a
         * self-evaluating literal or a pure-prim call over already-
         * folded args -- remember the value on the local so any
         * later (head x ...) pure-prim call can substitute it
         * during its own fold attempt. Only safe when the let
         * doesn't capture (no OP_ENV_BIND): with env publishing,
         * inner closures could pick up a stale folded value if
         * the global resolution changes. */
        if (!pushed_env) {
            mino_val *fv = NULL;
            if (try_fold_arg(c, val_form, &fv) && fv != NULL
                && fold_result_constable(fv)) {
                c->locals[c->n_locals - 1].folded = fv;
            }
        }
        if (pushed_env) {
            int k = add_const(c, name_form);
            if (k < 0) goto out;
            emit_abx(c, OP_ENV_BIND, (unsigned)reg, (unsigned)k);
        }
    }

    if (compile_body(c, body, dst, tail) < 0) goto out;

    if (pushed_env) emit_abc(c, OP_POP_ENV, 0, 0, 0);
    c->n_locals = saved_n_locals;
    c->next_reg = saved_next_reg;
    return 0;

out:
    c->n_locals = saved_n_locals;
    c->next_reg = saved_next_reg;
    return -1;
}

/* Forward decl: defined alongside the PURE_PRIMS table further down. */
static int head_is_canonical_pure_prim(compiler_t *c, mino_val *head);

/* Match a single-form body whose head is the named special form. The
 * (loop [...]) body is a cons-list of expressions; the fused pattern
 * detector only fires when there's exactly one such expression and
 * its head is `if`. Returns the (if cond then else?) tail or NULL. */
static mino_val *match_single_form(mino_val *body, const char *head)
{
    if (!mino_is_cons(body)) return NULL;
    if (body->as.cons.cdr != NULL && mino_type_of(body->as.cons.cdr) != MINO_NIL) {
        if (mino_is_cons(body->as.cons.cdr)) return NULL;
    }
    mino_val *first = body->as.cons.car;
    if (!mino_is_cons(first)) return NULL;
    mino_val *h = first->as.cons.car;
    if (h == NULL || mino_type_of(h) != MINO_SYMBOL) return NULL;
    if (strcmp(h->as.s.data, head) != 0) return NULL;
    return first;
}

/* True when `form` is (sym arg) where sym names the given prim head AND
 * resolves to the canonical PRIM (no user shadow), and arg is a symbol
 * matching `expected_name`. Used to recognise the (zero? bind),
 * (inc bind), (dec bind) shapes inside a counted-loop recur form. The
 * canonical-prim check is what makes the fused-loop emission sound
 * against a user `(defn dec [x] ...)` shadow. */
static int is_unary_call_of_local(compiler_t *c, mino_val *form,
                                  const char *head_name,
                                  const char *expected_name)
{
    if (!mino_is_cons(form)) return 0;
    mino_val *h = form->as.cons.car;
    if (h == NULL || mino_type_of(h) != MINO_SYMBOL) return 0;
    if (strcmp(h->as.s.data, head_name) != 0) return 0;
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) return 0;
    if (args->as.cons.cdr != NULL && mino_type_of(args->as.cons.cdr) != MINO_NIL) {
        return 0;
    }
    mino_val *a = args->as.cons.car;
    if (a == NULL || mino_type_of(a) != MINO_SYMBOL) return 0;
    if (strcmp(a->as.s.data, expected_name) != 0) return 0;
    return head_is_canonical_pure_prim(c, h);
}

/* Find the binding index whose name matches the given symbol's name,
 * or -1 if no match. */
static int find_binding_idx(mino_val *bindings, int n_bindings,
                            const char *name)
{
    for (int i = 0; i < n_bindings; i++) {
        mino_val *bn = vec_nth(bindings, (size_t)(i * 2));
        if (bn != NULL && mino_type_of(bn) == MINO_SYMBOL
            && strcmp(bn->as.s.data, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* True when `form` is `(+ bind 1)` or `(+ 1 bind)` resolving the head
 * to the canonical `+` prim, with the named binding on one side and a
 * literal 1 on the other. Used as an inc-equivalent for counter steps
 * since Clojure-canonical user code often writes `(+ i 1)` rather than
 * `(inc i)` -- both produce the same value for tagged-int counters and
 * the fused loop op needs both shapes to fire. */
static int is_plus_one_of_local(compiler_t *c, mino_val *form,
                                const char *expected_name)
{
    if (!mino_is_cons(form)) return 0;
    mino_val *h = form->as.cons.car;
    if (h == NULL || mino_type_of(h) != MINO_SYMBOL) return 0;
    if (strcmp(h->as.s.data, "+") != 0) return 0;
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) return 0;
    mino_val *a1 = args->as.cons.car;
    mino_val *rest = args->as.cons.cdr;
    if (!mino_is_cons(rest)) return 0;
    mino_val *a2 = rest->as.cons.car;
    if (rest->as.cons.cdr != NULL
        && mino_type_of(rest->as.cons.cdr) != MINO_NIL) {
        if (mino_is_cons(rest->as.cons.cdr)) return 0;
    }
    if (a1 == NULL || a2 == NULL) return 0;
    /* Exactly one operand is the binding symbol; the other is integer 1. */
    int a1_sym = (mino_type_of(a1) == MINO_SYMBOL
                  && strcmp(a1->as.s.data, expected_name) == 0);
    int a2_sym = (mino_type_of(a2) == MINO_SYMBOL
                  && strcmp(a2->as.s.data, expected_name) == 0);
    int a1_one = (mino_val_int_p(a1) && mino_val_int_get(a1) == 1);
    int a2_one = (mino_val_int_p(a2) && mino_val_int_get(a2) == 1);
    if (!((a1_sym && a2_one) || (a2_sym && a1_one))) return 0;
    return head_is_canonical_pure_prim(c, h);
}

/* Recognise the inc-equivalent step for a loop counter binding:
 *   (inc bname)      -- canonical
 *   (+ bname 1)      -- Clojure-canonical user-side equivalent
 *   (+ 1 bname)      -- commutative arg order. */
static int is_inc_step_of_local(compiler_t *c, mino_val *step,
                                const char *bname)
{
    return is_unary_call_of_local(c, step, "inc", bname)
        || is_plus_one_of_local(c, step, bname);
}

/* True when `form` is `(+ bind N)` or `(+ N bind)` resolving the head
 * to the canonical `+` prim, with the named binding on one side and a
 * tagged-int literal on the other. Distinct from `is_plus_one_of_local`
 * because the matched step can be any tagged-int constant -- the
 * caller materialises N via LOAD_K and feeds the register as the
 * ACC stencil's step source. Excludes N == 1 since the existing
 * `(+ acc 1)` path is already handled by `is_inc_step_of_local`. */
static int is_plus_int_const_of_local(compiler_t *c, mino_val *form,
                                       const char *expected_name,
                                       long long *out_n)
{
    if (!mino_is_cons(form)) return 0;
    mino_val *h = form->as.cons.car;
    if (h == NULL || mino_type_of(h) != MINO_SYMBOL) return 0;
    if (strcmp(h->as.s.data, "+") != 0) return 0;
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) return 0;
    mino_val *a1 = args->as.cons.car;
    mino_val *rest = args->as.cons.cdr;
    if (!mino_is_cons(rest)) return 0;
    mino_val *a2 = rest->as.cons.car;
    if (rest->as.cons.cdr != NULL
        && mino_type_of(rest->as.cons.cdr) != MINO_NIL) {
        if (mino_is_cons(rest->as.cons.cdr)) return 0;
    }
    if (a1 == NULL || a2 == NULL) return 0;
    int a1_sym = (mino_type_of(a1) == MINO_SYMBOL
                  && strcmp(a1->as.s.data, expected_name) == 0);
    int a2_sym = (mino_type_of(a2) == MINO_SYMBOL
                  && strcmp(a2->as.s.data, expected_name) == 0);
    int a1_int = mino_val_int_p(a1);
    int a2_int = mino_val_int_p(a2);
    long long n;
    if (a1_sym && a2_int) {
        n = mino_val_int_get(a2);
    } else if (a2_sym && a1_int) {
        n = mino_val_int_get(a1);
    } else {
        return 0;
    }
    /* N == 1 is handled by is_inc_step_of_local; skip so the const
     * lane stays distinct. */
    if (n == 1) return 0;
    if (!head_is_canonical_pure_prim(c, h)) return 0;
    if (out_n != NULL) *out_n = n;
    return 1;
}

/* True when `form` is `(+ this_name other_name)` or `(+ other_name
 * this_name)` resolving the head to the canonical `+` prim. Used to
 * recognise an accumulator step `(+ acc i)` where `acc` is the
 * accumulator binding and `i` is some other loop binding (typically
 * the counter). `*out_other_name` receives the matched other-side
 * symbol's name (a borrowed pointer into the form). */
static int is_plus_two_locals_of(compiler_t *c, mino_val *form,
                                 const char *this_name,
                                 const char **out_other_name)
{
    if (!mino_is_cons(form)) return 0;
    mino_val *h = form->as.cons.car;
    if (h == NULL || mino_type_of(h) != MINO_SYMBOL) return 0;
    if (strcmp(h->as.s.data, "+") != 0) return 0;
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) return 0;
    mino_val *a1 = args->as.cons.car;
    mino_val *rest = args->as.cons.cdr;
    if (!mino_is_cons(rest)) return 0;
    mino_val *a2 = rest->as.cons.car;
    if (rest->as.cons.cdr != NULL
        && mino_type_of(rest->as.cons.cdr) != MINO_NIL) {
        if (mino_is_cons(rest->as.cons.cdr)) return 0;
    }
    if (a1 == NULL || a2 == NULL) return 0;
    if (mino_type_of(a1) != MINO_SYMBOL) return 0;
    if (mino_type_of(a2) != MINO_SYMBOL) return 0;
    const char *n1 = a1->as.s.data;
    const char *n2 = a2->as.s.data;
    if (strcmp(n1, this_name) == 0 && strcmp(n2, this_name) != 0) {
        if (out_other_name != NULL) *out_other_name = n2;
        return head_is_canonical_pure_prim(c, h);
    }
    if (strcmp(n2, this_name) == 0 && strcmp(n1, this_name) != 0) {
        if (out_other_name != NULL) *out_other_name = n1;
        return head_is_canonical_pure_prim(c, h);
    }
    return 0;
}

/* Match the backward-counted shape:
 *   (if (zero? bX) <exit> (recur step0 step1 ...))
 * where each step is (inc bi) / (dec bi) self-referencing its own
 * binding and the tested binding is the decremented one.
 * Emits OP_LOOP_INT_DEC[_INC] and compiles the exit branch in place.
 * Returns 1 on emit, 0 on miss, -1 on error. */
static int try_match_dec_shape(compiler_t *c,
                               mino_val *cond_form,
                               mino_val *then_form,
                               mino_val *else_form,
                               mino_val *bindings, int n_bindings,
                               loop_target_t *this_loop, int dst, int tail)
{
    if (!mino_is_cons(cond_form)) return 0;
    mino_val *ch = cond_form->as.cons.car;
    if (ch == NULL || mino_type_of(ch) != MINO_SYMBOL) return 0;
    if (strcmp(ch->as.s.data, "zero?") != 0) return 0;
    if (!head_is_canonical_pure_prim(c, ch)) return 0;
    mino_val *cargs = cond_form->as.cons.cdr;
    if (!mino_is_cons(cargs)) return 0;
    mino_val *test_sym = cargs->as.cons.car;
    if (test_sym == NULL || mino_type_of(test_sym) != MINO_SYMBOL) return 0;
    if (cargs->as.cons.cdr != NULL
        && mino_type_of(cargs->as.cons.cdr) != MINO_NIL) {
        if (mino_is_cons(cargs->as.cons.cdr)) return 0;
    }
    int test_idx = find_binding_idx(bindings, n_bindings,
                                    test_sym->as.s.data);
    if (test_idx < 0) return 0;

    /* else must be (recur step0 step1 ...). For each binding the step is:
     *   test (counter) binding: (dec bname)
     *   acc binding:            (inc bname) / (+ bname 1) / (+ 1 bname)
     *                           OR (+ bname OTHER) / (+ OTHER bname)
     *                           where OTHER is another loop binding
     *                           (register-step ACC; emits
     *                           OP_LOOP_INT_DEC_ACC) OR
     *                           (+ bname N) / (+ N bname) where N is
     *                           a non-1 int constant (const-step ACC;
     *                           N is materialised via LOAD_K into a
     *                           temp register that feeds the same op). */
    int       acc_step_src_idx     = -1;
    int       acc_step_const_valid = 0;
    long long acc_step_const       = 0;
    if (!mino_is_cons(else_form)) return 0;
    mino_val *eh = else_form->as.cons.car;
    if (eh == NULL || mino_type_of(eh) != MINO_SYMBOL) return 0;
    if (strcmp(eh->as.s.data, "recur") != 0) return 0;
    mino_val *steps = else_form->as.cons.cdr;
    for (int i = 0; i < n_bindings; i++) {
        if (!mino_is_cons(steps)) return 0;
        mino_val *step = steps->as.cons.car;
        mino_val *bn = vec_nth(bindings, (size_t)(i * 2));
        if (bn == NULL || mino_type_of(bn) != MINO_SYMBOL) return 0;
        const char *bname = bn->as.s.data;
        if (i == test_idx) {
            if (!is_unary_call_of_local(c, step, "dec", bname)) return 0;
            steps = steps->as.cons.cdr;
            continue;
        }
        if (is_inc_step_of_local(c, step, bname)) {
            steps = steps->as.cons.cdr;
            continue;
        }
        if (n_bindings == 2
            && acc_step_src_idx < 0 && !acc_step_const_valid) {
            const char *other = NULL;
            if (is_plus_two_locals_of(c, step, bname, &other)) {
                int other_idx = find_binding_idx(bindings, n_bindings,
                                                  other);
                if (other_idx >= 0 && other_idx != i) {
                    acc_step_src_idx = other_idx;
                    steps = steps->as.cons.cdr;
                    continue;
                }
            }
            long long n;
            if (is_plus_int_const_of_local(c, step, bname, &n)) {
                acc_step_const_valid = 1;
                acc_step_const = n;
                steps = steps->as.cons.cdr;
                continue;
            }
        }
        return 0;
    }
    if (steps != NULL && mino_type_of(steps) != MINO_NIL) {
        if (mino_is_cons(steps)) return 0;
    }

    /* For const-step ACC: pre-load the literal N into a fresh
     * register before the loop entry so the fused opcode reads it as
     * a register-shaped step source. */
    int const_step_reg = -1;
    if (acc_step_const_valid) {
        const_step_reg = alloc_reg(c);
        if (const_step_reg < 0) return -1;
        int k = add_const(c, mino_int(c->S, acc_step_const));
        if (k < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)const_step_reg, (unsigned)k);
    }

    /* All matched. Emit the fused step op at the recur target, then
     * compile the exit branch in place. */
    this_loop->entry_pc = (int)c->bc->code_len;
    int test_reg = this_loop->bind_regs[test_idx];
    if (n_bindings == 1) {
        emit_abc(c, OP_LOOP_INT_DEC, (unsigned)test_reg, 0, 0);
    } else if (acc_step_src_idx >= 0 || acc_step_const_valid) {
        int acc_idx = 1 - test_idx;
        int acc_reg = this_loop->bind_regs[acc_idx];
        int src_reg = acc_step_const_valid
            ? const_step_reg
            : this_loop->bind_regs[acc_step_src_idx];
        emit_abc(c, OP_LOOP_INT_DEC_ACC,
                 (unsigned)test_reg, 0, (unsigned)acc_reg);
        emit_abx(c, OP_NOP, 0, (unsigned)src_reg);
    } else {
        int inc_idx = 1 - test_idx;
        int inc_reg = this_loop->bind_regs[inc_idx];
        emit_abc(c, OP_LOOP_INT_DEC_INC,
                 (unsigned)test_reg, (unsigned)inc_reg, 0);
    }
    if (!c->ok) return -1;
    if (compile_expr(c, then_form, dst, tail) < 0) return -1;
    return 1;
}

/* Match an (cmp lhs rhs) comparison test form where cmp is a canonical
 * ordering prim and one operand is a loop binding. On match, writes:
 *   *counter_idx       = binding index of the counter side
 *   *limit_form_out    = the form for the limit (the other operand)
 *   *test_true_is_exit = 1 when (cmp lhs rhs) being true means exit
 *                        (i.e., the test polarity is "while not done"),
 *                        0 when test true means continue.
 * Returns 1 on match, 0 on miss. Recognizes (< c L), (<= c L),
 * (>= c L), (> c L), and their swapped-arg forms. */
static int match_lt_test(compiler_t *c, mino_val *cond_form,
                         mino_val *bindings, int n_bindings,
                         int *counter_idx, mino_val **limit_form_out,
                         int *test_true_is_exit)
{
    if (!mino_is_cons(cond_form)) return 0;
    mino_val *h = cond_form->as.cons.car;
    if (h == NULL || mino_type_of(h) != MINO_SYMBOL) return 0;

    /* For each recognized ordering prim, encode the polarity:
     *   counter on the LEFT, limit on the RIGHT, the C-level test
     *   that drives the fused opcode's "continue" branch is
     *   `counter < limit`. So:
     *     (< c L)  -> continue iff c < L: test_true=continue, swap=0
     *     (<= c L) -> continue iff c < L+1 -- cannot fuse precisely
     *                 without representing the +1; reject for v1.
     *     (>= c L) -> continue iff c < L: test_true=exit, swap=0
     *     (> c L)  -> continue iff c < L+1 -- reject.
     *     (> L c)  -> equivalent to (< c L): test_true=continue, swap=1
     *     (<= L c) -> equivalent to (>= c L): test_true=exit, swap=1 */
    int  swap_args         = 0;
    int  matched           = 0;
    int  test_true_is_exit_local = 0;
    if (strcmp(h->as.s.data, "<") == 0) {
        matched = 1; test_true_is_exit_local = 0; swap_args = 0;
    } else if (strcmp(h->as.s.data, ">=") == 0) {
        matched = 1; test_true_is_exit_local = 1; swap_args = 0;
    } else if (strcmp(h->as.s.data, ">") == 0) {
        /* (> L c) acts like (< c L) with operands swapped; we then
         * detect on the swapped order. (> c L) on a fixed counter
         * would need L+1 to express precisely, so we reject when the
         * counter is on the LEFT below. */
        matched = 1; test_true_is_exit_local = 0; swap_args = 1;
    } else if (strcmp(h->as.s.data, "<=") == 0) {
        /* Symmetric: (<= L c) acts like (>= c L) swapped. */
        matched = 1; test_true_is_exit_local = 1; swap_args = 1;
    }
    if (!matched) return 0;
    if (!head_is_canonical_pure_prim(c, h)) return 0;

    mino_val *args = cond_form->as.cons.cdr;
    if (!mino_is_cons(args)) return 0;
    mino_val *arg1 = args->as.cons.car;
    mino_val *args2 = args->as.cons.cdr;
    if (!mino_is_cons(args2)) return 0;
    mino_val *arg2 = args2->as.cons.car;
    if (args2->as.cons.cdr != NULL
        && mino_type_of(args2->as.cons.cdr) != MINO_NIL) {
        if (mino_is_cons(args2->as.cons.cdr)) return 0;
    }

    /* Identify which side is the counter binding. */
    mino_val *lhs = swap_args ? arg2 : arg1;
    mino_val *rhs = swap_args ? arg1 : arg2;
    if (lhs == NULL || mino_type_of(lhs) != MINO_SYMBOL) return 0;
    int idx = find_binding_idx(bindings, n_bindings, lhs->as.s.data);
    if (idx < 0) return 0;

    /* The limit operand must not also be the same counter (and must
     * be expressible as a value the compiler can place in a register --
     * left to the caller via compile_expr on this form). */
    *counter_idx        = idx;
    *limit_form_out     = rhs;
    *test_true_is_exit  = test_true_is_exit_local;
    return 1;
}

/* Match the forward-counted shape:
 *   (if (< c L) (recur (inc c) ...) <exit>)        ; test_true=continue
 *   (if (>= c L) <exit> (recur (inc c) ...))       ; test_true=exit
 * (and the > / <= / arg-swap rearrangements via match_lt_test).
 * Two-binding form allowed when the second step is (inc carry).
 * Emits OP_LOOP_INT_LT[_INC] and compiles the exit branch.
 * Returns 1 on emit, 0 on miss, -1 on error. */
static int try_match_lt_shape(compiler_t *c,
                              mino_val *cond_form,
                              mino_val *then_form,
                              mino_val *else_form,
                              mino_val *bindings, int n_bindings,
                              loop_target_t *this_loop, int dst, int tail)
{
    int          counter_idx;
    mino_val  *limit_form;
    int          test_true_is_exit;
    if (!match_lt_test(c, cond_form, bindings, n_bindings,
                       &counter_idx, &limit_form, &test_true_is_exit)) {
        return 0;
    }

    /* Resolve which branch holds the recur and which holds the exit. */
    mino_val *recur_form;
    mino_val *exit_form;
    if (test_true_is_exit) {
        exit_form  = then_form;
        recur_form = else_form;
    } else {
        recur_form = then_form;
        exit_form  = else_form;
    }

    /* recur_form must be (recur step0 step1 ...). For each binding the
     * step is one of:
     *   counter binding:  (inc bname) / (+ bname 1) / (+ 1 bname)
     *   acc binding:      same as counter (carry-by-1 fast path) OR
     *                     (+ bname OTHER) / (+ OTHER bname) where OTHER
     *                     is another loop binding (register-step ACC
     *                     shape, emits OP_LOOP_INT_LT_ACC) OR
     *                     (+ bname N) / (+ N bname) where N is a
     *                     non-1 tagged-int constant (const-step ACC
     *                     shape; materialised via LOAD_K into a temp
     *                     register that feeds the same opcode).
     * `acc_step_src_idx` stays -1 in the inc-only path; on register-
     * step ACC match it holds the binding index whose register feeds
     * the accumulator. `acc_step_const_valid` / `acc_step_const` hold
     * the literal for the const-step ACC lane. */
    int       acc_step_src_idx     = -1;
    int       acc_step_const_valid = 0;
    long long acc_step_const       = 0;
    if (!mino_is_cons(recur_form)) return 0;
    mino_val *rh = recur_form->as.cons.car;
    if (rh == NULL || mino_type_of(rh) != MINO_SYMBOL) return 0;
    if (strcmp(rh->as.s.data, "recur") != 0) return 0;
    mino_val *steps = recur_form->as.cons.cdr;
    for (int i = 0; i < n_bindings; i++) {
        if (!mino_is_cons(steps)) return 0;
        mino_val *step = steps->as.cons.car;
        mino_val *bn = vec_nth(bindings, (size_t)(i * 2));
        if (bn == NULL || mino_type_of(bn) != MINO_SYMBOL) return 0;
        const char *bname = bn->as.s.data;
        if (is_inc_step_of_local(c, step, bname)) {
            steps = steps->as.cons.cdr;
            continue;
        }
        if (n_bindings == 2 && i != counter_idx
            && acc_step_src_idx < 0 && !acc_step_const_valid) {
            const char *other = NULL;
            if (is_plus_two_locals_of(c, step, bname, &other)) {
                int other_idx = find_binding_idx(bindings, n_bindings,
                                                  other);
                if (other_idx >= 0 && other_idx != i) {
                    acc_step_src_idx = other_idx;
                    steps = steps->as.cons.cdr;
                    continue;
                }
            }
            long long n;
            if (is_plus_int_const_of_local(c, step, bname, &n)) {
                acc_step_const_valid = 1;
                acc_step_const = n;
                steps = steps->as.cons.cdr;
                continue;
            }
        }
        return 0;
    }
    if (steps != NULL && mino_type_of(steps) != MINO_NIL) {
        if (mino_is_cons(steps)) return 0;
    }

    /* Materialize the limit into a fresh register BEFORE the fused
     * opcode emit. This runs once at loop setup. The limit register
     * survives the loop because the fused opcode doesn't clobber it
     * and no other emission inside the matched body writes to it. */
    int limit_reg = alloc_reg(c);
    if (limit_reg < 0) return -1;
    if (compile_expr(c, limit_form, limit_reg, 0) < 0) return -1;

    /* For const-step ACC: pre-load the literal N into a fresh
     * register before entering the loop body so the fused opcode
     * has a register-shaped step source. The temp register lives for
     * the loop's lifetime; no instruction inside the matched body
     * writes to it. */
    int const_step_reg = -1;
    if (acc_step_const_valid) {
        const_step_reg = alloc_reg(c);
        if (const_step_reg < 0) return -1;
        int k = add_const(c, mino_int(c->S, acc_step_const));
        if (k < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)const_step_reg, (unsigned)k);
    }

    this_loop->entry_pc = (int)c->bc->code_len;
    int counter_reg = this_loop->bind_regs[counter_idx];
    if (n_bindings == 1) {
        emit_abc(c, OP_LOOP_INT_LT,
                 (unsigned)counter_reg, (unsigned)limit_reg, 0);
    } else if (acc_step_src_idx >= 0 || acc_step_const_valid) {
        int acc_idx = 1 - counter_idx;
        int acc_reg = this_loop->bind_regs[acc_idx];
        int src_reg = acc_step_const_valid
            ? const_step_reg
            : this_loop->bind_regs[acc_step_src_idx];
        emit_abc(c, OP_LOOP_INT_LT_ACC,
                 (unsigned)counter_reg, (unsigned)limit_reg,
                 (unsigned)acc_reg);
        /* Trailing word: Bx carries the step-source register index so
         * the stencil reads it via IMM_BX2 / the interpreter via
         * Bx_OF(code[pc++]). OP_NOP keeps a stray decode harmless. */
        emit_abx(c, OP_NOP, 0, (unsigned)src_reg);
    } else {
        int carry_idx = 1 - counter_idx;
        int carry_reg = this_loop->bind_regs[carry_idx];
        emit_abc(c, OP_LOOP_INT_LT_INC,
                 (unsigned)counter_reg, (unsigned)limit_reg,
                 (unsigned)carry_reg);
    }
    if (!c->ok) return -1;
    if (compile_expr(c, exit_form, dst, tail) < 0) return -1;
    return 1;
}

/* Detect canonical counted-loop shapes and emit a fused opcode in
 * place of the recur-driven body. Returns 1 when the fused op was
 * emitted, 0 to fall through to the generic body compile. -1
 * propagates an error.
 *
 * What this leverages from Clojure: (loop ... (recur ...)) is a
 * stable, homoiconic recur form -- the compiler sees the exact shape
 * of the iteration step at compile time. Persistent bindings let the
 * step's value depend only on the same binding's prior value, never
 * on an aliased state from a parallel iteration. */
static int try_compile_counted_loop(compiler_t *c,
                                    mino_val *body,
                                    mino_val *bindings,
                                    int n_bindings,
                                    loop_target_t *this_loop,
                                    int dst, int tail)
{
    if (n_bindings < 1 || n_bindings > 2) return 0;

    mino_val *if_form = match_single_form(body, "if");
    if (if_form == NULL) return 0;

    /* (if cond then else?) -- need both branches. */
    mino_val *iargs = if_form->as.cons.cdr;
    if (!mino_is_cons(iargs)) return 0;
    mino_val *cond_form = iargs->as.cons.car;
    mino_val *r1 = iargs->as.cons.cdr;
    if (!mino_is_cons(r1)) return 0;
    mino_val *then_form = r1->as.cons.car;
    mino_val *r2 = r1->as.cons.cdr;
    if (!mino_is_cons(r2)) return 0;
    mino_val *else_form = r2->as.cons.car;
    if (r2->as.cons.cdr != NULL && mino_type_of(r2->as.cons.cdr) != MINO_NIL) {
        if (mino_is_cons(r2->as.cons.cdr)) return 0;
    }

    int rc;
    rc = try_match_dec_shape(c, cond_form, then_form, else_form,
                             bindings, n_bindings, this_loop, dst, tail);
    if (rc != 0) return rc;
    rc = try_match_lt_shape(c, cond_form, then_form, else_form,
                            bindings, n_bindings, this_loop, dst, tail);
    if (rc != 0) return rc;
    return 0;
}

/* ------------------------------------------------------------------- */
/* Builder-pattern rewrite                                              */
/* ------------------------------------------------------------------- */

/* Pre-compile rewrite for the canonical persistent-builder loop:
 *
 *   (loop [<v1> <v1-init> ... acc <empty-literal>]
 *     (if <test>
 *       (recur ... (conj acc <x>))           ; one branch
 *       acc))                                 ; other branch
 *
 * gets rewritten to
 *
 *   (persistent!
 *     (loop [<v1> <v1-init> ... acc (transient <empty-literal>)]
 *       (if <test>
 *         (recur ... (conj! acc <x>))
 *         acc)))
 *
 * Sister forms covered:
 *   - (assoc acc <k> <v>) builder step (init must be `{}`)
 *   - then/else swapped: acc as the recur branch's other half.
 *
 * Returns the rewritten form, or NULL when no match (caller falls
 * through to the generic compile_loop). The rewrite relies on
 * owner-tagged in-place transient mutation to pay off; without that
 * substrate the rewritten form runs slower than the persistent
 * baseline. */

static int is_symbol_named(const mino_val *v, const char *name)
{
    return v != NULL
        && mino_type_of(v) == MINO_SYMBOL
        && strcmp(v->as.s.data, name) == 0;
}

static int is_empty_vec_literal(const mino_val *v)
{
    return v != NULL
        && mino_type_of(v) == MINO_VECTOR
        && v->as.vec.len == 0;
}

static int is_empty_map_literal(const mino_val *v)
{
    return v != NULL
        && mino_type_of(v) == MINO_MAP
        && v->as.map.len == 0;
}

static int is_empty_set_literal(const mino_val *v)
{
    return v != NULL
        && mino_type_of(v) == MINO_SET
        && v->as.set.len == 0;
}

/* Match `(<head> <args>...)`. Returns the args cons cell when head
 * is a symbol named `name`, NULL otherwise. */
static mino_val *match_call(mino_val *form, const char *name)
{
    if (!mino_is_cons(form)) return NULL;
    if (!is_symbol_named(form->as.cons.car, name)) return NULL;
    return form->as.cons.cdr;
}

/* List length of a cons chain. */
static size_t cons_count(const mino_val *list)
{
    return (size_t)bc_cons_len(list);
}

/* Build a cons list from a NULL-terminated array of mino_val *. */
static mino_val *list_from_array(mino_state *S, mino_val **items,
                                     size_t n)
{
    mino_val *out = mino_nil(S);
    size_t      i;
    for (i = n; i > 0; i--) {
        out = mino_cons(S, items[i - 1], out);
    }
    return out;
}

/* Returns 1 if `form` dereferences a symbol named `name` in a way
 * that would diverge under transient semantics. The transient
 * protocol covers `get` / `nth` / `count` / `get-in` reads where
 * the acc is the *direct* first argument; those calls are
 * semantics-preserving when acc is a transient. Any other shape
 * (e.g., `(seq acc)`, `(reduce f acc)`, `(= acc x)`) is rejected.
 * Quote-forms are data and don't dereference symbols. */
static int form_contains_unsafe_acc(mino_val *form, const char *name)
{
    if (form == NULL) return 0;
    if (mino_type_of(form) == MINO_SYMBOL) {
        return strcmp(form->as.s.data, name) == 0;
    }
    if (mino_type_of(form) == MINO_VECTOR) {
        for (size_t i = 0; i < form->as.vec.len; i++) {
            if (form_contains_unsafe_acc(vec_nth(form, i), name)) return 1;
        }
        return 0;
    }
    if (mino_is_cons(form)) {
        mino_val *head = form->as.cons.car;
        if (head != NULL && mino_type_of(head) == MINO_SYMBOL) {
            const char *hname = head->as.s.data;
            if (strcmp(hname, "quote") == 0
                || strcmp(hname, "quote*") == 0) return 0;
            if (strcmp(hname, "get") == 0
                || strcmp(hname, "nth") == 0
                || strcmp(hname, "count") == 0
                || strcmp(hname, "get-in") == 0) {
                mino_val *args = form->as.cons.cdr;
                if (mino_is_cons(args)) {
                    mino_val *first = args->as.cons.car;
                    int first_is_acc =
                        (first != NULL
                         && mino_type_of(first) == MINO_SYMBOL
                         && strcmp(first->as.s.data, name) == 0);
                    if (first_is_acc) {
                        /* Direct acc read via transient-supported op.
                         * Skip the first-arg check; only later args
                         * (k / default) need scanning. */
                        mino_val *rest = args->as.cons.cdr;
                        while (mino_is_cons(rest)) {
                            if (form_contains_unsafe_acc(
                                    rest->as.cons.car, name)) return 1;
                            rest = rest->as.cons.cdr;
                        }
                        return 0;
                    }
                }
            }
        }
        while (mino_is_cons(form)) {
            if (form_contains_unsafe_acc(form->as.cons.car, name)) return 1;
            form = form->as.cons.cdr;
        }
        if (form != NULL && mino_type_of(form) == MINO_SYMBOL) {
            return strcmp(form->as.s.data, name) == 0;
        }
        return 0;
    }
    return 0;
}

/* Walk `recur-args` looking for the position whose value is
 * (conj acc <x>) or (assoc acc <k> <v>) referencing `acc_sym`.
 * Returns the position index (0-based) on match, -1 otherwise.
 * `*step_kind` is set to 0 for conj, 1 for assoc. */
static int find_acc_step(mino_val *recur_args, mino_val *acc_sym,
                          int *step_kind)
{
    int  pos = 0;
    while (mino_is_cons(recur_args)) {
        mino_val *arg = recur_args->as.cons.car;
        mino_val *call_args;
        if ((call_args = match_call(arg, "conj")) != NULL
            && mino_is_cons(call_args)
            && is_symbol_named(call_args->as.cons.car, acc_sym->as.s.data)) {
            *step_kind = 0;
            return pos;
        }
        if ((call_args = match_call(arg, "assoc")) != NULL
            && mino_is_cons(call_args)
            && is_symbol_named(call_args->as.cons.car, acc_sym->as.s.data)) {
            *step_kind = 1;
            return pos;
        }
        pos++;
        recur_args = recur_args->as.cons.cdr;
    }
    return -1;
}

/* Replace `recur-args[step_pos]` with a rewritten (conj! ...) or
 * (assoc! ...) call. Returns the new args list (cons chain). */
static mino_val *rewrite_recur_args(mino_state *S,
                                       mino_val *recur_args, int step_pos,
                                       int step_kind)
{
    size_t       n = cons_count(recur_args);
    mino_val **buf;
    mino_val  *result;
    size_t       i;
    mino_val  *cur = recur_args;
    if (n == 0) return recur_args;
    buf = (mino_val **)malloc(n * sizeof(*buf));
    if (buf == NULL) return NULL;
    for (i = 0; i < n; i++) {
        buf[i] = cur->as.cons.car;
        cur    = cur->as.cons.cdr;
    }
    {
        mino_val *step     = buf[step_pos];
        mino_val *new_head;
        mino_val *step_args;
        /* Pin step: buf is malloc'd and invisible to the GC; mino_symbol
         * may trigger a collection that would free step. */
        gc_pin(step);
        new_head = mino_symbol(S, step_kind == 0 ? "conj!" : "assoc!");
        if (new_head == NULL) { gc_unpin(1); free(buf); return NULL; }
        /* Re-read step_args after the allocation so we hold a live
         * pointer regardless of any intervening GC cycle. */
        step_args = buf[step_pos]->as.cons.cdr;  /* skip head sym */
        gc_unpin(1);
        buf[step_pos] = mino_cons(S, new_head, step_args);
        if (buf[step_pos] == NULL) { free(buf); return NULL; }
        gc_pin(buf[step_pos]);
    }
    /* Pin all remaining buf entries: list_from_array calls mino_cons
     * repeatedly and each call may trigger a GC cycle.  buf is
     * malloc-owned so the collector cannot see it; every slot must be
     * rooted before we enter the cons loop. buf[step_pos] is already
     * pinned above (count = 1); pin the rest now. */
    {
        size_t j;
        for (j = 0; j < n; j++) {
            if ((int)j != step_pos) gc_pin(buf[j]);
        }
    }
    result = list_from_array(S, buf, n);
    gc_unpin(n); /* unpin all n entries (step_pos + the n-1 others) */
    free(buf);
    return result;
}

static mino_val *try_builder_rewrite(mino_state *S, mino_val *form)
{
    mino_val *bindings;
    mino_val *body;
    mino_val *if_form;
    mino_val *if_args;
    mino_val *test;
    mino_val *then_form;
    mino_val *else_form;
    mino_val *acc_sym;
    mino_val *acc_init;
    mino_val *recur_args;
    mino_val *new_init;
    mino_val *new_bindings;
    mino_val *new_recur;
    mino_val *new_if;
    mino_val *new_loop;
    mino_val *new_persistent;
    int         step_kind = 0;
    int         step_pos;
    int         is_then_recur;
    size_t      blen, i;
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) return NULL;
    bindings = args->as.cons.car;
    body     = args->as.cons.cdr;
    if (bindings == NULL || mino_type_of(bindings) != MINO_VECTOR) return NULL;
    blen = bindings->as.vec.len;
    if (blen < 2 || (blen & 1) != 0) return NULL;
    /* Acc binding is the LAST pair. */
    acc_sym  = vec_nth(bindings, blen - 2);
    acc_init = vec_nth(bindings, blen - 1);
    if (acc_sym == NULL || mino_type_of(acc_sym) != MINO_SYMBOL) return NULL;
    /* Init can be an empty literal (fast path, statically shape-checked
     * against the step) or an arbitrary expression (runtime-typed).
     * Non-literal seeds wrap as (transient <expr>) and let transient,
     * conj!, and assoc! validate the shape at runtime. */
    int acc_is_empty_vec = is_empty_vec_literal(acc_init);
    int acc_is_empty_map = is_empty_map_literal(acc_init);
    int acc_is_empty_set = is_empty_set_literal(acc_init);
    int acc_is_empty_literal =
        acc_is_empty_vec || acc_is_empty_map || acc_is_empty_set;
    /* Body is exactly one form: (if <test> <then> <else>). */
    if (!mino_is_cons(body) || mino_is_cons(body->as.cons.cdr)) return NULL;
    if_form = body->as.cons.car;
    if_args = match_call(if_form, "if");
    if (if_args == NULL) return NULL;
    if (!mino_is_cons(if_args)) return NULL;
    test      = if_args->as.cons.car;
    if (!mino_is_cons(if_args->as.cons.cdr)) return NULL;
    then_form = if_args->as.cons.cdr->as.cons.car;
    if (!mino_is_cons(if_args->as.cons.cdr->as.cons.cdr)) return NULL;
    else_form = if_args->as.cons.cdr->as.cons.cdr->as.cons.car;
    /* Reject 3-clause when else is missing or there's a trailing arg. */
    if (mino_is_cons(if_args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
        return NULL;
    }
    /* Identify which branch is the recur, which is the bare acc-sym. */
    is_then_recur = -1;
    if (is_symbol_named(else_form, acc_sym->as.s.data)
        && match_call(then_form, "recur") != NULL) {
        is_then_recur = 1;
        recur_args    = match_call(then_form, "recur");
    } else if (is_symbol_named(then_form, acc_sym->as.s.data)
               && match_call(else_form, "recur") != NULL) {
        is_then_recur = 0;
        recur_args    = match_call(else_form, "recur");
    } else {
        return NULL;
    }
    step_pos = find_acc_step(recur_args, acc_sym, &step_kind);
    if (step_pos < 0) return NULL;
    /* When init is an empty literal we know the runtime shape; reject
     * step/init mismatches that the user clearly didn't intend
     * (e.g., (assoc [] k v) on an empty vector, an indexed-assoc the
     * rewriter doesn't model). Non-literal seeds defer to runtime. */
    if (acc_is_empty_literal) {
        if (step_kind == 1 && !acc_is_empty_map) return NULL;
        if (step_kind == 0 && !acc_is_empty_vec && !acc_is_empty_set) {
            return NULL;
        }
    }
    /* Safety: the rewrite reinterprets acc as a transient inside the
     * loop body. The transient protocol covers `count` / `nth` / `get`
     * but not `seq` / `reduce` / `=` / `contains?`. Any read of the
     * accumulator outside the recognized step is a divergence risk.
     * Conservative gate: reject if acc-sym is referenced in <test> at
     * all, or in any recur-arg position outside the step, unless the
     * acc-ref is the direct first arg of a transient-supported read
     * (`get` / `nth` / `count` / `get-in`). The bare-exit branch
     * (already validated above) is the only place acc is read as
     * a whole value, and it's read *after* the loop exits so the
     * persistent-wrap promotes it back. */
    {
        const char *name = acc_sym->as.s.data;
        if (form_contains_unsafe_acc(test, name)) return NULL;
        {
            mino_val *cur2 = recur_args;
            int         pos2 = 0;
            while (mino_is_cons(cur2)) {
                mino_val *arg = cur2->as.cons.car;
                if (pos2 == step_pos) {
                    /* The step is structurally (conj acc <x>) or
                     * (assoc acc <k> <v>). Skip the head and the acc
                     * reference; the remaining args may only reference
                     * acc via transient-supported reads. */
                    mino_val *after_head = arg->as.cons.cdr;
                    if (mino_is_cons(after_head)) {
                        mino_val *after_acc = after_head->as.cons.cdr;
                        while (mino_is_cons(after_acc)) {
                            if (form_contains_unsafe_acc(
                                    after_acc->as.cons.car, name)) {
                                return NULL;
                            }
                            after_acc = after_acc->as.cons.cdr;
                        }
                    }
                } else if (form_contains_unsafe_acc(arg, name)) {
                    return NULL;
                }
                pos2++;
                cur2 = cur2->as.cons.cdr;
            }
        }
    }
    /* --- Build the rewritten form. ---
     * Strategy: clone the loop's binding vector, replacing the last
     * acc init with `(transient <init>)`, then construct the new
     * `if` whose recur step calls `conj!` / `assoc!`, then wrap the
     * whole loop in `(persistent! ...)`. */
    {
        mino_val *transient_sym = mino_symbol(S, "transient");
        if (transient_sym == NULL) return NULL;
        /* Pin transient_sym and acc_init: mino_nil + inner mino_cons may
         * trigger GC between their allocations and the outer mino_cons
         * read of these pointers. */
        gc_pin(transient_sym);
        gc_pin(acc_init);
        new_init = mino_cons(
            S, transient_sym, mino_cons(S, acc_init, mino_nil(S)));
        gc_unpin(2);
    }
    /* Build a fresh vector for bindings. */
    {
        mino_val **bind_items;
        bind_items = (mino_val **)malloc(blen * sizeof(*bind_items));
        if (bind_items == NULL) return NULL;
        /* Pin bindings: bind_items is malloc'd (GC-invisible) and holds
         * pointers from vec_nth(bindings, i). Pinning bindings keeps all
         * elements alive through vec_from_array without pinning each slot
         * individually. Pin new_init for the same reason (it is the last
         * slot and was produced by the preceding mino_cons chain). */
        gc_pin(bindings);
        for (i = 0; i < blen - 1; i++) {
            bind_items[i] = vec_nth(bindings, i);
        }
        gc_pin(new_init);
        bind_items[blen - 1] = new_init;
        new_bindings = vec_from_array(S, bind_items, blen);
        gc_unpin(2);
        free(bind_items);
        if (new_bindings == NULL) return NULL;
    }
    /* Rewrite the recur arglist. */
    {
        mino_val *recur_sym = mino_symbol(S, "recur");
        mino_val *new_args  = rewrite_recur_args(
            S, recur_args, step_pos, step_kind);
        if (recur_sym == NULL || new_args == NULL) return NULL;
        gc_pin(recur_sym);
        gc_pin(new_args);
        new_recur = mino_cons(S, recur_sym, new_args);
        gc_unpin(2);
        if (new_recur == NULL) return NULL;
    }
    /* Reassemble the if. */
    {
        mino_val *if_sym = mino_symbol(S, "if");
        if (if_sym == NULL) return NULL;
        gc_pin(if_sym);
        gc_pin(new_recur);
        if (is_then_recur) {
            new_if = mino_cons(
                S, if_sym, mino_cons(
                    S, test, mino_cons(
                        S, new_recur, mino_cons(
                            S, acc_sym, mino_nil(S)))));
        } else {
            new_if = mino_cons(
                S, if_sym, mino_cons(
                    S, test, mino_cons(
                        S, acc_sym, mino_cons(
                            S, new_recur, mino_nil(S)))));
        }
        gc_unpin(2);
        if (new_if == NULL) return NULL;
    }
    /* Reassemble the loop. */
    {
        mino_val *loop_sym = mino_symbol(S, "loop");
        if (loop_sym == NULL) return NULL;
        gc_pin(loop_sym);
        gc_pin(new_bindings);
        gc_pin(new_if);
        new_loop = mino_cons(
            S, loop_sym, mino_cons(
                S, new_bindings, mino_cons(
                    S, new_if, mino_nil(S))));
        gc_unpin(3);
        if (new_loop == NULL) return NULL;
    }
    /* Wrap in (persistent! ...). */
    {
        mino_val *p_sym = mino_symbol(S, "persistent!");
        if (p_sym == NULL) return NULL;
        gc_pin(p_sym);
        gc_pin(new_loop);
        new_persistent = mino_cons(
            S, p_sym, mino_cons(S, new_loop, mino_nil(S)));
        gc_unpin(2);
    }
    return new_persistent;
}

/* Match (fn [a b] body) or (fn name [a b] body) or (fn* ...). Returns
 * 1 and fills *out_params (the param vector) + *out_body (the body cons
 * cell) on match; 0 otherwise. */
static int match_simple_fn(mino_val *fn_form,
                           mino_val **out_params,
                           mino_val **out_body)
{
    mino_val *head;
    mino_val *rest;
    mino_val *params_or_name;
    if (!mino_is_cons(fn_form)) return 0;
    head = fn_form->as.cons.car;
    if (head == NULL || mino_type_of(head) != MINO_SYMBOL) return 0;
    if (strcmp(head->as.s.data, "fn") != 0
        && strcmp(head->as.s.data, "fn*") != 0) return 0;
    rest = fn_form->as.cons.cdr;
    if (!mino_is_cons(rest)) return 0;
    params_or_name = rest->as.cons.car;
    if (params_or_name != NULL
        && mino_type_of(params_or_name) == MINO_SYMBOL) {
        rest = rest->as.cons.cdr;
        if (!mino_is_cons(rest)) return 0;
        params_or_name = rest->as.cons.car;
    }
    if (params_or_name == NULL
        || mino_type_of(params_or_name) != MINO_VECTOR) return 0;
    *out_params = params_or_name;
    *out_body   = rest->as.cons.cdr;
    return 1;
}

/* Recognise (assoc acc ...) / (conj acc ...) / (dissoc acc ...) /
 * (disj acc ...) tail call. Fills *out_bang with the matching *!
 * symbol name. */
static int match_reduce_step(mino_val *step, const char *acc_name,
                             const char **out_bang)
{
    mino_val *step_args;
    if (!mino_is_cons(step)) return 0;
    {
        mino_val *head = step->as.cons.car;
        const char *hname;
        if (head == NULL || mino_type_of(head) != MINO_SYMBOL) return 0;
        hname = head->as.s.data;
        if (strcmp(hname, "assoc") == 0)       *out_bang = "assoc!";
        else if (strcmp(hname, "conj") == 0)   *out_bang = "conj!";
        else if (strcmp(hname, "dissoc") == 0) *out_bang = "dissoc!";
        else if (strcmp(hname, "disj") == 0)   *out_bang = "disj!";
        else return 0;
    }
    step_args = step->as.cons.cdr;
    if (!mino_is_cons(step_args)) return 0;
    if (!is_symbol_named(step_args->as.cons.car, acc_name)) return 0;
    return 1;
}

/* try_reduce_rewrite -- recognise
 *   (reduce (fn [acc x] (assoc/conj/dissoc/disj acc ...)) seed coll)
 * shapes and rewrite to
 *   (persistent!
 *     (reduce (fn [acc x] (assoc!/conj!/dissoc!/disj! acc ...))
 *             (transient seed) coll))
 *
 * The fn must be a literal anonymous fn (named or unnamed) with
 * exactly 2 plain-symbol params and exactly one body form whose tail
 * call is the supported step. Any acc reference in non-first-arg
 * positions of the step must be a transient-protocol-safe read
 * (`get` / `nth` / `count` / `get-in`); otherwise decline. Head must
 * resolve to canonical `prim_reduce` -- a user shadow defeats the
 * rewrite. */
static mino_val *try_reduce_rewrite(compiler_t *c, mino_val *form)
{
    mino_val   *head;
    mino_val   *args;
    mino_val   *fn_form;
    mino_val   *seed;
    mino_val   *coll;
    mino_val   *params;
    mino_val   *body;
    mino_val   *step;
    mino_val   *acc_sym;
    mino_val   *x_sym;
    mino_val   *hv;
    const char   *bang_name = NULL;
    mino_state *S = c->S;
    head = form->as.cons.car;
    if (head == NULL || mino_type_of(head) != MINO_SYMBOL) return NULL;
    if (strcmp(head->as.s.data, "reduce") != 0) return NULL;
    if (find_local(c, head->as.s.data) >= 0) return NULL;
    hv = probe_head_value(c, head);
    if (hv == NULL) return NULL;
    /* The shipped reduce is a Lisp-side wrapper (clojure.core/reduce)
     * dispatching to prim_reduce after a CollReduce protocol check.
     * Bless the rewrite when hv is exactly that var's root -- the
     * canonical entry point -- so a user (defn reduce ...) shadow in
     * any namespace declines the rewrite. */
    {
        mino_val *canon_var = var_find(S, "clojure.core", "reduce");
        if (canon_var == NULL || mino_type_of(canon_var) != MINO_VAR) {
            return NULL;
        }
        if (hv != canon_var->as.var.root) return NULL;
    }
    args = form->as.cons.cdr;
    if (!mino_is_cons(args)) return NULL;
    fn_form = args->as.cons.car;
    if (!mino_is_cons(args->as.cons.cdr)) return NULL;
    seed = args->as.cons.cdr->as.cons.car;
    if (!mino_is_cons(args->as.cons.cdr->as.cons.cdr)) return NULL;
    coll = args->as.cons.cdr->as.cons.cdr->as.cons.car;
    /* Reject trailing args. */
    if (mino_is_cons(args->as.cons.cdr->as.cons.cdr->as.cons.cdr)) {
        return NULL;
    }
    if (!match_simple_fn(fn_form, &params, &body)) return NULL;
    if (params->as.vec.len != 2) return NULL;
    acc_sym = vec_nth(params, 0);
    x_sym   = vec_nth(params, 1);
    if (acc_sym == NULL || mino_type_of(acc_sym) != MINO_SYMBOL) return NULL;
    if (x_sym   == NULL || mino_type_of(x_sym)   != MINO_SYMBOL) return NULL;
    if (!mino_is_cons(body)) return NULL;
    /* Single body form required: the tail step must be the only form so
     * each iteration is exactly one assoc!/conj!/dissoc!/disj! call. A
     * multi-form body (let / when / threading) needs deeper analysis to
     * prove every return path is a transient-supported mutation; defer. */
    if (mino_is_cons(body->as.cons.cdr)) return NULL;
    step = body->as.cons.car;
    if (!match_reduce_step(step, acc_sym->as.s.data, &bang_name)) return NULL;
    /* Safety: acc must not be read in a transient-unsafe way inside
     * the non-first-arg positions of the step. */
    {
        const char *name = acc_sym->as.s.data;
        mino_val *after_head = step->as.cons.cdr;
        mino_val *after_acc  = after_head->as.cons.cdr;
        while (mino_is_cons(after_acc)) {
            if (form_contains_unsafe_acc(after_acc->as.cons.car, name)) {
                return NULL;
            }
            after_acc = after_acc->as.cons.cdr;
        }
    }
    /* The param `x` may not be the same symbol as `acc` (would be a
     * user error, but the rewrite would still be safe; keep the
     * structural check defensive). */
    if (strcmp(acc_sym->as.s.data, x_sym->as.s.data) == 0) return NULL;

    /* Build the rewritten form. */
    {
        mino_val *bang_sym;
        mino_val *new_step;
        mino_val *new_body;
        mino_val *fn_sym;
        mino_val *new_fn;
        mino_val *transient_sym;
        mino_val *transient_seed;
        mino_val *reduce_sym;
        mino_val *new_reduce;
        mino_val *persist_sym;
        /* Pin params (a GC-managed vector) across all allocating calls
         * that precede its use in the mino_cons that builds new_fn. */
        gc_pin(params);
        bang_sym = mino_symbol(S, bang_name);
        if (bang_sym == NULL) { gc_unpin(1); return NULL; }
        gc_pin(bang_sym);
        new_step = mino_cons(S, bang_sym, step->as.cons.cdr);
        gc_unpin(1);
        if (new_step == NULL) { gc_unpin(1); return NULL; }
        gc_pin(new_step);
        new_body = mino_cons(S, new_step, mino_nil(S));
        gc_unpin(1);
        if (new_body == NULL) { gc_unpin(1); return NULL; }
        fn_sym = mino_symbol(S, "fn");
        if (fn_sym == NULL) { gc_unpin(1); return NULL; }
        gc_pin(fn_sym);
        gc_pin(new_body);
        new_fn = mino_cons(S, fn_sym, mino_cons(S, params, new_body));
        gc_unpin(3); /* fn_sym, new_body, params */
        if (new_fn == NULL) return NULL;
        transient_sym = mino_symbol(S, "transient");
        if (transient_sym == NULL) return NULL;
        gc_pin(transient_sym);
        transient_seed = mino_cons(
            S, transient_sym, mino_cons(S, seed, mino_nil(S)));
        gc_unpin(1);
        if (transient_seed == NULL) return NULL;
        reduce_sym = mino_symbol(S, "reduce");
        if (reduce_sym == NULL) return NULL;
        gc_pin(reduce_sym);
        gc_pin(new_fn);
        gc_pin(transient_seed);
        new_reduce = mino_cons(
            S, reduce_sym, mino_cons(
                S, new_fn, mino_cons(
                    S, transient_seed, mino_cons(S, coll, mino_nil(S)))));
        gc_unpin(3);
        if (new_reduce == NULL) return NULL;
        persist_sym = mino_symbol(S, "persistent!");
        if (persist_sym == NULL) return NULL;
        gc_pin(persist_sym);
        gc_pin(new_reduce);
        mino_val *result = mino_cons(
            S, persist_sym, mino_cons(S, new_reduce, mino_nil(S)));
        gc_unpin(2);
        return result;
    }
}

#ifdef MINO_BUILDER_REWRITE_COUNTS
/* Instrumentation: count rewriter hits and misses across the
 * compiled-source span and (when MINO_BUILDER_REWRITE_DUMP=1) print
 * the first form-prefix of each miss so the reviewer can decide
 * whether the matcher should widen. Dumps on program exit so a
 * one-shot full test or bench run produces a coverage table.
 * Built behind a flag so the production binary pays no overhead. */
static struct {
    unsigned long hits;
    unsigned long misses;
} mino_builder_rewrite_counts = {0, 0};

static void mino_builder_rewrite_dump(void)
{
    fprintf(stderr,
            "[builder-rewrite] hits=%lu misses=%lu coverage=%.1f%%\n",
            mino_builder_rewrite_counts.hits,
            mino_builder_rewrite_counts.misses,
            mino_builder_rewrite_counts.hits + mino_builder_rewrite_counts.misses == 0
                ? 0.0
                : 100.0 * (double)mino_builder_rewrite_counts.hits
                  / (double)(mino_builder_rewrite_counts.hits
                             + mino_builder_rewrite_counts.misses));
}

static void mino_builder_rewrite_install(void)
{
    static int installed = 0;
    if (!installed) {
        atexit(mino_builder_rewrite_dump);
        installed = 1;
    }
}

static void mino_builder_rewrite_dump_miss(mino_state *S, mino_val *form)
{
    char buf[160];
    size_t off = 0;
    (void)S;
    if (form == NULL || !mino_is_cons(form)) return;
    /* Print the loop's bindings vector tag only; full forms would
     * flood the log. The bindings vector tells the reviewer how many
     * loop vars (and whether `acc []` is one of them). */
    mino_val *args = form->as.cons.cdr;
    if (mino_is_cons(args)
        && args->as.cons.car != NULL
        && mino_type_of(args->as.cons.car) == MINO_VECTOR) {
        size_t blen = args->as.cons.car->as.vec.len;
        off += snprintf(buf + off, sizeof(buf) - off,
                        "  miss: %zu bindings", blen);
        if (blen >= 2) {
            mino_val *acc_init = vec_nth(args->as.cons.car, blen - 1);
            if (acc_init != NULL) {
                int t = mino_type_of(acc_init);
                const char *tag = (t == MINO_VECTOR && acc_init->as.vec.len == 0)
                    ? "[]"
                    : (t == MINO_MAP && acc_init->as.map.len == 0)
                    ? "{}"
                    : (t == MINO_SET && acc_init->as.set.len == 0)
                    ? "#{}"
                    : "<other>";
                off += snprintf(buf + off, sizeof(buf) - off,
                                ", acc init=%s", tag);
            }
        }
    } else {
        off += snprintf(buf + off, sizeof(buf) - off,
                        "  miss: <non-vector bindings>");
    }
    fprintf(stderr, "%s\n", buf);
}
#endif

static int compile_loop(compiler_t *c, mino_val *form, int dst, int tail)
{
    /* Builder-pattern fast path: rewrite the canonical
     * (loop [... acc []] (if <t> (recur ... (conj acc x)) acc))
     * into a transient-driven equivalent that mutates in place.
     * Falls through on miss. */
    {
        mino_val *rewritten = try_builder_rewrite(c->S, form);
#ifdef MINO_BUILDER_REWRITE_COUNTS
        mino_builder_rewrite_install();
        if (rewritten != NULL) {
            mino_builder_rewrite_counts.hits++;
        } else {
            mino_builder_rewrite_counts.misses++;
            if (getenv("MINO_BUILDER_REWRITE_DUMP") != NULL) {
                mino_builder_rewrite_dump_miss(c->S, form);
            }
        }
#endif
        if (rewritten != NULL) {
            return compile_expr(c, rewritten, dst, tail);
        }
    }
    /* (loop [b1 v1 ...] body) -- like let, but installs a recur target
     * at the loop entry pc and tracks the binding registers so (recur
     * v1' ...) can rebind and jump. Plain-symbol bindings only.
     * When the loop is in tail position, the body's last expression
     * (when it isn't a recur) is the fn's result; propagate tail. */
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }
    mino_val *bindings = args->as.cons.car;
    mino_val *body     = args->as.cons.cdr;
    if (bindings == NULL || mino_type_of(bindings) != MINO_VECTOR) {
        c->ok = 0; return -1;
    }
    /* Loop bindings don't strictly need plain symbols at the source
     * level -- Clojure expands (loop [[a b] [1 2]] ...) too -- but
     * recur targets must be plain registers, so any destructure
     * pattern has to expand into `let`-style scaffolding rather than
     * become a loop binding directly. Decline non-plain bindings;
     * the rare loop-with-destructure cases fall back to the tree-
     * walker. */
    if (!bindings_are_plain(bindings)) { c->ok = 0; return -1; }
    size_t blen = bindings->as.vec.len;
    if ((blen & 1) != 0) { c->ok = 0; return -1; }
    int n_bindings = (int)(blen / 2);
    if (n_bindings >= BC_MAX_LOCALS) { c->ok = 0; return -1; }

    int saved_n_locals = c->n_locals;
    int saved_next_reg = c->next_reg;
    loop_target_t *saved_loop = c->loop;
    loop_target_t this_loop;
    this_loop.n_bindings = n_bindings;
    this_loop.captures   = c->bc->captures ? 1 : 0;
    for (int i = 0; i < n_bindings; i++) this_loop.bind_const_idx[i] = -1;
    int pushed_env = 0;
    if (c->bc->captures) {
        emit_abc(c, OP_PUSH_ENV, 0, 0, 0);
        pushed_env = 1;
    }

    /* Compile initial binding values into successive registers and
     * record those slots as the recur-rebind targets. */
    for (int i = 0; i < n_bindings; i++) {
        mino_val *name_form = vec_nth(bindings, (size_t)(i * 2));
        mino_val *val_form  = vec_nth(bindings, (size_t)(i * 2 + 1));
        if (name_form == NULL || mino_type_of(name_form) != MINO_SYMBOL) {
            c->ok = 0; goto out;
        }
        int reg = alloc_reg(c);
        if (reg < 0) goto out;
        if (compile_expr(c, val_form, reg, 0) < 0) goto out;
        if (!bind_local(c, name_form->as.s.data, reg)) goto out;
        this_loop.bind_regs[i] = reg;
        if (pushed_env) {
            int k = add_const(c, name_form);
            if (k < 0) goto out;
            this_loop.bind_const_idx[i] = k;
            emit_abx(c, OP_ENV_BIND, (unsigned)reg, (unsigned)k);
        }
    }

    /* Install recur target only after the initial bindings are in
     * place. A recur expression in tail position rebinds the loop
     * slots and jumps to entry_pc. */
    this_loop.entry_pc = (int)c->bc->code_len;
    c->loop = &this_loop;

    /* Fused counted-loop fast path: detects the common
     *   (loop [i 0]       (if (zero? i) <exit> (recur (dec i))))
     *   (loop [i 0 j N]   (if (zero? j) <exit> (recur (inc i) (dec j))))
     * shape and emits a single OP_LOOP_INT_DEC[_INC] at the recur
     * target. compile_body's generic emission is skipped on match.
     * On miss, fall through unchanged. */
    {
        int fused = try_compile_counted_loop(c, body, bindings, n_bindings,
                                             &this_loop, dst, tail);
        if (fused < 0) goto out;
        if (fused == 0) {
            if (compile_body(c, body, dst, tail) < 0) goto out;
        }
    }

    if (pushed_env) emit_abc(c, OP_POP_ENV, 0, 0, 0);
    c->loop     = saved_loop;
    c->n_locals = saved_n_locals;
    c->next_reg = saved_next_reg;
    return 0;

out:
    c->loop     = saved_loop;
    c->n_locals = saved_n_locals;
    c->next_reg = saved_next_reg;
    return -1;
}

/* Conservative free-name scan: does `ast` reference any symbol whose
 * lookup, in the current local scope, resolves to register `target_reg`?
 * Walks cons-cells and vector-literals; treats persistent-coll literals
 * as possible references rather than iterating them (rare in recur arg
 * position, walker simpler). Doesn't model shadowing forms -- a `let`
 * inside an arg that rebinds the loop's name is treated as still
 * referring to the loop's register, which is over-conservative (keeps
 * the move) but correct. Used by compile_recur to decide which
 * temp/move pairs can collapse into a direct write. */
static int recur_arg_reads_reg(compiler_t *c, mino_val *ast,
                               int target_reg)
{
    if (ast == NULL) return 0;
    switch (mino_type_of(ast)) {
    case MINO_SYMBOL:
        return find_local(c, ast->as.s.data) == target_reg;
    case MINO_CONS:
        if (recur_arg_reads_reg(c, ast->as.cons.car, target_reg)) return 1;
        return recur_arg_reads_reg(c, ast->as.cons.cdr, target_reg);
    case MINO_VECTOR:
        for (size_t i = 0; i < ast->as.vec.len; i++) {
            mino_val *elt = vec_nth(ast, i);
            if (recur_arg_reads_reg(c, elt, target_reg)) return 1;
        }
        return 0;
    case MINO_MAP:
    case MINO_SET:
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET:
        return 1;
    default:
        return 0;
    }
}

static int compile_recur(compiler_t *c, mino_val *form, int dst, int tail)
{
    (void)dst; (void)tail;  /* recur never produces a value -- it jumps. */
    if (c->loop == NULL) { c->ok = 0; return -1; }
    int n = c->loop->n_bindings;
    /* Walk the recur args. They must match the loop's binding count.
     * Gather pointers first so we can look ahead when picking the
     * compile destination for each arg. */
    mino_val *args[BC_MAX_LOCALS];
    {
        mino_val *cur = form->as.cons.cdr;
        for (int i = 0; i < n; i++) {
            if (!mino_is_cons(cur)) { c->ok = 0; return -1; }
            args[i] = cur->as.cons.car;
            cur = cur->as.cons.cdr;
        }
        if (mino_is_cons(cur)) { c->ok = 0; return -1; }  /* too many */
    }
    int saved_next = c->next_reg;
    /* For each binding, decide whether arg i can write directly to
     * bind_regs[i] (no intermediate temp + move). Safe when no later
     * arg reads bind_regs[i] under the current local scope -- because
     * any earlier arg's read of bind_regs[i] inside its own subtree
     * sees the OLD value (a register write only takes effect after
     * compile_expr's final emit, by which point the read has already
     * happened). Args 0..n-1 are compiled in order; pre-writing
     * bind_regs[i] is the OP_MOVE-elision win. */
    int temp_regs[BC_MAX_LOCALS];
    for (int i = 0; i < n; i++) {
        int target = c->loop->bind_regs[i];
        int needs_temp = 0;
        for (int j = i + 1; j < n; j++) {
            if (recur_arg_reads_reg(c, args[j], target)) {
                needs_temp = 1; break;
            }
        }
        if (needs_temp) {
            int t = alloc_reg(c);
            if (t < 0) return -1;
            temp_regs[i] = t;
            if (compile_expr(c, args[i], t, 0) < 0) return -1;
        } else {
            temp_regs[i] = -1;
            if (compile_expr(c, args[i], target, 0) < 0) return -1;
        }
    }
    for (int i = 0; i < n; i++) {
        if (temp_regs[i] < 0) continue;
        emit_abc(c, OP_MOVE, (unsigned)c->loop->bind_regs[i],
                 (unsigned)temp_regs[i], 0);
    }

    /* If the enclosing loop wrapped its bindings in an OP_PUSH_ENV /
     * OP_POP_ENV pair, replace the live env frame on every recur so a
     * closure built in iteration N captures iter N's bindings. Without
     * this, all closures share the env frame that was bound once at
     * loop entry; OP_MOVE updates the register but the env's
     * symbol -> value mapping still points at the iter-0 value (the
     * env stores a snapshot, not a register reference). The tree-
     * walker eval_loop in bindings.c does the equivalent fresh
     * env_child per recur. */
    if (c->loop->captures) {
        emit_abc(c, OP_POP_ENV, 0, 0, 0);
        emit_abc(c, OP_PUSH_ENV, 0, 0, 0);
        for (int i = 0; i < n; i++) {
            int k = c->loop->bind_const_idx[i];
            if (k < 0) { c->ok = 0; return -1; }
            emit_abx(c, OP_ENV_BIND, (unsigned)c->loop->bind_regs[i],
                     (unsigned)k);
        }
    }

    /* OP_JMP's offset is added to pc after the instruction is fetched.
     * Bias-encoded as 16-bit unsigned (+0x8000): valid range -0x8000..0x7FFF. */
    int off = c->loop->entry_pc - ((int)c->bc->code_len + 1);
    if (off < -0x8000 || off > 0x7FFF) {
        c->ok = 0; return -1;
    }
    emit_abc(c, OP_JMP, 0, 0, 0);
    /* Patch the offset into the instruction we just emitted. */
    c->bc->code[c->bc->code_len - 1] = MK_AsBx(OP_JMP, 0, off);
    c->next_reg = saved_next;
    return 0;
}

static int compile_lazy_seq(compiler_t *c, mino_val *form, int dst, int tail)
{
    (void)tail;
    /* (lazy-seq body...) -- stash the body forms (a list of AST
     * expressions) in the constant pool and have the runtime build a
     * MINO_LAZY whose body is that list and whose env is the live
     * lexical chain. Realisation runs the body via the tree-walker in
     * the captured env, so the enclosing fn must already publish its
     * locals into the env (bc->captures = 1, set automatically by the
     * pre-scan since `lazy-seq` is one of the env-capturing shapes). */
    mino_val *body = form->as.cons.cdr;
    int k = add_const(c, body);
    if (k < 0) return -1;
    emit_abx(c, OP_MAKE_LAZY, (unsigned)dst, (unsigned)k);
    return 0;
}

/* (throw expr) -- evaluate expr into a temp, emit OP_THROW. The handler
 * does not return; the caller's dst register is left untouched (callers
 * that need a value here are unreachable past the throw at runtime). */
static int compile_throw(compiler_t *c, mino_val *form, int dst, int tail)
{
    (void)dst; (void)tail;
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }
    if (mino_is_cons(args->as.cons.cdr)) { c->ok = 0; return -1; }
    mino_val *arg = args->as.cons.car;
    int saved_next = c->next_reg;
    int r = alloc_reg(c);
    if (r < 0) return -1;
    if (compile_expr(c, arg, r, 0) < 0) return -1;
    emit_abc(c, OP_THROW, (unsigned)r, 0, 0);
    c->has_try  = 1;
    c->next_reg = saved_next;
    return 0;
}

/* Partition a try form's args into (body... [catch e ...] [finally ...]).
 * Mirrors partition_try_clauses in src/eval/control.c but pulls only the
 * shape the BC compiler currently emits: zero-or-one catch clause, an
 * optional finally clause, both placed after the body forms. Returns 1
 * on success, 0 to decline (let the tree-walker handle it). */
typedef struct bc_try_clauses {
    mino_val *body;          /* GC-rooted list of body forms (linear, NULL-terminated cons) */
    mino_val *catch_body;    /* tail of args list after `e` -- not deep-copied */
    mino_val *finally_body;  /* tail of args list after `finally` */
    int         has_catch;
    int         has_finally;
    mino_val *catch_var;     /* MINO_SYMBOL: the catch binding name */
} bc_try_clauses_t;

/* Deviation from JVM: mino catch clauses do not check the exception type
 * class; all catch clauses catch any throwable. The first element after
 * `catch` is treated directly as the binding symbol rather than as an
 * exception type followed by a binding symbol. */
static int parse_try_clauses(compiler_t *c, mino_val *args,
                             bc_try_clauses_t *out)
{
    mino_val *body_tail = NULL;
    mino_val *rest      = args;

    out->body         = NULL;
    out->catch_body   = NULL;
    out->finally_body = NULL;
    out->has_catch    = 0;
    out->has_finally  = 0;
    out->catch_var    = NULL;

    while (mino_is_cons(rest)) {
        mino_val *clause = rest->as.cons.car;
        if (mino_is_cons(clause)
            && sym_is(clause->as.cons.car, "catch")) {
            if (!mino_is_cons(clause->as.cons.cdr)) { c->ok = 0; return 0; }
            mino_val *cv = clause->as.cons.cdr->as.cons.car;
            if (cv == NULL || mino_type_of(cv) != MINO_SYMBOL) {
                c->ok = 0; return 0;
            }
            out->catch_var  = cv;
            out->catch_body = clause->as.cons.cdr->as.cons.cdr;
            out->has_catch  = 1;
            rest = rest->as.cons.cdr;
            continue;
        }
        if (mino_is_cons(clause)
            && sym_is(clause->as.cons.car, "finally")) {
            out->finally_body = clause->as.cons.cdr;
            out->has_finally  = 1;
            rest = rest->as.cons.cdr;
            continue;
        }
        /* Body form. Append to the linked list. */
        {
            mino_val *cell = mino_cons(c->S, clause, mino_nil(c->S));
            if (cell == NULL) { c->ok = 0; return 0; }
            if (body_tail == NULL) {
                out->body = cell;
            } else {
                mino_cons_cdr_set(c->S, body_tail, cell);
            }
            body_tail = cell;
        }
        rest = rest->as.cons.cdr;
    }
    return 1;
}

/* (try body... [(catch e handler...)] [(finally f...)])
 *
 * Three shapes:
 *   try-no-handlers -- just (do body...).
 *   try-catch (no finally) -- PUSHCATCH around body; handler binds the
 *      exception and runs in the catch scope.
 *   try with finally -- wrap in an outer PUSHCATCH so the finally block
 *      runs on both normal completion and on an uncaught throw before
 *      re-raising. When a catch is also present, the inner PUSHCATCH
 *      handles the body's throw and the outer wraps everything so
 *      finally runs even on a re-throw from the handler. */
static int compile_try(compiler_t *c, mino_val *form, int dst, int tail)
{
    mino_val *args = form->as.cons.cdr;
    bc_try_clauses_t cl;
    if (!parse_try_clauses(c, args, &cl)) return -1;

    if (!cl.has_catch && !cl.has_finally) {
        /* Degenerate (try body) -- semantically (do body). Body
         * inherits tail. */
        return compile_body(c, cl.body, dst, tail);
    }

    /* From here on the compile is guaranteed to emit at least one
     * OP_PUSHCATCH (every catch and finally arm wraps the body in a
     * push-pop catch pair). Set has_try here so mino_bc_run's
     * prologue keeps its try-state snapshot for this fn. */
    c->has_try = 1;

    /* The catch handler's tail position matches our caller's tail
     * position when there is no finally clause: a value from the
     * handler is the try's value. When a finally is present, the
     * handler's result is the value but it has to run through the
     * finally block before returning, so the handler body isn't in
     * tail position. */
    int handler_tail = (!cl.has_finally) ? tail : 0;
    /* The body must NEVER run in tail position. An OP_TAILCALL inside
     * the body would return the trampoline sentinel from this fn,
     * bypassing POPCATCH and pulling the BC catch frame down with it
     * (bc_done's rollback). Any throw inside the trampolined callee
     * would then longjmp to an OUTER try, not ours -- semantic break.
     * Building C stack for recursion in try bodies is the right
     * trade-off; try bodies aren't typical hot recursion sites. */
    int body_tail = 0;

    int saved_next = c->next_reg;
    int saved_locals = c->n_locals;

    if (!cl.has_finally) {
        /* try + catch (no finally). */
        int ex_reg = alloc_reg(c);
        if (ex_reg < 0) goto fail;
        int push_pos = emit_jmp_placeholder(c, OP_PUSHCATCH, (unsigned)ex_reg);
        if (push_pos < 0) goto fail;
        if (compile_body(c, cl.body, dst, body_tail) < 0) goto fail;
        emit_abc(c, OP_POPCATCH, 0, 0, 0);
        int end_jmp = emit_jmp_placeholder(c, OP_JMP, 0);
        if (end_jmp < 0) goto fail;
        /* Handler entry: ex_reg holds the normalized exception. */
        patch_jmp(c, push_pos);
        if (c->bc->captures) {
            emit_abc(c, OP_PUSH_ENV, 0, 0, 0);
        }
        if (!bind_local(c, cl.catch_var->as.s.data, ex_reg)) goto fail;
        if (c->bc->captures) {
            int k = add_const(c, cl.catch_var);
            if (k < 0) goto fail;
            emit_abx(c, OP_ENV_BIND, (unsigned)ex_reg, (unsigned)k);
        }
        if (compile_body(c, cl.catch_body, dst, handler_tail) < 0) goto fail;
        if (c->bc->captures) {
            emit_abc(c, OP_POP_ENV, 0, 0, 0);
        }
        patch_jmp(c, end_jmp);
        c->n_locals = saved_locals;
        c->next_reg = saved_next;
        return 0;
    }

    /* Finally is involved. Outer try frame catches re-throws (or
     * uncaught body throws when there is no catch) so finally runs on
     * every exit path. The thrown value lands in outer_ex_reg; the
     * inner handler binding (when a catch is present) lives in
     * inner_ex_reg. */
    int outer_ex_reg = alloc_reg(c);
    if (outer_ex_reg < 0) goto fail;
    int outer_push = emit_jmp_placeholder(c, OP_PUSHCATCH,
                                           (unsigned)outer_ex_reg);
    if (outer_push < 0) goto fail;

    if (cl.has_catch) {
        int inner_ex_reg = alloc_reg(c);
        if (inner_ex_reg < 0) goto fail;
        int inner_push = emit_jmp_placeholder(c, OP_PUSHCATCH,
                                               (unsigned)inner_ex_reg);
        if (inner_push < 0) goto fail;
        if (compile_body(c, cl.body, dst, body_tail) < 0) goto fail;
        emit_abc(c, OP_POPCATCH, 0, 0, 0);
        int after_inner = emit_jmp_placeholder(c, OP_JMP, 0);
        if (after_inner < 0) goto fail;
        patch_jmp(c, inner_push);
        if (c->bc->captures) {
            emit_abc(c, OP_PUSH_ENV, 0, 0, 0);
        }
        int catch_locals = c->n_locals;
        if (!bind_local(c, cl.catch_var->as.s.data, inner_ex_reg)) goto fail;
        if (c->bc->captures) {
            int k = add_const(c, cl.catch_var);
            if (k < 0) goto fail;
            emit_abx(c, OP_ENV_BIND, (unsigned)inner_ex_reg, (unsigned)k);
        }
        if (compile_body(c, cl.catch_body, dst, 0) < 0) goto fail;
        if (c->bc->captures) {
            emit_abc(c, OP_POP_ENV, 0, 0, 0);
        }
        c->n_locals = catch_locals;
        patch_jmp(c, after_inner);
    } else {
        /* No catch, just finally. Body executes inside the outer try. */
        if (compile_body(c, cl.body, dst, body_tail) < 0) goto fail;
    }

    emit_abc(c, OP_POPCATCH, 0, 0, 0);
    /* Normal-completion finally. Result register is a throwaway -- the
     * finally body's value is discarded. */
    {
        int throwaway = alloc_reg(c);
        if (throwaway < 0) goto fail;
        if (compile_body(c, cl.finally_body, throwaway, 0) < 0) goto fail;
        c->next_reg = throwaway;  /* free the throwaway */
    }
    int end_jmp = emit_jmp_placeholder(c, OP_JMP, 0);
    if (end_jmp < 0) goto fail;

    /* Outer-handler entry: a throw reached here. Run finally, then
     * re-raise the original exception. */
    patch_jmp(c, outer_push);
    {
        int throwaway = alloc_reg(c);
        if (throwaway < 0) goto fail;
        if (compile_body(c, cl.finally_body, throwaway, 0) < 0) goto fail;
    }
    emit_abc(c, OP_THROW, (unsigned)outer_ex_reg, 0, 0);

    patch_jmp(c, end_jmp);
    c->n_locals = saved_locals;
    c->next_reg = saved_next;
    return 0;

fail:
    c->n_locals = saved_locals;
    c->next_reg = saved_next;
    return -1;
}

/* (binding [v1 e1 v2 e2 ...] body...) -- evaluate each binding value
 * into a fresh register, push a dyn frame whose names match v_i and
 * whose values are the just-computed registers, run the body, then
 * pop the frame. The names vector becomes a single constant (a
 * MINO_VECTOR of plain symbols) referenced by OP_PUSHDYN's Bx.
 *
 * Body is non-tail: OP_POPDYN must run after, so a tail-call inside
 * the body would skip the pop just like inside a try. The wrapping
 * fn's tail position is still reachable -- when (binding ...) is the
 * fn's last expression and we POPDYN here, the fn's natural OP_RETURN
 * follows. */
static int compile_binding(compiler_t *c, mino_val *form, int dst, int tail)
{
    (void)tail;
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }
    mino_val *binds = args->as.cons.car;
    mino_val *body  = args->as.cons.cdr;
    if (binds == NULL || mino_type_of(binds) != MINO_VECTOR) {
        c->ok = 0; return -1;
    }
    size_t blen = binds->as.vec.len;
    if ((blen & 1) != 0) { c->ok = 0; return -1; }
    int n_pairs = (int)(blen / 2);
    if (n_pairs == 0) {
        /* Empty binding form -- semantically (do body). */
        return compile_body(c, body, dst, tail);
    }

    int saved_next = c->next_reg;

    /* Build the names vec from binding's even slots. Compile-time
     * eager validation: each name must be a symbol; otherwise
     * decline so the tree-walker can produce its richer diagnostic.
     */
    mino_state *S = c->S;
    mino_val **names_buf = (mino_val **)gc_alloc_typed(
        S, GC_T_VALARR, (size_t)n_pairs * sizeof(mino_val *));
    if (names_buf == NULL) { c->ok = 0; return -1; }
    /* Pin names_buf so mino_vector cannot collect it mid-allocation. */
    gc_pin((mino_val *)names_buf);
    for (int i = 0; i < n_pairs; i++) {
        mino_val *sym = vec_nth(binds, (size_t)(i * 2));
        if (sym == NULL || mino_type_of(sym) != MINO_SYMBOL) {
            gc_unpin(1);
            c->ok = 0; goto fail;
        }
        names_buf[i] = sym;
    }
    mino_val *names_vec = mino_vector(S, names_buf, (size_t)n_pairs);
    gc_unpin(1);
    if (names_vec == NULL) { c->ok = 0; goto fail; }
    int names_k = add_const(c, names_vec);
    if (names_k < 0) goto fail;

    /* Allocate a contiguous run of n_pairs registers for the values,
     * then compile each value form into its slot. The OP_PUSHDYN
     * reads them in-order so the run must stay packed. */
    int base_reg = c->next_reg;
    for (int i = 0; i < n_pairs; i++) {
        int r = alloc_reg(c);
        if (r < 0) goto fail;
        mino_val *val_form = vec_nth(binds, (size_t)(i * 2 + 1));
        if (compile_expr(c, val_form, r, 0) < 0) goto fail;
    }

    emit_abx(c, OP_PUSHDYN, (unsigned)base_reg, (unsigned)names_k);

    /* Body runs non-tail: OP_POPDYN must follow. */
    if (compile_body(c, body, dst, 0) < 0) goto fail;

    emit_abc(c, OP_POPDYN, 1, 0, 0);

    c->next_reg = saved_next;
    return 0;

fail:
    c->next_reg = saved_next;
    return -1;
}

/* Compile an inner (fn ...) or (fn* ...) literal into a child template.
 * The template is a MINO_FN with .params / .body / .bc populated; it
 * lives in the outer fn's constant pool. OP_CLOSURE at runtime copies
 * the template's bc pointer into a fresh closure value and seals in
 * the current env, so each invocation that reaches OP_CLOSURE produces
 * a distinct closure over the live lexical chain.
 *
 * If the inner fn declines bc compilation (e.g., multi-arity, contains
 * a special form we can't yet emit), its template carries the declined
 * sentinel and the closure built from it falls back to the tree-walker
 * at apply_callable time. The outer fn can still compile -- decline of
 * an inner fn is local. */
static mino_val *probe_head_value(compiler_t *c, mino_val *head);

static int compile_fn_literal(compiler_t *c, mino_val *form, int dst, int tail)
{
    (void)tail;
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }

    /* Optional name: (fn name [params] body...). The name lets the
     * body recurse by referring to the just-built closure. eval_fn
     * does this by wrapping a fresh env_child whose only binding is
     * name -> closure; we reproduce the same shape by emitting an
     * OP_PUSH_ENV before the OP_CLOSURE and an OP_ENV_BIND right
     * after, then OP_POP_ENV to restore the outer's env. The closure
     * captures the pushed env, so the binding is visible to its body
     * via the env chain. */
    mino_val *fn_name = NULL;
    mino_val *first   = args->as.cons.car;
    mino_val *rest    = args->as.cons.cdr;
    if (first != NULL && mino_type_of(first) == MINO_SYMBOL
        && mino_is_cons(rest)) {
        mino_val *after = rest->as.cons.car;
        if (after != NULL
            && (mino_is_cons(after) || mino_is_nil(after)
                || mino_type_of(after) == MINO_VECTOR)) {
            fn_name = first;
            args    = rest;
        }
    }

    mino_val *params = args->as.cons.car;
    mino_val *body   = args->as.cons.cdr;

    /* Multi-arity detection. eval_fn recognises a fn whose first arg
     * is itself a (params-vec body...) clause -- the canonical
     * `(fn ([x] ...) ([x y] ...))` shape -- and rewrites params/body
     * via build_multi_arity_clauses. The bc compiler declines multi-
     * arity for now, but we still have to normalise the template's
     * shape so the tree-walker fallback at apply_callable time sees
     * what eval_fn would have produced. */
    if (mino_is_cons(params) && params->as.cons.car != NULL
        && (mino_type_of(params->as.cons.car) == MINO_VECTOR
            || mino_is_cons(params->as.cons.car)
            || mino_is_nil(params->as.cons.car))
        && mino_type_of(params->as.cons.car) == MINO_VECTOR) {
        mino_val *clauses = build_multi_arity_clauses(
            c->S, form, args, "MSY002", "fn");
        if (clauses == NULL) { c->ok = 0; return -1; }
        params = NULL;
        body   = clauses;
    }

    /* Build a template MINO_FN. The env field gets nil here; OP_CLOSURE
     * supplies the real env at each invocation. */
    mino_val *tmpl = make_fn(c->S, params, body, NULL);
    if (tmpl == NULL) { c->ok = 0; return -1; }
    tmpl->as.fn.defining_ns = c->defining_ns;

    /* Wraps-prim recogniser: `(fn [x] (inc x))` and friends compile to
     * a fn whose only behavior is to invoke a single primitive on a
     * single argument. The pipeline fast lanes can skip apply_callable
     * and route straight to the prim's specialised path; stamp the
     * wrapped prim on the fn template so the runtime can detect this
     * cheaply via `as.fn.wraps_prim`. The recogniser is intentionally
     * narrow: single arity, single body form, no destructuring, no
     * names, no closures over the param, no transformation of the arg. */
    if (fn_name == NULL && params != NULL && body != NULL
        && mino_type_of(params) == MINO_VECTOR
        && params->as.vec.len == 1
        && mino_is_cons(body)
        && (body->as.cons.cdr == NULL
            || mino_is_nil(body->as.cons.cdr)
            || (mino_type_of(body->as.cons.cdr) == MINO_EMPTY_LIST))) {
        mino_val *body_form = body->as.cons.car;
        if (mino_is_cons(body_form)) {
            mino_val *head = body_form->as.cons.car;
            mino_val *body_tail = body_form->as.cons.cdr;
            mino_val *param_sym = vec_nth(params, 0);
            if (mino_is_cons(body_tail)
                && (body_tail->as.cons.cdr == NULL
                    || mino_is_nil(body_tail->as.cons.cdr)
                    || (mino_type_of(body_tail->as.cons.cdr) == MINO_EMPTY_LIST))
                && body_tail->as.cons.car != NULL
                && mino_type_of(body_tail->as.cons.car) == MINO_SYMBOL
                && param_sym != NULL
                && mino_type_of(param_sym) == MINO_SYMBOL
                && body_tail->as.cons.car->as.s.len == param_sym->as.s.len
                && memcmp(body_tail->as.cons.car->as.s.data,
                          param_sym->as.s.data,
                          param_sym->as.s.len) == 0) {
                mino_val *resolved = probe_head_value(c, head);
                if (resolved != NULL && mino_type_of(resolved) == MINO_PRIM
                    && resolved->as.prim.fn2 != NULL) {
                    tmpl->as.fn.wraps_prim = resolved;
                }
            }
        }
    }

    /* Recurse: compile the inner. If decline, the template's bc gets
     * the declined sentinel and the closure falls back at call time.
     * Either result is fine for the outer's purposes. */
    (void)mino_bc_compile_fn(c->S, tmpl);

    int k = add_const(c, tmpl);
    if (k < 0) return -1;

    if (fn_name != NULL) {
        int name_k = add_const(c, fn_name);
        if (name_k < 0) return -1;
        emit_abc(c, OP_PUSH_ENV, 0, 0, 0);
        emit_abx(c, OP_CLOSURE, (unsigned)dst, (unsigned)k);
        emit_abx(c, OP_ENV_BIND, (unsigned)dst, (unsigned)name_k);
        emit_abc(c, OP_POP_ENV, 0, 0, 0);
    } else {
        emit_abx(c, OP_CLOSURE, (unsigned)dst, (unsigned)k);
    }
    return 0;
}

static int compile_quote(compiler_t *c, mino_val *form, int dst, int tail)
{
    (void)tail;
    /* (quote x) -- x is itself the value. */
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }
    if (mino_is_cons(args->as.cons.cdr)) { c->ok = 0; return -1; }
    mino_val *v = args->as.cons.car;
    int k = add_const(c, v);
    if (k < 0) return -1;
    emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
    return 0;
}

static int compile_def(compiler_t *c, mino_val *form, int dst, int tail)
{
    (void)tail;
    /* (def name) or (def name expr) -- plain form only, no metadata. */
    mino_val *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }
    mino_val *name_form = args->as.cons.car;
    mino_val *rest = args->as.cons.cdr;
    if (name_form == NULL || mino_type_of(name_form) != MINO_SYMBOL) {
        c->ok = 0; return -1;
    }
    /* Decline metadata-carrying defs: they need the eval_def codepath
     * for docstring, :private, :dynamic, etc. */
    if (name_form->meta != NULL) { c->ok = 0; return -1; }
    int k = add_const(c, name_form);
    if (k < 0) return -1;

    int saved_next = c->next_reg;
    int val_reg = alloc_reg(c);
    if (val_reg < 0) return -1;

    if (mino_is_cons(rest)) {
        mino_val *val_form = rest->as.cons.car;
        if (mino_is_cons(rest->as.cons.cdr)) { c->ok = 0; return -1; }
        if (compile_expr(c, val_form, val_reg, 0) < 0) return -1;
    } else {
        /* (def name) -- value is nil. */
        int nk = add_const(c, mino_nil(c->S));
        if (nk < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)val_reg, (unsigned)nk);
    }
    emit_abx(c, OP_SETGLOBAL, (unsigned)val_reg, (unsigned)k);
    /* Deviation from both JVM Clojure and mino tree-walker (eval_def):
     * BC path returns the bound value, not the Var (#'x). JVM Clojure
     * and eval_def both return the Var. Callers inside BC-compiled fns
     * must not rely on (def ...) returning a Var. */
    emit_abc(c, OP_MOVE, (unsigned)dst, (unsigned)val_reg, 0);
    c->next_reg = saved_next;
    return 0;
}

/* Walk the runtime's resolution cascade in probe mode: lexical env, then
 * the fn's defining-ns env, returning the bound value or NULL. Mirrors
 * eval_symbol's unqualified path but uses the fn's defining_ns rather
 * than S->ns_vars.current_ns -- compile happens lazily and S->ns_vars.current_ns at
 * that time is the caller's, not the fn's.
 *
 * For qualified ns/name symbols, walks alias_resolve -> var_find ->
 * ns_env_lookup the same way eval_qualified_symbol does, scoping the
 * alias lookup to the fn's defining_ns. */
static mino_val *probe_head_value(compiler_t *c, mino_val *head)
{
    if (head == NULL || mino_type_of(head) != MINO_SYMBOL) return NULL;
    const char *data = head->as.s.data;
    size_t      n    = head->as.s.len;
    const char *slash = (n > 1) ? memchr(data, '/', n) : NULL;
    mino_state *S = c->S;
    const char *owning = c->defining_ns != NULL ? c->defining_ns : "user";

    if (slash != NULL) {
        /* Qualified: ns/name. Try literal env binding first (rare:
         * something like (host/new ...) in lexical scope). */
        mino_val *v = mino_env_get_sym(c->env, head);
        if (v != NULL) return v;

        char  ns_buf[256];
        size_t ns_len = (size_t)(slash - data);
        if (ns_len >= sizeof(ns_buf)) return NULL;
        memcpy(ns_buf, data, ns_len);
        ns_buf[ns_len] = '\0';
        const char *sym_name = slash + 1;

        /* Resolve alias against the fn's defining_ns (the runtime does
         * the equivalent against S->ns_vars.current_ns, which during dispatch
         * is the defining_ns). */
        const char *resolved_ns = ns_buf;
        for (size_t i = 0; i < S->ns_vars.ns_alias_len; i++) {
            if (S->ns_vars.ns_aliases[i].owning_ns != NULL
                && strcmp(S->ns_vars.ns_aliases[i].owning_ns, owning) == 0
                && strcmp(S->ns_vars.ns_aliases[i].alias, ns_buf) == 0) {
                resolved_ns = S->ns_vars.ns_aliases[i].full_name;
                break;
            }
        }

        mino_val *var = var_find(S, resolved_ns, sym_name);
        if (var != NULL && mino_type_of(var) == MINO_VAR) {
            return var->as.var.root;
        }
        mino_env *target_env = ns_env_lookup(S, resolved_ns);
        if (target_env != NULL) {
            env_binding_t *b = env_find_here(target_env, sym_name);
            if (b != NULL) return b->val;
        }
        return NULL;
    }

    /* Unqualified: lexical -> defining_ns env. */
    mino_val *v = mino_env_get_sym(c->env, head);
    if (v != NULL) return v;
    if (c->defining_ns != NULL) {
        mino_env *ns_env = ns_env_lookup(S, c->defining_ns);
        if (ns_env != NULL) {
            v = mino_env_get_sym(ns_env, head);
            /* A :refer may store the source var rather than the raw value
             * (mirrors prim_refer; see apply_refer_options). Unwrap one
             * level so callers that test for MINO_MACRO still match. */
            if (v != NULL && mino_type_of(v) == MINO_VAR && v->as.var.bound) {
                v = v->as.var.root;
            }
            if (v != NULL) return v;
        }
    }
    return NULL;
}

/* Protocol-method detection. defprotocol expands each method into a
 * single-arity fn whose body is the one form
 *   (protocol-dispatch <dispatch-atom-sym> "<mname>" & params).
 * Recognize that exact shape; on match return 1 and fill *out_mname /
 * *out_atom with the method-name string and the resolved dispatch
 * atom. The fn body cons-spine is owned by the macroexpander and
 * persists across calls, so the recognizer can scan it once at
 * compile time without copying. */
static int try_protocol_method(compiler_t *c, mino_val *head,
                               mino_val **out_mname,
                               mino_val **out_atom)
{
    mino_val *fnv = probe_head_value(c, head);
    if (fnv == NULL || mino_type_of(fnv) != MINO_FN) return 0;
    /* Single-arity only: defprotocol always emits a single-arity defn.
     * params is the param vector; body is the body-form list. */
    if (fnv->as.fn.params == NULL) return 0;
    mino_val *body = fnv->as.fn.body;
    if (!mino_is_cons(body)) return 0;
    /* Exactly one body form: cdr is the tagged-nil terminator. */
    mino_val *body_tail = body->as.cons.cdr;
    if (body_tail != NULL && mino_type_of(body_tail) != MINO_NIL
        && mino_type_of(body_tail) != MINO_EMPTY_LIST) {
        return 0;
    }
    mino_val *form = body->as.cons.car;
    if (!mino_is_cons(form)) return 0;
    mino_val *call_head = form->as.cons.car;
    if (call_head == NULL || mino_type_of(call_head) != MINO_SYMBOL) return 0;
    /* Head is the bare symbol `protocol-dispatch` as emitted by the
     * defprotocol macro. A user-side rebinding under a different name
     * silently falls through to OP_CALL_CACHED -- correct, slower. */
    if (strcmp(call_head->as.s.data, "protocol-dispatch") != 0) return 0;
    mino_val *rest = form->as.cons.cdr;
    if (!mino_is_cons(rest)) return 0;
    mino_val *atom_sym = rest->as.cons.car;
    if (atom_sym == NULL || mino_type_of(atom_sym) != MINO_SYMBOL) return 0;
    mino_val *atom_val = probe_head_value(c, atom_sym);
    if (atom_val == NULL || mino_type_of(atom_val) != MINO_ATOM) return 0;
    mino_val *rest2 = rest->as.cons.cdr;
    if (!mino_is_cons(rest2)) return 0;
    mino_val *mname = rest2->as.cons.car;
    if (mname == NULL || mino_type_of(mname) != MINO_STRING) return 0;
    *out_mname = mname;
    *out_atom  = atom_val;
    return 1;
}

/* Macro-detection probe: does `head` resolve to a MINO_MACRO under the
 * same cascade the runtime would use at dispatch time? Used to gate
 * OP_CALL / OP_TAILCALL emission so macros stay on the tree-walker
 * path (their args are forms, not evaluated values). */
static int head_resolves_to_macro(compiler_t *c, mino_val *head)
{
    if (head == NULL || mino_type_of(head) != MINO_SYMBOL) return 0;
    if (find_local(c, head->as.s.data) >= 0) return 0;
    mino_val *v = probe_head_value(c, head);
    return v != NULL && mino_type_of(v) == MINO_MACRO;
}

/* Compile `form` so its value lands in some register, preferring to
 * reuse an existing register without emitting code. Returns the
 * register, or -1 on error.
 *
 * When `form` is a local symbol, the local's register is returned
 * directly: no alloc, no MOVE. This is the central register-pressure
 * win for the binop / unop fast lanes, which only constrain that
 * operands be in registers (not that they sit at any particular slot).
 *
 * For anything else, a fresh temp is allocated and the form is
 * compiled into it. The caller is responsible for the alloc/free
 * discipline via saved_next; any temps allocated here live until the
 * next_reg high-water restore. */
static int compile_operand_inplace(compiler_t *c, mino_val *form)
{
    if (form != NULL && mino_type_of(form) == MINO_SYMBOL) {
        int local = find_local(c, form->as.s.data);
        if (local >= 0) return local;
    }
    int r = alloc_reg(c);
    if (r < 0) return -1;
    if (compile_expr(c, form, r, 0) < 0) return -1;
    return r;
}

/* Pure core prims eligible for literal-arg compile-time fold.
 *
 * Soundness contract: the listed prims must be referentially
 * transparent on the value tier we can prove at compile time --
 * the args we feed in are self-evaluating literals (int / bool /
 * float / float32 / string / keyword / char / nil), so the prim
 * sees exactly the same input here as it would at runtime. Side
 * effects (I/O, atom mutation, var alter) are out -- listed prims
 * may allocate values but not observe or change state.
 *
 * What this leverages from Clojure: pure core arithmetic / compare
 * / bitwise / divmod / numeric-predicate prims have a contract
 * that's both stable across versions and statically known to the
 * compiler. The homoiconic source form gives us the literal args
 * before evaluation begins, and immutability guarantees the
 * folded value's identity stays valid for the bc's lifetime. */
struct pure_prim {
    const char *name;
    mino_val *(*prim)(mino_state *, mino_val *, mino_env *);
    int min_arity;   /* lower bound; -1 for unrestricted */
    int max_arity;   /* upper bound; -1 for unrestricted */
};

static const pure_prim_t PURE_PRIMS[] = {
    {"+",       prim_add,    0, -1},
    {"-",       prim_sub,    1, -1},
    {"*",       prim_mul,    0, -1},
    {"<",       prim_lt,     1, -1},
    {"<=",      prim_lte,    1, -1},
    {">",       prim_gt,     1, -1},
    {">=",      prim_gte,    1, -1},
    {"=",       prim_eq,     1, -1},
    {"inc",     prim_inc,    1,  1},
    {"dec",     prim_dec,    1,  1},
    {"zero?",   prim_zero_p, 1,  1},
    {"pos?",    prim_pos_p,  1,  1},
    {"neg?",    prim_neg_p,  1,  1},
    {"even?",   prim_even_p, 1,  1},
    {"odd?",    prim_odd_p,  1,  1},
    {"mod",     prim_mod,    2,  2},
    {"quot",    prim_quot,   2,  2},
    {"rem",     prim_rem,    2,  2},
    {"bit-and", prim_bit_and, 2, 2},
    {"bit-or",  prim_bit_or,  2, 2},
    {"bit-xor", prim_bit_xor, 2, 2},
    {"bit-not", prim_bit_not, 1, 1},
    {"bit-shift-left",  prim_bit_shift_left,  2, 2},
    {"bit-shift-right", prim_bit_shift_right, 2, 2},
    {"unsigned-bit-shift-right", prim_unsigned_bit_shift_right, 2, 2},
    /* Collection accessors are pure over immutable collections:
     * listed so the canonical-prim identity check at the
     * OP_NTH_VEC / OP_GET_KW_MAP emission sites recognises them. */
    {"nth",     prim_nth,    2,  3},
    {"get",     prim_get,    2,  3},
    /* Write-side collection ops. Their values (new collections) are
     * not const-pool-representable, so fold_result_constable rejects
     * them and try_fold_call never folds; the entries exist so the
     * canonical-prim identity check at the OP_CONJ_VEC / OP_ASSOC
     * emission sites recognises a non-shadowed call. */
    {"conj",    prim_conj,    1, -1},
    {"assoc",   prim_assoc,   3, -1},
    {"dissoc",  prim_dissoc,  1, -1},
    /* assoc! / conj! / dissoc! / disj! share the gating shape with
     * the persistent-side write prims: head must resolve to the
     * canonical bang prim, used by the OP_ASSOC_BANG / OP_CONJ_BANG /
     * OP_DISSOC_BANG / OP_DISJ_BANG emission sites. */
    {"assoc!",  prim_assoc_bang, 3, -1},
    {"conj!",   prim_conj_bang,  1, -1},
    {"dissoc!", prim_dissoc_bang, 2, -1},
    {"disj!",   prim_disj_bang,  2, -1},
    /* Read-side seq prims used by the OP_FIRST_VEC / OP_COUNT_VEC /
     * OP_EMPTY_VEC emission sites. count's result is an int -- can
     * be folded for literal vec args; first / empty? on literal
     * vectors fold too. fold_result_constable filters non-
     * representable results. */
    {"first",   prim_first,   1,  1},
    {"count",   prim_count,   1,  1},
    {"empty?",  prim_empty_p, 1,  1},
    {NULL, NULL, 0, 0},
};

static const pure_prim_t *find_pure_prim(const char *name)
{
    for (const pure_prim_t *p = PURE_PRIMS; p->name != NULL; p++) {
        if (strcmp(p->name, name) == 0) return p;
    }
    return NULL;
}

/* Result of a fold must be representable as a const-pool literal --
 * a value that can be the target of OP_LOAD_K without re-evaluation.
 * Numeric values, bools, strings, keywords, chars, and the empty
 * collections all qualify. Compound runtime values (cons lists,
 * closures, transients) do not. */
static int fold_result_constable(mino_val *v)
{
    if (v == NULL) return 1;
    switch (mino_type_of(v)) {
    case MINO_NIL: case MINO_BOOL:
    case MINO_INT: case MINO_FLOAT: case MINO_FLOAT32:
    case MINO_STRING: case MINO_KEYWORD: case MINO_CHAR:
    case MINO_RATIO: case MINO_BIGINT: case MINO_BIGDEC:
        return 1;
    default:
        return 0;
    }
}

/* True iff the head call should be folded at compile time. Returns the
 * matching pure_prim_t entry on a successful resolution; NULL when the
 * head isn't a recognized pure prim OR the symbol resolves to something
 * other than the canonical C prim (e.g., the user shadowed `+`). */
static const pure_prim_t *should_fold_call(compiler_t *c, mino_val *head)
{
    if (head == NULL || mino_type_of(head) != MINO_SYMBOL) return NULL;
    if (find_local(c, head->as.s.data) >= 0) return NULL;
    const pure_prim_t *pp = find_pure_prim(head->as.s.data);
    if (pp == NULL) return NULL;
    mino_val *hv = probe_head_value(c, head);
    if (hv == NULL || mino_type_of(hv) != MINO_PRIM) return NULL;
    /* Shadow check: the resolved value must be the canonical prim
     * with this name, not a different prim or a user redefinition
     * with the same name. Two MINO_PRIM values with the same stable
     * registered name denote the same primitive. */
    if (hv->as.prim.name == NULL
        || strcmp(hv->as.prim.name, pp->name) != 0) {
        return NULL;
    }
    return pp;
}

/* True iff `head` resolves at compile time to the canonical PRIM for
 * a name in PURE_PRIMS. Used to gate emission of the speculative
 * OP_*_II / OP_*_IK / unop fast-lane opcodes: a user-defined shadow
 * (e.g., (defn + [a b] ...)) must NOT pick up the C-prim semantics
 * at the fast-lane sites. Returns 0 for unknown names (caller already
 * filtered) AND for known names whose head resolves to a non-PRIM or
 * to a different PRIM. */
static int head_is_canonical_pure_prim(compiler_t *c, mino_val *head)
{
    if (head == NULL || mino_type_of(head) != MINO_SYMBOL) return 0;
    if (find_local(c, head->as.s.data) >= 0) return 0;
    const pure_prim_t *pp = find_pure_prim(head->as.s.data);
    if (pp == NULL) return 0;
    mino_val *hv = probe_head_value(c, head);
    if (hv == NULL || mino_type_of(hv) != MINO_PRIM) return 0;
    return hv->as.prim.name != NULL
        && strcmp(hv->as.prim.name, pp->name) == 0;
}

/* Try to resolve a compile-time-known value for `form`. Returns 1 and
 * writes the value through `out` when:
 *   - form is self-evaluating (literal),
 *   - form is a symbol bound to a local whose right-hand side itself
 *     folded (Phase E1 fold-through), or
 *   - form is a (pure-prim arg ...) call whose head resolves to the
 *     canonical PRIM and whose args themselves all fold via this
 *     same rule (Phase E1 recursive fold).
 * Returns 0 otherwise; `out` is left untouched on a 0 return.
 *
 * The recursive fold path lets `(let [x (+ 1 2)] (* x x))` collapse
 * to `9` at compile time -- compile_let records `x → 3`, then the
 * outer `*` call probes its args, finds `x → 3` via this helper,
 * and the existing try_fold_call path emits a single OP_LOAD_K. */
static int try_fold_arg(compiler_t *c, mino_val *v, mino_val **out)
{
    if (is_self_evaluating(v)) { *out = v; return 1; }
    if (v == NULL) return 0;
    if (mino_type_of(v) == MINO_SYMBOL) {
        mino_val *f = local_folded_value(c, v->as.s.data);
        if (f != NULL) { *out = f; return 1; }
        return 0;
    }
    if (mino_type_of(v) == MINO_CONS) {
        mino_state      *S    = c->S; /* needed for gc_pin / gc_unpin macros */
        mino_val        *head = v->as.cons.car;
        const pure_prim_t *pp;
        mino_val        *src;
        mino_val        *args;
        mino_val        *stack[64];
        mino_val        *folded;
        int                argc = 0;
        if (head == NULL || mino_type_of(head) != MINO_SYMBOL) return 0;
        pp = should_fold_call(c, head);
        if (pp == NULL) return 0;
        src = v->as.cons.cdr;
        while (mino_is_cons(src)) {
            mino_val *fa = NULL;
            if (argc >= (int)(sizeof(stack)/sizeof(stack[0]))) return 0;
            if (!try_fold_arg(c, src->as.cons.car, &fa)) return 0;
            stack[argc++] = fa;
            src = src->as.cons.cdr;
        }
        if (pp->min_arity >= 0 && argc < pp->min_arity) return 0;
        if (pp->max_arity >= 0 && argc > pp->max_arity) return 0;
        /* Pin every stack[] entry before building the cons-spine:
         * mino_cons may trigger a GC cycle and stack[] lives on the C
         * stack, which is invisible to the collector under -O2. */
        {
            int pi;
            for (pi = 0; pi < argc; pi++) gc_pin(stack[pi]);
        }
        args = mino_nil(c->S);
        for (int i = argc - 1; i >= 0; i--) {
            args = mino_cons(c->S, stack[i], args);
            if (args == NULL) { gc_unpin(argc); return 0; }
        }
        gc_unpin(argc);
        /* The speculative fold contract requires prim_throw_classified
         * to take the "set diag + return NULL" branch on error, not the
         * "longjmp to active try-frame" branch. If the compile is
         * happening underneath a live try (compile-on-call from inside
         * a user (try ...) block, for instance), longjmp would escape
         * the compile and surface a stale exception. Suppress try_depth
         * for the duration of the speculative prim call; the saved
         * value is restored regardless of outcome. */
        int saved_td = mino_current_ctx(c->S)->try_depth;
        mino_current_ctx(c->S)->try_depth = 0;
        folded = pp->prim(c->S, args, c->env);
        mino_current_ctx(c->S)->try_depth = saved_td;
        if (folded == NULL) { clear_error(c->S); return 0; }
        if (!fold_result_constable(folded)) return 0;
        *out = folded;
        return 1;
    }
    return 0;
}

/* Attempt to fold a (head literal-args...) call. Returns 0 on a
 * successful fold (LOAD_K already emitted), 1 to decline (no LOAD_K
 * emitted; caller should fall through to the normal emit path), or
 * -1 on a hard error (sets c->ok = 0). */
static int try_fold_call(compiler_t *c, mino_val *form, int dst,
                         const pure_prim_t *pp)
{
    mino_state *S    = c->S; /* needed for gc_pin / gc_unpin macros */
    int    argc      = 0;
    mino_val *p    = form->as.cons.cdr;
    mino_val *stack[64];
    while (mino_is_cons(p)) {
        mino_val *folded_arg = NULL;
        if (argc >= (int)(sizeof(stack)/sizeof(stack[0]))) return 1;
        if (!try_fold_arg(c, p->as.cons.car, &folded_arg)) return 1;
        stack[argc++] = folded_arg;
        p = p->as.cons.cdr;
    }
    if (pp->min_arity >= 0 && argc < pp->min_arity) return 1;
    if (pp->max_arity >= 0 && argc > pp->max_arity) return 1;

    /* Rebuild the args list as a fresh cons-spine the prim can walk.
     * Substituted args replace any symbol references that folded
     * through a let binding. */
    /* Pin every stack[] entry before building the cons-spine:
     * mino_cons may trigger a GC cycle and stack[] lives on the C
     * stack, which is invisible to the collector under -O2. */
    {
        int pi;
        for (pi = 0; pi < argc; pi++) gc_pin(stack[pi]);
    }
    mino_val *args = mino_nil(c->S);
    for (int i = argc - 1; i >= 0; i--) {
        args = mino_cons(c->S, stack[i], args);
        if (args == NULL) { gc_unpin(argc); return 1; }
    }
    gc_unpin(argc);

    /* Same try_depth suppression as try_fold_arg: the speculative
     * fold needs prim_throw_classified to take the diag-return-NULL
     * branch, not longjmp. */
    int saved_td = mino_current_ctx(c->S)->try_depth;
    mino_current_ctx(c->S)->try_depth = 0;
    mino_val *folded = pp->prim(c->S, args, c->env);
    mino_current_ctx(c->S)->try_depth = saved_td;
    if (folded == NULL) {
        /* The prim raised at fold time (e.g., division by zero).
         * Clear the diagnostic and decline -- the runtime path will
         * re-encounter the error in the right execution context with
         * the right stack trace. */
        clear_error(c->S);
        return 1;
    }
    if (!fold_result_constable(folded)) return 1;

    int k = add_const(c, folded);
    if (k < 0) return -1;
    emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
    c->has_folds = 1;
    return 0;
}

/* Map a binary arith / compare / bitwise / div-class prim name to its
 * BINOP_* subop. Returns -1 when the name isn't one of the speculative
 * fast lanes. */
static int binop_subop_for_name(const char *name)
{
    if (strcmp(name, "+") == 0)  return BINOP_ADD;
    if (strcmp(name, "-") == 0)  return BINOP_SUB;
    if (strcmp(name, "*") == 0)  return BINOP_MUL;
    if (strcmp(name, "<") == 0)  return BINOP_LT;
    if (strcmp(name, "<=") == 0) return BINOP_LE;
    if (strcmp(name, ">") == 0)  return BINOP_GT;
    if (strcmp(name, ">=") == 0) return BINOP_GE;
    if (strcmp(name, "=") == 0)  return BINOP_EQ;
    if (strcmp(name, "mod")  == 0) return BINOP_MOD;
    if (strcmp(name, "quot") == 0) return BINOP_QUOT;
    if (strcmp(name, "rem")  == 0) return BINOP_REM;
    if (strcmp(name, "bit-and") == 0) return BINOP_BAND;
    if (strcmp(name, "bit-or")  == 0) return BINOP_BOR;
    if (strcmp(name, "bit-xor") == 0) return BINOP_BXOR;
    if (strcmp(name, "bit-shift-left")  == 0) return BINOP_SHL;
    if (strcmp(name, "bit-shift-right") == 0) return BINOP_SHR;
    if (strcmp(name, "unsigned-bit-shift-right") == 0) return BINOP_USHR;
    return -1;
}

/* Map a unary numeric / predicate / bitwise prim name to its UNOP_*
 * subop. */
static int unop_subop_for_name(const char *name)
{
    if (strcmp(name, "inc")   == 0) return UNOP_INC;
    if (strcmp(name, "dec")   == 0) return UNOP_DEC;
    if (strcmp(name, "zero?") == 0) return UNOP_ZERO_P;
    if (strcmp(name, "pos?")  == 0) return UNOP_POS_P;
    if (strcmp(name, "neg?")  == 0) return UNOP_NEG_P;
    if (strcmp(name, "even?") == 0) return UNOP_EVEN_P;
    if (strcmp(name, "odd?")  == 0) return UNOP_ODD_P;
    if (strcmp(name, "bit-not") == 0) return UNOP_BNOT;
    return -1;
}

/* Count the args in a call form's cdr (cons-spine). Caller must have
 * already validated the head; returns the arg count. */
static int count_call_args(mino_val *form)
{
    return bc_cons_len(form->as.cons.cdr);
}

/* Speculative unary-arith fast lane (inc, dec, zero?, pos?, neg?, even?,
 * odd?, bit-not). Returns 0 if handled, -1 if handled with error, 1 to
 * fall through. The caller must have verified the head is the canonical
 * non-shadowed pure prim. */
static int try_emit_unop_fast(compiler_t *c, mino_val *form,
                              mino_val *head, int dst)
{
    int usubop = unop_subop_for_name(head->as.s.data);
    mino_val *a1;
    int saved_next, src_reg;
    if (usubop < 0) return 1;
    if (count_call_args(form) != 1) return 1;
    saved_next = c->next_reg;
    a1 = form->as.cons.cdr->as.cons.car;
    src_reg = compile_operand_inplace(c, a1);
    if (src_reg < 0) return -1;
    if (dst > 0xFF || src_reg > 0xFF) { c->ok = 0; return -1; }
    {
        static const mino_bc_op_t unop_op[UNOP__COUNT] = {
            OP_INC_I,    OP_DEC_I,    OP_ZERO_INT_P,
            OP_POS_P_I,  OP_NEG_P_I,
            OP_EVEN_P_I, OP_ODD_P_I,  OP_BNOT_I,
        };
        emit_abc(c, unop_op[usubop], (unsigned)dst, (unsigned)src_reg, 0);
    }
    c->next_reg = saved_next;
    return 0;
}

/* N-arity left-associative expansion of (+, -, *) on 3+ args. Emits
 * an accumulator-style chain of OP_*_II ops; each step keeps the
 * intrinsic overflow check from the two-operand opcode so literal
 * wrap behavior matches the prim. Returns 0 if handled, -1 on error,
 * 1 to fall through. */
static int try_emit_nary_binop(compiler_t *c, mino_val *form,
                               int subop, int argc, int dst)
{
    mino_val *args[64];
    int n = 0;
    mino_val *q;
    int saved_next, acc, a1, a2, i;
    static const mino_bc_op_t binop_op_nary[] = {
        OP_ADD_II, OP_SUB_II, OP_MUL_II,
    };
    if (subop != BINOP_ADD && subop != BINOP_SUB && subop != BINOP_MUL)
        return 1;
    if (argc < 3) return 1;
    q = form->as.cons.cdr;
    while (mino_is_cons(q) && n < (int)(sizeof(args)/sizeof(args[0]))) {
        args[n++] = q->as.cons.car;
        q = q->as.cons.cdr;
    }
    if (n != argc) {
        /* Overflow: too many args; fall back to prim. */
        return 1;
    }
    saved_next = c->next_reg;
    /* Accumulator reg holds the running result. */
    acc = alloc_reg(c);
    if (acc < 0) return -1;
    a1 = compile_operand_inplace(c, args[0]);
    if (a1 < 0) return -1;
    a2 = compile_operand_inplace(c, args[1]);
    if (a2 < 0) return -1;
    if (acc > 0xFF || a1 > 0xFF || a2 > 0xFF) { c->ok = 0; return -1; }
    emit_abc(c, binop_op_nary[subop],
             (unsigned)acc, (unsigned)a1, (unsigned)a2);
    for (i = 2; i < n; i++) {
        int ai = compile_operand_inplace(c, args[i]);
        if (ai < 0) return -1;
        if (ai > 0xFF) { c->ok = 0; return -1; }
        emit_abc(c, binop_op_nary[subop],
                 (unsigned)acc, (unsigned)acc, (unsigned)ai);
    }
    if (dst > 0xFF) { c->ok = 0; return -1; }
    if (dst != acc) {
        emit_abc(c, OP_MOVE, (unsigned)dst, (unsigned)acc, 0);
    }
    c->next_reg = saved_next;
    return 0;
}

/* Two-operand binop fast lane (+, -, *, <, <=, >, >=, =, mod, quot,
 * rem, bit-*, shifts) with an immediate-operand sub-path for ops that
 * have an IK form. Returns 0 if handled, -1 on error, 1 to fall
 * through. */
static int try_emit_binop_two_arg(compiler_t *c, mino_val *form,
                                  int subop, int dst)
{
    int saved_next = c->next_reg;
    mino_val *a1 = form->as.cons.cdr->as.cons.car;
    mino_val *a2 = form->as.cons.cdr->as.cons.cdr->as.cons.car;
    int lhs_reg, rhs_reg;
    /* Immediate-operand fast lane. If one operand is a compile-time
     * int literal that fits in signed 8 bits and the op has an IK
     * form, emit it directly -- sparing the OP_LOAD_K + register slot
     * for the literal. Commutative ops (+, =) tolerate the literal on
     * either side; subtract and comparators require the literal on
     * the right (a < lit, not lit < a). */
    {
        int ik_op = -1;
        mino_val *lit_val = NULL;
        mino_val *reg_val = NULL;
        switch (subop) {
        case BINOP_ADD: ik_op = OP_ADD_IK; break;
        case BINOP_SUB: ik_op = OP_SUB_IK; break;
        case BINOP_LT:  ik_op = OP_LT_IK;  break;
        case BINOP_LE:  ik_op = OP_LE_IK;  break;
        case BINOP_EQ:  ik_op = OP_EQ_IK;  break;
        default: break;
        }
        if (ik_op >= 0) {
            int commutative = (subop == BINOP_ADD || subop == BINOP_EQ);
            if (MINO_IS_INT(a2)) {
                long long n = MINO_INT_VAL(a2);
                if (n >= -128 && n <= 127) { lit_val = a2; reg_val = a1; }
            } else if (commutative && MINO_IS_INT(a1)) {
                long long n = MINO_INT_VAL(a1);
                if (n >= -128 && n <= 127) { lit_val = a1; reg_val = a2; }
            }
            if (lit_val != NULL) {
                int rg = compile_operand_inplace(c, reg_val);
                long long n;
                if (rg < 0) return -1;
                if (dst > 0xFF || rg > 0xFF) { c->ok = 0; return -1; }
                n = MINO_INT_VAL(lit_val);
                emit_abc(c, (mino_bc_op_t)ik_op,
                         (unsigned)dst, (unsigned)rg,
                         (unsigned)(n & 0xFF));
                c->next_reg = saved_next;
                return 0;
            }
        }
    }
    lhs_reg = compile_operand_inplace(c, a1);
    if (lhs_reg < 0) return -1;
    rhs_reg = compile_operand_inplace(c, a2);
    if (rhs_reg < 0) return -1;
    if (dst > 0xFF || lhs_reg > 0xFF || rhs_reg > 0xFF) {
        c->ok = 0; return -1;
    }
    /* Per-op specialized opcode. ABC form: A=dst, B=lhs, C=rhs. The
     * B/C operands are independent reg indices so
     * compile_operand_inplace can return locals' registers directly,
     * sparing the alloc + OP_MOVE pair for the common (op local
     * local) shape. */
    {
        static const mino_bc_op_t binop_op[BINOP__COUNT] = {
            OP_ADD_II,  OP_SUB_II,  OP_MUL_II,
            OP_LT_II,   OP_LE_II,   OP_GT_II,
            OP_GE_II,   OP_EQ_II,
            OP_MOD_II,  OP_QUOT_II, OP_REM_II,
            OP_BAND_II, OP_BOR_II,  OP_BXOR_II,
            OP_SHL_II,  OP_SHR_II,  OP_USHR_II,
        };
        emit_abc(c, binop_op[subop], (unsigned)dst,
                 (unsigned)lhs_reg, (unsigned)rhs_reg);
    }
    c->next_reg = saved_next;
    return 0;
}

/* Speculative arith fast lane. Tries the unary subset first, then
 * the variadic N-arity expansion of +, -, *, then the two-arg binop
 * IK / II forms. Returns 0 if handled, -1 on error, 1 to fall
 * through. */
static int try_emit_arith_fast(compiler_t *c, mino_val *form,
                               mino_val *head, int dst)
{
    int r;
    int subop;
    int argc;
    r = try_emit_unop_fast(c, form, head, dst);
    if (r <= 0) return r;
    subop = binop_subop_for_name(head->as.s.data);
    if (subop < 0) return 1;
    argc = count_call_args(form);
    r = try_emit_nary_binop(c, form, subop, argc, dst);
    if (r <= 0) return r;
    if (argc == 2) return try_emit_binop_two_arg(c, form, subop, dst);
    return 1;
}

/* Two-operand collection-op fast lane. Each entry pairs a prim name
 * to its specialised ABC opcode; the lane fires only when the call's
 * argc matches. nth/get/dissoc/conj!/dissoc!/disj! are the
 * persistent + transient family that takes (coll, key-or-item) and
 * emits a single ABC opcode whose runtime checks for the expected
 * collection shape and falls back to the prim on miss. */
static const struct {
    const char  *name;
    mino_bc_op_t op;
} k_collection_bin_ops[] = {
    {"nth",     OP_NTH_VEC},
    {"get",     OP_GET_KW_MAP},
    {"conj",    OP_CONJ_VEC},
    {"dissoc",  OP_DISSOC},
    {"conj!",   OP_CONJ_BANG},
    {"dissoc!", OP_DISSOC_BANG},
    {"disj!",   OP_DISJ_BANG},
};

/* Single-arg read-side fast lane. (first / count / empty?) on a
 * MINO_VECTOR fall through to OP_*_VEC; runtime checks the type and
 * routes to the canonical prim for anything else. */
static const struct {
    const char  *name;
    mino_bc_op_t op;
} k_collection_unary_ops[] = {
    {"first",  OP_FIRST_VEC},
    {"count",  OP_COUNT_VEC},
    {"empty?", OP_EMPTY_VEC},
};

static int emit_abc_two_arg(compiler_t *c, mino_bc_op_t op,
                            mino_val *a1, mino_val *a2, int dst)
{
    int saved_next = c->next_reg;
    int r1 = compile_operand_inplace(c, a1);
    int r2;
    if (r1 < 0) return -1;
    r2 = compile_operand_inplace(c, a2);
    if (r2 < 0) return -1;
    if (dst > 0xFF || r1 > 0xFF || r2 > 0xFF) { c->ok = 0; return -1; }
    emit_abc(c, op, (unsigned)dst, (unsigned)r1, (unsigned)r2);
    c->next_reg = saved_next;
    return 0;
}

static int emit_abc_one_arg(compiler_t *c, mino_bc_op_t op,
                            mino_val *a1, int dst)
{
    int saved_next = c->next_reg;
    int r1 = compile_operand_inplace(c, a1);
    if (r1 < 0) return -1;
    if (dst > 0xFF || r1 > 0xFF) { c->ok = 0; return -1; }
    emit_abc(c, op, (unsigned)dst, (unsigned)r1, 0);
    c->next_reg = saved_next;
    return 0;
}

/* Three-arg coll-k-v fast lane shared by `assoc` and `assoc!`.
 * Both opcodes consume three consecutive regs at `base` and emit one
 * OP_ASSOC / OP_ASSOC_BANG into dst. */
static int try_emit_assoc_three(compiler_t *c, mino_val *form,
                                mino_bc_op_t op, int dst)
{
    int saved_next = c->next_reg;
    mino_val *a1 = form->as.cons.cdr->as.cons.car;
    mino_val *a2 = form->as.cons.cdr->as.cons.cdr->as.cons.car;
    mino_val *a3 = form->as.cons.cdr->as.cons.cdr
                         ->as.cons.cdr->as.cons.car;
    int base_reg, k_reg, v_reg;
    /* OP_ASSOC / OP_ASSOC_BANG need three consecutive regs for
     * coll, k, v starting at `base`. Allocate them up front so the
     * slots can't be aliased by sub-expressions. */
    base_reg = alloc_reg(c);
    if (base_reg < 0) return -1;
    k_reg = alloc_reg(c);
    if (k_reg < 0) return -1;
    v_reg = alloc_reg(c);
    if (v_reg < 0) return -1;
    if (compile_expr(c, a1, base_reg, 0) < 0) return -1;
    if (compile_expr(c, a2, k_reg, 0)    < 0) return -1;
    if (compile_expr(c, a3, v_reg, 0)    < 0) return -1;
    if (dst > 0xFF || base_reg > 0xFF) { c->ok = 0; return -1; }
    emit_abc(c, op, (unsigned)dst, (unsigned)base_reg, 0);
    c->next_reg = saved_next;
    return 0;
}

/* Collection-op fast lanes. nth/get/conj/dissoc/conj!/dissoc!/disj!
 * gate on arity 2; first/count/empty? gate on arity 1;
 * assoc/assoc! gate on arity 3. Returns 0 if handled, -1 on error,
 * 1 to fall through. Variadic call forms (e.g. (dissoc m k1 k2 k3))
 * stay on the regular OP_CALL path so the prim's loop handles them. */
static int try_emit_collection_fast(compiler_t *c, mino_val *form,
                                    mino_val *head, int dst)
{
    int argc = count_call_args(form);
    const char *name = head->as.s.data;
    size_t i;

    if (argc == 2) {
        mino_val *a1 = form->as.cons.cdr->as.cons.car;
        mino_val *a2 = form->as.cons.cdr->as.cons.cdr->as.cons.car;
        for (i = 0; i < sizeof(k_collection_bin_ops)
                                 / sizeof(k_collection_bin_ops[0]); i++) {
            if (strcmp(name, k_collection_bin_ops[i].name) == 0) {
                return emit_abc_two_arg(c, k_collection_bin_ops[i].op,
                                        a1, a2, dst);
            }
        }
    }
    if (argc == 1) {
        mino_val *a1 = form->as.cons.cdr->as.cons.car;
        for (i = 0; i < sizeof(k_collection_unary_ops)
                                 / sizeof(k_collection_unary_ops[0]); i++) {
            if (strcmp(name, k_collection_unary_ops[i].name) == 0) {
                return emit_abc_one_arg(c, k_collection_unary_ops[i].op,
                                        a1, dst);
            }
        }
    }
    if (argc == 3) {
        if (strcmp(name, "assoc") == 0) {
            return try_emit_assoc_three(c, form, OP_ASSOC, dst);
        }
        if (strcmp(name, "assoc!") == 0) {
            return try_emit_assoc_three(c, form, OP_ASSOC_BANG, dst);
        }
    }
    return 1;
}

/* Keyword-as-fn fast lane: `(:kw coll)` with a literal keyword head
 * and exactly one arg compiles to OP_LOAD_K + OP_GET_KW_MAP. Returns
 * 0 if handled, -1 on error, 1 to fall through. */
static int try_emit_keyword_call(compiler_t *c, mino_val *form,
                                 mino_val *head, int argc, int dst)
{
    int saved_next, coll_reg, key_k, key_reg;
    if (argc != 1) return 1;
    if (head == NULL || mino_type_of(head) != MINO_KEYWORD) return 1;
    saved_next = c->next_reg;
    coll_reg = compile_operand_inplace(c, form->as.cons.cdr->as.cons.car);
    if (coll_reg < 0) return -1;
    key_k = add_const(c, head);
    if (key_k < 0) return -1;
    key_reg = alloc_reg(c);
    if (key_reg < 0) return -1;
    if (dst > 0xFF || coll_reg > 0xFF || key_reg > 0xFF) {
        c->ok = 0; return -1;
    }
    emit_abx(c, OP_LOAD_K, (unsigned)key_reg, (unsigned)key_k);
    emit_abc(c, OP_GET_KW_MAP, (unsigned)dst,
             (unsigned)coll_reg, (unsigned)key_reg);
    c->next_reg = saved_next;
    return 0;
}

/* Inline-cache fast lane for calls whose head is a global symbol with
 * no static local binding. Two-word encoding (ABC + slot index in
 * word-2's Bx) shared by three opcodes:
 *   non-tail head=fn          -> OP_CALL_CACHED
 *   non-tail head=protocol fn -> OP_PROTOCOL_CALL_CACHED
 *   tail     head=protocol fn -> OP_PROTOCOL_TAILCALL_CACHED
 *
 * Non-protocol tail calls fall through to the slow path's
 * OP_TAILCALL emit (no cached tail-call variant for plain heads
 * yet). The non-protocol non-tail branch skips the
 * OP_GETGLOBAL_CACHED step entirely (callee resolution + caching
 * fuses into the call opcode itself). Qualified symbols (foo/bar)
 * are eligible -- the runtime's resolve_global handles both forms.
 * Returns 0 if handled, -1 on error, 1 to fall through. */
static int try_emit_cached_call(compiler_t *c, mino_val *form,
                                mino_val *head, int argc, int dst,
                                int tail)
{
    mino_val *proto_mname = NULL;
    mino_val *proto_atom  = NULL;
    int is_proto;
    int saved_next, arg_base, slot, i;
    mino_val *cur;
    mino_bc_op_t op;
    if (head == NULL || mino_type_of(head) != MINO_SYMBOL) return 1;
    if (find_local(c, head->as.s.data) >= 0) return 1;
    is_proto = (argc >= 1)
        && try_protocol_method(c, head, &proto_mname, &proto_atom);
    if (tail && !is_proto) return 1;
    saved_next = c->next_reg;
    arg_base = c->next_reg;
    for (i = 0; i < argc; i++) {
        if (alloc_reg(c) < 0) return -1;
    }
    cur = form->as.cons.cdr;
    for (i = 0; i < argc; i++) {
        if (compile_expr(c, cur->as.cons.car, arg_base + i, 0) < 0)
            return -1;
        cur = cur->as.cons.cdr;
    }
    if (is_proto) {
        slot = alloc_protocol_ic_slot(c, proto_mname, proto_atom);
        op   = tail ? OP_PROTOCOL_TAILCALL_CACHED
                    : OP_PROTOCOL_CALL_CACHED;
    } else {
        slot = alloc_ic_slot(c, head);
        op   = OP_CALL_CACHED;
    }
    if (slot < 0) return -1;
    if (arg_base > 0xFF || dst > 0xFF) { c->ok = 0; return -1; }
    emit_abc(c, op, (unsigned)arg_base, (unsigned)argc, (unsigned)dst);
    /* Second instruction word: carries the slot index in Bx so the
     * handler can fetch it via code[pc++]. The main dispatch never
     * sees this word -- the handler consumes it before the loop
     * reads its next op. The op byte is OP_NOP so a stray decode
     * (e.g., a bytecode dumper) is harmless. */
    emit_abx(c, OP_NOP, 0, (unsigned)slot);
    c->next_reg = saved_next;
    return 0;
}

/* Plain OP_CALL / OP_TAILCALL fallback. Allocates fn_reg + argc
 * consecutive arg slots, compiles head + args, emits the opcode.
 * Returns 0 on success, -1 on error. */
static int emit_plain_call(compiler_t *c, mino_val *form,
                           mino_val *head, int argc, int dst, int tail)
{
    int saved_next = c->next_reg;
    int fn_reg, arg_base, i;
    mino_val *cur;
    fn_reg = alloc_reg(c);
    if (fn_reg < 0) return -1;
    arg_base = c->next_reg;
    for (i = 0; i < argc; i++) {
        if (alloc_reg(c) < 0) return -1;
    }
    /* Compile the head into fn_reg. */
    if (compile_expr(c, head, fn_reg, 0) < 0) return -1;
    /* Compile each argument into its slot. */
    cur = form->as.cons.cdr;
    for (i = 0; i < argc; i++) {
        if (compile_expr(c, cur->as.cons.car, arg_base + i, 0) < 0) return -1;
        cur = cur->as.cons.cdr;
    }
    if (tail) {
        /* OP_TAILCALL: A=fn, B=argc. Returns MINO_TAIL_CALL sentinel;
         * the apply_callable trampoline picks up (fn, args) without
         * growing the C stack. dst is ignored -- the body's OP_RETURN
         * is dead code after a tail call. */
        emit_abc(c, OP_TAILCALL, (unsigned)fn_reg, (unsigned)argc, 0);
    } else {
        /* OP_CALL: A=fn, B=argc, C=ret. Result lands at register `dst`. */
        if (dst > 0xFF) { c->ok = 0; return -1; }
        emit_abc(c, OP_CALL, (unsigned)fn_reg, (unsigned)argc, (unsigned)dst);
    }
    c->next_reg = saved_next;
    return 0;
}

static int compile_call_impl(compiler_t *c, mino_val *form, int dst, int tail)
{
    mino_val *head = form->as.cons.car;
    mino_val *cur;
    int argc;
    int r;

    if (head_resolves_to_macro(c, head)) { c->ok = 0; return -1; }

    /* Reduce-pattern rewrite: (reduce (fn [acc x] (assoc acc ...)) seed coll)
     * routes through a transient like the loop-builder rewrite, but
     * for the (reduce ...) call shape. Skips when the head is shadowed
     * or the matcher declines. */
    {
        mino_val *rewritten = try_reduce_rewrite(c, form);
        if (rewritten != NULL) {
            return compile_expr(c, rewritten, dst, tail);
        }
    }

    /* Literal-arg pure-fn fold (Clojure semantics: pure prims with
     * self-evaluating literal args have an answer the compiler can
     * compute at compile time). If the head resolves to the canonical
     * C prim for a name in the pure-prims allow list AND every arg is
     * a self-evaluating literal, run the prim now and emit OP_LOAD_K
     * with the result. The fold records its dependency on the var-
     * resolution state via bc->compile_ic_gen (set at end of compile);
     * any subsequent def / ns-unmap bumps S->ns_vars.ic_gen and forces a
     * recompile at the next call so a redefined `+` doesn't keep
     * yielding the stale constant. */
    {
        const pure_prim_t *pp = should_fold_call(c, head);
        if (pp != NULL) {
            int fold_r = try_fold_call(c, form, dst, pp);
            if (fold_r == 0)  return 0;
            if (fold_r < 0)   return -1;
            /* fold_r == 1: decline -- fall through to normal emit path. */
        }
    }

    /* Speculative fast lanes for unary and binary arith / compare
     * calls. The head must be the named prim (non-local, non-macro,
     * and the var must still point at the canonical C prim -- a user
     * shadow like `(defn + [a b] ...)` makes head_is_canonical_pure_prim
     * return false so we fall through to the regular OP_CALL emission
     * and the shadow runs at runtime). */
    if (head != NULL && mino_type_of(head) == MINO_SYMBOL
        && find_local(c, head->as.s.data) < 0
        && head_is_canonical_pure_prim(c, head)) {
        r = try_emit_arith_fast(c, form, head, dst);
        if (r <= 0) return r;
        r = try_emit_collection_fast(c, form, head, dst);
        if (r <= 0) return r;
    }

    /* Walk the cdr, counting args while validating each is a cons. */
    cur = form->as.cons.cdr;
    argc = 0;
    while (mino_is_cons(cur)) {
        argc++;
        cur = cur->as.cons.cdr;
    }
    if (cur != NULL && mino_type_of(cur) != MINO_NIL
        && mino_type_of(cur) != MINO_EMPTY_LIST) {
        /* Improper list form -- not a regular call. */
        c->ok = 0; return -1;
    }
    if (argc > 0xFF) { c->ok = 0; return -1; }   /* B operand is 8 bits */

    r = try_emit_keyword_call(c, form, head, argc, dst);
    if (r <= 0) return r;

    r = try_emit_cached_call(c, form, head, argc, dst, tail);
    if (r <= 0) return r;

    return emit_plain_call(c, form, head, argc, dst, tail);
}


/* ------------------------------------------------------------------- */
/* Form dispatch                                                       */
/* ------------------------------------------------------------------- */

static int compile_symbol_ref(compiler_t *c, mino_val *sym, int dst)
{
    /* Special pseudo-symbols that always resolve through GETGLOBAL:
     * nil/true/false aren't symbols, they're typed singletons -- so
     * any MINO_SYMBOL falls through to local/global lookup. */
    const char *data = sym->as.s.data;
    /* Reject qualified names (foo/bar): the runtime fast path is the
     * eval_qualified_symbol cascade, route it through OP_GETGLOBAL. */
    int local_reg = find_local(c, data);
    if (local_reg >= 0) {
        emit_abc(c, OP_MOVE, (unsigned)dst, (unsigned)local_reg, 0);
        return 0;
    }
    int slot = alloc_ic_slot(c, sym);
    if (slot < 0) return -1;
    emit_abx(c, OP_GETGLOBAL_CACHED, (unsigned)dst, (unsigned)slot);
    return 0;
}

static int compile_expr(compiler_t *c, mino_val *form, int dst, int tail)
{
    if (c->depth >= BC_MAX_COMPILE_DEPTH) {
        prim_throw_classified(c->S, "user", "MBC002",
            "form too deeply nested: compile-time recursion limit exceeded");
        c->ok = 0;
        return -1;
    }
    c->depth++;
    int saved_line = c->cur_line;
    int saved_col  = c->cur_column;
    if (form != NULL && mino_is_cons(form)) {
        if (form->as.cons.line > 0)   c->cur_line   = form->as.cons.line;
        if (form->as.cons.column > 0) c->cur_column = form->as.cons.column;
        if (c->bc->source_map.file == NULL && form->as.cons.file != NULL) {
            c->bc->source_map.file = form->as.cons.file;
        }
    }
    int rc = compile_expr_dispatch(c, form, dst, tail);
    c->cur_line   = saved_line;
    c->cur_column = saved_col;
    c->depth--;
    return rc;
}

static int compile_expr_dispatch(compiler_t *c, mino_val *form,
                                 int dst, int tail)
{
    if (form == NULL) {
        int k = add_const(c, mino_nil(c->S));
        if (k < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
        return 0;
    }
    if (is_self_evaluating(form)) {
        int k = add_const(c, form);
        if (k < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
        return 0;
    }
    if (mino_type_of(form) == MINO_SYMBOL) {
        return compile_symbol_ref(c, form, dst);
    }
    if (mino_type_of(form) == MINO_EMPTY_LIST) {
        int k = add_const(c, form);
        if (k < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
        return 0;
    }
    if (mino_type_of(form) == MINO_VECTOR || mino_type_of(form) == MINO_MAP
        || mino_type_of(form) == MINO_SET) {
        /* Vector literal with only self-evaluating elements is a
         * constant: stash the whole vector in the pool and emit a
         * single OP_LOAD_K. Vectors with non-const elements still
         * decline (their tree-walker lowering handles element
         * evaluation). Empty map / set literals are trivially
         * constant and pool-safe -- this lets the builder rewrite's
         * `(transient {})` / `(transient #{})` shapes compile to BC
         * rather than falling back to tree-walk eval for the whole
         * enclosing fn. Non-empty maps / sets keep declining: the
         * cross-thread future-exception path observed loss of
         * pooled-map fields when their const-pool entry is reached
         * from a worker thread; see .local/BUGS.md for the audit. */
        if (mino_type_of(form) == MINO_VECTOR) {
            int all_const = 1;
            for (size_t i = 0; i < form->as.vec.len; i++) {
                if (!is_self_evaluating(vec_nth(form, i))) {
                    all_const = 0;
                    break;
                }
            }
            if (all_const) {
                int k = add_const(c, form);
                if (k < 0) return -1;
                emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
                return 0;
            }
            /* Constructor lane: lower `[a b c]` with non-const
             * elements to `(clojure.core/vector a b c)` so the
             * enclosing fn keeps compiling to BC. The qualified head
             * avoids breakage when a user namespace declares
             * `:refer-clojure :exclude [vector]`; the literal must
             * still build a vector regardless of the local `vector`
             * binding (matches JVM Clojure's reader-time literal
             * semantics). Without this lowering, every `[a b c]`
             * literal in a defn body forces tree-walk eval. */
            {
                mino_state *S = c->S;
                mino_val *head = mino_symbol(S, "clojure.core/vector");
                if (head == NULL) return -1;
                /* Root head across the mino_nil + mino_cons loop: a minor
                 * GC triggered inside mino_cons would otherwise see head
                 * only via the C stack, which the conservative scanner may
                 * miss if the value was spilled to a non-pointer-aligned
                 * slot.  gc_pin anchors it precisely. */
                gc_pin(head);
                mino_val *args = mino_nil(S);
                for (size_t i = form->as.vec.len; i > 0; i--) {
                    args = mino_cons(S, vec_nth(form, i - 1), args);
                    if (args == NULL) { gc_unpin(1); return -1; }
                }
                gc_unpin(1);
                mino_val *call = mino_cons(S, head, args);
                if (call == NULL) return -1;
                return compile_expr(c, call, dst, tail);
            }
        } else if (mino_type_of(form) == MINO_MAP) {
            if (form->as.map.len == 0) {
                int k = add_const(c, form);
                if (k < 0) return -1;
                emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
                return 0;
            }
            /* Reader records (tagged-literal fallback, preserved
             * reader conditionals) self-evaluate; the hash-map
             * lowering below would re-evaluate their stored tag
             * symbol / form. Decline so the tree-walker's literal
             * path returns the record unchanged. */
            if (mino_is_reader_record(c->S, form)) {
                c->ok = 0;
                return -1;
            }
            /* Constructor lane: lower `{:k0 v0 :k1 v1 ...}` to
             * `(hash-map :k0 v0 :k1 v1 ...)` so the call site builds
             * a fresh map per invocation with each value evaluated
             * in the current local scope. Without this lowering a
             * defn body returning a literal map with non-self-eval
             * values falls back to tree-walk eval. The literal's
             * keys are iterated in insertion order via key_order /
             * val_order (flat-map representation; promotes to HAMT
             * only above MINO_FLATMAP_THRESHOLD which the reader
             * doesn't cross for typical source literals). */
            {
                mino_state *S = c->S;
                mino_val *head = mino_symbol(S, "clojure.core/hash-map");
                size_t      i;
                if (head == NULL) return -1;
                /* Root head across the mino_cons loop (same pattern as
                 * the vector lowering above). */
                gc_pin(head);
                mino_val *call_args = mino_nil(S);
                for (i = form->as.map.len; i > 0; i--) {
                    mino_val *kk = vec_nth(form->as.map.key_order, i - 1);
                    mino_val *vv = (form->as.map.val_order != NULL)
                                     ? vec_nth(form->as.map.val_order, i - 1)
                                     : map_get_val(form, kk);
                    call_args = mino_cons(S, vv, call_args);
                    if (call_args == NULL) { gc_unpin(1); return -1; }
                    call_args = mino_cons(S, kk, call_args);
                    if (call_args == NULL) { gc_unpin(1); return -1; }
                }
                gc_unpin(1);
                mino_val *call = mino_cons(S, head, call_args);
                if (call == NULL) return -1;
                return compile_expr(c, call, dst, tail);
            }
        } else { /* MINO_SET */
            if (form->as.set.len == 0) {
                int k = add_const(c, form);
                if (k < 0) return -1;
                emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
                return 0;
            }
            /* Constructor lane: lower `#{a b c}` to
             * `(hash-set a b c)`. Same shape as the map lowering;
             * iteration follows insertion order via key_order. */
            {
                mino_state *S = c->S;
                mino_val *head = mino_symbol(S, "clojure.core/hash-set");
                size_t      i;
                if (head == NULL) return -1;
                /* Root head across the mino_cons loop (same pattern as
                 * the vector lowering above). */
                gc_pin(head);
                mino_val *call_args = mino_nil(S);
                for (i = form->as.set.len; i > 0; i--) {
                    mino_val *e = vec_nth(form->as.set.key_order, i - 1);
                    call_args = mino_cons(S, e, call_args);
                    if (call_args == NULL) { gc_unpin(1); return -1; }
                }
                gc_unpin(1);
                mino_val *call = mino_cons(S, head, call_args);
                if (call == NULL) return -1;
                return compile_expr(c, call, dst, tail);
            }
        }
    }
    if (!mino_is_cons(form)) {
        c->S->bc_declines[BC_DECLINE_BAD_FORM]++;
        c->ok = 0;
        return -1;
    }

    /* If the call head resolves to a macro at compile time, decline.
     * The full resolution cascade (lexical -> defining-ns env, aliases
     * for qualified heads) is required; a lexical-only check misses
     * macros that live in the ns env, which is where most macros sit. */
    if (head_resolves_to_macro(c, form->as.cons.car)) {
        c->S->bc_declines[BC_DECLINE_MACRO]++;
        c->ok = 0;
        return -1;
    }

    mino_val *head = form->as.cons.car;

    if (head != NULL && mino_type_of(head) == MINO_SYMBOL
        && find_local(c, head->as.s.data) < 0) {
        const char *name = head->as.s.data;
        /* Accept the clojure.core/-qualified spelling of a macro-family
         * form (e.g. (clojure.core/let ...)) by matching on the bare
         * suffix, so BC-compiled code dispatches it like the tree-walker
         * and JIT/interpreter parity holds for syntax-quoted expansions. */
        if (strncmp(name, "clojure.core/", 13) == 0
            && is_core_macro_form(name + 13)) {
            name = name + 13;
        }
        if (strcmp(name, "if") == 0)   return compile_if(c, form, dst, tail);
        if (strcmp(name, "do") == 0)   return compile_do(c, form, dst, tail);
        if (strcmp(name, "let") == 0
            || strcmp(name, "let*") == 0) return compile_let(c, form, dst, tail);
        if (strcmp(name, "quote") == 0) return compile_quote(c, form, dst, tail);
        if (strcmp(name, "def") == 0)   return compile_def(c, form, dst, tail);
        if (strcmp(name, "fn") == 0
            || strcmp(name, "fn*") == 0) return compile_fn_literal(c, form, dst, tail);
        if (strcmp(name, "loop") == 0
            || strcmp(name, "loop*") == 0) return compile_loop(c, form, dst, tail);
        if (strcmp(name, "recur") == 0) return compile_recur(c, form, dst, tail);
        if (strcmp(name, "lazy-seq") == 0) return compile_lazy_seq(c, form, dst, tail);
        if (strcmp(name, "try") == 0)   return compile_try(c, form, dst, tail);
        if (strcmp(name, "throw") == 0) return compile_throw(c, form, dst, tail);
        if (strcmp(name, "binding") == 0) return compile_binding(c, form, dst, tail);
        if (strcmp(name, "when") == 0)  return compile_when(c, form, dst, tail);
        if (strcmp(name, "and") == 0)   return compile_and(c, form, dst, tail);
        if (strcmp(name, "or") == 0)    return compile_or(c, form, dst, tail);

        /* Other special forms have no compile-time handler yet; decline
         * so the tree-walker picks them up rather than emitting a
         * regular call that would fail at runtime. */
        if (is_special_form_name(name)) {
            c->S->bc_declines[BC_DECLINE_SPECIAL_FORM]++;
            c->ok = 0;
            return -1;
        }
    }
    return compile_call_impl(c, form, dst, tail);
}

static int compile_body(compiler_t *c, mino_val *body, int dst, int tail)
{
    if (!mino_is_cons(body)) {
        /* Empty body: result is nil. */
        int k = add_const(c, mino_nil(c->S));
        if (k < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
        return 0;
    }
    int saved_next = c->next_reg;
    while (mino_is_cons(body)) {
        mino_val *expr = body->as.cons.car;
        mino_val *next = body->as.cons.cdr;
        int is_last = !mino_is_cons(next);
        if (is_last) {
            /* Last expression carries the body's tail flag. compile_expr
             * propagates it into control forms (if/do/let/loop) and into
             * compile_call_impl, which emits OP_TAILCALL when tail is
             * set so the trampoline keeps the C stack flat. */
            if (compile_expr(c, expr, dst, tail) < 0) return -1;
        } else {
            int t = alloc_reg(c);
            if (t < 0) return -1;
            if (compile_expr(c, expr, t, 0) < 0) return -1;
            c->next_reg = saved_next;
        }
        body = next;
    }
    return 0;
}

/* ------------------------------------------------------------------- */
/* Compile-fn entry                                                    */
/* ------------------------------------------------------------------- */

/* Params must be a vector. Each entry is a plain symbol; we also
 * accept a trailing `& rest` pair, in which case the rest binding
 * collects overflow args into a list at call time. No destructure
 * for this cycle. out_n returns the fixed-param count; out_rest is
 * set to 1 iff the last binding is the rest param. */
static int params_simple_plain(mino_val *params, int *out_n, int *out_rest)
{
    *out_rest = 0;
    if (params == NULL) return 0;
    if (mino_type_of(params) != MINO_VECTOR) return 0;
    size_t n = params->as.vec.len;
    if (n > BC_MAX_REGS) return 0;
    for (size_t i = 0; i < n; i++) {
        mino_val *p = vec_nth(params, i);
        if (p == NULL || mino_type_of(p) != MINO_SYMBOL) return 0;
        const char *d = p->as.s.data;
        /* A bare & marks the rest separator. The slot right after it
         * is the rest-binding name (must be a plain symbol; we don't
         * yet accept destructure inside the rest slot). The & itself
         * occupies no register. */
        if (strcmp(d, "&") == 0) {
            if (i != n - 2) return 0;     /* must be second-to-last */
            mino_val *rest_sym = vec_nth(params, n - 1);
            if (rest_sym == NULL || mino_type_of(rest_sym) != MINO_SYMBOL) return 0;
            const char *rd = rest_sym->as.s.data;
            if (rd[0] == '&') return 0;
            if (strchr(rd, '.') != NULL) return 0;
            *out_n    = (int)i;            /* fixed count before & */
            *out_rest = 1;
            return 1;
        }
        if (d[0] == '&') return 0;
        if (strchr(d, '.') != NULL) return 0;
    }
    *out_n = (int)n;
    return 1;
}

/* Walk the params vector and return 1 iff every fixed-arity slot is
 * a plain MINO_SYMBOL (and any trailing rest slot is too). Used to
 * decide whether compile_clause should rewrite the body with a
 * wrapping let to destructure non-plain params. */
static int params_all_plain(mino_val *params)
{
    if (params == NULL || mino_type_of(params) != MINO_VECTOR) return 0;
    size_t n = params->as.vec.len;
    for (size_t i = 0; i < n; i++) {
        mino_val *p = vec_nth(params, i);
        if (p == NULL) return 0;
        if (mino_type_of(p) != MINO_SYMBOL) return 0;
    }
    return 1;
}

/* Rewrite a destructuring params vector into a pair of:
 *   - a fresh plain-symbol params vector (gensyms in the non-plain
 *     slots, originals in the plain slots),
 *   - a wrapping let whose bindings extract each pattern from its
 *     matching gensym.
 *
 * Used so compile_clause can stay in its plain-symbol-only happy
 * path. Both & rest-as-destructure and pattern-on-rest decline so
 * the tree-walker handles those rare shapes. Returns 1 with the
 * rewritten params / body installed via the out params on success,
 * 0 on decline. */
static int rewrite_destructure_params(compiler_t *c, mino_val *params,
                                       mino_val *body,
                                       mino_val **out_params,
                                       mino_val **out_body)
{
    size_t n = params->as.vec.len;
    /* Find & to demarcate fixed args from the rest. & must be the
     * second-to-last entry and its successor must be a plain symbol
     * (matches the params_simple_plain contract). */
    size_t amp_pos = SIZE_MAX;
    for (size_t i = 0; i < n; i++) {
        mino_val *p = vec_nth(params, i);
        if (p != NULL && mino_type_of(p) == MINO_SYMBOL
            && strcmp(p->as.s.data, "&") == 0) {
            if (i != n - 2) return 0;
            amp_pos = i;
            break;
        }
    }
    if (amp_pos != SIZE_MAX) {
        /* Require the rest binding itself to be a plain symbol: we
         * don't yet rewrite (& [a b]) into nested destructure. */
        mino_val *rest_sym = vec_nth(params, n - 1);
        if (rest_sym == NULL || mino_type_of(rest_sym) != MINO_SYMBOL) {
            return 0;
        }
    }

    /* Build the new params vector and the let-binding pairs in
     * parallel. */
    mino_state *S = c->S;  /* needed for gc_pin / gc_unpin macros */
    mino_val **new_params_buf = (mino_val **)gc_alloc_typed(
        c->S, GC_T_VALARR, n * sizeof(mino_val *));
    if (new_params_buf == NULL) return 0;
    /* Pin new_params_buf so subsequent allocating calls (mino_symbol_n,
     * mino_cons) cannot collect it while the loop fills it. */
    gc_pin((mino_val *)new_params_buf);
    /* binding-pairs is built in reverse, then a final pass writes
     * them in source order into the vector backing the let form. */
    mino_val *acc = mino_nil(c->S);
    size_t n_pairs = 0;
    for (size_t i = 0; i < n; i++) {
        mino_val *p = vec_nth(params, i);
        if (p != NULL && mino_type_of(p) == MINO_SYMBOL) {
            /* Plain slot -- pass through; & and the rest sym land here. */
            new_params_buf[i] = p;
            continue;
        }
        /* Destructure pattern -- mint a gensym, push (pattern gensym)
         * onto the let binding pairs. Same shape as destr_gensym in
         * src/eval/bindings.c so symbols collide with neither user
         * code nor the destructure expander's own intermediates. */
        char    gbuf[64];
        int     gused;
        gused = snprintf(gbuf, sizeof(gbuf), "p__%ld__auto__",
                         ++c->S->gensym_counter);
        if (gused < 0) { gc_unpin(1); return 0; }
        mino_val *gs = mino_symbol_n(c->S, gbuf, (size_t)gused);
        if (gs == NULL) { gc_unpin(1); return 0; }
        new_params_buf[i] = gs;
        acc = mino_cons(c->S, gs, acc);
        if (acc == NULL) { gc_unpin(1); return 0; }
        acc = mino_cons(c->S, p, acc);
        if (acc == NULL) { gc_unpin(1); return 0; }
        n_pairs += 2;
    }
    if (n_pairs == 0) {
        /* Nothing to rewrite; caller's plain path already handles this. */
        gc_unpin(1);
        return 0;
    }
    /* Collect the binding-pair list into a vector for the let form. */
    /* Pin new_params_buf again before allocating bind_buf so a GC
     * triggered by gc_alloc_typed cannot collect new_params_buf. */
    mino_val **bind_buf = (mino_val **)gc_alloc_typed(
        c->S, GC_T_VALARR, n_pairs * sizeof(mino_val *));
    if (bind_buf == NULL) { gc_unpin(1); return 0; }
    gc_pin((mino_val *)bind_buf);
    {
        mino_val *cur = acc;
        for (size_t i = 0; i < n_pairs; i++) {
            if (!mino_is_cons(cur)) { gc_unpin(2); return 0; }
            bind_buf[i] = cur->as.cons.car;
            cur = cur->as.cons.cdr;
        }
    }
    mino_val *bind_vec = mino_vector(c->S, bind_buf, n_pairs);
    gc_unpin(1);  /* bind_buf no longer needed after mino_vector */
    if (bind_vec == NULL) { gc_unpin(1); return 0; }
    /* Wrap body in (let [pat1 g1 pat2 g2 ...] body...). The body is
     * a cons list of forms; the let needs (let bind-vec body...) so
     * we cons the bind-vec onto body and prepend `let`. */
    mino_val *let_sym = mino_symbol(c->S, "let");
    if (let_sym == NULL) { gc_unpin(1); return 0; }
    mino_val *wrapped = mino_cons(c->S, bind_vec, body);
    if (wrapped == NULL) { gc_unpin(1); return 0; }
    wrapped = mino_cons(c->S, let_sym, wrapped);
    if (wrapped == NULL) { gc_unpin(1); return 0; }
    /* The new body for compile_clause is a single-element list whose
     * one form is the let. compile_body walks it as (let ...). */
    mino_val *new_body = mino_cons(c->S, wrapped, mino_nil(c->S));
    if (new_body == NULL) { gc_unpin(1); return 0; }
    mino_val *new_params = mino_vector(c->S, new_params_buf, n);
    gc_unpin(1);  /* new_params_buf no longer needed after mino_vector */
    if (new_params == NULL) return 0;
    *out_params = new_params;
    *out_body   = new_body;
    return 1;
}

/* Compile a single arity clause (params, body) into the current
 * compile context's code stream, starting at the current code_len.
 * On success, populates *clause and returns 0; the caller is
 * responsible for adding the clause to the bc record's clauses
 * array. Plain-symbol params plus an optional trailing `& rest`;
 * destructure params get rewritten into a wrapping let before we
 * reach the simple-plain check below. */
static int compile_clause(compiler_t *c, mino_val *params, mino_val *body,
                          mino_bc_clause_t *clause)
{
    /* Vector params with one or more destructure patterns get
     * rewritten into plain gensym params + a wrapping let.
     * Non-vector params (Lisp-style list form, NULL, etc.) and pure
     * plain-vector params skip the rewrite -- the latter is the hot
     * path, the former lets params_simple_plain decline below. */
    if (params != NULL && mino_type_of(params) == MINO_VECTOR
        && !params_all_plain(params)) {
        mino_val *new_params = NULL;
        mino_val *new_body   = NULL;
        if (!rewrite_destructure_params(c, params, body,
                                          &new_params, &new_body)) {
            return -1;
        }
        params = new_params;
        body   = new_body;
    }
    int n_params = 0;
    int has_rest = 0;
    if (!params_simple_plain(params, &n_params, &has_rest)) return -1;

    /* Reset locals/register allocator: each clause has its own bind
     * map and may reuse register slots from the previous clause. */
    c->n_locals = 0;
    int total_param_regs = n_params + (has_rest ? 1 : 0);
    c->next_reg = total_param_regs;
    if (total_param_regs > c->n_regs) c->n_regs = total_param_regs;

    for (int i = 0; i < n_params; i++) {
        mino_val *p = vec_nth(params, (size_t)i);
        if (!bind_local(c, p->as.s.data, i)) return -1;
    }
    if (has_rest) {
        mino_val *rest_sym = vec_nth(params,
            (size_t)(params->as.vec.len - 1));
        if (!bind_local(c, rest_sym->as.s.data, n_params)) return -1;
    }

    clause->n_params   = n_params;
    clause->has_rest   = has_rest;
    clause->entry_pc   = (int)c->bc->code_len;
    clause->params_vec = params;

    int ret_reg = alloc_reg(c);
    if (ret_reg < 0) return -1;
    if (compile_body(c, body, ret_reg, /*tail=*/1) < 0) return -1;
    if (!c->ok) return -1;
    /* Body ended with OP_MOVE ret_reg, X for a (let [x form] x)-style
     * tail expression? Fold the producer's dst onto ret_reg and drop
     * the MOVE before stamping the OP_RETURN. */
    peephole_tail_move(c);
    emit_abc(c, OP_RETURN, (unsigned)ret_reg, 0, 0);
    if (!c->ok) return -1;
    return 0;
}

int mino_bc_compile_fn(mino_state *S, mino_val *fn)
{
    if (fn == NULL || mino_type_of(fn) != MINO_FN) return MINO_BC_ERROR;
    if (fn->as.fn.bc != NULL) return MINO_BC_OK;  /* already compiled */

    /* Count clauses and walk them out into stack arrays so the rest
     * of the compile can index by clause without re-walking the cdr
     * spine. Single-arity (params is the params vector, body is the
     * body forms) becomes one clause; multi-arity (params == NULL,
     * body is a (clause clause ...) cons list of (params body...))
     * becomes N clauses. */
    int n_clauses;
    mino_val *clause_params_arr[32];
    mino_val *clause_body_arr[32];
    if (fn->as.fn.params != NULL) {
        n_clauses = 1;
        clause_params_arr[0] = fn->as.fn.params;
        clause_body_arr[0]   = fn->as.fn.body;
    } else {
        n_clauses = 0;
        mino_val *cur = fn->as.fn.body;
        while (mino_is_cons(cur)) {
            if (n_clauses >= 32) {
                fn->as.fn.bc = &mino_bc_declined;
                return MINO_BC_UNSUPPORTED;
            }
            mino_val *cl = cur->as.cons.car;
            if (!mino_is_cons(cl)) {
                fn->as.fn.bc = &mino_bc_declined;
                return MINO_BC_UNSUPPORTED;
            }
            clause_params_arr[n_clauses] = cl->as.cons.car;
            clause_body_arr[n_clauses]   = cl->as.cons.cdr;
            n_clauses++;
            cur = cur->as.cons.cdr;
        }
        if (n_clauses == 0) {
            fn->as.fn.bc = &mino_bc_declined;
            return MINO_BC_UNSUPPORTED;
        }
    }

    /* Materialize the bc record FIRST and attach to fn so the GC has a
     * root for the in-progress code/consts/clauses buffers throughout
     * the compile. If compilation declines later, we overwrite
     * fn->as.fn.bc with the sentinel and the partial buffers fall out
     * of reachability. */
    mino_bc_fn_t *bc = (mino_bc_fn_t *)gc_alloc_typed(
        S, GC_T_BC, sizeof(*bc));
    if (bc == NULL) { fn->as.fn.bc = &mino_bc_declined; return MINO_BC_UNSUPPORTED; }
    memset(bc, 0, sizeof(*bc));
    bc->n_clauses = n_clauses;
    /* Attach bc to fn immediately so the GC can reach it (and all
     * subsequent allocations reachable through bc) during the clauses
     * allocation below.  fn may already be OLD; the write barrier
     * ensures the next minor GC does not miss the YOUNG bc pointer. */
    gc_write_barrier(S, fn, NULL, bc);
    fn->as.fn.bc = bc;
    /* clauses array stored as GC_T_RAW since it contains POD data plus
     * a single pointer field (param_syms) into an already-rooted
     * vector. The GC trace pushes only the bc record and the params
     * vectors are reachable through fn->as.fn.params (single arity)
     * or fn->as.fn.body's clause spine (multi-arity). */
    mino_bc_clause_t *clauses = (mino_bc_clause_t *)gc_alloc_typed(
        S, GC_T_RAW, sizeof(*clauses) * (size_t)n_clauses);
    if (clauses == NULL) { fn->as.fn.bc = &mino_bc_declined; return MINO_BC_UNSUPPORTED; }
    memset(clauses, 0, sizeof(*clauses) * (size_t)n_clauses);
    gc_write_barrier(S, bc, NULL, clauses);
    bc->clauses = clauses;
    /* captures reflects any clause's body needing env capture. */
    {
        int caps = 0;
        for (int i = 0; i < n_clauses; i++) {
            if (contains_env_capture(clause_body_arr[i])) { caps = 1; break; }
        }
        bc->captures = caps;
    }

    compiler_t c;
    memset(&c, 0, sizeof(c));
    c.S           = S;
    c.env         = fn->as.fn.env;
    c.defining_ns = fn->as.fn.defining_ns;
    c.bc          = bc;
    c.ok          = 1;
    c.n_regs      = 0;

    /* Decline-bucket snapshot: if no specific reason ticks during this
     * fn's compile but it still declines, the residual gets attributed
     * to BC_DECLINE_OTHER at the decline label below. Sums every
     * specific reason in the [1..OTHER) range. */
    uint64_t decline_snapshot = 0;
    {
        int i;
        for (i = 1; i < BC_DECLINE_OTHER; i++) {
            decline_snapshot += S->bc_declines[i];
        }
    }

    for (int i = 0; i < n_clauses; i++) {
        if (compile_clause(&c, clause_params_arr[i], clause_body_arr[i],
                           &clauses[i]) < 0) {
            goto decline;
        }
    }

    /* Mirror the first clause into the top-level n_params/has_rest
     * fields. Old call sites that look at these directly (the single-
     * arity fast path in mino_bc_run before clause selection) keep
     * working unchanged. */
    bc->n_params = clauses[0].n_params;
    bc->has_rest = clauses[0].has_rest;
    bc->n_regs   = c.n_regs;
    /* Soundness tracker for any literal-arg fold that fired during this
     * compile (see find_pure_prim / compile_call_impl). apply_callable
     * compares compile_ic_gen against S->ns_vars.ic_gen on each call entry; a
     * mismatch means a var was redefined since the fold was computed,
     * so the cached fold-result might no longer be Clojure-correct. The
     * mismatch path drops fn->bc back to NULL and the next call
     * recompiles from source. */
    bc->has_folds      = c.has_folds;
    bc->has_try        = c.has_try;
    bc->compile_ic_gen = S->ns_vars.ic_gen;
    /* Tighten the source-map length: keep only the slots that match
     * actual emitted instructions. Over-allocated tail entries are
     * harmless but the len field is the only signal a lookup uses to
     * detect an out-of-range pc, so make it precise. */
    if (bc->source_map.len > bc->code_len) {
        bc->source_map.len = bc->code_len;
    }
    return MINO_BC_OK;

decline:
    /* If no specific decline bucket ticked during this compile, route
     * the bail to OTHER so the histogram still records the event. */
    {
        uint64_t now = 0;
        int      i;
        for (i = 1; i < BC_DECLINE_OTHER; i++) {
            now += S->bc_declines[i];
        }
        if (now == decline_snapshot) {
            S->bc_declines[BC_DECLINE_OTHER]++;
        }
    }
    fn->as.fn.bc = &mino_bc_declined;
    return MINO_BC_UNSUPPORTED;
}
