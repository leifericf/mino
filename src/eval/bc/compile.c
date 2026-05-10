/*
 * eval/bc/compile.c -- AST-to-bytecode compiler.
 *
 * Phase 1 coverage:
 *   - literals (nil / bool / int / float / string / keyword / char)
 *   - bare symbol refs (locals via register, globals via OP_GETGLOBAL)
 *   - (if c t e), (if c t)
 *   - (do e1 e2 ...)
 *   - (let [b v ...] body...) with plain-symbol bindings only
 *   - (quote x)
 *   - function application -- single arity, no &-rest, no destructure
 *   - (def name expr)
 *
 * Decline conditions (any of these -> MINO_BC_UNSUPPORTED, fn falls back
 * to the tree-walker):
 *   - multi-arity fn (fn->as.fn.params == NULL)
 *   - params with &-rest, destructuring, :as, &-list shape
 *   - body uses (try/catch/finally), (throw), (loop/recur),
 *     (binding), (lazy-seq), (set!), inner (fn ...) literal
 *   - macro expansion not present in the form (we don't expand inside
 *     the compiler in Phase 1; macros expand on the eval path before
 *     reaching the compiler when applicable)
 *
 * Compilation is single-pass; the register allocator is "stupid first":
 * one fresh register per AST temp, freed only at the end of the
 * enclosing `let` scope. The linear-scan smart allocator and the
 * peephole pass land in Phase 2.
 *
 * Var-indirection discipline: every reference to a global name compiles
 * to OP_GETGLOBAL with the symbol in the const pool, never a baked
 * value. OP_SETGLOBAL bumps S->ic_gen so cached call sites invalidate.
 */

#include <stddef.h>
#include <string.h>

#include "mino.h"
#include "runtime/internal.h"
#include "eval/internal.h"
#include "eval/bc/internal.h"

extern mino_val_t *mino_nil(mino_state_t *S);

/* Sentinel for failed compiles. apply_callable checks against this
 * pointer to skip the retry on subsequent calls. */
const mino_bc_fn_t mino_bc_declined = {0};

#define BC_MAX_LOCALS 256
#define BC_MAX_REGS   255            /* operand width fits in 8 bits */

typedef struct local {
    const char *name;   /* interned symbol data; pointer-stable */
    int         reg;
} bc_local_t;

