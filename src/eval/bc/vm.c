/*
 * eval/bc/vm.c -- register-based bytecode VM dispatch.
 *
 * Switch-based interpreter. Each compiled fn carries its own
 * instruction stream and constant pool; mino_bc_run pushes a register
 * window onto S->bc_regs for the call and pops it on return. The
 * window is a slice into a single per-state stack so the GC can walk
 * every live register slot in one pass.
 *
 * Var-indirection discipline (see plan): OP_GETGLOBAL resolves through
 * the var registry every time. No call site closes over a fn value at
 * compile time; redefinition stays visible.
 */

#include <limits.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mino.h"
#include "runtime/internal.h"       /* mino_state_t, gc_alloc_typed, GC_T_VALARR */
#include "eval/internal.h"          /* eval_impl, apply_callable */
#include "eval/special_internal.h"  /* normalize_exception for OP_PUSHCATCH */
#include "eval/bc/internal.h"
#include "eval/bc/jit.h"
#include "collections/internal.h"   /* make_fn */
#include "prim/internal.h"          /* binary arith prim_* on bc fast-lane miss */

extern mino_val_t *mino_nil(mino_state_t *S);

#ifdef MINO_CALL_SITE_SHAPES
/* Per-site tally of OP_CALL_CACHED hits keyed by (slot pointer, arg-
 * type-pair). Populated when the binary is built with
 * -DMINO_CALL_SITE_SHAPES=1. Dumped to stderr at exit. Used to gate
 * the type-feedback IC item: counts sites that hit a canonical arith
 * callee with stable monomorphic-int operand types. Not for production. */
#define CALL_SHAPE_SITES_MAX 8192
typedef struct {
    const void *slot;          /* mino_bc_ic_slot_t * -- unique site key */
    mino_prim_fn callee_fn;    /* resolved callee prim (NULL = not a prim) */
    size_t       total;
    size_t       monomorphic_int_pair;  /* both args tagged-int */
    size_t       other_shapes;
} call_shape_row_t;
static call_shape_row_t g_call_shapes[CALL_SHAPE_SITES_MAX];
static int              g_call_shapes_used;
static int              g_call_shapes_atexit_done;
static const char *call_shape_prim_name(mino_prim_fn fn);
static void call_shapes_dump(void)
{
    int i, hot = 0, mono_hot = 0;
    size_t hot_hits = 0, mono_hits = 0;
    fprintf(stderr, "call-site-shapes: sites tracked = %d (cap=%d)\n",
            g_call_shapes_used, CALL_SHAPE_SITES_MAX);
    for (i = 0; i < g_call_shapes_used; i++) {
        if (g_call_shapes[i].total >= 10000) {
            hot++;
            hot_hits += g_call_shapes[i].total;
            if (g_call_shapes[i].callee_fn != NULL
                && g_call_shapes[i].monomorphic_int_pair * 10
                       >= g_call_shapes[i].total * 9) {
                mono_hot++;
                mono_hits += g_call_shapes[i].monomorphic_int_pair;
            }
        }
    }
    fprintf(stderr, "  hot sites (>=10k calls): %d, hits=%zu\n",
            hot, hot_hits);
    fprintf(stderr, "  hot+monomorphic-int-prim (>=90%% int pair) sites:"
                    " %d, hits=%zu\n",
            mono_hot, mono_hits);
    /* Detail of top monomorphic-int sites */
    if (mono_hot > 0) {
        int printed = 0;
        fprintf(stderr, "  top monomorphic-int sites:\n");
        for (i = 0; i < g_call_shapes_used && printed < 20; i++) {
            if (g_call_shapes[i].total >= 10000
                && g_call_shapes[i].callee_fn != NULL
                && g_call_shapes[i].monomorphic_int_pair * 10
                       >= g_call_shapes[i].total * 9) {
                fprintf(stderr,
                        "    %p %-16s total=%zu mono=%zu (%.1f%%)\n",
                        g_call_shapes[i].slot,
                        call_shape_prim_name(g_call_shapes[i].callee_fn),
                        g_call_shapes[i].total,
                        g_call_shapes[i].monomorphic_int_pair,
                        100.0
                            * (double)g_call_shapes[i].monomorphic_int_pair
                            / (double)g_call_shapes[i].total);
                printed++;
            }
        }
    }
}
static void call_shape_record(const void *slot_ptr, mino_val_t *callee,
                              mino_val_t **argv, int argc)
{
    int i;
    int idx = -1;
    if (!g_call_shapes_atexit_done) {
        atexit(call_shapes_dump);
        g_call_shapes_atexit_done = 1;
    }
    for (i = 0; i < g_call_shapes_used; i++) {
        if (g_call_shapes[i].slot == slot_ptr) { idx = i; break; }
    }
    if (idx < 0) {
        if (g_call_shapes_used >= CALL_SHAPE_SITES_MAX) return;
        idx = g_call_shapes_used++;
        g_call_shapes[idx].slot = slot_ptr;
        g_call_shapes[idx].callee_fn =
            (callee != NULL && mino_type_of(callee) == MINO_PRIM)
                ? callee->as.prim.fn
                : NULL;
        g_call_shapes[idx].total = 0;
        g_call_shapes[idx].monomorphic_int_pair = 0;
        g_call_shapes[idx].other_shapes = 0;
    }
    g_call_shapes[idx].total++;
    if (argc == 2 && argv[0] != NULL && argv[1] != NULL
        && mino_val_int_p(argv[0]) && mino_val_int_p(argv[1])) {
        g_call_shapes[idx].monomorphic_int_pair++;
    } else {
        g_call_shapes[idx].other_shapes++;
    }
}
static const char *call_shape_prim_name(mino_prim_fn fn)
{
    if (fn == NULL) return "(non-prim)";
    if (fn == prim_add) return "prim_add";
    if (fn == prim_sub) return "prim_sub";
    if (fn == prim_mul) return "prim_mul";
    if (fn == prim_addp) return "prim_addp";
    if (fn == prim_subp) return "prim_subp";
    if (fn == prim_mulp) return "prim_mulp";
    if (fn == prim_bit_and) return "prim_bit_and";
    if (fn == prim_bit_or)  return "prim_bit_or";
    if (fn == prim_bit_xor) return "prim_bit_xor";
    return "(other-prim)";
}
#endif

#ifdef MINO_BC_OP_COUNTS
/* Per-opcode dispatch counter, populated when the binary is built with
 * -DMINO_BC_OP_COUNTS=1. Dumped to stderr at process exit. Used during
 * VM design experiments to identify which opcodes dominate the
 * dispatch loop (hot/cold partition decisions). Not for production. */
static size_t g_op_counts[OP__COUNT];
/* Adjacent-pair (bigram) counts in the dispatch loop. g_op_bigrams[a][b]
 * is incremented when op b dispatches immediately after op a within the
 * same dispatch session (one mino_bc_run frame); cross-frame transitions
 * are NOT counted because they don't correspond to a fusable pair in
 * the same bytecode stream. Used to rank candidate superinstructions
 * for stencil fusion. */
static size_t g_op_bigrams[OP__COUNT][OP__COUNT];
static int    g_op_counts_atexit_registered;

static void op_counts_dump(void)
{
    size_t total = 0;
    int    i;
    /* Pack (idx, count) pairs so the sort preserves opcode identity. */
    typedef struct { unsigned op; size_t count; } row_t;
    row_t  rows[OP__COUNT];
    for (i = 0; i < OP__COUNT; i++) {
        rows[i].op = (unsigned)i;
        rows[i].count = g_op_counts[i];
        total += rows[i].count;
    }
    /* Sort by count descending. N is small (~63); bubble-sort fine. */
    for (i = 0; i < OP__COUNT; i++) {
        int j;
        for (j = i + 1; j < OP__COUNT; j++) {
            if (rows[j].count > rows[i].count) {
                row_t t = rows[i]; rows[i] = rows[j]; rows[j] = t;
            }
        }
    }
    fprintf(stderr, "bc-op-counts: total dispatches = %zu\n", total);
    if (total == 0) return;
    size_t cumulative = 0;
    for (i = 0; i < OP__COUNT; i++) {
        if (rows[i].count == 0) break;
        cumulative += rows[i].count;
        fprintf(stderr, "  %-25s %12zu  %6.2f%%  cum=%6.2f%%\n",
                mino_bc_op_name(rows[i].op), rows[i].count,
                100.0 * (double)rows[i].count / (double)total,
                100.0 * (double)cumulative / (double)total);
    }
    /* Top-30 bigrams ranked by absolute frequency. A bigram is one
     * "if I fuse these two ops into one stencil, this many dispatches
     * collapse to one". */
    typedef struct { unsigned a; unsigned b; size_t count; } pair_t;
    enum { TOP_N = 30 };
    pair_t top[TOP_N];
    int    top_n = 0;
    size_t total_pairs = 0;
    for (int a = 0; a < OP__COUNT; a++) {
        for (int b = 0; b < OP__COUNT; b++) {
            size_t c = g_op_bigrams[a][b];
            if (c == 0) continue;
            total_pairs += c;
            int insert = top_n;
            for (int k = 0; k < top_n; k++) {
                if (c > top[k].count) { insert = k; break; }
            }
            if (insert < TOP_N) {
                int end = top_n < TOP_N ? top_n : TOP_N - 1;
                for (int j = end; j > insert; j--) top[j] = top[j - 1];
                top[insert].a = (unsigned)a;
                top[insert].b = (unsigned)b;
                top[insert].count = c;
                if (top_n < TOP_N) top_n++;
            }
        }
    }
    fprintf(stderr, "bc-op-bigrams: total pairs = %zu\n", total_pairs);
    if (total_pairs == 0) return;
    for (i = 0; i < top_n; i++) {
        fprintf(stderr, "  %-25s -> %-25s %12zu  %6.2f%%\n",
                mino_bc_op_name(top[i].a),
                mino_bc_op_name(top[i].b),
                top[i].count,
                100.0 * (double)top[i].count / (double)total_pairs);
    }
}

#endif

const char *mino_bc_op_name(unsigned op)
{
    switch (op) {
    case OP_NOP: return "OP_NOP";
    case OP_MOVE: return "OP_MOVE";
    case OP_LOAD_K: return "OP_LOAD_K";
    case OP_GETGLOBAL: return "OP_GETGLOBAL";
    case OP_SETGLOBAL: return "OP_SETGLOBAL";
    case OP_JMP: return "OP_JMP";
    case OP_JMPIFNOT: return "OP_JMPIFNOT";
    case OP_CALL: return "OP_CALL";
    case OP_TAILCALL: return "OP_TAILCALL";
    case OP_RETURN: return "OP_RETURN";
    case OP_CLOSURE: return "OP_CLOSURE";
    case OP_BINOP_INT: return "OP_BINOP_INT";
    case OP_PUSHCATCH: return "OP_PUSHCATCH";
    case OP_POPCATCH: return "OP_POPCATCH";
    case OP_THROW: return "OP_THROW";
    case OP_PUSHDYN: return "OP_PUSHDYN";
    case OP_POPDYN: return "OP_POPDYN";
    case OP_MAKE_LAZY: return "OP_MAKE_LAZY";
    case OP_GETGLOBAL_CACHED: return "OP_GETGLOBAL_CACHED";
    case OP_CALL_CACHED: return "OP_CALL_CACHED";
    case OP_PROTOCOL_CALL_CACHED: return "OP_PROTOCOL_CALL_CACHED";
    case OP_PROTOCOL_TAILCALL_CACHED: return "OP_PROTOCOL_TAILCALL_CACHED";
    case OP_ADD_II: return "OP_ADD_II";
    case OP_SUB_II: return "OP_SUB_II";
    case OP_MUL_II: return "OP_MUL_II";
    case OP_LT_II: return "OP_LT_II";
    case OP_LE_II: return "OP_LE_II";
    case OP_GT_II: return "OP_GT_II";
    case OP_GE_II: return "OP_GE_II";
    case OP_EQ_II: return "OP_EQ_II";
    case OP_INC_I: return "OP_INC_I";
    case OP_DEC_I: return "OP_DEC_I";
    case OP_ZERO_INT_P: return "OP_ZERO_INT_P";
    case OP_MOD_II: return "OP_MOD_II";
    case OP_QUOT_II: return "OP_QUOT_II";
    case OP_REM_II: return "OP_REM_II";
    case OP_BAND_II: return "OP_BAND_II";
    case OP_BOR_II: return "OP_BOR_II";
    case OP_BXOR_II: return "OP_BXOR_II";
    case OP_SHL_II: return "OP_SHL_II";
    case OP_SHR_II: return "OP_SHR_II";
    case OP_USHR_II: return "OP_USHR_II";
    case OP_POS_P_I: return "OP_POS_P_I";
    case OP_NEG_P_I: return "OP_NEG_P_I";
    case OP_EVEN_P_I: return "OP_EVEN_P_I";
    case OP_ODD_P_I: return "OP_ODD_P_I";
    case OP_BNOT_I: return "OP_BNOT_I";
    case OP_ADD_IK: return "OP_ADD_IK";
    case OP_SUB_IK: return "OP_SUB_IK";
    case OP_LT_IK: return "OP_LT_IK";
    case OP_LE_IK: return "OP_LE_IK";
    case OP_EQ_IK: return "OP_EQ_IK";
    case OP_GET_KW_MAP: return "OP_GET_KW_MAP";
    case OP_NTH_VEC: return "OP_NTH_VEC";
    case OP_CONJ_VEC: return "OP_CONJ_VEC";
    case OP_ASSOC: return "OP_ASSOC";
    case OP_DISSOC: return "OP_DISSOC";
    case OP_ASSOC_BANG: return "OP_ASSOC_BANG";
    case OP_CONJ_BANG: return "OP_CONJ_BANG";
    case OP_DISSOC_BANG: return "OP_DISSOC_BANG";
    case OP_DISJ_BANG: return "OP_DISJ_BANG";
    case OP_FIRST_VEC: return "OP_FIRST_VEC";
    case OP_COUNT_VEC: return "OP_COUNT_VEC";
    case OP_EMPTY_VEC: return "OP_EMPTY_VEC";
    case OP_LOOP_INT_DEC: return "OP_LOOP_INT_DEC";
    case OP_LOOP_INT_DEC_INC: return "OP_LOOP_INT_DEC_INC";
    case OP_LOOP_INT_LT: return "OP_LOOP_INT_LT";
    case OP_LOOP_INT_LT_INC: return "OP_LOOP_INT_LT_INC";
    case OP_PUSH_ENV: return "OP_PUSH_ENV";
    case OP_POP_ENV: return "OP_POP_ENV";
    case OP_ENV_BIND: return "OP_ENV_BIND";
    default: return "OP_UNKNOWN";
    }
}

