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
 * value. OP_SETGLOBAL bumps S->ic_gen so cached call sites invalidate.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mino.h"
#include "runtime/internal.h"
#include "eval/internal.h"
#include "eval/special_internal.h"  /* build_multi_arity_clauses */
#include "eval/bc/internal.h"
#include "collections/internal.h"   /* make_fn */

extern mino_val_t *mino_nil(mino_state_t *S);

/* Sentinel for failed compiles. apply_callable checks against this
 * pointer to skip the retry on subsequent calls. */
const mino_bc_fn_t mino_bc_declined = {0};

/* Bytecode-required mode. When non-zero, the tree-walker fallback at
 * apply_callable's bc-entry path is treated as a fatal error so VM
 * regressions surface loudly instead of degrading silently. Set from
 * the embedder (e.g., by reading the MINO_BC_REQUIRE env var at
 * startup). Defaults to zero in production builds. */
int mino_bc_require_flag = 0;

void mino_bc_check_require(mino_state_t *S, mino_val_t *fn)
{
    (void)S;
    if (!mino_bc_require_flag) return;
    if (fn == NULL || fn->type != MINO_FN) return;
    if (MINO_BC_RUNNABLE(fn)) return;
    /* Decline mode: report and abort. The fn's params/body are not
     * compilable yet; whoever flipped require_flag should run with
     * an expanded compiler. */
    fprintf(stderr, "MINO_BC_REQUIRE: fn declined by compiler\n");
    abort();
}

#define BC_MAX_LOCALS 256
#define BC_MAX_REGS   255            /* operand width fits in 8 bits */

typedef struct local {
    const char *name;   /* interned symbol data; pointer-stable */
    int         reg;
} bc_local_t;

/* The innermost active loop, if any. `entry_pc` is the bytecode offset
 * to jump back to on (recur ...); `bind_regs[i]` is the register slot
 * that binding `i` of (loop [b1 v1 ... bN vN]) lives in. n_bindings is
 * the arity recur must match. The compiler stacks these so a nested
 * loop's recur targets only the innermost one. */
typedef struct loop_target {
    int  entry_pc;
    int  bind_regs[BC_MAX_LOCALS];
    int  n_bindings;
} loop_target_t;

typedef struct compiler {
    mino_state_t      *S;
    mino_env_t        *env;          /* fn's captured env, for macroexpand1 */
    const char        *defining_ns;  /* fn->as.fn.defining_ns, for ns-env probes */
    mino_bc_fn_t      *bc;           /* attached to fn from the start so the
                                      * GC can find the in-progress code and
                                      * consts buffers through it */
    size_t             consts_cap;
    size_t             code_cap;
    bc_local_t         locals[BC_MAX_LOCALS];
    int                n_locals;
    int                n_regs;       /* high-water mark */
    int                next_reg;     /* next register to allocate */
    int                n_params;
    int                ok;            /* 0 means decline */
    loop_target_t     *loop;          /* innermost loop or NULL */
} compiler_t;

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
    mino_val_t **grown = (mino_val_t **)gc_alloc_typed(
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

static int add_const(compiler_t *c, mino_val_t *v)
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
    c->bc->code[c->bc->code_len++] = MK_ABC(op, a, b, cc);
}

static void emit_abx(compiler_t *c, mino_bc_op_t op,
                     unsigned a, unsigned bx)
{
    if (!ensure_code(c, c->bc->code_len + 1)) return;
    c->bc->code[c->bc->code_len++] = MK_ABx(op, a, bx);
}