typedef struct compiler {
    mino_state_t      *S;
    mino_env_t        *env;          /* fn's captured env, for macroexpand1 */
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
    /* No dedup in Phase 1: the const pool is per-fn and typical bodies
     * have small pools, so a duplicate symbol const is cheap. */
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
     * so the natural "fall-through here" offset is code_len - (pos+1). */
    int off = (int)c->bc->code_len - (pos + 1);
    if (off < INT16_MIN + 0x8000 || off > INT16_MAX - 0x8000) {
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

/* Forward declarations. */
static int compile_expr(compiler_t *c, mino_val_t *form, int dst);
static int compile_body(compiler_t *c, mino_val_t *body, int dst);

/* ------------------------------------------------------------------- */
/* Special-form compilers                                              */
/* ------------------------------------------------------------------- */

static int compile_if(compiler_t *c, mino_val_t *form, int dst)
{
    /* (if cond then) or (if cond then else) */
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

    int saved_next = c->next_reg;
    int cond_reg = alloc_reg(c);
    if (cond_reg < 0) return -1;
    if (compile_expr(c, cond_form, cond_reg) < 0) return -1;
    c->next_reg = saved_next;

    int jmp_to_else = emit_jmp_placeholder(c, OP_JMPIFNOT, (unsigned)cond_reg);
    if (jmp_to_else < 0) return -1;

    if (compile_expr(c, then_form, dst) < 0) return -1;

    int jmp_to_end = emit_jmp_placeholder(c, OP_JMP, 0);
    if (jmp_to_end < 0) return -1;

    patch_jmp(c, jmp_to_else);

    if (else_form != NULL) {
        if (compile_expr(c, else_form, dst) < 0) return -1;
    } else {
        /* (if cond then) without else: result is nil. */
        int k = add_const(c, mino_nil(c->S));
        if (k < 0) return -1;
        emit_abx(c, OP_LOAD_K, (unsigned)dst, (unsigned)k);
    }

    patch_jmp(c, jmp_to_end);
    return 0;
}

static int compile_do(compiler_t *c, mino_val_t *form, int dst)
{
    /* (do e1 e2 ... eN) -> eval each, result is eN. Empty (do) -> nil. */
    mino_val_t *body = form->as.cons.cdr;
    return compile_body(c, body, dst);
}

static int compile_let(compiler_t *c, mino_val_t *form, int dst)
{
    /* (let [b1 v1 b2 v2 ...] body...) -- plain-symbol bindings only. */
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

    for (size_t i = 0; i < blen; i += 2) {
        mino_val_t *name_form = vec_nth(bindings, i);
        mino_val_t *val_form  = vec_nth(bindings, i + 1);
        if (name_form == NULL || name_form->type != MINO_SYMBOL) {
            c->ok = 0; goto out;
        }
        int reg = alloc_reg(c);
        if (reg < 0) goto out;
        if (compile_expr(c, val_form, reg) < 0) goto out;
        if (!bind_local(c, name_form->as.s.data, reg)) goto out;
    }

    if (compile_body(c, body, dst) < 0) goto out;

    c->n_locals = saved_n_locals;
    c->next_reg = saved_next_reg;
    return 0;

out:
    c->n_locals = saved_n_locals;
    c->next_reg = saved_next_reg;
    return -1;
}

static int compile_quote(compiler_t *c, mino_val_t *form, int dst)
{
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

static int compile_def(compiler_t *c, mino_val_t *form, int dst)
{
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
        if (compile_expr(c, val_form, val_reg) < 0) return -1;
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

static int compile_call(compiler_t *c, mino_val_t *form, int dst)
{
    /* Phase 1 keeps fn calls on the tree-walker path. OP_CALL is wired
     * in the VM but the compiler does not emit it yet: without
     * cross-frame tail-call elimination, deeply-recursive Clojure
     * patterns would blow the C stack. Phase 2 adds OP_TAILCALL
     * emission and OP_CALL becomes viable. */
    (void)form; (void)dst;
    c->ok = 0;
    return -1;
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

static int compile_expr(compiler_t *c, mino_val_t *form, int dst)
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
        /* Collection literals: decline if they contain non-self-evaluating
         * elements, since we don't yet compile element evaluation. The
         * tree-walker handles them. */
        c->ok = 0;
        return -1;
    }
    if (!mino_is_cons(form)) {
        c->ok = 0;
        return -1;
    }

    /* If the call head resolves to a macro at compile time, decline.
     * Phase 1 keeps macro-using fns on the tree-walker path; the
     * compile-time-then-tree-eval boundary is delicate enough that
     * macros are deferred to a later cycle. */
    {
        mino_val_t *head = form->as.cons.car;
        if (head != NULL && head->type == MINO_SYMBOL
            && find_local(c, head->as.s.data) < 0) {
            mino_val_t *hv = mino_env_get_sym(c->env, head);
            if (hv != NULL && hv->type == MINO_MACRO) {
                c->ok = 0;
                return -1;
            }
        }
    }

    mino_val_t *head = form->as.cons.car;

    if (head != NULL && head->type == MINO_SYMBOL) {
        const char *name = head->as.s.data;
        if (strcmp(name, "if") == 0)   return compile_if(c, form, dst);
        if (strcmp(name, "do") == 0)   return compile_do(c, form, dst);
        if (strcmp(name, "let") == 0
            || strcmp(name, "let*") == 0) return compile_let(c, form, dst);
        if (strcmp(name, "quote") == 0) return compile_quote(c, form, dst);
        if (strcmp(name, "def") == 0)   return compile_def(c, form, dst);

        /* Forms we explicitly decline so a missing handler doesn't
         * silently compile to a regular call (which would fail at
         * runtime because the special form has no callable binding). */
        if (sym_is(head, "fn")        || sym_is(head, "fn*")
            || sym_is(head, "loop")    || sym_is(head, "loop*")
            || sym_is(head, "recur")
            || sym_is(head, "try")     || sym_is(head, "catch")
            || sym_is(head, "finally")
            || sym_is(head, "throw")
            || sym_is(head, "lazy-seq")
            || sym_is(head, "binding")
            || sym_is(head, "set!")
            || sym_is(head, "quote*")
            || sym_is(head, "var")
            || sym_is(head, "defmacro")
            || sym_is(head, "defrecord")
            || sym_is(head, "deftype")
            || sym_is(head, "defprotocol")
            || sym_is(head, "reify")
            || sym_is(head, "case")
            || sym_is(head, "cond")
            || sym_is(head, "new")
            || sym_is(head, "."))
        {
            c->ok = 0;
            return -1;
        }
    }
    return compile_call(c, form, dst);
}

static int compile_body(compiler_t *c, mino_val_t *body, int dst)
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
            if (compile_expr(c, expr, dst) < 0) return -1;
        } else {
            int t = alloc_reg(c);
            if (t < 0) return -1;
            if (compile_expr(c, expr, t) < 0) return -1;
            c->next_reg = saved_next + 0;  /* discard intermediate temp */
            /* Actually keep growing next_reg to avoid stomping on
             * "live across forms" register slots; the simple
             * compiler doesn't reason about liveness, so each
             * non-last form just gets its own slot. */
            c->next_reg = saved_next;
        }
        body = next;
    }
    return 0;
}