/* Grow S->bc_regs to hold an additional `n` slots and return the base
 * index of the new window. Returns (size_t)-1 on allocation failure. */
static size_t bc_push_window(mino_state_t *S, int n)
{
    if (n < 0) return (size_t)-1;
    size_t need = S->bc_top + (size_t)n;
    if (need > S->bc_regs_cap) {
        size_t new_cap = S->bc_regs_cap == 0 ? 256 : S->bc_regs_cap * 2;
        while (new_cap < need) new_cap *= 2;
        mino_val_t **grown = (mino_val_t **)gc_alloc_typed(
            S, GC_T_VALARR, new_cap * sizeof(*grown));
        if (grown == NULL) return (size_t)-1;
        if (S->bc_regs != NULL && S->bc_top > 0) {
            memcpy(grown, S->bc_regs, S->bc_top * sizeof(*grown));
        }
        for (size_t i = S->bc_top; i < new_cap; i++) grown[i] = NULL;
        S->bc_regs     = grown;
        S->bc_regs_cap = new_cap;
    }
    size_t base = S->bc_top;
    /* The window is left uninitialized: bc_pop_window zeroes every
     * slot before the next push lands on it, and fresh bc_regs growth
     * paths zero the new tail explicitly above. Skipping the per-slot
     * NULL loop here trims a hot per-call cost. The GC root walk
     * (gc_mark_roots) only scans [0, bc_top), and the body's compiler
     * emits a write to every register it later reads -- there is no
     * GC point in mino_bc_run between fn entry and the body's writes
     * to its own registers, because all dispatch ops that may collect
     * (OP_CALL, OP_GETGLOBAL_CACHED, OP_CLOSURE, ...) come after the
     * regs[a] := producer step. */
    S->bc_top = need;
    return base;
}

static void bc_pop_window(mino_state_t *S, size_t base)
{
    while (S->bc_top > base) {
        S->bc_top--;
        S->bc_regs[S->bc_top] = NULL;
    }
}

/* Type discriminator for the OP_PROTOCOL_CALL_CACHED hot path. Records
 * return their MINO_TYPE pointer directly so the cache key stays a
 * pure-pointer compare; non-records hash into the interned keyword
 * table the way prim_type would. Mirrors prim_type's first-arg path
 * but avoids the cons-spine + arity-check wrapper. */
static mino_val_t *bc_protocol_type_disc(mino_state_t *S, mino_val_t *v)
{
    if (v == NULL) return mino_keyword(S, "nil");
    if (mino_type_of(v) == MINO_RECORD) return v->as.record.type;
    /* Honor :type metadata so user types tagged via (with-meta x
     * {:type :foo}) dispatch to the :foo impl. Same precedence as
     * prim_type. */
    if (MINO_IS_PTR(v) && v->meta != NULL
        && mino_type_of(v->meta) == MINO_MAP) {
        mino_val_t *tk = mino_keyword(S, "type");
        mino_val_t *tv = map_get_val(v->meta, tk);
        if (tv != NULL) return tv;
    }
    switch (mino_type_of(v)) {
    case MINO_NIL:        return mino_keyword(S, "nil");
    case MINO_BOOL:       return mino_keyword(S, "bool");
    case MINO_INT:        return mino_keyword(S, "int");
    case MINO_FLOAT:      return mino_keyword(S, "float");
    case MINO_FLOAT32:    return mino_keyword(S, "float32");
    case MINO_CHAR:       return mino_keyword(S, "char");
    case MINO_STRING:     return mino_keyword(S, "string");
    case MINO_SYMBOL:     return mino_keyword(S, "symbol");
    case MINO_KEYWORD:    return mino_keyword(S, "keyword");
    case MINO_EMPTY_LIST: return mino_keyword(S, "list");
    case MINO_CONS:       return mino_keyword(S, "list");
    case MINO_VECTOR:     return mino_keyword(S, "vector");
    case MINO_MAP:        return mino_keyword(S, "map");
    case MINO_SET:        return mino_keyword(S, "set");
    case MINO_SORTED_MAP: return mino_keyword(S, "sorted-map");
    case MINO_SORTED_SET: return mino_keyword(S, "sorted-set");
    case MINO_PRIM:       return mino_keyword(S, "fn");
    case MINO_FN:         return mino_keyword(S, "fn");
    case MINO_MACRO:      return mino_keyword(S, "macro");
    case MINO_HANDLE:     return mino_keyword(S, "handle");
    case MINO_ATOM:       return mino_keyword(S, "atom");
    case MINO_VOLATILE:   return mino_keyword(S, "volatile");
    case MINO_LAZY:       return mino_keyword(S, "lazy-seq");
    case MINO_CHUNK:      return mino_keyword(S, "chunk");
    case MINO_CHUNKED_CONS: return mino_keyword(S, "list");
    case MINO_RECUR:      return mino_keyword(S, "recur");
    case MINO_TAIL_CALL:  return mino_keyword(S, "tail-call");
    case MINO_REDUCED:    return mino_keyword(S, "reduced");
    case MINO_VAR:        return mino_keyword(S, "var");
    case MINO_TRANSIENT:  return mino_keyword(S, "transient");
    case MINO_BIGINT:     return mino_keyword(S, "bigint");
    case MINO_RATIO:      return mino_keyword(S, "ratio");
    case MINO_BIGDEC:     return mino_keyword(S, "bigdec");
    case MINO_TYPE:       return mino_keyword(S, "record-type");
    case MINO_RECORD:     return v->as.record.type; /* unreachable */
    case MINO_FUTURE:     return mino_keyword(S, "future");
    case MINO_UUID:       return mino_keyword(S, "uuid");
    case MINO_REGEX:      return mino_keyword(S, "regex");
    case MINO_HOST_ARRAY: {
        static const char *kinds[] = {
            "object-array", "int-array", "long-array", "short-array",
            "byte-array",   "float-array", "double-array", "char-array",
            "boolean-array"
        };
        unsigned k = v->as.host_array.element_kind;
        if (k >= sizeof(kinds) / sizeof(kinds[0])) k = 0;
        return mino_keyword(S, kinds[k]);
    }
    case MINO_MAP_ENTRY:  return mino_keyword(S, "map-entry");
    case MINO_TX_REF:     return mino_keyword(S, "ref");
    case MINO_AGENT:      return mino_keyword(S, "agent");
    }
    return mino_keyword(S, "unknown");
}

/* Build a cons list head-first from `argc` register slots starting at
 * `base`. Used by OP_CALL / OP_TAILCALL to hand off arguments to
 * apply_callable, which still consumes the cons-spine ABI. The new
 * cells are reachable through the GC root walk because the register
 * slots in [base, base+argc) keep their referents alive, and the new
 * head cells are themselves stored into a temporary that the
 * conservative stack scan covers. */
static mino_val_t *args_from_regs(mino_state_t *S, mino_val_t **regs,
                                  unsigned argc)
{
    mino_val_t *list = mino_nil(S);
    if (list == NULL) return NULL;
    for (int i = (int)argc - 1; i >= 0; i--) {
        mino_val_t *cell = mino_cons(S, regs[i], list);
        if (cell == NULL) return NULL;
        list = cell;
    }
    return list;
}

/* Encode a 61-bit signed `r` as a tagged int. Falls back to the boxed
 * constructor for the narrow band beyond MINO_INT_MAX where the tag
 * would lose precision (in practice unreachable from the +/-/inc/dec
 * fast lanes: their operands are both already in 61-bit range and the
 * overflow check prior to encoding caught LLONG_MAX-class wraps). */
mino_val_t *tag_or_box_int(mino_state_t *S, long long r)
{
#ifdef MINO_BC_PROFILE_COUNTS
    S->bc_int_make_count++;
#endif
    if (r >= MINO_INT_MIN && r <= MINO_INT_MAX) {
#ifdef MINO_BC_PROFILE_COUNTS
        S->bc_int_alloc_avoided++;
#endif
        return MINO_MAKE_INT(r);
    }
    return mino_int(S, r);
}

/* Integer fast-lane for unary inc / dec / zero?. The Phase D rewrite
 * skips mino_val_int_p / mino_val_int_get -- both inputs are required
 * to be inline-tagged ints, so the helper functions' NULL + tag + type
 * three-step check is replaced with a single MINO_IS_INT tag-bit test
 * and MINO_INT_VAL inline decode. The boxed-int slow path falls
 * through to the prim via the same NULL-return-bails-to-fallback
 * contract the binop lane uses. */
mino_val_t *unop_int_fast(mino_state_t *S, mino_val_t *v,
                          unsigned subop)
{
    long long a, r;
    if (!MINO_IS_INT(v)) return NULL;
    a = MINO_INT_VAL(v);
    switch (subop) {
    case UNOP_INC:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_saddll_overflow(a, 1, &r)) return NULL;
#else
        r = a + 1;
#endif
        return tag_or_box_int(S, r);
    case UNOP_DEC:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_ssubll_overflow(a, 1, &r)) return NULL;
#else
        r = a - 1;
#endif
        return tag_or_box_int(S, r);
    case UNOP_ZERO_P:
        return (a == 0) ? mino_true(S) : mino_false(S);
    case UNOP_POS_P:
        return (a >  0) ? mino_true(S) : mino_false(S);
    case UNOP_NEG_P:
        return (a <  0) ? mino_true(S) : mino_false(S);
    case UNOP_EVEN_P:
        return ((a & 1) == 0) ? mino_true(S) : mino_false(S);
    case UNOP_ODD_P:
        return ((a & 1) != 0) ? mino_true(S) : mino_false(S);
    case UNOP_BNOT:
        /* ~a == -a - 1; always fits in the tagged range when a does. */
        return tag_or_box_int(S, ~a);
    default:
        return NULL;
    }
}

/* Integer fast-lane for OP_BINOP_INT. Same Phase D tag-extract shape
 * as unop_int_fast: a single MINO_IS_INT check per operand replaces
 * mino_val_int_p's NULL + tag + type chain, and MINO_INT_VAL decodes
 * inline without the boxed-fallback branch. Overflow stays on the
 * __builtin_*_overflow intrinsics; the encoded result rides through
 * tag_or_box_int. Returns NULL on a tag miss or overflow so the
 * dispatcher bails to the cons-spine prim. */
mino_val_t *binop_int_fast(mino_state_t *S, mino_val_t *lhs,
                           mino_val_t *rhs, unsigned subop)
{
    long long a, b, r;
    if (!MINO_IS_INT(lhs) || !MINO_IS_INT(rhs)) return NULL;
    a = MINO_INT_VAL(lhs);
    b = MINO_INT_VAL(rhs);
    switch (subop) {
    case BINOP_ADD:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_saddll_overflow(a, b, &r)) return NULL;
#else
        /* Non-GCC/Clang fallback. Direct signed `a + b` is UB on
         * overflow per ISO C; the unsigned wrap form is well-defined,
         * and the textbook sign-bit comparison detects the overflow. */
        {
            unsigned long long ua = (unsigned long long)a;
            unsigned long long ub = (unsigned long long)b;
            unsigned long long ur = ua + ub;
            if (((ua ^ ur) & (ub ^ ur))
                >> (sizeof(long long) * 8 - 1))
                return NULL;
            r = (long long)ur;
        }
#endif
        return tag_or_box_int(S, r);
    case BINOP_SUB:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_ssubll_overflow(a, b, &r)) return NULL;
#else
        {
            unsigned long long ua = (unsigned long long)a;
            unsigned long long ub = (unsigned long long)b;
            unsigned long long ur = ua - ub;
            if (((ua ^ ub) & (ua ^ ur))
                >> (sizeof(long long) * 8 - 1))
                return NULL;
            r = (long long)ur;
        }
#endif
        return tag_or_box_int(S, r);
    case BINOP_MUL:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_smulll_overflow(a, b, &r)) return NULL;
#else
        /* Pre-check via division so the multiply itself never
         * overflows. LLONG_MIN * -1 is the special case that division
         * can't handle; reject it explicitly. */
        if (a == LLONG_MIN && b == -1) return NULL;
        if (b == LLONG_MIN && a == -1) return NULL;
        if (b != 0) {
            long long limit = (a >= 0) == (b >= 0) ? LLONG_MAX : LLONG_MIN;
            if ((a > 0 && b > 0 && a > limit / b)
             || (a < 0 && b < 0 && a < limit / b)
             || (a > 0 && b < 0 && b < limit / a)
             || (a < 0 && b > 0 && a < limit / b))
                return NULL;
        }
        r = a * b;