static int emit_jmp_placeholder(compiler_t *c, mino_bc_op_t op, unsigned a)
{
    if (!ensure_code(c, c->bc->code_len + 1)) return -1;
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
    c->locals[c->n_locals].name = name;
    c->locals[c->n_locals].reg  = reg;
    c->n_locals++;
    return 1;
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
    case OP_CLOSURE:
    case OP_MAKE_LAZY:
    case OP_ADD_II: case OP_SUB_II: case OP_MUL_II:
    case OP_LT_II:  case OP_LE_II:  case OP_GT_II:
    case OP_GE_II:  case OP_EQ_II:
    case OP_INC_I:  case OP_DEC_I:  case OP_ZERO_INT_P:
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
        long target = (long)pc + 1 + off;
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

static int is_self_evaluating(const mino_val_t *v)
{
    if (v == NULL) return 1;
    switch (v->type) {
    case MINO_NIL: case MINO_BOOL: case MINO_INT: case MINO_FLOAT:
    case MINO_FLOAT32: case MINO_STRING: case MINO_KEYWORD: case MINO_CHAR:
        return 1;
    default:
        return 0;
    }
}

static int sym_is(const mino_val_t *v, const char *name)
{
    if (v == NULL || v->type != MINO_SYMBOL) return 0;
    return strcmp(v->as.s.data, name) == 0;
}

/* Names of every special form mino's eval recognizes, plus the syntactic
 * sub-forms that only appear as heads inside their parent (catch,
 * finally) but would otherwise look like regular calls. Kept in sync
 * with k_special_forms in src/eval/special_registry.c. */
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
    return 0;
}

/* Forward declarations. `tail` is 1 when the result of this expression
 * is the fn's return value; control forms (if/do/let) propagate it to
 * their inner tail position so a nested call there emits OP_TAILCALL
 * and goes through the trampoline (constant C stack). */
static int compile_expr(compiler_t *c, mino_val_t *form, int dst, int tail);
static int compile_body(compiler_t *c, mino_val_t *body, int dst, int tail);

/* Pre-scan: does the form tree contain a form that captures the
 * current lexical env -- an inner (fn ...) literal or a (lazy-seq ...)
 * that will realise its body in the env later? When yes, the enclosing
 * fn sets bc->captures = 1, which forces env-binding of params and
 * bracketing of let scopes with OP_PUSH_ENV / OP_POP_ENV so any
 * captured shape sees the right lexical chain. Quote-forms are data,
 * not code, and don't count. */