/* ------------------------------------------------------------------- */
/* Compile-fn entry                                                    */
/* ------------------------------------------------------------------- */

/* Params must be a vector of plain symbols, no &-rest, no destructure. */
static int params_simple_plain(mino_val_t *params, int *out_n)
{
    if (params == NULL) return 0;
    if (params->type != MINO_VECTOR) return 0;
    size_t n = params->as.vec.len;
    if (n > BC_MAX_REGS) return 0;
    for (size_t i = 0; i < n; i++) {
        mino_val_t *p = vec_nth(params, i);
        if (p == NULL || p->type != MINO_SYMBOL) return 0;
        const char *d = p->as.s.data;
        if (d[0] == '&') return 0;             /* &-rest */
        if (strchr(d, '.') != NULL) return 0;  /* dotted */
    }
    *out_n = (int)n;
    return 1;
}

int mino_bc_compile_fn(mino_state_t *S, mino_val_t *fn)
{
    if (fn == NULL || fn->type != MINO_FN) return MINO_BC_ERROR;
    if (fn->as.fn.bc != NULL) return MINO_BC_OK;  /* already compiled */

    /* Multi-arity fns: params == NULL, body is a clause list. Decline
     * for Phase 1. */
    if (fn->as.fn.params == NULL) {
        fn->as.fn.bc = &mino_bc_declined;
        return MINO_BC_UNSUPPORTED;
    }
    int n_params = 0;
    if (!params_simple_plain(fn->as.fn.params, &n_params)) {
        fn->as.fn.bc = &mino_bc_declined;
        return MINO_BC_UNSUPPORTED;
    }

    /* Materialize the bc record FIRST and attach to fn so the GC has a
     * root for the in-progress code/consts buffers throughout the
     * compile. If compilation declines later, we overwrite
     * fn->as.fn.bc with the sentinel and the partial buffers fall out
     * of reachability. */
    mino_bc_fn_t *bc = (mino_bc_fn_t *)gc_alloc_typed(
        S, GC_T_RAW, sizeof(*bc));
    if (bc == NULL) { fn->as.fn.bc = &mino_bc_declined; return MINO_BC_UNSUPPORTED; }
    bc->code       = NULL;
    bc->code_len   = 0;
    bc->consts     = NULL;
    bc->consts_len = 0;
    bc->n_regs     = n_params;
    bc->n_params   = n_params;
    /* Write barrier: fn may already be OLD (top-level defns survive
     * many minor cycles before the first call triggers compile). The
     * just-allocated bc is YOUNG. Without the barrier, the next minor
     * misses bc and frees it from under the field. */
    gc_write_barrier(S, fn, NULL, bc);
    fn->as.fn.bc   = bc;

    compiler_t c;
    memset(&c, 0, sizeof(c));
    c.S        = S;
    c.env      = fn->as.fn.env;
    c.bc       = bc;
    c.n_params = n_params;
    c.next_reg = n_params;
    c.n_regs   = n_params;
    c.ok       = 1;

    /* Bind params to registers 0..n_params-1. */
    for (int i = 0; i < n_params; i++) {
        mino_val_t *p = vec_nth(fn->as.fn.params, (size_t)i);
        if (!bind_local(&c, p->as.s.data, i)) goto decline;
    }

    /* Compile body. Result goes into a fresh "ret" register that's
     * separate from param regs so OP_RETURN's source slot can be
     * predicted ahead of body emission. */
    int ret_reg = alloc_reg(&c);
    if (ret_reg < 0) goto decline;
    if (compile_body(&c, fn->as.fn.body, ret_reg) < 0) goto decline;
    if (!c.ok) goto decline;

    /* Final OP_RETURN. */
    emit_abc(&c, OP_RETURN, (unsigned)ret_reg, 0, 0);
    if (!c.ok) goto decline;

    bc->n_regs = c.n_regs;
    return MINO_BC_OK;

decline:
    fn->as.fn.bc = &mino_bc_declined;
    return MINO_BC_UNSUPPORTED;
}