#endif
        return tag_or_box_int(S, r);
    case BINOP_LT: return (a <  b) ? mino_true(S) : mino_false(S);
    case BINOP_LE: return (a <= b) ? mino_true(S) : mino_false(S);
    case BINOP_GT: return (a >  b) ? mino_true(S) : mino_false(S);
    case BINOP_GE: return (a >= b) ? mino_true(S) : mino_false(S);
    case BINOP_EQ: return (a == b) ? mino_true(S) : mino_false(S);
    case BINOP_MOD:
    case BINOP_QUOT:
    case BINOP_REM:
        /* Bail on b==0 (prim throws division-by-zero) or on the
         * MINO_INT_MIN / -1 corner where the quotient escapes the
         * tagged range and the prim's bigint-promote path is the
         * Clojure-correct answer. */
        if (b == 0) return NULL;
        if (a == MINO_INT_MIN && b == -1) return NULL;
        if (subop == BINOP_QUOT) return tag_or_box_int(S, a / b);
        r = a % b;
        if (subop == BINOP_MOD && r != 0 && ((r < 0) != (b < 0))) r += b;
        return tag_or_box_int(S, r);
    /* Bitwise ops are i64 operations -- the prims call mino_int_wrap
     * which always boxes as MINO_INT, never promotes to bigint. Route
     * the fast-path results through mino_int_wrap as well so a BC-
     * compiled call has the same result type as the prim path. Using
     * tag_or_box_int here promoted to bigint via mino_int's overflow
     * branch when the bignum capability was installed, and the
     * surface bug was a downstream bit-xor refusing the promoted
     * bigint (MTY001 "bit-xor expects integers"). */
    case BINOP_BAND: return mino_int_wrap(S, a & b);
    case BINOP_BOR:  return mino_int_wrap(S, a | b);
    case BINOP_BXOR: return mino_int_wrap(S, a ^ b);
    case BINOP_SHL:
        /* Shift amount must be in [0, 63]; route through unsigned so
         * that bit-shift-left of negative values matches the prim's
         * wrap-around result (and stays clear of signed-overflow UB). */
        if (b < 0 || b >= 64) return NULL;
        return mino_int_wrap(S, (long long)((unsigned long long)a << b));
    case BINOP_SHR:
        if (b < 0 || b >= 64) return NULL;
        return mino_int_wrap(S, a >> b);
    case BINOP_USHR:
        if (b < 0 || b >= 64) return NULL;
        return mino_int_wrap(S, (long long)((unsigned long long)a >> b));
    default:       return NULL;
    }
}

/* Resolve a global symbol through eval_impl. Goes through the same
 * dyn/lexical/ns/ambient cascade as eval_symbol; respects redefinition
 * because the lookup hits the var cell live, not a cached value. */
static mino_val_t *resolve_global(mino_state_t *S, mino_val_t *sym,
                                  mino_env_t *env)
{
    if (sym == NULL || mino_type_of(sym) != MINO_SYMBOL) return NULL;
    return eval_impl(S, sym, env, 0);
}

/* Classify a resolved callable's shape for the IC slot's cached_*
 * fields. Walks one var-deref but no further; if the value isn't
 * something that can be called (or is a tree-walker fn whose bc has
 * declined or hasn't been compiled yet), returns CALLABLE_OTHER and
 * leaves *out_has_rest / *out_n_params at zero. Conservative on
 * purpose: the v0.221 fast path only fires on SINGLE/PRIM_ARGV, and
 * a misclassification toward OTHER costs one fallback dispatch
 * whereas a misclassification toward SINGLE/PRIM_ARGV would skip
 * the dispatch switch for the wrong callable. */
static unsigned char classify_callable_kind(mino_val_t *v,
                                            unsigned char *out_has_rest,
                                            unsigned short *out_n_params)
{
    *out_has_rest = 0;
    *out_n_params = 0;
    if (v == NULL) return MINO_IC_CALLABLE_NONE;
    /* Var unwrap (apply_callable_argv mirrors this). */
    if (mino_type_of(v) == MINO_VAR) {
        if (!v->as.var.bound || v->as.var.root == NULL) {
            return MINO_IC_CALLABLE_NONE;
        }
        v = v->as.var.root;
    }
    if (mino_type_of(v) == MINO_PRIM) {
        if (v->as.prim.fn2 != NULL) return MINO_IC_CALLABLE_PRIM_ARGV;
        return MINO_IC_CALLABLE_OTHER;
    }
    if (mino_type_of(v) == MINO_FN) {
        mino_bc_fn_t *bc = v->as.fn.bc;
        if (bc == NULL || bc == &mino_bc_declined) {
            return MINO_IC_CALLABLE_OTHER;
        }
        if (bc->clauses == NULL || bc->n_clauses <= 0) {
            return MINO_IC_CALLABLE_OTHER;
        }
        if (bc->n_clauses == 1) {
            *out_has_rest = (unsigned char)(bc->clauses[0].has_rest ? 1 : 0);
            if (bc->clauses[0].n_params >= 0
                && bc->clauses[0].n_params <= 0xFFFF) {
                *out_n_params = (unsigned short)bc->clauses[0].n_params;
            }
            return MINO_IC_CALLABLE_MINO_FN_BC_SINGLE;
        }
        return MINO_IC_CALLABLE_MINO_FN_BC_MULTI;
    }
    return MINO_IC_CALLABLE_OTHER;
}

/* Shared resolve path for the two GLOBAL-kind IC consumers
 * (OP_GETGLOBAL_CACHED and OP_CALL_CACHED). Mirrors eval_symbol's
 * shadowing order: dynamic binding -> lexical env -> cached var ->
 * resolve. The cached lookup is gated on !dyn_active so a live
 * `(binding [*x* ...] ...)` doesn't mask the dyn value with a stale
 * cached var root. Returns the resolved value on hit, NULL on
 * failure (the caller surfaces the error via bc_done).
 *
 * On a fresh resolve the slot is refilled under a write barrier so
 * the slot array, which may be OLD after a minor cycle, keeps a
 * correct remset entry for the freshly resolved YOUNG value. */
static mino_val_t *ic_resolve_global(mino_state_t *S,
                                      const mino_bc_fn_t *bc,
                                      mino_bc_ic_slot_t *slot,
                                      mino_env_t *env,
                                      int dyn_active)
{
    if (dyn_active && slot->sym != NULL) {
        mino_val_t *dyn_v = dyn_lookup(S, slot->sym->as.s.data);
        if (dyn_v != NULL) return dyn_v;
    }
    if (env != NULL) {
        mino_val_t *env_v = mino_env_get_sym(env, slot->sym);
        if (env_v != NULL) return env_v;
    }
    if (!dyn_active
        && slot->cached != NULL
        && slot->gen == S->ic_gen) {
        return slot->cached;
    }
    mino_val_t *v = resolve_global(S, slot->sym, env);
    if (v == NULL) return NULL;
    if (!dyn_active) {
        unsigned char  has_rest = 0;
        unsigned short n_params = 0;
        unsigned char  kind     = classify_callable_kind(v, &has_rest,
                                                         &n_params);
        gc_write_barrier(S, bc->ic_slots, slot->cached, v);
        slot->cached               = v;
        slot->gen                  = S->ic_gen;
        slot->cached_callable_kind = kind;
        slot->cached_fn_has_rest   = has_rest;
        slot->cached_fn_n_params   = n_params;
        /* Stash the bc pointer for the JIT's FN_BC_SINGLE fast lane.
         * Reads through the same var-unwrap that classify_callable_kind
         * did so a Var-wrapped callable's bc is reached without a
         * second deref. NULL stays sticky for non-FN-shaped callables;
         * the stencil's hit check screens on cached_bc != NULL anyway. */
        if (kind == MINO_IC_CALLABLE_MINO_FN_BC_SINGLE) {
            mino_val_t *fnv = v;
            if (mino_type_of(fnv) == MINO_VAR) fnv = fnv->as.var.root;
            slot->cached_bc = (fnv != NULL && mino_type_of(fnv) == MINO_FN)
                ? fnv->as.fn.bc : NULL;
        } else {
            slot->cached_bc = NULL;
        }
    }
    return v;
}

/* Public entry point for the IC GLOBAL load: bounds-checks slot_idx
 * and routes through ic_resolve_global. Native tiers and external
 * tooling that wants to read a fn's resolved global without going
 * through the interpreter dispatch loop call this directly. */
mino_val_t *mino_bc_ic_global_load(mino_state_t *S,
                                   mino_bc_fn_t *bc,
                                   int slot_idx,
                                   mino_env_t *env,
                                   int dyn_active)
{
    if (bc == NULL || bc->ic_slots == NULL) return NULL;
    if (slot_idx < 0 || slot_idx >= bc->ic_slots_len) return NULL;
    return ic_resolve_global(S, bc, &bc->ic_slots[slot_idx],
                             env, dyn_active);
}

/* Shared resolve path for the PROTOCOL-kind IC consumers
 * (OP_PROTOCOL_CALL_CACHED and OP_PROTOCOL_TAILCALL_CACHED).
 * Mirrors the protocol-method dispatch fast lane: deref the captured
 * atom (one pointer load), compute the type discriminator for the
 * first argument, and pointer-compare the pair against the cached
 * (atom_map, type_disc) state. On a hit returns the cached impl
 * directly; on a miss looks up the impl via map_get_val with a
 * :default fallback, refills the IC under write barriers, and
 * returns. NULL return means the user-visible error was already
 * surfaced via prim_throw_classified (bad dispatch table shape, no
 * impl for type) and the caller should goto bc_done. The atom-NULL
 * / non-atom guard is the caller's responsibility (argn-and-shape
 * validation belongs at the dispatch site). */
mino_val_t *mino_bc_ic_resolve_protocol(mino_state_t *S,
                                        const mino_bc_fn_t *bc,
                                        mino_bc_ic_slot_t *slot,
                                        mino_val_t *first_arg)
{
    mino_val_t *atom_map = slot->atom->as.atom.val;
    mino_val_t *type_disc;
    if (first_arg != NULL
        && mino_type_of(first_arg) == MINO_RECORD) {
        type_disc = first_arg->as.record.type;
    } else {
        type_disc = bc_protocol_type_disc(S, first_arg);
    }
    if (slot->cached != NULL
        && slot->cached_map == atom_map
        && slot->cached_type == type_disc) {
        return slot->cached;
    }
    if (atom_map == NULL || mino_type_of(atom_map) != MINO_MAP) {
        char emsg[160];
        const char *mname_s = slot->sym != NULL
            ? slot->sym->as.s.data : "?";
        snprintf(emsg, sizeof(emsg),
                 "protocol dispatch table for %s is not a map",
                 mname_s);
        prim_throw_classified(S, "user", "MPR002", emsg);
        return NULL;
    }
    mino_val_t *impl = map_get_val(atom_map, type_disc);
    if (impl == NULL) {
        mino_val_t *defkw = mino_keyword(S, "default");
        impl = map_get_val(atom_map, defkw);
    }
    if (impl == NULL) {
        char emsg[256];
        const char *mname_s = slot->sym != NULL
            ? slot->sym->as.s.data : "?";
        const char *tname = "?";
        if (type_disc != NULL) {
            if (mino_type_of(type_disc) == MINO_KEYWORD
                || mino_type_of(type_disc) == MINO_STRING) {
                tname = type_disc->as.s.data;
            } else if (mino_type_of(type_disc) == MINO_TYPE) {
                tname = type_disc->as.record_type.name != NULL
                    ? type_disc->as.record_type.name : "?";
            }
        }
        snprintf(emsg, sizeof(emsg),
                 "No implementation of method: %s for type: %s",
                 mname_s, tname);
        prim_throw_classified(S, "user", "MPR001", emsg);
        return NULL;
    }
    gc_write_barrier(S, bc->ic_slots,
                     slot->cached_map, atom_map);
    slot->cached_map = atom_map;
    gc_write_barrier(S, bc->ic_slots,
                     slot->cached_type, type_disc);
    slot->cached_type = type_disc;
    gc_write_barrier(S, bc->ic_slots,
                     slot->cached, impl);
    slot->cached = impl;
    return impl;
}

/* Cold-op handler. The E1 op-count profile shows 18 of 63 opcodes
 * carry ~99% of dispatches; the long tail (NOP, GETGLOBAL non-cached,
 * CALL non-cached, closure build, env push/pop/bind, try/throw/dyn
 * frames, legacy BINOP_INT, infrequent vec ops) accounts for the
 * remaining <1%. Splitting those out of the main dispatch switch
 * keeps the hot dispatch's case ladder small enough for clang to lay
 * out a tight jump table and keep the hot-op locals in registers
 * across iterations. Each cold op pays more than a function call's
 * worth of work (allocation, env mutation, exception unwind, longjmp
 * setup), so the indirection amortizes for free.
 *
 * Contract: return 1 to continue the dispatch loop; return 0 to bail
 * to bc_done. *env_p is mutated by OP_PUSH_ENV / OP_POP_ENV; *ok is
 * cleared on error paths. The saved_* snapshots are read-only (used
 * by OP_POPCATCH / OP_POPDYN bounds checks). OP_THROW longjmps when
 * a try frame is live; that unwinds through this function's stack
 * frame back to the setjmp in OP_PUSHCATCH inside mino_bc_run, which
 * is well-defined because the jmp_buf lives on ctx->try_stack
 * (heap-backed) and not in this frame's automatic storage. */