static int contains_env_capture(mino_val_t *form)
{
    if (form == NULL) return 0;
    if (form->type == MINO_VECTOR) {
        for (size_t i = 0; i < form->as.vec.len; i++) {
            if (contains_env_capture(vec_nth(form, i))) return 1;
        }
        return 0;
    }
    if (form->type == MINO_MAP || form->type == MINO_SET) {
        return 0;
    }
    if (!mino_is_cons(form)) return 0;
    mino_val_t *head = form->as.cons.car;
    if (head != NULL && head->type == MINO_SYMBOL) {
        if (sym_is(head, "fn") || sym_is(head, "fn*")) return 1;
        if (sym_is(head, "lazy-seq")) return 1;
        if (sym_is(head, "quote") || sym_is(head, "quote*")) return 0;
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

static int compile_if(compiler_t *c, mino_val_t *form, int dst, int tail)
{
    /* (if cond then) or (if cond then else). When `tail` is set, the
     * value of the chosen branch is the fn's result, so each branch
     * inherits the tail position from us. */
    mino_val_t *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }
    mino_val_t *cond_form = args->as.cons.car;
    mino_val_t *rest1 = args->as.cons.cdr;
    if (!mino_is_cons(rest1)) { c->ok = 0; return -1; }
    mino_val_t *then_form = rest1->as.cons.car;
    mino_val_t *rest2 = rest1->as.cons.cdr;
    mino_val_t *else_form = NULL;
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

static int compile_do(compiler_t *c, mino_val_t *form, int dst, int tail)
{
    /* (do e1 e2 ... eN) -> eval each, result is eN. Empty (do) -> nil.
     * Tail position propagates to the last expression. */
    mino_val_t *body = form->as.cons.cdr;
    return compile_body(c, body, dst, tail);
}

static int compile_let(compiler_t *c, mino_val_t *form, int dst, int tail)
{
    /* (let [b1 v1 b2 v2 ...] body...) -- plain-symbol bindings only.
     * Tail position propagates to the body's last expression. */
    mino_val_t *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }
    mino_val_t *bindings = args->as.cons.car;
    mino_val_t *body     = args->as.cons.cdr;
    if (bindings == NULL || bindings->type != MINO_VECTOR) {
        c->ok = 0; return -1;
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
        mino_val_t *name_form = vec_nth(bindings, i);
        mino_val_t *val_form  = vec_nth(bindings, i + 1);
        if (name_form == NULL || name_form->type != MINO_SYMBOL) {
            c->ok = 0; goto out;
        }
        int reg = alloc_reg(c);
        if (reg < 0) goto out;
        if (compile_expr(c, val_form, reg, 0) < 0) goto out;
        if (!bind_local(c, name_form->as.s.data, reg)) goto out;
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

static int compile_loop(compiler_t *c, mino_val_t *form, int dst, int tail)
{
    /* (loop [b1 v1 ...] body) -- like let, but installs a recur target
     * at the loop entry pc and tracks the binding registers so (recur
     * v1' ...) can rebind and jump. Plain-symbol bindings only.
     * When the loop is in tail position, the body's last expression
     * (when it isn't a recur) is the fn's result; propagate tail. */
    mino_val_t *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }
    mino_val_t *bindings = args->as.cons.car;
    mino_val_t *body     = args->as.cons.cdr;
    if (bindings == NULL || bindings->type != MINO_VECTOR) {
        c->ok = 0; return -1;
    }
    size_t blen = bindings->as.vec.len;
    if ((blen & 1) != 0) { c->ok = 0; return -1; }
    int n_bindings = (int)(blen / 2);
    if (n_bindings >= BC_MAX_LOCALS) { c->ok = 0; return -1; }

    int saved_n_locals = c->n_locals;
    int saved_next_reg = c->next_reg;
    loop_target_t *saved_loop = c->loop;
    loop_target_t this_loop;
    this_loop.n_bindings = n_bindings;
    int pushed_env = 0;
    if (c->bc->captures) {
        emit_abc(c, OP_PUSH_ENV, 0, 0, 0);
        pushed_env = 1;
    }

    /* Compile initial binding values into successive registers and
     * record those slots as the recur-rebind targets. */
    for (int i = 0; i < n_bindings; i++) {
        mino_val_t *name_form = vec_nth(bindings, (size_t)(i * 2));
        mino_val_t *val_form  = vec_nth(bindings, (size_t)(i * 2 + 1));
        if (name_form == NULL || name_form->type != MINO_SYMBOL) {
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
            emit_abx(c, OP_ENV_BIND, (unsigned)reg, (unsigned)k);
        }
    }

    /* Install recur target only after the initial bindings are in
     * place. A recur expression in tail position rebinds the loop
     * slots and jumps to entry_pc. */
    this_loop.entry_pc = (int)c->bc->code_len;
    c->loop = &this_loop;

    if (compile_body(c, body, dst, tail) < 0) goto out;

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

static int compile_recur(compiler_t *c, mino_val_t *form, int dst, int tail)
{
    (void)dst; (void)tail;  /* recur never produces a value -- it jumps. */
    if (c->loop == NULL) { c->ok = 0; return -1; }
    /* Walk the recur args. They must match the loop's binding count.
     * Evaluate each into a fresh temp register so a recur arg can
     * reference a loop binding without being clobbered by an earlier
     * MOVE; only after all temps are filled do we copy them onto the
     * loop binding slots. */
    int n = c->loop->n_bindings;
    mino_val_t *cur = form->as.cons.cdr;
    int saved_next = c->next_reg;
    int temp_base  = c->next_reg;
    for (int i = 0; i < n; i++) {
        if (!mino_is_cons(cur)) { c->ok = 0; return -1; }
        int t = alloc_reg(c);
        if (t < 0) return -1;
        if (compile_expr(c, cur->as.cons.car, t, 0) < 0) return -1;
        cur = cur->as.cons.cdr;
    }
    if (mino_is_cons(cur)) { c->ok = 0; return -1; }  /* too many */
    for (int i = 0; i < n; i++) {
        emit_abc(c, OP_MOVE, (unsigned)c->loop->bind_regs[i],
                 (unsigned)(temp_base + i), 0);
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

static int compile_lazy_seq(compiler_t *c, mino_val_t *form, int dst, int tail)
{
    (void)tail;
    /* (lazy-seq body...) -- stash the body forms (a list of AST
     * expressions) in the constant pool and have the runtime build a
     * MINO_LAZY whose body is that list and whose env is the live
     * lexical chain. Realisation runs the body via the tree-walker in
     * the captured env, so the enclosing fn must already publish its
     * locals into the env (bc->captures = 1, set automatically by the
     * pre-scan since `lazy-seq` is one of the env-capturing shapes). */
    mino_val_t *body = form->as.cons.cdr;
    int k = add_const(c, body);
    if (k < 0) return -1;
    emit_abx(c, OP_MAKE_LAZY, (unsigned)dst, (unsigned)k);
    return 0;
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
static int compile_fn_literal(compiler_t *c, mino_val_t *form, int dst, int tail)
{
    (void)tail;
    mino_val_t *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }

    /* Optional name: (fn name [params] body...). The name lets the
     * body recurse by referring to the just-built closure. eval_fn
     * does this by wrapping a fresh env_child whose only binding is
     * name -> closure; we reproduce the same shape by emitting an
     * OP_PUSH_ENV before the OP_CLOSURE and an OP_ENV_BIND right
     * after, then OP_POP_ENV to restore the outer's env. The closure
     * captures the pushed env, so the binding is visible to its body
     * via the env chain. */
    mino_val_t *fn_name = NULL;
    mino_val_t *first   = args->as.cons.car;
    mino_val_t *rest    = args->as.cons.cdr;
    if (first != NULL && first->type == MINO_SYMBOL
        && mino_is_cons(rest)) {
        mino_val_t *after = rest->as.cons.car;
        if (after != NULL
            && (mino_is_cons(after) || mino_is_nil(after)
                || after->type == MINO_VECTOR)) {
            fn_name = first;
            args    = rest;
        }
    }

    mino_val_t *params = args->as.cons.car;
    mino_val_t *body   = args->as.cons.cdr;

    /* Multi-arity detection. eval_fn recognises a fn whose first arg
     * is itself a (params-vec body...) clause -- the canonical
     * `(fn ([x] ...) ([x y] ...))` shape -- and rewrites params/body
     * via build_multi_arity_clauses. The bc compiler declines multi-
     * arity for now, but we still have to normalise the template's
     * shape so the tree-walker fallback at apply_callable time sees
     * what eval_fn would have produced. */
    if (mino_is_cons(params) && params->as.cons.car != NULL
        && (params->as.cons.car->type == MINO_VECTOR
            || mino_is_cons(params->as.cons.car)
            || mino_is_nil(params->as.cons.car))
        && params->as.cons.car->type == MINO_VECTOR) {
        mino_val_t *clauses = build_multi_arity_clauses(
            c->S, form, args, "MSY002", "fn");
        if (clauses == NULL) { c->ok = 0; return -1; }
        params = NULL;
        body   = clauses;
    }

    /* Build a template MINO_FN. The env field gets nil here; OP_CLOSURE
     * supplies the real env at each invocation. */
    mino_val_t *tmpl = make_fn(c->S, params, body, NULL);
    if (tmpl == NULL) { c->ok = 0; return -1; }
    tmpl->as.fn.defining_ns = c->defining_ns;
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

static int compile_quote(compiler_t *c, mino_val_t *form, int dst, int tail)
{
    (void)tail;
    /* (quote x) -- x is itself the value. */
    mino_val_t *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }
    if (mino_is_cons(args->as.cons.cdr)) { c->ok = 0; return -1; }
    mino_val_t *v = args->as.cons.car;
    int k = add_const(c, v);
    if (k < 0) return -1;
    emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
    return 0;
}

static int compile_def(compiler_t *c, mino_val_t *form, int dst, int tail)
{
    (void)tail;
    /* (def name) or (def name expr) -- plain form only, no metadata. */
    mino_val_t *args = form->as.cons.cdr;
    if (!mino_is_cons(args)) { c->ok = 0; return -1; }
    mino_val_t *name_form = args->as.cons.car;
    mino_val_t *rest = args->as.cons.cdr;
    if (name_form == NULL || name_form->type != MINO_SYMBOL) {
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
        mino_val_t *val_form = rest->as.cons.car;
        if (mino_is_cons(rest->as.cons.cdr)) { c->ok = 0; return -1; }
        if (compile_expr(c, val_form, val_reg, 0) < 0) return -1;
    } else {
        /* (def name) -- value is nil. */
        int nk = add_const(c, mino_nil(c->S));
        if (nk < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)val_reg, (unsigned)nk);
    }
    emit_abx(c, OP_SETGLOBAL, (unsigned)val_reg, (unsigned)k);
    emit_abc(c, OP_MOVE, (unsigned)dst, (unsigned)val_reg, 0);
    c->next_reg = saved_next;
    return 0;
}

/* Walk the runtime's resolution cascade in probe mode: lexical env, then
 * the fn's defining-ns env, returning the bound value or NULL. Mirrors
 * eval_symbol's unqualified path but uses the fn's defining_ns rather
 * than S->current_ns -- compile happens lazily and S->current_ns at
 * that time is the caller's, not the fn's.
 *
 * For qualified ns/name symbols, walks alias_resolve -> var_find ->
 * ns_env_lookup the same way eval_qualified_symbol does, scoping the
 * alias lookup to the fn's defining_ns. */
static mino_val_t *probe_head_value(compiler_t *c, mino_val_t *head)
{
    if (head == NULL || head->type != MINO_SYMBOL) return NULL;
    const char *data = head->as.s.data;
    size_t      n    = head->as.s.len;
    const char *slash = (n > 1) ? memchr(data, '/', n) : NULL;
    mino_state_t *S = c->S;
    const char *owning = c->defining_ns != NULL ? c->defining_ns : "user";

    if (slash != NULL) {
        /* Qualified: ns/name. Try literal env binding first (rare:
         * something like (host/new ...) in lexical scope). */
        mino_val_t *v = mino_env_get_sym(c->env, head);
        if (v != NULL) return v;

        char  ns_buf[256];
        size_t ns_len = (size_t)(slash - data);
        if (ns_len >= sizeof(ns_buf)) return NULL;
        memcpy(ns_buf, data, ns_len);
        ns_buf[ns_len] = '\0';
        const char *sym_name = slash + 1;

        /* Resolve alias against the fn's defining_ns (the runtime does
         * the equivalent against S->current_ns, which during dispatch
         * is the defining_ns). */
        const char *resolved_ns = ns_buf;
        for (size_t i = 0; i < S->ns_alias_len; i++) {
            if (S->ns_aliases[i].owning_ns != NULL
                && strcmp(S->ns_aliases[i].owning_ns, owning) == 0
                && strcmp(S->ns_aliases[i].alias, ns_buf) == 0) {
                resolved_ns = S->ns_aliases[i].full_name;
                break;
            }
        }

        mino_val_t *var = var_find(S, resolved_ns, sym_name);
        if (var != NULL && var->type == MINO_VAR) {
            return var->as.var.root;
        }
        mino_env_t *target_env = ns_env_lookup(S, resolved_ns);
        if (target_env != NULL) {
            env_binding_t *b = env_find_here(target_env, sym_name);
            if (b != NULL) return b->val;
        }
        return NULL;
    }

    /* Unqualified: lexical -> defining_ns env. */
    mino_val_t *v = mino_env_get_sym(c->env, head);
    if (v != NULL) return v;
    if (c->defining_ns != NULL) {
        mino_env_t *ns_env = ns_env_lookup(S, c->defining_ns);
        if (ns_env != NULL) {
            v = mino_env_get_sym(ns_env, head);
            if (v != NULL) return v;
        }
    }
    return NULL;
}

/* Macro-detection probe: does `head` resolve to a MINO_MACRO under the
 * same cascade the runtime would use at dispatch time? Used to gate
 * OP_CALL / OP_TAILCALL emission so macros stay on the tree-walker
 * path (their args are forms, not evaluated values). */
static int head_resolves_to_macro(compiler_t *c, mino_val_t *head)
{
    if (head == NULL || head->type != MINO_SYMBOL) return 0;
    if (find_local(c, head->as.s.data) >= 0) return 0;
    mino_val_t *v = probe_head_value(c, head);
    return v != NULL && v->type == MINO_MACRO;
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
static int compile_operand_inplace(compiler_t *c, mino_val_t *form)
{
    if (form != NULL && form->type == MINO_SYMBOL) {
        int local = find_local(c, form->as.s.data);
        if (local >= 0) return local;
    }
    int r = alloc_reg(c);
    if (r < 0) return -1;
    if (compile_expr(c, form, r, 0) < 0) return -1;
    return r;
}

/* Map a binary arith / compare prim name to its BINOP_* subop. Returns
 * -1 when the name isn't one of the speculative fast lanes. */
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
    return -1;
}

/* Map a unary numeric prim name to its UNOP_* subop. */
static int unop_subop_for_name(const char *name)
{
    if (strcmp(name, "inc")   == 0) return UNOP_INC;
    if (strcmp(name, "dec")   == 0) return UNOP_DEC;
    if (strcmp(name, "zero?") == 0) return UNOP_ZERO_P;
    return -1;
}

static int compile_call_impl(compiler_t *c, mino_val_t *form, int dst, int tail)
{
    mino_val_t *head = form->as.cons.car;
    if (head_resolves_to_macro(c, head)) { c->ok = 0; return -1; }

    /* Speculative fast lane for the unary and binary arith / compare
     * calls. The head must be the named prim (non-local, non-macro);
     * we emit the per-op specialised opcode directly. Speculation
     * fires in tail position too: the result is a value, not a tail
     * call, so writing to dst and falling through to OP_RETURN gives
     * the same observable behaviour as OP_TAILCALL through the
     * trampoline -- minus the dispatch overhead. */
    if (head != NULL && head->type == MINO_SYMBOL
        && find_local(c, head->as.s.data) < 0) {
        int usubop = unop_subop_for_name(head->as.s.data);
        if (usubop >= 0) {
            int argc_check = 0;
            mino_val_t *p = form->as.cons.cdr;
            while (mino_is_cons(p)) { argc_check++; p = p->as.cons.cdr; }
            if (argc_check == 1) {
                int saved_next = c->next_reg;
                mino_val_t *a1 = form->as.cons.cdr->as.cons.car;
                int src_reg = compile_operand_inplace(c, a1);
                if (src_reg < 0) return -1;
                if (dst > 0xFF || src_reg > 0xFF) {
                    c->ok = 0; return -1;
                }
                static const mino_bc_op_t unop_op[UNOP__COUNT] = {
                    OP_INC_I, OP_DEC_I, OP_ZERO_INT_P,
                };
                emit_abc(c, unop_op[usubop], (unsigned)dst,
                         (unsigned)src_reg, 0);
                c->next_reg = saved_next;
                return 0;
            }
        }
        int subop = binop_subop_for_name(head->as.s.data);
        if (subop >= 0) {
            int argc_check = 0;
            mino_val_t *p = form->as.cons.cdr;
            while (mino_is_cons(p)) { argc_check++; p = p->as.cons.cdr; }
            if (argc_check == 2) {
                int saved_next = c->next_reg;
                mino_val_t *a1 = form->as.cons.cdr->as.cons.car;
                mino_val_t *a2 = form->as.cons.cdr->as.cons.cdr->as.cons.car;
                int lhs_reg = compile_operand_inplace(c, a1);
                if (lhs_reg < 0) return -1;
                int rhs_reg = compile_operand_inplace(c, a2);
                if (rhs_reg < 0) return -1;
                if (dst > 0xFF || lhs_reg > 0xFF || rhs_reg > 0xFF) {
                    c->ok = 0; return -1;
                }
                /* Per-op specialized opcode. ABC form: A=dst, B=lhs,
                 * C=rhs. The B/C operands are independent reg indices
                 * so compile_operand_inplace can return locals'
                 * registers directly, sparing the alloc + OP_MOVE pair
                 * for the common (op local local) shape. */
                static const mino_bc_op_t binop_op[BINOP__COUNT] = {
                    OP_ADD_II, OP_SUB_II, OP_MUL_II,
                    OP_LT_II,  OP_LE_II,  OP_GT_II,
                    OP_GE_II,  OP_EQ_II,
                };
                emit_abc(c, binop_op[subop], (unsigned)dst,
                         (unsigned)lhs_reg, (unsigned)rhs_reg);
                c->next_reg = saved_next;
                return 0;
            }
        }
    }

    /* Walk the cdr, counting args while validating each is a cons. */
    mino_val_t *cur = form->as.cons.cdr;
    int argc = 0;
    while (mino_is_cons(cur)) {
        argc++;
        cur = cur->as.cons.cdr;
    }
    if (cur != NULL && cur->type != MINO_NIL && cur->type != MINO_EMPTY_LIST) {
        /* Improper list form -- not a regular call. */
        c->ok = 0; return -1;
    }
    if (argc > 0xFF) { c->ok = 0; return -1; }   /* B operand is 8 bits */

    /* Allocate consecutive regs: fn slot then argc arg slots. The OP_CALL
     * ABI requires args at A+1..A+argc. */
    int saved_next = c->next_reg;
    int fn_reg = alloc_reg(c);
    if (fn_reg < 0) return -1;
    int arg_base = c->next_reg;
    for (int i = 0; i < argc; i++) {
        if (alloc_reg(c) < 0) return -1;
    }

    /* Compile the head into fn_reg. */
    if (compile_expr(c, head, fn_reg, 0) < 0) return -1;

    /* Compile each argument into its slot. */
    cur = form->as.cons.cdr;
    for (int i = 0; i < argc; i++) {
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


/* ------------------------------------------------------------------- */
/* Form dispatch                                                       */
/* ------------------------------------------------------------------- */

static int compile_symbol_ref(compiler_t *c, mino_val_t *sym, int dst)
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
    int k = add_const(c, sym);
    if (k < 0) return -1;
    emit_abx(c, OP_GETGLOBAL, (unsigned)dst, (unsigned)k);
    return 0;
}

static int compile_expr(compiler_t *c, mino_val_t *form, int dst, int tail)
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
    if (form->type == MINO_SYMBOL) {
        return compile_symbol_ref(c, form, dst);
    }
    if (form->type == MINO_EMPTY_LIST) {
        int k = add_const(c, form);
        if (k < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
        return 0;
    }
    if (form->type == MINO_VECTOR || form->type == MINO_MAP
        || form->type == MINO_SET) {
        /* Vector literal with only self-evaluating elements is a
         * constant: stash the whole vector in the pool and emit a
         * single OP_LOAD_K. Vectors with non-const elements and
         * non-empty maps / sets still decline (their tree-walker
         * lowering handles element evaluation). */
        if (form->type == MINO_VECTOR) {
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
        }
        c->ok = 0;
        return -1;
    }
    if (!mino_is_cons(form)) {
        c->ok = 0;
        return -1;
    }

    /* If the call head resolves to a macro at compile time, decline.
     * The full resolution cascade (lexical -> defining-ns env, aliases
     * for qualified heads) is required; a lexical-only check misses
     * macros that live in the ns env, which is where most macros sit. */
    if (head_resolves_to_macro(c, form->as.cons.car)) {
        c->ok = 0;
        return -1;
    }

    mino_val_t *head = form->as.cons.car;

    if (head != NULL && head->type == MINO_SYMBOL
        && find_local(c, head->as.s.data) < 0) {
        const char *name = head->as.s.data;
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

        /* Other special forms have no compile-time handler yet; decline
         * so the tree-walker picks them up rather than emitting a
         * regular call that would fail at runtime. */
        if (is_special_form_name(name)) {
            c->ok = 0;
            return -1;
        }
    }
    return compile_call_impl(c, form, dst, tail);
}

static int compile_body(compiler_t *c, mino_val_t *body, int dst, int tail)
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
        mino_val_t *expr = body->as.cons.car;
        mino_val_t *next = body->as.cons.cdr;
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
static int params_simple_plain(mino_val_t *params, int *out_n, int *out_rest)
{
    *out_rest = 0;
    if (params == NULL) return 0;
    if (params->type != MINO_VECTOR) return 0;
    size_t n = params->as.vec.len;
    if (n > BC_MAX_REGS) return 0;
    for (size_t i = 0; i < n; i++) {
        mino_val_t *p = vec_nth(params, i);
        if (p == NULL || p->type != MINO_SYMBOL) return 0;
        const char *d = p->as.s.data;
        /* A bare & marks the rest separator. The slot right after it
         * is the rest-binding name (must be a plain symbol; we don't
         * yet accept destructure inside the rest slot). The & itself
         * occupies no register. */
        if (strcmp(d, "&") == 0) {
            if (i != n - 2) return 0;     /* must be second-to-last */
            mino_val_t *rest_sym = vec_nth(params, n - 1);
            if (rest_sym == NULL || rest_sym->type != MINO_SYMBOL) return 0;
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

/* Compile a single arity clause (params, body) into the current
 * compile context's code stream, starting at the current code_len.
 * On success, populates *clause and returns 0; the caller is
 * responsible for adding the clause to the bc record's clauses
 * array. Plain-symbol params plus an optional trailing `& rest`. */
static int compile_clause(compiler_t *c, mino_val_t *params, mino_val_t *body,
                          mino_bc_clause_t *clause)
{
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
        mino_val_t *p = vec_nth(params, (size_t)i);
        if (!bind_local(c, p->as.s.data, i)) return -1;
    }
    if (has_rest) {
        mino_val_t *rest_sym = vec_nth(params,
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

int mino_bc_compile_fn(mino_state_t *S, mino_val_t *fn)
{
    if (fn == NULL || fn->type != MINO_FN) return MINO_BC_ERROR;
    if (fn->as.fn.bc != NULL) return MINO_BC_OK;  /* already compiled */

    /* Count clauses and walk them out into stack arrays so the rest
     * of the compile can index by clause without re-walking the cdr
     * spine. Single-arity (params is the params vector, body is the
     * body forms) becomes one clause; multi-arity (params == NULL,
     * body is a (clause clause ...) cons list of (params body...))
     * becomes N clauses. */
    int n_clauses;
    mino_val_t *clause_params_arr[32];
    mino_val_t *clause_body_arr[32];
    if (fn->as.fn.params != NULL) {
        n_clauses = 1;
        clause_params_arr[0] = fn->as.fn.params;
        clause_body_arr[0]   = fn->as.fn.body;
    } else {
        n_clauses = 0;
        mino_val_t *cur = fn->as.fn.body;
        while (mino_is_cons(cur)) {
            if (n_clauses >= 32) {
                fn->as.fn.bc = &mino_bc_declined;
                return MINO_BC_UNSUPPORTED;
            }
            mino_val_t *cl = cur->as.cons.car;
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
        S, GC_T_RAW, sizeof(*bc));
    if (bc == NULL) { fn->as.fn.bc = &mino_bc_declined; return MINO_BC_UNSUPPORTED; }
    memset(bc, 0, sizeof(*bc));
    bc->n_clauses = n_clauses;
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
    /* Write barrier: fn may already be OLD (top-level defns survive
     * many minor cycles before the first call triggers compile). The
     * just-allocated bc is YOUNG. Without the barrier, the next minor
     * misses bc and frees it from under the field. */
    gc_write_barrier(S, fn, NULL, bc);
    fn->as.fn.bc = bc;

    compiler_t c;
    memset(&c, 0, sizeof(c));
    c.S           = S;
    c.env         = fn->as.fn.env;
    c.defining_ns = fn->as.fn.defining_ns;
    c.bc          = bc;
    c.ok          = 1;
    c.n_regs      = 0;

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
    return MINO_BC_OK;

decline:
    fn->as.fn.bc = &mino_bc_declined;
    return MINO_BC_UNSUPPORTED;
}