static int bc_cold_op(mino_state_t *S, const mino_bc_fn_t *bc,
                      size_t base, mino_thread_ctx_t *ctx,
                      int saved_try_depth,
                      int saved_bc_catch_depth,
                      dyn_frame_t *saved_dyn_stack,
                      mino_bc_insn_t ins, unsigned op,
                      mino_env_t **env_p, int *ok)
{
    mino_env_t  *env  = *env_p;
    mino_val_t **regs = S->bc_regs + base;

    switch (op) {
    case OP_NOP:
        return 1;

    case OP_GETGLOBAL: {
        unsigned a  = A_OF(ins);
        unsigned bx = Bx_OF(ins);
        if (bx >= bc->consts_len) { *ok = 0; return 0; }
        mino_val_t *sym = bc->consts[bx];
        mino_val_t *v   = resolve_global(S, sym, env);
        if (v == NULL) { *ok = 0; return 0; }
        regs = S->bc_regs + base;
        regs[a] = v;
        return 1;
    }

    case OP_SETGLOBAL: {
        unsigned a  = A_OF(ins);
        unsigned bx = Bx_OF(ins);
        if (bx >= bc->consts_len) { *ok = 0; return 0; }
        mino_val_t *sym = bc->consts[bx];
        if (sym == NULL || mino_type_of(sym) != MINO_SYMBOL) {
            *ok = 0; return 0;
        }
        mino_val_t *v   = regs[a];
        mino_val_t *var = var_intern(S, S->current_ns, sym->as.s.data);
        if (var == NULL) { *ok = 0; return 0; }
        var_set_root(S, var, v);
        S->ic_gen++;
        regs = S->bc_regs + base;
        regs[a] = v;
        return 1;
    }

    case OP_CALL: {
        unsigned a    = A_OF(ins);
        unsigned argn = B_OF(ins);
        unsigned ret  = C_OF(ins);
        mino_val_t *callee = regs[a];
        mino_val_t *r = apply_callable_argv(S, callee, regs + a + 1,
                                            (int)argn, env);
        if (r == NULL) { *ok = 0; return 0; }
        S->bc_regs[base + ret] = r;
        return 1;
    }

    case OP_CLOSURE: {
        unsigned a  = A_OF(ins);
        unsigned bx = Bx_OF(ins);
        if (bx >= bc->consts_len) { *ok = 0; return 0; }
        mino_val_t *child = bc->consts[bx];
        if (child == NULL || mino_type_of(child) != MINO_FN) {
            *ok = 0; return 0;
        }
        mino_val_t *closure = make_fn(S, child->as.fn.params,
                                      child->as.fn.body, env);
        if (closure == NULL) { *ok = 0; return 0; }
        closure->as.fn.defining_ns = child->as.fn.defining_ns;
        *(const mino_bc_fn_t **)&closure->as.fn.bc = child->as.fn.bc;
        closure->as.fn.shape = child->as.fn.shape;
        regs = S->bc_regs + base;
        regs[a] = closure;
        return 1;
    }

    case OP_MAKE_LAZY: {
        unsigned a  = A_OF(ins);
        unsigned bx = Bx_OF(ins);
        if (bx >= bc->consts_len) { *ok = 0; return 0; }
        mino_val_t *body = bc->consts[bx];
        mino_val_t *lz = alloc_val(S, MINO_LAZY);
        if (lz == NULL) { *ok = 0; return 0; }
        lz->as.lazy.body     = body;
        lz->as.lazy.env      = env;
        lz->as.lazy.cached   = NULL;
        lz->as.lazy.realized = 0;
        regs = S->bc_regs + base;
        regs[a] = lz;
        return 1;
    }

    case OP_PUSH_ENV: {
        mino_env_t *child = env_child(S, env);
        if (child == NULL) { *ok = 0; return 0; }
        *env_p = child;
        return 1;
    }

    case OP_POP_ENV: {
        if (env == NULL || env->parent == NULL) {
            *ok = 0; return 0;
        }
        *env_p = env->parent;
        return 1;
    }

    case OP_ENV_BIND: {
        unsigned a  = A_OF(ins);
        unsigned bx = Bx_OF(ins);
        if (bx >= bc->consts_len) { *ok = 0; return 0; }
        mino_val_t *sym = bc->consts[bx];
        if (sym == NULL || mino_type_of(sym) != MINO_SYMBOL) {
            *ok = 0; return 0;
        }
        env_bind_sym(S, env, sym, regs[a]);
        return 1;
    }

    case OP_BINOP_INT: {
        unsigned a = A_OF(ins);
        unsigned b = B_OF(ins);
        unsigned c = C_OF(ins);
        unsigned subop = BINOP_OF(ins);
        mino_val_t *r = binop_int_fast(S, regs[b], regs[c], subop);
        if (r == NULL) { *ok = 0; return 0; }
        regs[a] = r;
        return 1;
    }

    case OP_POPCATCH: {
        if (ctx->bc_catch_depth <= saved_bc_catch_depth
            || ctx->try_depth <= saved_try_depth) {
            *ok = 0; return 0;
        }
        ctx->bc_catch_depth--;
        ctx->try_depth--;
        return 1;
    }

    case OP_PUSHDYN: {
        unsigned a  = A_OF(ins);
        unsigned bx = Bx_OF(ins);
        if (bx >= bc->consts_len) { *ok = 0; return 0; }
        mino_val_t *names = bc->consts[bx];
        if (names == NULL || mino_type_of(names) != MINO_VECTOR) {
            *ok = 0; return 0;
        }
        size_t n = names->as.vec.len;
        dyn_binding_t *bhead = NULL;
        for (size_t i = 0; i < n; i++) {
            mino_val_t *sym = vec_nth(names, i);
            if (sym == NULL || mino_type_of(sym) != MINO_SYMBOL) {
                while (bhead != NULL) {
                    dyn_binding_t *nxt = bhead->next;
                    free(bhead); bhead = nxt;
                }
                *ok = 0; return 0;
            }
            dyn_binding_t *b = (dyn_binding_t *)malloc(sizeof(*b));
            if (b == NULL) {
                while (bhead != NULL) {
                    dyn_binding_t *nxt = bhead->next;
                    free(bhead); bhead = nxt;
                }
                *ok = 0; return 0;
            }
            b->name = sym->as.s.data;
            b->val  = regs[a + (unsigned)i];
            b->next = bhead;
            bhead   = b;
        }
        dyn_frame_t *frame = (dyn_frame_t *)malloc(sizeof(*frame));
        if (frame == NULL) {
            while (bhead != NULL) {
                dyn_binding_t *nxt = bhead->next;
                free(bhead); bhead = nxt;
            }
            *ok = 0; return 0;
        }
        frame->bindings = bhead;
        frame->prev     = ctx->dyn_stack;
        ctx->dyn_stack  = frame;
        return 1;
    }

    case OP_POPDYN: {
        unsigned a = A_OF(ins);
        if (a == 0) a = 1;
        for (unsigned i = 0; i < a; i++) {
            dyn_frame_t *f = ctx->dyn_stack;
            if (f == NULL || f == saved_dyn_stack) {
                *ok = 0; return 0;
            }
            ctx->dyn_stack = f->prev;
            dyn_binding_list_free(f->bindings);
            free(f);
        }
        return 1;
    }

    case OP_THROW: {
        unsigned a = A_OF(ins);
        mino_val_t *exc = regs[a];
        if (ctx->try_depth > 0) {
            ctx->try_stack[ctx->try_depth - 1].exception = exc;
            longjmp(ctx->try_stack[ctx->try_depth - 1].buf, 1);
            /* unreachable */
        }
        /* No enclosing try -- format as a fatal user error and bail
         * through the standard error path. Mirror prim_throw's
         * "unhandled exception: <value>" shape so the original
         * thrown value survives in the diagnostic message. ex-info-
         * style maps that carry a :data (or :mino/data) payload route
         * through set_eval_diag_with_data so future workers preserve
         * the data field on consumer-side rethrow. */
        if (exc != NULL && mino_type_of(exc) == MINO_MAP) {
            mino_val_t *msg  = map_get_val(exc,
                mino_keyword(S, "mino/message"));
            mino_val_t *kind = map_get_val(exc,
                mino_keyword(S, "mino/kind"));
            mino_val_t *code = map_get_val(exc,
                mino_keyword(S, "mino/code"));
            mino_val_t *data = map_get_val(exc,
                mino_keyword(S, "mino/data"));
            if (msg == NULL || mino_type_of(msg) != MINO_STRING) {
                msg = map_get_val(exc, mino_keyword(S, "message"));
            }
            if (data == NULL) {
                data = map_get_val(exc, mino_keyword(S, "data"));
            }
            const char *kind_str =
                (kind != NULL && mino_type_of(kind) == MINO_KEYWORD)
                    ? kind->as.s.data : "user";
            const char *code_str =
                (code != NULL && mino_type_of(code) == MINO_STRING)
                    ? code->as.s.data : "MUS001";
            const char *msg_str =
                (msg != NULL && mino_type_of(msg) == MINO_STRING)
                    ? msg->as.s.data : "unhandled exception";
            if (data != NULL) {
                set_eval_diag_with_data(S, ctx->eval_current_form,
                                        kind_str, code_str, msg_str,
                                        data, NULL);
            } else {
                prim_throw_classified(S, kind_str, code_str, msg_str);
            }
        } else if (exc != NULL && mino_type_of(exc) == MINO_STRING) {
            char msg[512];
            snprintf(msg, sizeof(msg), "unhandled exception: %.*s",
                     (int)exc->as.s.len, exc->as.s.data);
            prim_throw_classified(S, "user", "MUS001", msg);
        } else {
            prim_throw_classified(S, "user", "MUS001",
                "unhandled exception");
        }
        *ok = 0;
        return 0;
    }

    case OP_NTH_VEC: {
        unsigned a = A_OF(ins);
        unsigned b = B_OF(ins);
        unsigned cc = C_OF(ins);
        mino_val_t *coll = regs[b];
        mino_val_t *idx_v = regs[cc];
        if (coll != NULL && mino_type_of(coll) == MINO_VECTOR
            && idx_v != NULL && MINO_IS_INT(idx_v)) {
            long long idx = MINO_INT_VAL(idx_v);
            if (idx >= 0 && (size_t)idx < coll->as.vec.len) {
                regs[a] = vec_nth(coll, (size_t)idx);
                return 1;
            }
        }
        mino_val_t *list = mino_nil(S);
        list = mino_cons(S, idx_v, list);
        list = mino_cons(S, coll, list);
        if (list == NULL) { *ok = 0; return 0; }
        mino_val_t *r = prim_nth(S, list, env);
        if (r == NULL) { *ok = 0; return 0; }
        regs = S->bc_regs + base;
        regs[a] = r;
        return 1;
    }

    case OP_EMPTY_VEC: {
        unsigned a = A_OF(ins);
        unsigned b = B_OF(ins);
        mino_val_t *coll = regs[b];
        if (coll != NULL && mino_type_of(coll) == MINO_VECTOR) {
            regs[a] = coll->as.vec.len == 0
                        ? mino_true(S) : mino_false(S);
            return 1;
        }
        mino_val_t *list = mino_nil(S);
        list = mino_cons(S, coll, list);
        if (list == NULL) { *ok = 0; return 0; }
        mino_val_t *r = prim_empty_p(S, list, env);
        if (r == NULL) { *ok = 0; return 0; }
        regs = S->bc_regs + base;
        regs[a] = r;
        return 1;
    }

    case OP_CONJ_VEC: {
        unsigned a = A_OF(ins);
        unsigned b = B_OF(ins);
        unsigned cc = C_OF(ins);
        mino_val_t *coll = regs[b];
        mino_val_t *item = regs[cc];
        if (coll != NULL && mino_type_of(coll) == MINO_VECTOR) {
            mino_val_t *r = vec_conj1(S, coll, item);
            if (r == NULL) { *ok = 0; return 0; }
            regs = S->bc_regs + base;
            regs[a] = r;
            return 1;
        }
        mino_val_t *list = mino_nil(S);
        list = mino_cons(S, item, list);
        list = mino_cons(S, coll, list);
        if (list == NULL) { *ok = 0; return 0; }
        mino_val_t *r = prim_conj(S, list, env);
        if (r == NULL) { *ok = 0; return 0; }
        regs = S->bc_regs + base;
        regs[a] = r;
        return 1;
    }

    case OP_DISSOC: {
        unsigned a  = A_OF(ins);
        unsigned b  = B_OF(ins);
        unsigned cc = C_OF(ins);
        mino_val_t *coll = regs[b];
        mino_val_t *key  = regs[cc];
        if (coll != NULL && mino_type_of(coll) == MINO_MAP) {
            mino_val_t *r = mino_map_dissoc1(S, coll, key);
            if (r == NULL) { *ok = 0; return 0; }
            regs = S->bc_regs + base;
            regs[a] = r;
            return 1;
        }
        mino_val_t *list = mino_nil(S);
        list = mino_cons(S, key, list);
        list = mino_cons(S, coll, list);
        if (list == NULL) { *ok = 0; return 0; }
        mino_val_t *r = prim_dissoc(S, list, env);
        if (r == NULL) { *ok = 0; return 0; }
        regs = S->bc_regs + base;
        regs[a] = r;
        return 1;
    }

    default:
        *ok = 0;
        return 0;
    }
}


/* Dispatch loop body extracted from mino_bc_run so a future caller
 * can resume execution at an arbitrary PC with a pre-populated regs
 * window. The caller owns the per-fn snapshots (try_depth, bc_catch_depth,
 * dyn_stack) used for bounds checks in the cold-op handler. Returns 1
 * on a normal completion (with *retval_out set), 0 on error. The env
 * pointer is in/out because OP_PUSH_ENV / OP_POP_ENV mutate it via
 * bc_cold_op's env_p. */
static int bc_run_dispatch_from(mino_state_t *S, const mino_bc_fn_t *bc,
                                size_t base, mino_thread_ctx_t *ctx,
                                mino_env_t **env_p, size_t start_pc,
                                mino_val_t **retval_out,
                                int saved_try_depth,
                                int saved_bc_catch_depth,
                                dyn_frame_t *saved_dyn_stack)
{
    mino_val_t **regs = S->bc_regs + base;
    const mino_bc_insn_t *code = bc->code;
    mino_env_t *env = *env_p;
    size_t pc = start_pc;
    mino_val_t *retval = NULL;
    int ok = 1;
#ifdef MINO_BC_OP_COUNTS
    /* Per-frame previous-op tracker so bigram counts only span adjacent
     * dispatches within the same bytecode stream. Sentinel = OP__COUNT. */
    unsigned prev_op = OP__COUNT;
#endif

    while (pc < bc->code_len) {
        /* Refresh the window pointer every cycle. Any op that can
         * trigger user code (OP_CALL/TAILCALL via apply_callable,
         * OP_GETGLOBAL via eval_impl, OP_CLOSURE via make_fn that
         * may collect, OP_SETGLOBAL via var_intern) can cascade into
         * a recursive mino_bc_run that grows S->bc_regs and frees
         * the prior buffer. Recomputing from base on each iteration
         * keeps the window pointer correct without per-op clutter. */
        regs = S->bc_regs + base;
        /* Publish the about-to-execute pc to the bc cursor. Error
         * paths that fire from primitives invoked by this op resolve
         * a precise source span via mino_bc_source_lookup against
         * (bc_current_bc, bc_current_pc). One write per opcode, well
         * inside per-cycle noise on every benchmark measured. */
        ctx->bc_current_pc = pc;
        mino_bc_insn_t ins = code[pc++];
        unsigned op = OP_OF(ins);
#ifdef MINO_BC_OP_COUNTS
        if (!g_op_counts_atexit_registered) {
            atexit(op_counts_dump);
            g_op_counts_atexit_registered = 1;
        }
        if (op < OP__COUNT) {
            g_op_counts[op]++;
            if (prev_op < OP__COUNT) g_op_bigrams[prev_op][op]++;
            prev_op = op;
        }
#endif
        switch (op) {
        case OP_MOVE: {
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            regs[a] = regs[b];
            break;
        }

        case OP_LOAD_K: {
            unsigned a  = A_OF(ins);
            unsigned bx = Bx_OF(ins);
            if (bx >= bc->consts_len) { ok = 0; goto dispatch_done; }
            regs[a] = bc->consts[bx];
            break;
        }

        case OP_GETGLOBAL_CACHED: {
            /* Inline cache for global symbol resolution. The slot is
             * filled on first miss with (cached, gen=S->ic_gen) and
             * re-read while the gen still matches; bumps to ic_gen
             * (def / ns-unmap / var_set_root / var_unintern)
             * invalidate naturally. The shared ic_resolve_global
             * helper carries the dyn / env / cache / resolve cascade
             * and the on-miss write-barrier refill; this case is the
             * pure-read consumer (no apply afterwards) so any
             * resolved value -- shadowed dyn, lexical env hit, or
             * cached var -- is written to regs[a] uniformly. */
            unsigned a  = A_OF(ins);
            unsigned bx = Bx_OF(ins);
            if ((int)bx >= bc->ic_slots_len) { ok = 0; goto dispatch_done; }
            mino_bc_ic_slot_t *slot = &bc->ic_slots[bx];
            int dyn_active = (ctx->dyn_stack != NULL);
            mino_val_t *v = ic_resolve_global(S, bc, slot, env, dyn_active);
            if (v == NULL) { ok = 0; goto dispatch_done; }
            regs[a] = v;
            break;
        }

        case OP_JMP: {
            int off = sBx_OF(ins);
            pc = (size_t)((long)pc + off);
            /* Backward jump: poll for cancel + auto-yield. Forward
             * jumps skip the poll since they don't form a loop. */
            if (off < 0 && !mino_bc_safepoint(S)) {
                ok = 0; goto dispatch_done;
            }
            break;
        }

        case OP_JMPIFNOT: {
            unsigned a = A_OF(ins);
            int off = sBx_OF(ins);
            if (!mino_is_truthy_inline(regs[a])) {
                pc = (size_t)((long)pc + off);
                if (off < 0 && !mino_bc_safepoint(S)) {
                    ok = 0; goto dispatch_done;
                }
            }
            break;
        }

        case OP_CALL_CACHED: {
            /* Fused (resolve-global + call) for sites whose head is an
             * unqualified or qualified global symbol with no static
             * local binding. Shares ic_resolve_global with the
             * read-only OP_GETGLOBAL_CACHED consumer: same dyn / env /
             * cache / resolve cascade, same write-barrier refill on
             * miss. Two-word encoding: word-1 carries A=arg_base /
             * B=argc / C=dst, word-2 carries the slot index in Bx
             * and is consumed via pc++ below so the main dispatch
             * never sees it. Args live at regs[A..A+B-1] -- no fn-reg
             * shift since the callee comes from the slot, not a
             * register. */
            unsigned a    = A_OF(ins);   /* arg_base */
            unsigned argn = B_OF(ins);   /* argc     */
            unsigned ret  = C_OF(ins);   /* dst      */
            if (pc >= bc->code_len) { ok = 0; goto dispatch_done; }
            mino_bc_insn_t slot_word = code[pc++];
            unsigned slot_idx = Bx_OF(slot_word);
            if ((int)slot_idx >= bc->ic_slots_len) {
                ok = 0; goto dispatch_done;
            }
            mino_bc_ic_slot_t *slot = &bc->ic_slots[slot_idx];
            int dyn_active = (ctx->dyn_stack != NULL);
            mino_val_t *callee = ic_resolve_global(S, bc, slot, env,
                                                    dyn_active);
            if (callee == NULL) { ok = 0; goto dispatch_done; }
#ifdef MINO_CALL_SITE_SHAPES
            call_shape_record(slot, callee, regs + a, (int)argn);
#endif
            mino_val_t *r = apply_callable_argv(S, callee, regs + a,
                                                (int)argn, env);
            if (r == NULL) { ok = 0; goto dispatch_done; }
            S->bc_regs[base + ret] = r;
            break;
        }

        case OP_PROTOCOL_CALL_CACHED:
        case OP_PROTOCOL_TAILCALL_CACHED: {
            /* Protocol-method dispatch fast lane. Shares
             * ic_resolve_protocol with the tail variant: deref the
             * captured atom (one pointer load), compute the
             * type-discriminator for the first argument, pointer-
             * compare against the cached (atom_map, type_disc) state,
             * and on hit return the cached impl directly. On miss the
             * helper performs map_get_val with a :default fallback,
             * refills the IC under write barriers, or surfaces the
             * MPR001 / MPR002 diagnostic for no-impl / bad-table
             * shape. Tail variant invokes the impl directly via
             * apply_callable_argv -- self-tail-recursive protocol
             * methods grow the C stack linearly, a deliberate
             * trade-off against the cons-spine build the
             * MINO_TAIL_CALL sentinel path would otherwise pay. */
            int is_tail = (OP_OF(ins) == OP_PROTOCOL_TAILCALL_CACHED);
            unsigned a    = A_OF(ins);
            unsigned argn = B_OF(ins);
            unsigned ret  = C_OF(ins);
            if (pc >= bc->code_len) { ok = 0; goto dispatch_done; }
            mino_bc_insn_t slot_word = code[pc++];
            unsigned slot_idx = Bx_OF(slot_word);
            if ((int)slot_idx >= bc->ic_slots_len) {
                ok = 0; goto dispatch_done;
            }
            mino_bc_ic_slot_t *slot = &bc->ic_slots[slot_idx];
            if (argn < 1 || slot->atom == NULL
                || mino_type_of(slot->atom) != MINO_ATOM) {
                ok = 0; goto dispatch_done;
            }
            mino_val_t *impl = mino_bc_ic_resolve_protocol(S, bc, slot, regs[a]);
            if (impl == NULL) { ok = 0; goto dispatch_done; }
            mino_val_t *r = apply_callable_argv(S, impl, regs + a,
                                                (int)argn, env);
            if (r == NULL) { ok = 0; goto dispatch_done; }
            if (is_tail) {
                retval = r;
                goto dispatch_done;
            }
            S->bc_regs[base + ret] = r;
            break;
        }

        case OP_TAILCALL: {
            unsigned a    = A_OF(ins);
            unsigned argn = B_OF(ins);
            mino_val_t *callee = regs[a];
            mino_val_t *args   = args_from_regs(S, regs + a + 1, argn);
            if (args == NULL) { ok = 0; goto dispatch_done; }
            /* Hand off via the MINO_TAIL_CALL sentinel; the outer
             * apply_callable trampoline picks up the new (fn, args)
             * without growing the C stack. The sentinel's args field
             * stays in cons-format for legacy callers that read it
             * directly; the trampoline inside apply_callable_argv
             * walks it back to argv for bc-FN targets. */
            S->tail_call_sentinel.as.tail_call.fn   = callee;
            S->tail_call_sentinel.as.tail_call.args = args;
            retval = &S->tail_call_sentinel;
            goto dispatch_done;
        }

        case OP_RETURN: {
            unsigned a = A_OF(ins);
            retval = regs[a];
            goto dispatch_done;
        }

        case OP_ADD_II:
        case OP_SUB_II:
        case OP_MUL_II:
        case OP_LT_II:
        case OP_LE_II:
        case OP_GT_II:
        case OP_GE_II:
        case OP_EQ_II:
        case OP_MOD_II:
        case OP_QUOT_II:
        case OP_REM_II:
        case OP_BAND_II:
        case OP_BOR_II:
        case OP_BXOR_II:
        case OP_SHL_II:
        case OP_SHR_II:
        case OP_USHR_II: {
            /* Speculative int+int fast lanes for the binary arith /
             * compare / bitwise / div-class ops. On a type miss or a
             * bail (div-by-zero, shift-out-of-range, MIN/-1 overflow)
             * we fall through to the corresponding prim with the same
             * argv ABI as a regular OP_CALL so the prim raises the
             * Clojure-correct diagnostic or promotes through the
             * numeric tower. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            unsigned c = C_OF(ins);
            unsigned subop;
            mino_val_t *(*fallback)(mino_state_t *, mino_val_t *, mino_env_t *);
            switch (op) {
            case OP_ADD_II:  subop = BINOP_ADD;  fallback = prim_add; break;
            case OP_SUB_II:  subop = BINOP_SUB;  fallback = prim_sub; break;
            case OP_MUL_II:  subop = BINOP_MUL;  fallback = prim_mul; break;
            case OP_LT_II:   subop = BINOP_LT;   fallback = prim_lt;  break;
            case OP_LE_II:   subop = BINOP_LE;   fallback = prim_lte; break;
            case OP_GT_II:   subop = BINOP_GT;   fallback = prim_gt;  break;
            case OP_GE_II:   subop = BINOP_GE;   fallback = prim_gte; break;
            case OP_EQ_II:   subop = BINOP_EQ;   fallback = prim_eq;  break;
            case OP_MOD_II:  subop = BINOP_MOD;  fallback = prim_mod; break;
            case OP_QUOT_II: subop = BINOP_QUOT; fallback = prim_quot; break;
            case OP_REM_II:  subop = BINOP_REM;  fallback = prim_rem; break;
            case OP_BAND_II: subop = BINOP_BAND; fallback = prim_bit_and; break;
            case OP_BOR_II:  subop = BINOP_BOR;  fallback = prim_bit_or;  break;
            case OP_BXOR_II: subop = BINOP_BXOR; fallback = prim_bit_xor; break;
            case OP_SHL_II:  subop = BINOP_SHL;  fallback = prim_bit_shift_left; break;
            case OP_SHR_II:  subop = BINOP_SHR;  fallback = prim_bit_shift_right; break;
            case OP_USHR_II: subop = BINOP_USHR; fallback = prim_unsigned_bit_shift_right; break;
            default: ok = 0; goto dispatch_done;
            }
            mino_val_t *r = binop_int_fast(S, regs[b], regs[c], subop);
            if (r == NULL) {
                mino_val_t *list = mino_nil(S);
                list = mino_cons(S, regs[c], list);
                if (list == NULL) { ok = 0; goto dispatch_done; }
                list = mino_cons(S, regs[b], list);
                if (list == NULL) { ok = 0; goto dispatch_done; }
                r = fallback(S, list, env);
                if (r == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
            }
            regs[a] = r;
            break;
        }

        case OP_INC_I:
        case OP_DEC_I:
        case OP_ZERO_INT_P:
        case OP_POS_P_I:
        case OP_NEG_P_I:
        case OP_EVEN_P_I:
        case OP_ODD_P_I:
        case OP_BNOT_I: {
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            unsigned subop;
            mino_val_t *(*fallback)(mino_state_t *, mino_val_t *, mino_env_t *);
            switch (op) {
            case OP_INC_I:      subop = UNOP_INC;    fallback = prim_inc;    break;
            case OP_DEC_I:      subop = UNOP_DEC;    fallback = prim_dec;    break;
            case OP_ZERO_INT_P: subop = UNOP_ZERO_P; fallback = prim_zero_p; break;
            case OP_POS_P_I:    subop = UNOP_POS_P;  fallback = prim_pos_p;  break;
            case OP_NEG_P_I:    subop = UNOP_NEG_P;  fallback = prim_neg_p;  break;
            case OP_EVEN_P_I:   subop = UNOP_EVEN_P; fallback = prim_even_p; break;
            case OP_ODD_P_I:    subop = UNOP_ODD_P;  fallback = prim_odd_p;  break;
            case OP_BNOT_I:     subop = UNOP_BNOT;   fallback = prim_bit_not; break;
            default: ok = 0; goto dispatch_done;
            }
            mino_val_t *r = unop_int_fast(S, regs[b], subop);
            if (r == NULL) {
                mino_val_t *list = mino_nil(S);
                list = mino_cons(S, regs[b], list);
                if (list == NULL) { ok = 0; goto dispatch_done; }
                r = fallback(S, list, env);
                if (r == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
            }
            regs[a] = r;
            break;
        }

        case OP_LOOP_INT_DEC: {
            /* Fused counted-loop step (single binding):
             *   if regs[A] == 0: fall through (exit branch follows).
             *   else: regs[A]-- and re-fetch (pc-=1).
             * Hot path: tagged-int test, in-range decrement, single
             * back-jump. Cold paths (non-int test, MIN_INT decrement)
             * delegate to prim_zero_p / prim_dec so the user-visible
             * diagnostic ("zero? requires a number", "integer
             * overflow") fires exactly as the unfused emission
             * would have. */
            unsigned a = A_OF(ins);
            mino_val_t *v = regs[a];
            if (v != NULL && MINO_IS_INT(v)) {
                long long t = MINO_INT_VAL(v);
                if (t == 0) break;
                if (t != MINO_INT_MIN) {
                    regs[a] = MINO_MAKE_INT(t - 1);
                    pc -= 1;
                    if (!mino_bc_safepoint(S)) { ok = 0; goto dispatch_done; }
                    break;
                }
                /* MIN_INT: fall through to the prim_dec slow path so
                 * the throw fires. */
            }
            /* Slow path: call prim_zero_p first to decide the branch
             * and to surface any non-number diagnostic. Then on
             * non-zero, call prim_dec which raises on overflow. */
            {
                mino_val_t *list = mino_nil(S);
                list = mino_cons(S, regs[a], list);
                if (list == NULL) { ok = 0; goto dispatch_done; }
                mino_val_t *zp = prim_zero_p(S, list, env);
                if (zp == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                if (mino_is_truthy(zp)) {
                    /* Fall through to the exit branch (no recur). */
                    break;
                }
                mino_val_t *list2 = mino_nil(S);
                list2 = mino_cons(S, regs[a], list2);
                if (list2 == NULL) { ok = 0; goto dispatch_done; }
                mino_val_t *decv = prim_dec(S, list2, env);
                if (decv == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                regs[a] = decv;
                pc -= 1;
                if (!mino_bc_safepoint(S)) { ok = 0; goto dispatch_done; }
                break;
            }
        }

        case OP_LOOP_INT_DEC_INC: {
            /* Fused counted-loop step (two bindings). Hot path is the
             * tagged-int / in-range case; everything else delegates to
             * prim_zero_p / prim_dec / prim_inc so the
             * non-number / overflow diagnostics still fire. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            mino_val_t *vt = regs[a];
            mino_val_t *vi = regs[b];
            if (vt != NULL && vi != NULL
                && MINO_IS_INT(vt) && MINO_IS_INT(vi)) {
                long long t = MINO_INT_VAL(vt);
                if (t == 0) break;
                long long i = MINO_INT_VAL(vi);
                if (t != MINO_INT_MIN && i != MINO_INT_MAX) {
                    regs[a] = MINO_MAKE_INT(t - 1);
                    regs[b] = MINO_MAKE_INT(i + 1);
                    pc -= 1;
                    if (!mino_bc_safepoint(S)) { ok = 0; goto dispatch_done; }
                    break;
                }
                /* Overflow on dec or inc: fall through to the prim
                 * slow path so the throw fires. */
            }
            {
                mino_val_t *list = mino_nil(S);
                list = mino_cons(S, regs[a], list);
                if (list == NULL) { ok = 0; goto dispatch_done; }
                mino_val_t *zp = prim_zero_p(S, list, env);
                if (zp == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                if (mino_is_truthy(zp)) break;
                mino_val_t *list2 = mino_nil(S);
                list2 = mino_cons(S, regs[a], list2);
                if (list2 == NULL) { ok = 0; goto dispatch_done; }
                mino_val_t *decv = prim_dec(S, list2, env);
                if (decv == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                mino_val_t *list3 = mino_nil(S);
                list3 = mino_cons(S, regs[b], list3);
                if (list3 == NULL) { ok = 0; goto dispatch_done; }
                mino_val_t *incv = prim_inc(S, list3, env);
                if (incv == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                regs[a] = decv;
                regs[b] = incv;
                pc -= 1;
                if (!mino_bc_safepoint(S)) { ok = 0; goto dispatch_done; }
                break;
            }
        }

        case OP_LOOP_INT_LT: {
            /* Forward-counted single-binding loop step:
             *   if regs[A] < regs[B]: regs[A]++ and back-jump
             *   else: fall through to the compiled exit branch.
             * Hot path is the tagged-int in-range case. Cold paths
             * (non-int operand, MAX_INT counter) delegate to prim_lt /
             * prim_inc so the canonical diagnostic still fires. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            mino_val_t *vc = regs[a];
            mino_val_t *vl = regs[b];
            if (vc != NULL && vl != NULL
                && MINO_IS_INT(vc) && MINO_IS_INT(vl)) {
                long long c_ = MINO_INT_VAL(vc);
                long long l_ = MINO_INT_VAL(vl);
                if (c_ >= l_) break;
                if (c_ != MINO_INT_MAX) {
                    regs[a] = MINO_MAKE_INT(c_ + 1);
                    pc -= 1;
                    if (!mino_bc_safepoint(S)) { ok = 0; goto dispatch_done; }
                    break;
                }
                /* MAX_INT: fall through to the prim_inc slow path so
                 * the overflow throw fires. */
            }
            {
                mino_val_t *list = mino_nil(S);
                list = mino_cons(S, regs[b], list);
                list = mino_cons(S, regs[a], list);
                if (list == NULL) { ok = 0; goto dispatch_done; }
                mino_val_t *ltv = prim_lt(S, list, env);
                if (ltv == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                if (!mino_is_truthy(ltv)) break;
                mino_val_t *list2 = mino_nil(S);
                list2 = mino_cons(S, regs[a], list2);
                if (list2 == NULL) { ok = 0; goto dispatch_done; }
                mino_val_t *incv = prim_inc(S, list2, env);
                if (incv == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                regs[a] = incv;
                pc -= 1;
                if (!mino_bc_safepoint(S)) { ok = 0; goto dispatch_done; }
                break;
            }
        }

        case OP_LOOP_INT_LT_INC: {
            /* Forward-counted two-binding loop step: counter A < limit
             * B, with carry C incremented in lockstep with A. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            unsigned c = C_OF(ins);
            mino_val_t *vc = regs[a];
            mino_val_t *vl = regs[b];
            mino_val_t *vk = regs[c];
            if (vc != NULL && vl != NULL && vk != NULL
                && MINO_IS_INT(vc) && MINO_IS_INT(vl)
                && MINO_IS_INT(vk)) {
                long long c_ = MINO_INT_VAL(vc);
                long long l_ = MINO_INT_VAL(vl);
                if (c_ >= l_) break;
                long long k_ = MINO_INT_VAL(vk);
                if (c_ != MINO_INT_MAX && k_ != MINO_INT_MAX) {
                    regs[a] = MINO_MAKE_INT(c_ + 1);
                    regs[c] = MINO_MAKE_INT(k_ + 1);
                    pc -= 1;
                    if (!mino_bc_safepoint(S)) { ok = 0; goto dispatch_done; }
                    break;
                }
            }
            {
                mino_val_t *list = mino_nil(S);
                list = mino_cons(S, regs[b], list);
                list = mino_cons(S, regs[a], list);
                if (list == NULL) { ok = 0; goto dispatch_done; }
                mino_val_t *ltv = prim_lt(S, list, env);
                if (ltv == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                if (!mino_is_truthy(ltv)) break;
                mino_val_t *list2 = mino_nil(S);
                list2 = mino_cons(S, regs[a], list2);
                if (list2 == NULL) { ok = 0; goto dispatch_done; }
                mino_val_t *incv = prim_inc(S, list2, env);
                if (incv == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                mino_val_t *list3 = mino_nil(S);
                list3 = mino_cons(S, regs[c], list3);
                if (list3 == NULL) { ok = 0; goto dispatch_done; }
                mino_val_t *incv2 = prim_inc(S, list3, env);
                if (incv2 == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                regs[a] = incv;
                regs[c] = incv2;
                pc -= 1;
                if (!mino_bc_safepoint(S)) { ok = 0; goto dispatch_done; }
                break;
            }
        }

        case OP_GET_KW_MAP: {
            /* Fast lane for (get coll k) on maps and records.
             * For MINO_MAP: any hashable key (string, keyword, int,
             * symbol, ...) routes through mino_map_lookup. For
             * MINO_RECORD: declared-fields are interned by keyword
             * identity, so the slot index lookup requires a keyword
             * key; other key types fall through to prim_get which
             * scans the optional ext-map. Misses fall back to prim_get
             * so non-map / non-record collections, sorted-maps,
             * 3-arg default forms, and records carrying the key in
             * the ext-map keep their full semantics. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            unsigned cc = C_OF(ins);
            mino_val_t *coll = regs[b];
            mino_val_t *key  = regs[cc];
            if (coll != NULL && key != NULL) {
                int t = mino_type_of(coll);
                /* Transient unwrap to the persistent backing collection
                 * so the bump-5k reducer-body shape `(get tcoll k)` hits
                 * the same MAP / VECTOR inline path the persistent
                 * lookup uses. An invalidated transient (sealed via
                 * persistent! and then read) falls through to prim_get
                 * for the diagnostic. */
                if (t == MINO_TRANSIENT && coll->as.transient.valid) {
                    mino_val_t *inner = coll->as.transient.current;
                    if (inner != NULL) {
                        int it = mino_type_of(inner);
                        if (it == MINO_MAP) {
                            mino_val_t *v = map_get_val(inner, key);
                            regs[a] = v == NULL ? mino_nil(S) : v;
                            break;
                        }
                        if (it == MINO_VECTOR && mino_val_int_p(key)) {
                            long long idx = mino_val_int_get(key);
                            if (idx >= 0
                                && (size_t)idx < inner->as.vec.len) {
                                regs[a] = vec_nth(inner, (size_t)idx);
                                break;
                            }
                            /* Out-of-range idx on a transient vector
                             * returns nil per Clojure's (get t-vec i)
                             * contract. */
                            regs[a] = mino_nil(S);
                            break;
                        }
                    }
                }
                if (t == MINO_MAP) {
                    mino_val_t *v = map_get_val(coll, key);
                    regs[a] = v == NULL ? mino_nil(S) : v;
                    break;
                }
                if (t == MINO_RECORD && mino_type_of(key) == MINO_KEYWORD) {
                    int idx = record_field_index(coll, key);
                    if (idx >= 0) {
                        regs[a] = coll->as.record.vals[idx];
                        break;
                    }
                    /* Keyword isn't a declared field. The slow path
                     * also checks the optional ext-map and returns
                     * nil for an absent key, so we route there
                     * instead of returning nil ourselves. */
                }
            }
            mino_val_t *list = mino_nil(S);
            list = mino_cons(S, key, list);
            list = mino_cons(S, coll, list);
            if (list == NULL) { ok = 0; goto dispatch_done; }
            mino_val_t *r = prim_get(S, list, env);
            if (r == NULL) { ok = 0; goto dispatch_done; }
            regs = S->bc_regs + base;
            regs[a] = r;
            break;
        }

        case OP_FIRST_VEC: {
            /* Fast lane for (first v) on a vector. Empty vector
             * returns nil; non-empty returns vec_nth(coll, 0). Any
             * other type (lazy, chunked-cons, string, map, set,
             * sorted-coll, host-array, map-entry, nil/empty-list)
             * falls back through prim_first so the full semantics --
             * lazy-seq force, string code-point decode, map-entry
             * key, etc. -- stay intact. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            mino_val_t *coll = regs[b];
            if (coll != NULL && mino_type_of(coll) == MINO_VECTOR) {
                regs[a] = coll->as.vec.len == 0
                            ? mino_nil(S)
                            : vec_nth(coll, 0);
                break;
            }
            mino_val_t *list = mino_nil(S);
            list = mino_cons(S, coll, list);
            if (list == NULL) { ok = 0; goto dispatch_done; }
            mino_val_t *r = prim_first(S, list, env);
            if (r == NULL) { ok = 0; goto dispatch_done; }
            regs = S->bc_regs + base;
            regs[a] = r;
            break;
        }

        case OP_COUNT_VEC: {
            /* Fast lane for (count v) on a vector. Returns the
             * vector's .len as a tagged int directly; tag_or_box_int
             * handles the rare path where .len overflows the tagged
             * range. Other coll types fall through to prim_count so
             * lazy-seq walk, string code-point count, map / set / host
             * array len read, etc. stay correct. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            mino_val_t *coll = regs[b];
            if (coll != NULL && mino_type_of(coll) == MINO_VECTOR) {
                regs[a] = tag_or_box_int(S, (long long)coll->as.vec.len);
                if (regs[a] == NULL) { ok = 0; goto dispatch_done; }
                break;
            }
            mino_val_t *list = mino_nil(S);
            list = mino_cons(S, coll, list);
            if (list == NULL) { ok = 0; goto dispatch_done; }
            mino_val_t *r = prim_count(S, list, env);
            if (r == NULL) { ok = 0; goto dispatch_done; }
            regs = S->bc_regs + base;
            regs[a] = r;
            break;
        }

        case OP_ASSOC: {
            /* Fast lane for the 3-arg (assoc coll k v) shape. The
             * compiler arranges three consecutive registers starting
             * at B for [coll, k, v]; A is the destination. Two fast
             * paths fire: vector (when coll is MINO_VECTOR and k is
             * a tagged int with 0 <= idx <= len; equality with len
             * triggers the conj-style append that vec_assoc1
             * handles transparently) and map (when coll is
             * MINO_MAP). Any other shape -- sorted-map, record,
             * transient, non-int vec key, out-of-range vec idx,
             * variadic forms -- falls back to prim_assoc which
             * raises the Clojure-correct diagnostic. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            mino_val_t *coll = regs[b];
            mino_val_t *k    = regs[b + 1];
            mino_val_t *v    = regs[b + 2];
            if (coll != NULL && k != NULL) {
                int t = mino_type_of(coll);
                if (t == MINO_VECTOR
                    && MINO_IS_INT(k)) {
                    long long idx = MINO_INT_VAL(k);
                    if (idx >= 0 && (size_t)idx <= coll->as.vec.len) {
                        mino_val_t *r = vec_assoc1(S, coll, (size_t)idx, v);
                        if (r == NULL) { ok = 0; goto dispatch_done; }
                        regs = S->bc_regs + base;
                        regs[a] = r;
                        break;
                    }
                }
                if (t == MINO_MAP) {
                    mino_val_t *r = mino_map_assoc1(S, coll, k, v);
                    if (r == NULL) { ok = 0; goto dispatch_done; }
                    regs = S->bc_regs + base;
                    regs[a] = r;
                    break;
                }
            }
            /* Miss: cons args head-first and call prim_assoc. */
            mino_val_t *list = mino_nil(S);
            list = mino_cons(S, v, list);
            list = mino_cons(S, k, list);
            list = mino_cons(S, coll, list);
            if (list == NULL) { ok = 0; goto dispatch_done; }
            mino_val_t *r = prim_assoc(S, list, env);
            if (r == NULL) { ok = 0; goto dispatch_done; }
            regs = S->bc_regs + base;
            regs[a] = r;
            break;
        }

        case OP_ASSOC_BANG: {
            /* Fast lane for the 3-arg (assoc! tcoll k v) shape used in
             * the transient reducer bodies the builder rewrite emits.
             * Three consecutive registers at B carry [tcoll, k, v]; A
             * is the destination. The fast path requires a valid
             * transient and calls mino_assoc_bang directly. Anything
             * else -- invalidated transient, persistent coll, variadic
             * arity -- falls back to prim_assoc_bang which raises the
             * Clojure-correct diagnostic. */
            unsigned a = A_OF(ins);
            unsigned b = B_OF(ins);
            mino_val_t *coll = regs[b];
            mino_val_t *k    = regs[b + 1];
            mino_val_t *v    = regs[b + 2];
            if (coll != NULL
                && mino_type_of(coll) == MINO_TRANSIENT
                && coll->as.transient.valid) {
                mino_val_t *r = mino_assoc_bang(S, coll, k, v);
                if (r == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                regs[a] = r;
                break;
            }
            mino_val_t *list = mino_nil(S);
            list = mino_cons(S, v, list);
            list = mino_cons(S, k, list);
            list = mino_cons(S, coll, list);
            if (list == NULL) { ok = 0; goto dispatch_done; }
            mino_val_t *r = prim_assoc_bang(S, list, env);
            if (r == NULL) { ok = 0; goto dispatch_done; }
            regs = S->bc_regs + base;
            regs[a] = r;
            break;
        }

        case OP_CONJ_BANG: {
            /* (conj! tcoll x) arity-2 transient fast lane. Valid
             * transient routes to mino_conj_bang directly; miss falls
             * through to prim_conj_bang. */
            unsigned a  = A_OF(ins);
            unsigned b  = B_OF(ins);
            unsigned cc = C_OF(ins);
            mino_val_t *coll = regs[b];
            mino_val_t *item = regs[cc];
            if (coll != NULL
                && mino_type_of(coll) == MINO_TRANSIENT
                && coll->as.transient.valid) {
                mino_val_t *r = mino_conj_bang(S, coll, item);
                if (r == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                regs[a] = r;
                break;
            }
            mino_val_t *list = mino_nil(S);
            list = mino_cons(S, item, list);
            list = mino_cons(S, coll, list);
            if (list == NULL) { ok = 0; goto dispatch_done; }
            mino_val_t *r = prim_conj_bang(S, list, env);
            if (r == NULL) { ok = 0; goto dispatch_done; }
            regs = S->bc_regs + base;
            regs[a] = r;
            break;
        }

        case OP_DISSOC_BANG: {
            /* (dissoc! tcoll k) arity-2 transient fast lane. */
            unsigned a  = A_OF(ins);
            unsigned b  = B_OF(ins);
            unsigned cc = C_OF(ins);
            mino_val_t *coll = regs[b];
            mino_val_t *key  = regs[cc];
            if (coll != NULL
                && mino_type_of(coll) == MINO_TRANSIENT
                && coll->as.transient.valid) {
                mino_val_t *r = mino_dissoc_bang(S, coll, key);
                if (r == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                regs[a] = r;
                break;
            }
            mino_val_t *list = mino_nil(S);
            list = mino_cons(S, key, list);
            list = mino_cons(S, coll, list);
            if (list == NULL) { ok = 0; goto dispatch_done; }
            mino_val_t *r = prim_dissoc_bang(S, list, env);
            if (r == NULL) { ok = 0; goto dispatch_done; }
            regs = S->bc_regs + base;
            regs[a] = r;
            break;
        }

        case OP_DISJ_BANG: {
            /* (disj! tcoll x) arity-2 transient fast lane. */
            unsigned a  = A_OF(ins);
            unsigned b  = B_OF(ins);
            unsigned cc = C_OF(ins);
            mino_val_t *coll = regs[b];
            mino_val_t *item = regs[cc];
            if (coll != NULL
                && mino_type_of(coll) == MINO_TRANSIENT
                && coll->as.transient.valid) {
                mino_val_t *r = mino_disj_bang(S, coll, item);
                if (r == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
                regs[a] = r;
                break;
            }
            mino_val_t *list = mino_nil(S);
            list = mino_cons(S, item, list);
            list = mino_cons(S, coll, list);
            if (list == NULL) { ok = 0; goto dispatch_done; }
            mino_val_t *r = prim_disj_bang(S, list, env);
            if (r == NULL) { ok = 0; goto dispatch_done; }
            regs = S->bc_regs + base;
            regs[a] = r;
            break;
        }

        case OP_ADD_IK:
        case OP_SUB_IK:
        case OP_LT_IK:
        case OP_LE_IK:
        case OP_EQ_IK: {
            /* Immediate-operand variants: lhs in B reg, signed 8-bit
             * imm in C. The imm is by-construction an int (compile-time
             * literal), so only the lhs register needs a tag check. On
             * a tag miss we synthesize the literal back into a tagged
             * int and reuse the existing prim fallback path. */
            unsigned a    = A_OF(ins);
            unsigned b    = B_OF(ins);
            long long imm = (long long)(int8_t)C_OF(ins);
            mino_val_t *lhs = regs[b];
            mino_val_t *r;
            if (MINO_IS_INT(lhs)) {
                long long la = MINO_INT_VAL(lhs);
                long long out;
                switch (op) {
                case OP_ADD_IK:
#if defined(__GNUC__) || defined(__clang__)
                    if (__builtin_saddll_overflow(la, imm, &out)) { r = NULL; break; }
#else
                    out = la + imm;
#endif
                    r = tag_or_box_int(S, out); break;
                case OP_SUB_IK:
#if defined(__GNUC__) || defined(__clang__)
                    if (__builtin_ssubll_overflow(la, imm, &out)) { r = NULL; break; }
#else
                    out = la - imm;
#endif
                    r = tag_or_box_int(S, out); break;
                case OP_LT_IK: r = (la <  imm) ? mino_true(S) : mino_false(S); break;
                case OP_LE_IK: r = (la <= imm) ? mino_true(S) : mino_false(S); break;
                case OP_EQ_IK: r = (la == imm) ? mino_true(S) : mino_false(S); break;
                default: ok = 0; goto dispatch_done;
                }
            } else {
                r = NULL;
            }
            if (r == NULL) {
                /* Fallback path: rebuild a cons-spine arg list with the
                 * literal as a freshly-tagged int and call the prim. */
                mino_val_t *(*fallback)(mino_state_t *, mino_val_t *, mino_env_t *);
                mino_val_t *list, *imv;
                switch (op) {
                case OP_ADD_IK: fallback = prim_add; break;
                case OP_SUB_IK: fallback = prim_sub; break;
                case OP_LT_IK:  fallback = prim_lt;  break;
                case OP_LE_IK:  fallback = prim_lte; break;
                case OP_EQ_IK:  fallback = prim_eq;  break;
                default: ok = 0; goto dispatch_done;
                }
                imv  = mino_int(S, imm);
                if (imv == NULL) { ok = 0; goto dispatch_done; }
                list = mino_cons(S, imv, mino_nil(S));
                if (list == NULL) { ok = 0; goto dispatch_done; }
                list = mino_cons(S, regs[b], list);
                if (list == NULL) { ok = 0; goto dispatch_done; }
                r = fallback(S, list, env);
                if (r == NULL) { ok = 0; goto dispatch_done; }
                regs = S->bc_regs + base;
            }
            regs[a] = r;
            break;
        }

        case OP_PUSHCATCH: {
            unsigned a  = A_OF(ins);
            int off     = sBx_OF(ins);
            int td      = ctx->try_depth;
            size_t hpc  = (size_t)((long)pc + off);
            if (td >= MAX_TRY_DEPTH
                || ctx->bc_catch_depth >= MAX_TRY_DEPTH) {
                /* Surface the same MLM002 diagnostic the tree-walker
                 * raises at this cap. Without the explicit set, a
                 * deeply recursive `(try ... (catch ...))` body just
                 * unwinds with no message and the user sees a silent
                 * NULL. */
                set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                              "limit", "MLM002", "try nesting too deep");
                ok = 0; goto dispatch_done;
            }
            /* Record the BC-side resume state BEFORE the setjmp call.
             * The longjmp-return branch only reads from S/ctx; no
             * post-setjmp writes need to survive the longjmp. The
             * local `td` is intentionally NOT used after setjmp -- a
             * sibling PUSHCATCH frame may share its stack slot, so by
             * the time longjmp lands we cannot trust `td` to still
             * carry our value. `bc_catch_stack[d].try_depth_at_push`
             * holds the same value in heap-backed storage. */
            ctx->bc_catch_stack[ctx->bc_catch_depth].handler_pc        = hpc;
            ctx->bc_catch_stack[ctx->bc_catch_depth].reg_window_base   = base;
            ctx->bc_catch_stack[ctx->bc_catch_depth].try_depth_at_push = td;
            ctx->bc_catch_stack[ctx->bc_catch_depth].ex_reg            = a;
            ctx->bc_catch_stack[ctx->bc_catch_depth].env_at_push       = env;
            ctx->bc_catch_stack[ctx->bc_catch_depth].dyn_stack_at_push = ctx->dyn_stack;
            ctx->bc_catch_depth++;

            ctx->try_stack[td].exception      = NULL;
            ctx->try_stack[td].saved_ns       = S->current_ns;
            ctx->try_stack[td].saved_ambient  = S->fn_ambient_ns;
            ctx->try_stack[td].saved_load_len = S->load_stack_len;

            if (setjmp(ctx->try_stack[td].buf) == 0) {
                /* Normal entry: arm the try frame and run the body. */
                ctx->try_depth = td + 1;
            } else {
                /* longjmp landed here: a throw inside the body (BC or
                 * tree-walker callee) targeted our setjmp. Recover the
                 * VM state from the catch entry, drop the try frame,
                 * stash the normalized exception in ex_reg, and resume
                 * at the handler pc. Locals modified between setjmp
                 * and longjmp (pc, env, regs, retval, ok) are
                 * overwritten here; base / bc / code / match never
                 * change after fn entry so they survive untouched.
                 *
                 * Restore ctx->bc_current_bc to this fn -- an inner
                 * BC fn that threw may have left it pointing at the
                 * inner fn (whose mino_bc_run never reached its
                 * normal exit-time restore). Without this, any code
                 * reading bc_current_bc on the catch-handler side
                 * (including normalize_exception's mino_bc_source_lookup
                 * for :mino/location) would dereference a stale
                 * pointer once the inner fn's allocation is freed. */
                ctx->bc_current_bc = bc;
                ctx->bc_current_pc = (size_t)ctx->bc_catch_stack[ctx->bc_catch_depth - 1].handler_pc;
                int d         = --ctx->bc_catch_depth;
                int my_td     = ctx->bc_catch_stack[d].try_depth_at_push;
                mino_val_t *ex = ctx->try_stack[my_td].exception;
                S->current_ns    = ctx->try_stack[my_td].saved_ns;
                S->fn_ambient_ns = ctx->try_stack[my_td].saved_ambient;
                load_stack_truncate(S, ctx->try_stack[my_td].saved_load_len);
                ctx->try_depth = my_td;
                /* Pop any dyn frames that the body PUSHDYN'd but never
                 * POPDYN'd because the throw bypassed the matching
                 * cleanup. Matches eval_try's saved_dyn unwind so a
                 * `(binding [...] (throw ...))` body doesn't leave its
                 * binding visible to the catch handler. */
                {
                    dyn_frame_t *anchor =
                        ctx->bc_catch_stack[d].dyn_stack_at_push;
                    while (ctx->dyn_stack != anchor) {
                        dyn_frame_t *f = ctx->dyn_stack;
                        if (f == NULL) break;
                        ctx->dyn_stack = f->prev;
                        dyn_binding_list_free(f->bindings);
                        free(f);
                    }
                }
                pc      = ctx->bc_catch_stack[d].handler_pc;
                env     = ctx->bc_catch_stack[d].env_at_push;
                regs    = S->bc_regs + base;
                regs[ctx->bc_catch_stack[d].ex_reg] =
                    normalize_exception(S, ex);
                retval  = NULL;
                ok      = 1;
                clear_error(S);
            }
            break;
        }

        default:
            if (!bc_cold_op(S, bc, base, ctx,
                            saved_try_depth, saved_bc_catch_depth,
                            saved_dyn_stack,
                            ins, op, &env, &ok)) {
                goto dispatch_done;
            }
            break;
        }
    }

dispatch_done:
    *env_p = env;
    *retval_out = retval;
    return ok;
}


mino_val_t *mino_bc_run(mino_state_t *S, mino_val_t *fn_val,
                        mino_val_t **argv, int argc, mino_env_t *env)
{
    const mino_bc_fn_t *bc = fn_val->as.fn.bc;
    if (bc == NULL || bc->code == NULL) return NULL;
    if (bc->n_clauses <= 0 || bc->clauses == NULL) return NULL;

    /* Select a clause whose arity matches argc. Prefer fixed-arity
     * matches over variadic ones (Clojure semantics: the most-specific
     * clause wins). If two clauses share the same min arity we pick
     * the first in source order.
     *
     * Single-clause fast path: the vast majority of fns have one
     * clause, so peel that case off here. Saves two pointer
     * dereferences + two branches per call vs walking the
     * generic loop. Hot for recursion-heavy workloads like fib. */
    const mino_bc_clause_t *match = NULL;
    if (__builtin_expect(bc->n_clauses == 1, 1)) {
        const mino_bc_clause_t *cl = &bc->clauses[0];
        if (!cl->has_rest && cl->n_params == argc) {
            match = cl;
        } else if (cl->has_rest && argc >= cl->n_params) {
            match = cl;
        }
    } else {
        for (int i = 0; i < bc->n_clauses; i++) {
            const mino_bc_clause_t *cl = &bc->clauses[i];
            if (!cl->has_rest && cl->n_params == argc) { match = cl; break; }
        }
        if (match == NULL) {
            for (int i = 0; i < bc->n_clauses; i++) {
                const mino_bc_clause_t *cl = &bc->clauses[i];
                if (cl->has_rest && argc >= cl->n_params) { match = cl; break; }
            }
        }
    }
    if (match == NULL) {
        /* No matching clause: surface the same MAR002 diagnostic the
         * tree-walker's dispatch_multi_arity raises so callers see
         * "no matching arity for N args" instead of a silent NULL
         * that propagates up as an "unhandled exception" with no
         * message. Name the callee when the in-progress form's head
         * is a symbol so the user sees which fn / macro mismatched
         * rather than a bare arity message. */
        char              msg[256];
        char              name_buf[128] = {0};
        const mino_val_t *cur = mino_current_ctx(S)->eval_current_form;
        if (cur != NULL && mino_is_cons(cur)) {
            mino_val_t *head = cur->as.cons.car;
            if (head != NULL && mino_type_of(head) == MINO_SYMBOL
                && head->as.s.len > 0
                && head->as.s.len < sizeof(name_buf) - 4) {
                snprintf(name_buf, sizeof(name_buf), " `%.*s`",
                         (int)head->as.s.len, head->as.s.data);
            }
        }
        snprintf(msg, sizeof(msg),
                 "no matching arity%s for %d args", name_buf, argc);
        set_eval_diag(S, mino_current_ctx(S)->eval_current_form,
                      "eval/arity", "MAR002", msg);
        return NULL;
    }

    size_t base = bc_push_window(S, bc->n_regs);
    if (base == (size_t)-1) return NULL;

    for (int i = 0; i < match->n_params; i++) {
        S->bc_regs[base + (size_t)i] = argv[i];
    }
    /* Collect overflow args into a list and place it in the slot
     * right after the fixed params. mino_cons walks back-to-front so
     * we get the values in their original order. When argc ==
     * n_params the rest binding is the empty list. */
    if (match->has_rest) {
        mino_val_t *rest = mino_nil(S);
        for (int i = argc - 1; i >= match->n_params; i--) {
            rest = mino_cons(S, argv[i], rest);
            if (rest == NULL) { bc_pop_window(S, base); return NULL; }
        }
        S->bc_regs[base + (size_t)match->n_params] = rest;
    }

    /* When the body contains an inner fn literal or a (lazy-seq ...),
     * extend the lexical env with a fresh child and publish the
     * matched clause's params into it. */
    if (bc->captures) {
        env = env_child(S, env);
        if (env == NULL) { bc_pop_window(S, base); return NULL; }
        for (int i = 0; i < match->n_params; i++) {
            mino_val_t *p = vec_nth(match->params_vec, (size_t)i);
            if (p == NULL || mino_type_of(p) != MINO_SYMBOL) {
                bc_pop_window(S, base);
                return NULL;
            }
            env_bind_sym(S, env, p, argv[i]);
        }
        if (match->has_rest) {
            mino_val_t *rest_sym = vec_nth(match->params_vec,
                match->params_vec->as.vec.len - 1);
            if (rest_sym != NULL && mino_type_of(rest_sym) == MINO_SYMBOL) {
                env_bind_sym(S, env, rest_sym,
                    S->bc_regs[base + (size_t)match->n_params]);
            }
        }
    }

    mino_val_t **regs = S->bc_regs + base;
    size_t pc = (size_t)match->entry_pc;
    mino_val_t *retval = NULL;
    int ok = 1;

    /* Save the try-state snapshot at fn entry so any abnormal exit
     * (early goto bc_done while a PUSHCATCH is still live, or a fn
     * that body-faults inside a try) rolls bc_catch_depth and
     * try_depth back to where they were before this fn ran. A
     * leaked frame would leave a stale setjmp landing pad pointing
     * into this stack frame after we return, and the next longjmp
     * up the chain would jump to garbage. Gated on bc->has_try so
     * bodies that never emit PUSHCATCH / POPCATCH / THROW skip the
     * load+store pair on every call -- the cleanup at bc_done is
     * gated on the same flag and the dummy saved values are then
     * never read. */
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    int saved_try_depth        = 0;
    int saved_bc_catch_depth   = 0;
    if (bc->has_try) {
        saved_try_depth      = ctx->try_depth;
        saved_bc_catch_depth = ctx->bc_catch_depth;
    }
    /* Anchor the dyn_stack at fn entry so bc_done can unwind any
     * OP_PUSHDYN frames that survived an early exit (NULL return /
     * tail-call sentinel / catch landing). The body either matches
     * each PUSHDYN with a POPDYN -- the normal path -- or one of
     * those error paths kicks in and the cleanup below frees the
     * orphaned frames. Mirrors the longjmp-unwind loop in
     * control.c's eval_try. */
    dyn_frame_t       *saved_dyn_stack    = ctx->dyn_stack;
    /* Save/restore the bc cursor so a recursive mino_bc_run leaves the
     * outer caller's cursor intact on return. bc_done restores both
     * fields on every exit path. */
    const mino_bc_fn_t *saved_bc_current  = ctx->bc_current_bc;
    size_t              saved_bc_current_pc = ctx->bc_current_pc;
    ctx->bc_current_bc = bc;
    ctx->bc_current_pc = (size_t)match->entry_pc;

#ifdef MINO_CPJIT
    /* Native-tier fast path. The JIT compiles only a narrow shape
     * today (linear bodies of OP_MOVE / OP_LOAD_K / OP_RETURN with a
     * single arity and no captures); mino_jit_eligible gates the
     * compile so the field is only set for shapes the patched code
     * actually handles. Bail back to the interpreter when the
     * ic_gen snapshot is stale -- a def / ns-unmap / var_set_root
     * has invalidated the JIT'd code's globally-cached resolutions
     * since the page was emitted. */
    /* JIT enters at pc=0 only. Multi-arity bodies whose matched
     * clause starts at a non-zero entry_pc fall through to the
     * interpreter -- the JIT region's first stencil owns the
     * ARM64 function prologue, so a mid-region entry would skip
     * the callee-saved register saves and corrupt the caller's
     * frame on epilogue. */
    if (bc->native != NULL && bc->native_gen == S->ic_gen
        && match->entry_pc == 0) {
        /* Publish the per-fn snapshots so mino_jit_invoke can pass
         * them to mino_bc_run_resume on a side-exit deopt. The locals
         * here are the values at fn entry; the JIT prefix may push
         * additional try / dyn frames before deopt fires, and the
         * resume's cleanup must roll back to these anchors. */
        S->jit_resume_saved_try_depth      = saved_try_depth;
        S->jit_resume_saved_bc_catch_depth = saved_bc_catch_depth;
        S->jit_resume_saved_dyn_stack      = saved_dyn_stack;
        retval = mino_jit_invoke(S, (mino_bc_fn_t *)bc, regs,
                                 (mino_val_t **)bc->consts, env);
        ok = (retval != NULL);
        goto bc_done;
    }
#endif

    ok = bc_run_dispatch_from(S, bc, base, ctx, &env, pc, &retval,
                              saved_try_depth, saved_bc_catch_depth,
                              saved_dyn_stack);

bc_done:
    /* Roll any BC catch frames that survived back so the try_stack is
     * exactly as it was at fn entry. Normal POPCATCH paths balance the
     * count on their own; this only kicks in on error / unwind paths.
     * Gated on bc->has_try -- bodies that never emit PUSHCATCH /
     * POPCATCH / THROW cannot have grown either counter past its
     * entry value, so the rollback would be a no-op anyway. */
    if (bc->has_try) {
        if (ctx->bc_catch_depth > saved_bc_catch_depth) {
            ctx->bc_catch_depth = saved_bc_catch_depth;
        }
        if (ctx->try_depth > saved_try_depth) {
            ctx->try_depth = saved_try_depth;
        }
    }
    /* Unwind any dyn frames that the body PUSHDYN'd but didn't POP --
     * happens on NULL-return error paths, tail-call sentinel paths,
     * and catch landing pads (which restore try state but leave the
     * dyn frames pushed by the failing body). Mirrors the unwind loop
     * in control.c's eval_try longjmp branch. */
    while (ctx->dyn_stack != saved_dyn_stack) {
        dyn_frame_t *f = ctx->dyn_stack;
        if (f == NULL) break;
        ctx->dyn_stack = f->prev;
        dyn_binding_list_free(f->bindings);
        free(f);
    }
    bc_pop_window(S, base);
    ctx->bc_current_bc = saved_bc_current;
    ctx->bc_current_pc = saved_bc_current_pc;
    if (!ok) return NULL;
    return retval != NULL ? retval : mino_nil(S);
}

/* Side-exit resume entry. Called from mino_jit_invoke when the JIT'd
 * native region took the deopt-to-interp exit. The caller (mino_jit_invoke)
 * owns the regs window push, dyn / try / cursor snapshots, and the
 * cleanup tail -- those live in the outer mino_bc_run frame that
 * invoked the JIT in the first place. This entry only drives the
 * dispatch loop from `pc` over the existing window and returns the
 * result; nothing here pushes / pops state. */
mino_val_t *mino_bc_run_resume(mino_state_t *S, mino_bc_fn_t *bc,
                                size_t base, mino_env_t *env, size_t pc,
                                int saved_try_depth,
                                int saved_bc_catch_depth,
                                dyn_frame_t *saved_dyn_stack)
{
    if (bc == NULL || bc->code == NULL) return NULL;
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    mino_val_t *retval = NULL;
    int ok = bc_run_dispatch_from(S, bc, base, ctx, &env, pc, &retval,
                                  saved_try_depth, saved_bc_catch_depth,
                                  saved_dyn_stack);
    if (!ok) return NULL;
    return retval != NULL ? retval : mino_nil(S);
}

/* JIT-call fast lane. Skips mino_bc_run's clause matcher + the
 * captures branch + the bc_current_pc-per-op write that the bytecode
 * dispatch loop owns. The preconditions in the header docstring
 * are what make this safe: single-clause, fixed-arity, captures-free
 * bodies have no work to do between arg copy and mino_jit_invoke.
 *
 * Any precondition miss falls through to mino_bc_run -- a stale IC
 * slot, a fold-driven recompile that flipped clause count, or a
 * fn whose JIT compile lost a race against a redef will all land
 * on the safe path and produce the same answer the interpreter
 * would. */
mino_val_t *mino_bc_run_known_native(mino_state_t *S, mino_val_t *fn_val,
                                      mino_val_t **argv, int argc,
                                      mino_env_t *env)
{
#ifdef MINO_CPJIT
    const mino_bc_fn_t *bc = fn_val->as.fn.bc;
    if (bc == NULL || bc->code == NULL) goto fallback;
    if (bc->native == NULL || bc->native_gen != S->ic_gen) goto fallback;
    if (bc->n_clauses != 1 || bc->clauses == NULL) goto fallback;
    const mino_bc_clause_t *cl = &bc->clauses[0];
    if (cl->has_rest) goto fallback;
    if (cl->n_params != argc) goto fallback;
    if (cl->entry_pc != 0) goto fallback;
    if (bc->captures) goto fallback;

    size_t base = bc_push_window(S, bc->n_regs);
    if (base == (size_t)-1) return NULL;
    for (int i = 0; i < argc; i++) {
        S->bc_regs[base + (size_t)i] = argv[i];
    }

    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    int saved_try_depth      = 0;
    int saved_bc_catch_depth = 0;
    if (bc->has_try) {
        saved_try_depth      = ctx->try_depth;
        saved_bc_catch_depth = ctx->bc_catch_depth;
    }
    dyn_frame_t       *saved_dyn_stack   = ctx->dyn_stack;
    const mino_bc_fn_t *saved_bc_current = ctx->bc_current_bc;
    size_t              saved_bc_pc      = ctx->bc_current_pc;
    ctx->bc_current_bc = bc;
    ctx->bc_current_pc = 0;

    /* Publish the per-fn snapshots so mino_jit_invoke can pass them
     * to mino_bc_run_resume on a side-exit deopt. The locals captured
     * above are the values at fn entry; the resume's cleanup tail
     * lives back in this function below, and bc_run_dispatch_from's
     * cold-op handler reads them for POPCATCH / POPDYN bounds checks. */
    S->jit_resume_saved_try_depth      = saved_try_depth;
    S->jit_resume_saved_bc_catch_depth = saved_bc_catch_depth;
    S->jit_resume_saved_dyn_stack      = saved_dyn_stack;
    mino_val_t *retval = mino_jit_invoke(S, (mino_bc_fn_t *)bc,
                                          S->bc_regs + base,
                                          (mino_val_t **)bc->consts, env);

    if (bc->has_try) {
        if (ctx->bc_catch_depth > saved_bc_catch_depth) {
            ctx->bc_catch_depth = saved_bc_catch_depth;
        }
        if (ctx->try_depth > saved_try_depth) {
            ctx->try_depth = saved_try_depth;
        }
    }
    while (ctx->dyn_stack != saved_dyn_stack) {
        dyn_frame_t *f = ctx->dyn_stack;
        if (f == NULL) break;
        ctx->dyn_stack = f->prev;
        dyn_binding_list_free(f->bindings);
        free(f);
    }
    bc_pop_window(S, base);
    ctx->bc_current_bc = saved_bc_current;
    ctx->bc_current_pc = saved_bc_pc;
    if (retval == NULL) return NULL;
    return retval;
fallback:
#endif
    return mino_bc_run(S, fn_val, argv, argc, env);
}

void mino_bc_fn_mark(mino_state_t *S, const mino_bc_fn_t *bc)
{
    (void)S; (void)bc;
}
