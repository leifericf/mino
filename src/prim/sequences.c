/*
 * sequences.c -- sequence iteration and higher-order primitives:
 *                     seq, realized?, seq_iter_*, reduce, reduced,
 *                     into, apply, reverse, sort, set, rangev, mapv,
 *                     filterv, peek, pop, find, empty, rseq,
 *                     sorted-map, sorted-set.
 */

#include "prim/internal.h"
#include "runtime/host_threads.h"
#include "collections/internal.h"   /* make_fn */

#ifdef MINO_REDUCE_STEP_COUNTS
#include <stdio.h>
#include <stdlib.h>

/* Tally of reduce_step invocations bucketed by reducer prim identity.
 * Populated when the binary is built with -DMINO_REDUCE_STEP_COUNTS=1.
 * Dumped to stderr at process exit. Used to gate item A of the pre-JIT
 * sweep: confirm canonical numeric reducers (+, *, etc.) dominate
 * real-workload reduce_step traffic. Not for production. */
static size_t g_rstep_add;
static size_t g_rstep_mul;
static size_t g_rstep_sub;
static size_t g_rstep_band;
static size_t g_rstep_bor;
static size_t g_rstep_bxor;
static size_t g_rstep_other_prim;
static size_t g_rstep_user_fn;
static int    g_rstep_atexit_done;

static void rstep_counts_dump(void)
{
    size_t total = g_rstep_add + g_rstep_mul + g_rstep_sub
                 + g_rstep_band + g_rstep_bor + g_rstep_bxor
                 + g_rstep_other_prim + g_rstep_user_fn;
    fprintf(stderr, "reduce-step-counts: total = %zu\n", total);
    if (total == 0) return;
#define ROW(name, c) \
    fprintf(stderr, "  %-20s %12zu  %6.2f%%\n", (name), (c), \
            100.0 * (double)(c) / (double)total)
    ROW("prim_add",      g_rstep_add);
    ROW("prim_mul",      g_rstep_mul);
    ROW("prim_sub",      g_rstep_sub);
    ROW("prim_bit_and",  g_rstep_band);
    ROW("prim_bit_or",   g_rstep_bor);
    ROW("prim_bit_xor",  g_rstep_bxor);
    ROW("other_prim",    g_rstep_other_prim);
    ROW("user_fn",       g_rstep_user_fn);
#undef ROW
}

static void rstep_count(mino_val *fn)
{
    if (!g_rstep_atexit_done) {
        atexit(rstep_counts_dump);
        g_rstep_atexit_done = 1;
    }
    if (fn != NULL && mino_type_of(fn) == MINO_PRIM
        && fn->as.prim.fn != NULL) {
        mino_prim_fn p = fn->as.prim.fn;
        if      (p == prim_add)     g_rstep_add++;
        else if (p == prim_mul)     g_rstep_mul++;
        else if (p == prim_sub)     g_rstep_sub++;
        else if (p == prim_bit_and) g_rstep_band++;
        else if (p == prim_bit_or)  g_rstep_bor++;
        else if (p == prim_bit_xor) g_rstep_bxor++;
        else                        g_rstep_other_prim++;
    } else {
        g_rstep_user_fn++;
    }
}
#endif


/* ------------------------------------------------------------------------- */
/* Sequence iterator                                                         */
/* ------------------------------------------------------------------------- */

void seq_iter_init(mino_state *S, seq_iter_t *it, const mino_val *coll)
{
    /* Force lazy seqs so they behave as cons lists. */
    if (coll != NULL && mino_type_of(coll) == MINO_LAZY) {
        coll = lazy_force(S, (mino_val *)coll);
    }
    /* Sorted collections: flatten to cons list for uniform iteration. */
    if (coll != NULL &&
        (mino_type_of(coll) == MINO_SORTED_MAP || mino_type_of(coll) == MINO_SORTED_SET)) {
        coll = sorted_seq(S, coll);
    }
    /* Records: route through prim_seq which builds a cons list of
     * [k v] pairs in declared-field-then-ext-insertion order. Without
     * this, seq_iter_done returns true immediately for MINO_RECORD
     * (default case), so `(into {} record)` returned `{}` and any
     * other seq-based aggregate over a record came back empty. */
    if (coll != NULL && mino_type_of(coll) == MINO_RECORD) {
        mino_val *args = mino_cons(S, (mino_val *)coll, mino_nil(S));
        coll = prim_seq(S, args, NULL);
        if (coll != NULL && mino_type_of(coll) == MINO_LAZY) {
            coll = lazy_force(S, (mino_val *)coll);
        }
    }
    /* Queues: flatten to a cons list in deque order. The seq_iter
     * switches don't handle MINO_QUEUE directly. */
    if (coll != NULL && mino_type_of(coll) == MINO_QUEUE) {
        if (mino_queue_count(coll) == 0) {
            coll = NULL;
        } else {
            coll = mino_queue_seq(S, coll);
        }
    }
    /* Bytes: flatten to a cons list of unsigned 0..255 ints. The
     * seq_iter switches don't handle MINO_BYTES directly. */
    if (coll != NULL && mino_type_of(coll) == MINO_BYTES) {
        if (mino_bytes_len(coll) == 0) {
            coll = NULL;
        } else {
            coll = mino_bytes_seq(S, coll);
        }
    }
    it->coll  = coll;
    it->idx   = 0;
    it->cons_p = (coll != NULL && mino_type_of(coll) == MINO_CONS) ? coll : NULL;
    if (coll != NULL && mino_type_of(coll) == MINO_CHUNKED_CONS) {
        it->cons_p = coll;
        it->idx    = (size_t)coll->as.chunked_cons.off;
    }
}

int seq_iter_done(const seq_iter_t *it)
{
    const mino_val *c = it->coll;
    if (c == NULL || mino_type_of(c) == MINO_NIL) return 1;
    switch (mino_type_of(c)) {
    case MINO_CONS:   return it->cons_p == NULL || mino_type_of(it->cons_p) != MINO_CONS;
    case MINO_CHUNKED_CONS:
        return it->cons_p == NULL || mino_type_of(it->cons_p) != MINO_CHUNKED_CONS;
    case MINO_VECTOR:     return it->idx >= c->as.vec.len;
    case MINO_HOST_ARRAY: return it->idx >= c->as.host_array.len;
    case MINO_MAP_ENTRY:  return it->idx >= 2;
    case MINO_MAP:    return it->idx >= c->as.map.len;
    case MINO_SET:    return it->idx >= c->as.set.len;
    case MINO_STRING: return it->idx >= c->as.s.len;
    default:          return 1;
    }
}

/* Decode the UTF-8 codepoint at byte position pos in `data` (length
 * len bytes), writing the decoded codepoint to *cp_out and the byte
 * step to *step_out. Falls back to a 1-byte literal for malformed
 * leading bytes or truncated continuations, matching prim_seq's
 * lenient string walk. Caller must ensure pos < len. */
static void utf8_step(const unsigned char *data, size_t len, size_t pos,
                      unsigned int *cp_out, size_t *step_out)
{
    unsigned int b = data[pos];
    unsigned int cp;
    size_t       n;
    if (b < 0x80u)                 { cp = b;          n = 1; }
    else if ((b & 0xE0u) == 0xC0u) { cp = b & 0x1Fu;  n = 2; }
    else if ((b & 0xF0u) == 0xE0u) { cp = b & 0x0Fu;  n = 3; }
    else if ((b & 0xF8u) == 0xF0u) { cp = b & 0x07u;  n = 4; }
    else                           { cp = b;          n = 1; }
    if (pos + n > len) { cp = b; n = 1; }
    else {
        size_t k;
        int    valid = 1;
        for (k = 1; k < n; k++) {
            unsigned int c2 = data[pos + k];
            if ((c2 & 0xC0u) != 0x80u) { valid = 0; break; }
            cp = (cp << 6) | (c2 & 0x3Fu);
        }
        if (!valid) { cp = b; n = 1; }
    }
    *cp_out   = cp;
    *step_out = n;
}

mino_val *seq_iter_val(mino_state *S, const seq_iter_t *it)
{
    const mino_val *c = it->coll;
    switch (mino_type_of(c)) {
    case MINO_CONS:   return it->cons_p->as.cons.car;
    case MINO_CHUNKED_CONS: {
        const mino_val *cell = it->cons_p;
        const mino_val *ch   = cell->as.chunked_cons.chunk;
        return ch->as.chunk.vals[it->idx];
    }
    case MINO_VECTOR: return vec_nth(c, it->idx);
    case MINO_HOST_ARRAY: return c->as.host_array.vals[it->idx];
    case MINO_MAP_ENTRY:
        return it->idx == 0 ? c->as.map_entry.k : c->as.map_entry.v;
    case MINO_MAP: {
        /* Yield MINO_MAP_ENTRY (one alloc) instead of a 2-vector
         * (header + 2-slot trie node, two allocs). Equal to [k v]
         * across mino_eq's cross-type sequential path, prints as
         * `[k v]`, and `vector?` is true -- the same contract
         * prim_seq's seq_kv_pair already uses for maps. */
        mino_val *key = vec_nth(c->as.map.key_order, it->idx);
        mino_val *val = c->as.map.val_order != NULL
                              ? vec_nth(c->as.map.val_order, it->idx)
                              : map_get_val(c, key);
        return mino_map_entry(S, key, val);
    }
    case MINO_SET:    return vec_nth(c->as.set.key_order, it->idx);
    case MINO_STRING: {
        /* Yield a MINO_CHAR per codepoint, matching prim_seq. */
        unsigned int cp;
        size_t       step;
        utf8_step((const unsigned char *)c->as.s.data, c->as.s.len,
                  it->idx, &cp, &step);
        (void)step;
        return mino_char(S, (int)cp);
    }
    default:          return mino_nil(S);
    }
}

void seq_iter_next(mino_state *S, seq_iter_t *it)
{
    if (it->coll != NULL && mino_type_of(it->coll) == MINO_CONS) {
        if (it->cons_p != NULL && mino_type_of(it->cons_p) == MINO_CONS) {
            const mino_val *next = it->cons_p->as.cons.cdr;
            /* Force lazy tail if present. */
            if (next != NULL && mino_type_of(next) == MINO_LAZY) {
                next = lazy_force(S, (mino_val *)next);
            }
            it->cons_p = next;
        }
    } else if (it->coll != NULL && mino_type_of(it->coll) == MINO_CHUNKED_CONS) {
        if (it->cons_p != NULL && mino_type_of(it->cons_p) == MINO_CHUNKED_CONS) {
            const mino_val *cell = it->cons_p;
            const mino_val *ch   = cell->as.chunked_cons.chunk;
            unsigned          next_idx = (unsigned)(it->idx + 1);
            if (next_idx < ch->as.chunk.len) {
                it->idx = next_idx;
                return;
            }
            {
                const mino_val *more = cell->as.chunked_cons.more;
                if (more != NULL && mino_type_of(more) == MINO_LAZY) {
                    more = lazy_force(S, (mino_val *)more);
                }
                if (more != NULL && mino_type_of(more) == MINO_CONS) {
                    /* Switch dispatch to cons-mode for the rest of the
                     * walk. */
                    it->coll   = more;
                    it->cons_p = more;
                    it->idx    = 0;
                } else if (more != NULL
                           && mino_type_of(more) == MINO_CHUNKED_CONS) {
                    it->cons_p = more;
                    it->idx    = (size_t)more->as.chunked_cons.off;
                } else {
                    it->cons_p = NULL;
                }
            }
        }
    } else if (it->coll != NULL && mino_type_of(it->coll) == MINO_STRING) {
        /* Step by the current codepoint's byte length so multi-byte
         * UTF-8 characters advance correctly. */
        unsigned int cp;
        size_t       step;
        utf8_step((const unsigned char *)it->coll->as.s.data,
                  it->coll->as.s.len, it->idx, &cp, &step);
        (void)cp;
        it->idx += step;
    } else {
        it->idx++;
    }
}

/* ------------------------------------------------------------------------- */
/* Sequence primitives (strict — no lazy seqs)                               */
/* ------------------------------------------------------------------------- */

/* (reduced val) — wrap val to signal early termination in reduce. */
static mino_val *prim_reduced(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "reduced requires exactly 1 argument");
    }
    v = alloc_val(S, MINO_REDUCED);
    v->as.reduced.val = args->as.cons.car;
    return v;
}

/* (reduced? x) — true if x is a reduced wrapper. */
static mino_val *prim_reduced_p(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "reduced? requires exactly 1 argument");
    }
    return (args->as.cons.car != NULL && mino_type_of(args->as.cons.car) == MINO_REDUCED)
        ? mino_true(S) : mino_false(S);
}

/* Reducer-kind tag for the inline int-acc fast lane shared across
 * reduce_int_range, reduce_pipeline_walk, reduce_vec_direct, and the
 * seq_iter fallback. NONE means "no canonical int reducer; bail to
 * the generic apply_callable path". */
typedef enum {
    REDUCE_KIND_NONE = 0,
    REDUCE_KIND_ADD,
    REDUCE_KIND_MUL,
    REDUCE_KIND_SUB,
    REDUCE_KIND_BAND,
    REDUCE_KIND_BOR,
    REDUCE_KIND_BXOR
} reduce_int_kind_t;

static reduce_int_kind_t reduce_int_kind_from_fn(mino_val *fn)
{
    if (fn == NULL || mino_type_of(fn) != MINO_PRIM) return REDUCE_KIND_NONE;
    const char *name = fn->as.prim.name;
    if (name == NULL)               return REDUCE_KIND_NONE;
    if (strcmp(name, "+")       == 0) return REDUCE_KIND_ADD;
    if (strcmp(name, "*")       == 0) return REDUCE_KIND_MUL;
    if (strcmp(name, "-")       == 0) return REDUCE_KIND_SUB;
    if (strcmp(name, "bit-and") == 0) return REDUCE_KIND_BAND;
    if (strcmp(name, "bit-or")  == 0) return REDUCE_KIND_BOR;
    if (strcmp(name, "bit-xor") == 0) return REDUCE_KIND_BXOR;
    return REDUCE_KIND_NONE;
}

/* Inline tagged-int reducer step. Returns 1 on success (acc updated),
 * 0 on miss (overflow / non-int reducer-kind combination). Marked
 * inline so leaf-loop callers (reduce_ctx_step) get the switch
 * dispatch folded into their hot path. */
static inline int reduce_step_int_acc(reduce_int_kind_t kind,
                                       long long *acc_io, long long elem)
{
    long long r;
    switch (kind) {
    case REDUCE_KIND_ADD:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_add_overflow(*acc_io, elem, &r)) return 0;
        *acc_io = r;
#else
        *acc_io += elem;
#endif
        return 1;
    case REDUCE_KIND_MUL:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_mul_overflow(*acc_io, elem, &r)) return 0;
        *acc_io = r;
#else
        *acc_io *= elem;
#endif
        return 1;
    case REDUCE_KIND_SUB:
#if defined(__GNUC__) || defined(__clang__)
        if (__builtin_sub_overflow(*acc_io, elem, &r)) return 0;
        *acc_io = r;
#else
        *acc_io -= elem;
#endif
        return 1;
    case REDUCE_KIND_BAND: *acc_io &= elem; return 1;
    case REDUCE_KIND_BOR:  *acc_io |= elem; return 1;
    case REDUCE_KIND_BXOR: *acc_io ^= elem; return 1;
    default:               return 0;
    }
}

/* Specialised inner loops for reduce_int_range. The reducer kind is
 * loop-invariant, so we hoist the switch out and let each kind run a
 * tight `__builtin_*_overflow` / bitwise inner loop. Returns 0 = ok,
 * -1 = overflow (caller bails to generic path). */
static int reduce_int_range_add(long long start, long long end,
                                long long step, long long *acc_io)
{
    long long i;
    long long acc = *acc_io;
    if (step > 0) {
        for (i = start; i < end; i += step) {
#if defined(__GNUC__) || defined(__clang__)
            long long r;
            if (__builtin_add_overflow(acc, i, &r)) return -1;
            acc = r;
#else
            acc += i;
#endif
        }
    } else {
        for (i = start; i > end; i += step) {
#if defined(__GNUC__) || defined(__clang__)
            long long r;
            if (__builtin_add_overflow(acc, i, &r)) return -1;
            acc = r;
#else
            acc += i;
#endif
        }
    }
    *acc_io = acc;
    return 0;
}

static int reduce_int_range_mul(long long start, long long end,
                                long long step, long long *acc_io)
{
    long long i;
    long long acc = *acc_io;
    if (step > 0) {
        for (i = start; i < end; i += step) {
#if defined(__GNUC__) || defined(__clang__)
            long long r;
            if (__builtin_mul_overflow(acc, i, &r)) return -1;
            acc = r;
#else
            acc *= i;
#endif
        }
    } else {
        for (i = start; i > end; i += step) {
#if defined(__GNUC__) || defined(__clang__)
            long long r;
            if (__builtin_mul_overflow(acc, i, &r)) return -1;
            acc = r;
#else
            acc *= i;
#endif
        }
    }
    *acc_io = acc;
    return 0;
}

static int reduce_int_range_sub(long long start, long long end,
                                long long step, long long *acc_io)
{
    long long i;
    long long acc = *acc_io;
    if (step > 0) {
        for (i = start; i < end; i += step) {
#if defined(__GNUC__) || defined(__clang__)
            long long r;
            if (__builtin_sub_overflow(acc, i, &r)) return -1;
            acc = r;
#else
            acc -= i;
#endif
        }
    } else {
        for (i = start; i > end; i += step) {
#if defined(__GNUC__) || defined(__clang__)
            long long r;
            if (__builtin_sub_overflow(acc, i, &r)) return -1;
            acc = r;
#else
            acc -= i;
#endif
        }
    }
    *acc_io = acc;
    return 0;
}

static int reduce_int_range_band(long long start, long long end,
                                 long long step, long long *acc_io)
{
    long long i;
    long long acc = *acc_io;
    if (step > 0) for (i = start; i < end; i += step) acc &= i;
    else          for (i = start; i > end; i += step) acc &= i;
    *acc_io = acc;
    return 0;
}

static int reduce_int_range_bor(long long start, long long end,
                                long long step, long long *acc_io)
{
    long long i;
    long long acc = *acc_io;
    if (step > 0) for (i = start; i < end; i += step) acc |= i;
    else          for (i = start; i > end; i += step) acc |= i;
    *acc_io = acc;
    return 0;
}

static int reduce_int_range_bxor(long long start, long long end,
                                 long long step, long long *acc_io)
{
    long long i;
    long long acc = *acc_io;
    if (step > 0) for (i = start; i < end; i += step) acc ^= i;
    else          for (i = start; i > end; i += step) acc ^= i;
    *acc_io = acc;
    return 0;
}

/* Numeric reducer fast path: (reduce <op> [init] (range start end step))
 * where <op> is one of the canonical arithmetic prims (+, *, -, &, |, ^).
 * Walks the range as a tight integer loop, never materialising chunks
 * or boxing intermediates. Returns NULL on miss (overflow / unsupported
 * reducer / non-int init); the caller falls through to the generic
 * path. */
static mino_val *reduce_int_range(mino_state *S, mino_val *fn,
                                     mino_val *init, int has_init,
                                     long long start, long long end,
                                     long long step)
{
    long long          acc = 0;
    int                rc  = 0;
    reduce_int_kind_t  kind = reduce_int_kind_from_fn(fn);
    if (kind == REDUCE_KIND_NONE) return NULL;
    if (step == 0) return NULL;
    if (has_init) {
        if (init == NULL || !mino_val_int_p(init)) return NULL;
        acc = mino_val_int_get(init);
    } else {
        if (step > 0 ? start >= end : start <= end) {
            return apply_callable(S, fn, mino_nil(S), NULL);
        }
        acc   = start;
        start = start + step;
    }
    switch (kind) {
    case REDUCE_KIND_ADD:  rc = reduce_int_range_add (start, end, step, &acc); break;
    case REDUCE_KIND_MUL:  rc = reduce_int_range_mul (start, end, step, &acc); break;
    case REDUCE_KIND_SUB:  rc = reduce_int_range_sub (start, end, step, &acc); break;
    case REDUCE_KIND_BAND: rc = reduce_int_range_band(start, end, step, &acc); break;
    case REDUCE_KIND_BOR:  rc = reduce_int_range_bor (start, end, step, &acc); break;
    case REDUCE_KIND_BXOR: rc = reduce_int_range_bxor(start, end, step, &acc); break;
    default:               return NULL;
    }
    if (rc < 0) return NULL;
    return mino_int(S, acc);
}

/* Inner reduction step: combine acc and elem under fn, honoring the
 * int+int arithmetic fast lane and MINO_REDUCED early-exit. `kind` is
 * the caller-classified arithmetic reducer kind (or REDUCE_KIND_NONE
 * to skip the fast lane). Returns 0 = continue, 1 = stop (acc holds
 * the final), -1 = error. */
static int reduce_step(mino_state *S, mino_val *fn,
                       reduce_int_kind_t kind, mino_val **acc_io,
                       mino_val *elem, mino_env *env)
{
    mino_val *acc = *acc_io;
#ifdef MINO_REDUCE_STEP_COUNTS
    rstep_count(fn);
#endif
    if (kind != REDUCE_KIND_NONE
        && acc != NULL && mino_val_int_p(acc)
        && elem != NULL && mino_val_int_p(elem)) {
        long long ai = mino_val_int_get(acc);
        long long bi = mino_val_int_get(elem);
        long long r;
        int       handled = 0;
        switch (kind) {
        case REDUCE_KIND_ADD:
#if defined(__GNUC__) || defined(__clang__)
            if (!__builtin_add_overflow(ai, bi, &r)) {
                *acc_io = mino_int(S, r);
                handled = 1;
            }
#endif
            break;
        case REDUCE_KIND_MUL:
#if defined(__GNUC__) || defined(__clang__)
            if (!__builtin_mul_overflow(ai, bi, &r)) {
                *acc_io = mino_int(S, r);
                handled = 1;
            }
#endif
            break;
        case REDUCE_KIND_SUB:
#if defined(__GNUC__) || defined(__clang__)
            if (!__builtin_sub_overflow(ai, bi, &r)) {
                *acc_io = mino_int(S, r);
                handled = 1;
            }
#endif
            break;
        case REDUCE_KIND_BAND:
            *acc_io = mino_int(S, ai & bi); handled = 1; break;
        case REDUCE_KIND_BOR:
            *acc_io = mino_int(S, ai | bi); handled = 1; break;
        case REDUCE_KIND_BXOR:
            *acc_io = mino_int(S, ai ^ bi); handled = 1; break;
        case REDUCE_KIND_NONE:
            break;
        }
        if (handled) return 0;
    }
    {
        mino_val *argv2[2];
        argv2[0] = acc;
        argv2[1] = elem;
        *acc_io = apply_callable_argv(S, fn, argv2, 2, env);
    }
    if (*acc_io == NULL) return -1;
    if (mino_type_of(*acc_io) == MINO_REDUCED) {
        *acc_io = (*acc_io)->as.reduced.val;
        return 1;
    }
    return 0;
}

/* Unified reduce accumulator state. Carries two branches: an
 * unboxed `acc_int` for the canonical numeric reducer fast lane
 * (when `kind` is set and every element so far has been tagged-int
 * with no overflow), and a boxed `acc` for the generic path. The
 * first overflow / non-int element boxes acc_int into acc and the
 * rest of the walk runs in boxed mode. Shared across the pipeline,
 * vec/set/seq_iter reducer entry points. */
typedef struct {
    mino_val        *fn;
    reduce_int_kind_t  kind;
    long long          acc_int;
    int                has_int_acc;
    mino_val        *acc;
    int                has_init;
} reduce_ctx_t;

static void reduce_ctx_init(reduce_ctx_t *ctx, mino_val *fn,
                            mino_val *acc, int has_init)
{
    ctx->fn          = fn;
    ctx->kind        = reduce_int_kind_from_fn(fn);
    ctx->acc_int     = 0;
    ctx->has_int_acc = 0;
    ctx->acc         = acc;
    ctx->has_init    = has_init;
    if (has_init && ctx->kind != REDUCE_KIND_NONE
        && acc != NULL && mino_val_int_p(acc)) {
        ctx->has_int_acc = 1;
        ctx->acc_int     = mino_val_int_get(acc);
        ctx->acc         = NULL;
    }
}

/* Apply `elem` to the accumulator under fn. Returns 0=continue,
 * 1=stop (Reduced), -1=error. Stays on the unboxed fast lane until
 * the first non-int element or arithmetic overflow boxes acc and
 * falls through to reduce_step's generic path. */
static int reduce_ctx_step(mino_state *S, reduce_ctx_t *ctx,
                           mino_val *elem, mino_env *env)
{
    if (!ctx->has_init) {
        ctx->has_init = 1;
        if (ctx->kind != REDUCE_KIND_NONE && elem != NULL
            && mino_val_int_p(elem)) {
            ctx->has_int_acc = 1;
            ctx->acc_int     = mino_val_int_get(elem);
        } else {
            ctx->acc = elem;
        }
        return 0;
    }
    if (ctx->has_int_acc && elem != NULL && mino_val_int_p(elem)) {
        if (reduce_step_int_acc(ctx->kind, &ctx->acc_int,
                                 mino_val_int_get(elem))) {
            return 0;
        }
        /* overflow -- box and fall through to boxed path */
    }
    if (ctx->has_int_acc) {
        ctx->acc         = mino_int(S, ctx->acc_int);
        ctx->has_int_acc = 0;
    }
    return reduce_step(S, ctx->fn, ctx->kind, &ctx->acc, elem, env);
}

static mino_val *reduce_ctx_finalize(mino_state *S,
                                       reduce_ctx_t *ctx,
                                       mino_env *env)
{
    if (!ctx->has_init) {
        return apply_callable(S, ctx->fn, mino_nil(S), env);
    }
    if (ctx->has_int_acc) return mino_int(S, ctx->acc_int);
    return ctx->acc;
}

/* Direct-walk fast path for (reduce fn coll [init]) over a persistent
 * map. Yields a MINO_MAP_ENTRY per pair (cheaper than the [k v] vector
 * pair seq_iter_val once produced) and skips the seq_iter switch
 * dispatch. Iteration order matches key_order (insertion order),
 * which is the order seq_iter walks. For flatmaps we read val_order
 * in parallel; for HAMT we resolve via map_get_val. */
static mino_val *reduce_map_direct(mino_state *S, mino_val *fn,
                                     mino_val *acc, int has_init,
                                     mino_val *m, mino_env *env)
{
    size_t      i, n = m->as.map.len;
    int         is_flat = (m->as.map.val_order != NULL);
    reduce_int_kind_t kind = reduce_int_kind_from_fn(fn);
    mino_val *ko, *vo;
    if (n == 0) {
        if (has_init) return acc;
        return apply_callable(S, fn, mino_nil(S), env);
    }
    ko = m->as.map.key_order;
    vo = m->as.map.val_order;
    if (!has_init) {
        mino_val *k0 = vec_nth(ko, 0);
        mino_val *v0 = is_flat ? vec_nth(vo, 0) : map_get_val(m, k0);
        acc = mino_map_entry(S, k0, v0);
        i   = 1;
    } else {
        i = 0;
    }
    for (; i < n; i++) {
        mino_val *k = vec_nth(ko, i);
        mino_val *v = is_flat ? vec_nth(vo, i) : map_get_val(m, k);
        mino_val *entry = mino_map_entry(S, k, v);
        int rc = reduce_step(S, fn, kind, &acc, entry, env);
        if (rc == -1) return NULL;
        if (rc == 1)  return acc;
    }
    return acc;
}

/* Recursive trie walker for vector reduce. `pos_io` tracks the
 * absolute backing position visited so far so subvec offset/len
 * windows can be honored without a separate offset-aware codepath.
 * The `ctx` carries the unboxed-acc fast lane plus the boxed
 * fallback. Returns 0 = continue, 1 = stop (Reduced fired), -1 = error. */
static int reduce_vec_trie_walk(mino_state *S, reduce_ctx_t *ctx,
                                mino_env *env,
                                const mino_vec_node_t *node, unsigned shift,
                                size_t *pos_io, size_t start, size_t end)
{
    unsigned i;
    if (shift == 0) {
        /* Leaf: iterate the 32-slot block directly. When ctx carries
         * a canonical-int reducer kind and every element is tagged-int,
         * the inner loop stays in long-long arithmetic with no per-
         * element heap allocation. */
        for (i = 0; i < node->count; i++) {
            size_t p = *pos_io;
            (*pos_io)++;
            if (p >= end) return 0;
            if (p < start) continue;
            {
                int rc = reduce_ctx_step(S, ctx,
                                         (mino_val *)node->slots[i], env);
                if (rc != 0) return rc;
            }
        }
        return 0;
    }
    for (i = 0; i < node->count; i++) {
        mino_vec_node_t *child = (mino_vec_node_t *)node->slots[i];
        int rc;
        if (*pos_io >= end) return 0;
        if (child == NULL) continue;
        rc = reduce_vec_trie_walk(S, ctx, env, child,
                                  shift - MINO_VEC_B, pos_io, start, end);
        if (rc != 0) return rc;
    }
    return 0;
}

/* Walk a vector (or, indirectly, a set's key_order vector) applying
 * the unified reduce_ctx_step. Skips the seq_iter dispatch and the
 * per-element vec_nth's O(log32) trie navigation; one leaf pass visits
 * each element with a tight 32-slot inner loop. Honors offset (subvec)
 * by passing absolute backing positions into the walker. */
static mino_val *reduce_vec_apply(mino_state *S, mino_val *fn,
                                    mino_val *acc, int has_init,
                                    const mino_val *v, mino_env *env)
{
    size_t n          = v->as.vec.len;
    size_t offset     = v->as.vec.offset;
    size_t trie_count = v->as.vec.blen - v->as.vec.tail_len;
    size_t start      = offset;
    size_t end        = offset + n;
    size_t pos        = 0;
    reduce_ctx_t ctx;
    if (n == 0) {
        if (has_init) return acc;
        return apply_callable(S, fn, mino_nil(S), env);
    }
    reduce_ctx_init(&ctx, fn, acc, has_init);
    if (trie_count > 0 && v->as.vec.root != NULL && start < trie_count
        && end > 0) {
        size_t trie_end = end < trie_count ? end : trie_count;
        int rc = reduce_vec_trie_walk(S, &ctx, env, v->as.vec.root,
                                      v->as.vec.shift, &pos, start,
                                      trie_end);
        if (rc == -1) return NULL;
        if (rc == 1)  return reduce_ctx_finalize(S, &ctx, env);
    }
    /* Tail: first element sits at absolute position trie_count. */
    if (v->as.vec.tail_len > 0 && end > trie_count) {
        unsigned i;
        for (i = 0; i < v->as.vec.tail_len; i++) {
            size_t p = trie_count + i;
            if (p >= end) break;
            if (p < start) continue;
            {
                int rc = reduce_ctx_step(S, &ctx,
                                         (mino_val *)v->as.vec.tail->slots[i],
                                         env);
                if (rc == -1) return NULL;
                if (rc == 1)  return reduce_ctx_finalize(S, &ctx, env);
            }
        }
    }
    return reduce_ctx_finalize(S, &ctx, env);
}

/* Direct-walk fast path for (reduce fn vec [init]). */
static mino_val *reduce_vec_direct(mino_state *S, mino_val *fn,
                                     mino_val *acc, int has_init,
                                     mino_val *v, mino_env *env)
{
    return reduce_vec_apply(S, fn, acc, has_init, v, env);
}

/* Direct-walk fast path for (reduce fn set [init]). Routes through
 * reduce_vec_apply since the set's insertion-order key_order is
 * itself a vector. */
static mino_val *reduce_set_direct(mino_state *S, mino_val *fn,
                                     mino_val *acc, int has_init,
                                     mino_val *s, mino_env *env)
{
    if (s->as.set.len == 0) {
        if (has_init) return acc;
        return apply_callable(S, fn, mino_nil(S), env);
    }
    return reduce_vec_apply(S, fn, acc, has_init, s->as.set.key_order, env);
}

/* Transducer-shape fusion for reduce over a (->> src (map ...) (filter ...)
 * (take ...)) chain. The chain is built bottom-up by the lazy-map-1 /
 * lazy-filter / lazy-take primitives, each wrapping the previous in a
 * fresh MINO_LAZY cell. When prim_reduce sees the outermost cell of
 * such a chain, it can unwind the stages, walk the bottom source once,
 * and apply the stages inline per element — eliminating the lazy-seq
 * cells (the dominant cost: ~877 cons cells per pipeline iter in the
 * baseline profile of (->> (range 1000) (map inc) (filter odd?)
 * (take 100) (reduce + 0))).
 *
 * Soundness is automatic: the unwinder only matches LAZY cells with the
 * exact thunk pointers from lazy.c. A user-defined shadow of `map` /
 * `filter` / `take` produces LAZY cells with different thunks (or none
 * at all), the unwinder stops, and the regular slow path takes over.
 * Reduced short-circuit, exception propagation, and chunked-source
 * forcing are all handled by the reused reduce_step / seq_iter
 * machinery. */

#define PIPELINE_MAX_STAGES 8

typedef enum {
    PIPELINE_STAGE_MAP,
    PIPELINE_STAGE_FILTER,
    PIPELINE_STAGE_TAKE
} pipeline_stage_kind_t;

typedef struct {
    pipeline_stage_kind_t kind;
    mino_val           *callable;   /* MAP/FILTER: stage fn. TAKE: NULL. */
    long long             counter;    /* TAKE: remaining countdown. */
} pipeline_stage_t;

/* Try to unwind a chain of map/filter/take LAZY cells from `coll`,
 * filling stages[] in outermost-first order. Returns the number of
 * stages unwound; sets *src_out to the bottom non-stage value. Returns
 * 0 (with *src_out == coll) when coll isn't a recognised pipeline. */
static int try_unwind_pipeline(mino_val *coll,
                               pipeline_stage_t *stages,
                               int max_stages,
                               mino_val **src_out)
{
    int n = 0;
    while (n < max_stages && coll != NULL
           && mino_type_of(coll) == MINO_LAZY
           && coll->as.lazy.realized == LAZY_UNREALIZED) {
        mino_val *body = coll->as.lazy.body;
        if (body == NULL || !mino_is_cons(body)
            || !mino_is_cons(body->as.cons.cdr)) {
            break;
        }
        if (lazy_thunk_is_map1(coll)) {
            stages[n].kind     = PIPELINE_STAGE_MAP;
            stages[n].callable = body->as.cons.car;
            stages[n].counter  = 0;
            coll = body->as.cons.cdr->as.cons.car;
            n++;
        } else if (lazy_thunk_is_filter(coll)) {
            stages[n].kind     = PIPELINE_STAGE_FILTER;
            stages[n].callable = body->as.cons.car;
            stages[n].counter  = 0;
            coll = body->as.cons.cdr->as.cons.car;
            n++;
        } else if (lazy_thunk_is_take(coll)) {
            long long m;
            if (!mino_to_int(body->as.cons.car, &m) || m < 0) break;
            stages[n].kind     = PIPELINE_STAGE_TAKE;
            stages[n].callable = NULL;
            stages[n].counter  = m;
            coll = body->as.cons.cdr->as.cons.car;
            n++;
        } else {
            break;
        }
    }
    *src_out = coll;
    return n;
}

/* Per-element callback contract for the generic pipeline walker.
 * Return 0 to continue, 1 to stop early (success), -1 to abort with
 * the diag already set. The walker drives elements through the
 * unwound stages and invokes step() with each surviving element. */
typedef int (*pipeline_step_fn)(mino_state *S, void *ctx,
                                mino_val *elem, mino_env *env);

/* Per-stage canonical-callable recognizer. The pipeline walker
 * compares a MAP / FILTER stage's callable's argv-ABI fn pointer
 * against the canonical install-time pointer for inc / dec /
 * odd? / even? / pos? / neg? / zero?. On match, the inner loop
 * inlines the operation without going through apply_callable_argv
 * (which costs ~30-50 ns per call). On the bench
 * (reduce + 0 (map inc (range 1m))) this saves the dominant per-
 * element cost when stages[i] is the bench's `(map inc ...)`. */
typedef enum {
    PIPELINE_FAST_NONE = 0,
    PIPELINE_FAST_INC,
    PIPELINE_FAST_DEC,
    PIPELINE_FAST_ODD_P,
    PIPELINE_FAST_EVEN_P,
    PIPELINE_FAST_POS_P,
    PIPELINE_FAST_NEG_P,
    PIPELINE_FAST_ZERO_P,
    PIPELINE_FAST_KW       /* keyword-as-fn: inline (get coll kw) per elem */
} pipeline_fast_kind_t;

static pipeline_fast_kind_t pipeline_fast_callable(mino_val *callable)
{
    /* Var deref: the callable might be a MINO_VAR pointing to the
     * canonical prim (e.g., when defprotocol's expanded body or
     * lazy-map-1 receives an unresolved var-reference). */
    if (callable != NULL && mino_type_of(callable) == MINO_VAR) {
        callable = callable->as.var.root;
    }
    if (callable == NULL) return PIPELINE_FAST_NONE;
    /* User-fn-wrapping-prim recogniser. `(fn [x] (inc x))` compiles to
     * a MINO_FN whose body invokes the canonical inc prim and nothing
     * else; compile_fn_literal stamps `wraps_prim` with that prim, so
     * the pipeline can skip apply_callable and route straight to the
     * specialised inline path here. */
    if (mino_type_of(callable) == MINO_FN
        && callable->as.fn.wraps_prim != NULL) {
        callable = callable->as.fn.wraps_prim;
    }
    if (mino_type_of(callable) == MINO_KEYWORD) return PIPELINE_FAST_KW;
    if (mino_type_of(callable) != MINO_PRIM) return PIPELINE_FAST_NONE;
    const char *name = callable->as.prim.name;
    if (name == NULL) return PIPELINE_FAST_NONE;
    if (strcmp(name, "inc")    == 0) return PIPELINE_FAST_INC;
    if (strcmp(name, "dec")    == 0) return PIPELINE_FAST_DEC;
    if (strcmp(name, "odd?")   == 0) return PIPELINE_FAST_ODD_P;
    if (strcmp(name, "even?")  == 0) return PIPELINE_FAST_EVEN_P;
    if (strcmp(name, "pos?")   == 0) return PIPELINE_FAST_POS_P;
    if (strcmp(name, "neg?")   == 0) return PIPELINE_FAST_NEG_P;
    if (strcmp(name, "zero?")  == 0) return PIPELINE_FAST_ZERO_P;
    return PIPELINE_FAST_NONE;
}

/* Inline-apply a canonical MAP-kind step on an integer element. The
 * non-int and overflow paths bail to the slow apply_callable_argv
 * path so Clojure's promotion semantics stay correct. Returns NULL on
 * error (with the diag set by apply_callable_argv). */
static mino_val *pipeline_apply_fast_map(mino_state *S,
                                           pipeline_fast_kind_t k,
                                           mino_val *callable,
                                           mino_val *elem,
                                           mino_env *env)
{
    if (MINO_IS_INT(elem)) {
        long long v = MINO_INT_VAL(elem);
        if (k == PIPELINE_FAST_INC && v < MINO_INT_MAX) {
            return MINO_MAKE_INT(v + 1);
        }
        if (k == PIPELINE_FAST_DEC && v > -MINO_INT_MAX) {
            /* Symmetric overflow check: dec's slow path matches +' semantics. */
            return MINO_MAKE_INT(v - 1);
        }
    }
    /* Keyword-as-fn: (map :k coll) and (map :k records) routes here
     * after the callable resolved to a MINO_KEYWORD. For records
     * (declared fields and the ext-map) and maps we inline the
     * lookup; other coll types (sorted-map, transient, nil, etc.)
     * fall through so the keyword-as-fn diagnostic path stays
     * intact. */
    if (k == PIPELINE_FAST_KW && elem != NULL) {
        int t = mino_type_of(elem);
        if (t == MINO_RECORD) {
            int idx = record_field_index(elem, callable);
            if (idx >= 0) return elem->as.record.vals[idx];
            if (elem->as.record.ext != NULL) {
                mino_val *v = map_get_val(elem->as.record.ext, callable);
                return v == NULL ? mino_nil(S) : v;
            }
            return mino_nil(S);
        }
        if (t == MINO_MAP) {
            mino_val *v = map_get_val(elem, callable);
            return v == NULL ? mino_nil(S) : v;
        }
        if (t == MINO_NIL) return mino_nil(S);
        /* sorted-map / transient / other: fall through to keyword-as-
         * fn dispatch in apply_callable_argv. */
    }
    mino_val *argv1[1];
    argv1[0] = elem;
    return apply_callable_argv(S, callable, argv1, 1, env);
}

/* Inline-test a canonical FILTER-kind predicate on an int element.
 * Returns 1 = truthy (passed), 0 = falsy (rejected), -1 = error +
 * out_err set so the caller can propagate. Falls back to
 * apply_callable_argv on non-int. */
static int pipeline_test_fast_filter(mino_state *S,
                                     pipeline_fast_kind_t k,
                                     mino_val *callable,
                                     mino_val *elem,
                                     mino_env *env,
                                     int *out_err)
{
    *out_err = 0;
    if (MINO_IS_INT(elem)) {
        long long v = MINO_INT_VAL(elem);
        switch (k) {
        case PIPELINE_FAST_ODD_P:  return (v & 1ll) != 0;
        case PIPELINE_FAST_EVEN_P: return (v & 1ll) == 0;
        case PIPELINE_FAST_POS_P:  return v > 0;
        case PIPELINE_FAST_NEG_P:  return v < 0;
        case PIPELINE_FAST_ZERO_P: return v == 0;
        default: break;
        }
    }
    /* Keyword-as-fn as a predicate: returns truthy iff the coll has
     * the keyword bound to a truthy value. Mirrors the map fast lane
     * above for the same coll-types. */
    if (k == PIPELINE_FAST_KW && elem != NULL) {
        int t = mino_type_of(elem);
        if (t == MINO_RECORD) {
            int idx = record_field_index(elem, callable);
            if (idx >= 0) {
                return mino_is_truthy_inline(elem->as.record.vals[idx])
                    ? 1 : 0;
            }
            if (elem->as.record.ext != NULL) {
                mino_val *v = map_get_val(elem->as.record.ext, callable);
                return v != NULL && mino_is_truthy_inline(v) ? 1 : 0;
            }
            return 0;
        }
        if (t == MINO_MAP) {
            mino_val *v = map_get_val(elem, callable);
            return v != NULL && mino_is_truthy_inline(v) ? 1 : 0;
        }
        if (t == MINO_NIL) return 0;
        /* sorted-map / transient / other: fall through. */
    }
    mino_val *argv1[1];
    argv1[0] = elem;
    mino_val *r = apply_callable_argv(S, callable, argv1, 1, env);
    if (r == NULL) { *out_err = 1; return -1; }
    return mino_is_truthy_inline(r) ? 1 : 0;
}

/* Drive a single element through all stages. Shared by the chunked
 * fast-path inner loop and the seq_iter fallback. fast_kinds[] is
 * the pre-resolved canonical-op kind per stage (NONE for stages we
 * have to apply_callable_argv normally). Returns:
 *   0 = pass to step, continue
 *   1 = skip element (filter rejected)
 *   2 = take counter exhausted, return done after this element
 *  -1 = error (diag set) */
static int pipeline_apply_stages(mino_state *S,
                                 pipeline_stage_t *stages,
                                 pipeline_fast_kind_t *fast_kinds,
                                 int n_stages,
                                 mino_val **elem_io,
                                 mino_env *env)
{
    mino_val *elem = *elem_io;
    int take_exhausted = 0;
    for (int i = n_stages - 1; i >= 0; i--) {
        pipeline_stage_t *st = &stages[i];
        if (st->kind == PIPELINE_STAGE_MAP) {
            pipeline_fast_kind_t k = fast_kinds[i];
            if (k != PIPELINE_FAST_NONE) {
                elem = pipeline_apply_fast_map(S, k, st->callable,
                                               elem, env);
            } else {
                mino_val *argv1[1];
                argv1[0] = elem;
                elem = apply_callable_argv(S, st->callable, argv1,
                                           1, env);
            }
            if (elem == NULL) return -1;
        } else if (st->kind == PIPELINE_STAGE_FILTER) {
            pipeline_fast_kind_t k = fast_kinds[i];
            int err = 0;
            int pass;
            if (k != PIPELINE_FAST_NONE) {
                pass = pipeline_test_fast_filter(S, k, st->callable,
                                                 elem, env, &err);
            } else {
                mino_val *argv1[1];
                argv1[0] = elem;
                mino_val *r = apply_callable_argv(S, st->callable,
                                                    argv1, 1, env);
                if (r == NULL) return -1;
                pass = mino_is_truthy_inline(r) ? 1 : 0;
            }
            if (err) return -1;
            if (!pass) return 1;
        } else {
            /* TAKE */
            if (st->counter <= 0) return 2;
            st->counter--;
            if (st->counter == 0) take_exhausted = 1;
        }
    }
    *elem_io = elem;
    return take_exhausted ? 2 : 0;
}

/* Drive `src` element by element through stages[n_stages-1 .. 0]
 * (innermost-stage-first per element). For each element that passes
 * every stage, invoke step(S, ctx, elem, env). Caller pre-handled
 * the nil-source and empty-source cases. Returns 0 on normal
 * exhaustion, 1 on take-exhausted or step-requested stop, -1 on
 * error. When `src` is (or resolves to) a chunked-cons, the walk
 * iterates the chunk's value array directly instead of calling
 * seq_iter_val / seq_iter_next per element. Combined with the
 * canonical-callable fast path inside pipeline_apply_stages, the
 * (map inc ...) / (filter odd? ...) / (map dec ...) shapes on a
 * (range N) source run with one tagged-int op per element instead
 * of an apply_callable_argv per stage per element. */
static int pipeline_walk(mino_state *S,
                         mino_val *src,
                         pipeline_stage_t *stages,
                         int n_stages,
                         pipeline_step_fn step,
                         void *ctx,
                         mino_env *env)
{
    if (src == NULL || mino_type_of(src) == MINO_NIL
        || mino_type_of(src) == MINO_EMPTY_LIST) {
        return 0;
    }
    pipeline_fast_kind_t fast_kinds[PIPELINE_MAX_STAGES];
    for (int i = 0; i < n_stages; i++) {
        if (stages[i].kind == PIPELINE_STAGE_MAP
            || stages[i].kind == PIPELINE_STAGE_FILTER) {
            fast_kinds[i] = pipeline_fast_callable(stages[i].callable);
        } else {
            fast_kinds[i] = PIPELINE_FAST_NONE;
        }
    }

    /* Int-range source fast path: drive the stages from an inline
     * `for (cur = start; cur != end; cur += step)` loop instead of
     * forcing range_thunk every 32 elements (which allocates a fresh
     * 32-int chunk plus a chunked-cons cell per chunk). Bounded
     * (non-infinite) ranges only; an infinite range stays on the
     * chunked path so `take` and friends can terminate the walk. */
    {
        long long start = 0, end = 0, range_step = 0;
        int       infinite = 0;
        if (lazy_is_int_range(src, &start, &end, &range_step, &infinite)
            && !infinite && range_step != 0) {
            long long cur = start;
            while ((range_step > 0) ? cur < end : cur > end) {
                mino_val *elem = MINO_MAKE_INT(cur);
                int rc = pipeline_apply_stages(S, stages, fast_kinds,
                                               n_stages, &elem, env);
                if (rc < 0) return -1;
                if (rc != 1) {
                    int srcc = step(S, ctx, elem, env);
                    if (srcc < 0) return -1;
                    if (srcc > 0) return 1;
                    if (rc == 2) return 1;
                }
                cur += range_step;
            }
            return 0;
        }
    }

    /* Force outer LAZY once so we can examine the resolved shape. */
    if (mino_type_of(src) == MINO_LAZY) {
        src = lazy_force(S, src);
        if (src == NULL) return -1;
    }

    /* Chunked-cons fast path: read chunk values directly, skip the
     * seq_iter call per element. Falls into the seq_iter walk once
     * the chunked chain transitions to a plain cons / vector / etc.
     * suffix. */
    while (src != NULL && mino_type_of(src) == MINO_CHUNKED_CONS) {
        const mino_val *ch = src->as.chunked_cons.chunk;
        unsigned          start = src->as.chunked_cons.off;
        unsigned          end   = ch->as.chunk.len;
        for (unsigned k = start; k < end; k++) {
            mino_val *elem = ch->as.chunk.vals[k];
            int rc = pipeline_apply_stages(S, stages, fast_kinds,
                                           n_stages, &elem, env);
            if (rc < 0) return -1;
            if (rc == 1) continue;            /* filter rejected */
            int srcc = step(S, ctx, elem, env);
            if (srcc < 0) return -1;
            if (srcc > 0) return 1;
            if (rc == 2) return 1;            /* take exhausted */
        }
        mino_val *more = src->as.chunked_cons.more;
        if (more != NULL && mino_type_of(more) == MINO_LAZY) {
            more = lazy_force(S, more);
            if (more == NULL) return -1;
        }
        src = more;
    }
    if (src == NULL || mino_type_of(src) == MINO_NIL
        || mino_type_of(src) == MINO_EMPTY_LIST) {
        return 0;
    }

    seq_iter_t it;
    seq_iter_init(S, &it, src);
    while (!seq_iter_done(&it)) {
        mino_val *elem = seq_iter_val(S, &it);
        int rc = pipeline_apply_stages(S, stages, fast_kinds,
                                       n_stages, &elem, env);
        if (rc < 0) return -1;
        if (rc != 1) {
            int srcc = step(S, ctx, elem, env);
            if (srcc < 0) return -1;
            if (srcc > 0) return 1;
            if (rc == 2) return 1;
        }
        seq_iter_next(S, &it);
    }
    return 0;
}

/* pipeline_walk's per-element callback shim around reduce_ctx_step. */
static int reduce_pipeline_step(mino_state *S, void *ctx_,
                                mino_val *elem, mino_env *env)
{
    return reduce_ctx_step(S, (reduce_ctx_t *)ctx_, elem, env);
}

static mino_val *reduce_pipeline_walk(mino_state *S,
                                        mino_val *fn,
                                        mino_val *acc,
                                        int has_init,
                                        mino_val *src,
                                        pipeline_stage_t *stages,
                                        int n_stages,
                                        mino_env *env)
{
    reduce_ctx_t ctx;
    int          rc;
    reduce_ctx_init(&ctx, fn, acc, has_init);
    rc = pipeline_walk(S, src, stages, n_stages,
                       reduce_pipeline_step, &ctx, env);
    if (rc < 0) return NULL;
    return reduce_ctx_finalize(S, &ctx, env);
}

/* True iff coll's outer LAZY is one of the recognised pipeline stages.
 * Cheap pre-check to gate the unwinder allocation-free. */
static int coll_is_pipeline_head(const mino_val *coll)
{
    return lazy_thunk_is_map1(coll)
        || lazy_thunk_is_filter(coll)
        || lazy_thunk_is_take(coll);
}

static mino_val *prim_reduce(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *fn;
    mino_val *acc;
    mino_val *coll;
    seq_iter_t  it;
    size_t      n;
    long long   r_start = 0, r_end = 0, r_step = 1;
    int         r_inf   = 0;
    arg_count(S, args, &n);
    if (n == 2) {
        /* (reduce f coll) — first element is the initial accumulator. */
        fn   = args->as.cons.car;
        coll = args->as.cons.cdr->as.cons.car;
        if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
            /* (reduce f nil) → (f) */
            return apply_callable(S, fn, mino_nil(S), env);
        }
        if (coll_is_pipeline_head(coll)) {
            pipeline_stage_t stages[PIPELINE_MAX_STAGES];
            mino_val *src = NULL;
            int ns = try_unwind_pipeline(
                coll, stages, PIPELINE_MAX_STAGES, &src);
            if (ns > 0) {
                return reduce_pipeline_walk(
                    S, fn, NULL, 0, src, stages, ns, env);
            }
        }
        if (lazy_is_int_range(coll, &r_start, &r_end, &r_step, &r_inf)
            && !r_inf) {
            mino_val *fast = reduce_int_range(
                S, fn, NULL, 0, r_start, r_end, r_step);
            if (fast != NULL) return fast;
        }
        if (mino_type_of(coll) == MINO_MAP)
            return reduce_map_direct(S, fn, NULL, 0, coll, env);
        if (mino_type_of(coll) == MINO_SET)
            return reduce_set_direct(S, fn, NULL, 0, coll, env);
        if (mino_type_of(coll) == MINO_VECTOR)
            return reduce_vec_direct(S, fn, NULL, 0, coll, env);
        seq_iter_init(S, &it, coll);
        if (seq_iter_done(&it)) {
            return apply_callable(S, fn, mino_nil(S), env);
        }
        {
            reduce_ctx_t ctx;
            reduce_ctx_init(&ctx, fn, NULL, 0);
            while (!seq_iter_done(&it)) {
                mino_val *elem = seq_iter_val(S, &it);
                int rc = reduce_ctx_step(S, &ctx, elem, env);
                if (rc == -1) return NULL;
                if (rc == 1)  return reduce_ctx_finalize(S, &ctx, env);
                seq_iter_next(S, &it);
            }
            return reduce_ctx_finalize(S, &ctx, env);
        }
    } else if (n == 3) {
        /* (reduce f init coll) */
        fn   = args->as.cons.car;
        acc  = args->as.cons.cdr->as.cons.car;
        coll = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
            return acc;
        }
        if (coll_is_pipeline_head(coll)) {
            pipeline_stage_t stages[PIPELINE_MAX_STAGES];
            mino_val *src = NULL;
            int ns = try_unwind_pipeline(
                coll, stages, PIPELINE_MAX_STAGES, &src);
            if (ns > 0) {
                return reduce_pipeline_walk(
                    S, fn, acc, 1, src, stages, ns, env);
            }
        }
        if (lazy_is_int_range(coll, &r_start, &r_end, &r_step, &r_inf)
            && !r_inf) {
            mino_val *fast = reduce_int_range(
                S, fn, acc, 1, r_start, r_end, r_step);
            if (fast != NULL) return fast;
        }
        if (mino_type_of(coll) == MINO_MAP)
            return reduce_map_direct(S, fn, acc, 1, coll, env);
        if (mino_type_of(coll) == MINO_SET)
            return reduce_set_direct(S, fn, acc, 1, coll, env);
        if (mino_type_of(coll) == MINO_VECTOR)
            return reduce_vec_direct(S, fn, acc, 1, coll, env);
        seq_iter_init(S, &it, coll);
    } else {
        return prim_throw_classified(S, "eval/arity", "MAR001", "reduce requires 2 or 3 arguments");
    }
    /* Inner loop: reduce_ctx_step keeps the accumulator unboxed when
     * fn is a canonical numeric reducer and every element so far has
     * been tagged-int (no overflow). The first miss falls through to
     * reduce_step's generic int+int / apply_callable path so the
     * numeric tower stays Clojure-correct. */
    {
        reduce_ctx_t ctx;
        reduce_ctx_init(&ctx, fn, acc, 1);
        while (!seq_iter_done(&it)) {
            mino_val *elem = seq_iter_val(S, &it);
            int rc = reduce_ctx_step(S, &ctx, elem, env);
            if (rc == -1) return NULL;
            if (rc == 1)  return reduce_ctx_finalize(S, &ctx, env);
            seq_iter_next(S, &it);
        }
        return reduce_ctx_finalize(S, &ctx, env);
    }
}

/* (set coll) — create a set from a collection. */
static mino_val *prim_set(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    mino_val *result;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "set requires exactly 1 argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_set(S, NULL, 0);
    }
    /* Validate seqability eagerly. */
    {
        mino_val *seq_args = mino_cons(S, coll, mino_nil(S));
        mino_val *seqd = prim_seq(S, seq_args, env);
        if (seqd == NULL) return NULL;
        if (mino_type_of(seqd) == MINO_NIL) return mino_set(S, NULL, 0);
        coll = seqd;
    }
    result = mino_set(S, NULL, 0);
    gc_pin(result);
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        result = set_conj1(S, result, seq_iter_val(S, &it));
        seq_iter_next(S, &it);
    }
    gc_unpin(1);
    return result;
}

/* Lazy primitives (range, lazy-map-1, lazy-filter) live in prim_lazy.c. */

/* (doall coll) -- walks coll forcing every lazy cell, returns coll.
 * (dorun coll) -- same but returns nil. Both iterate in C so each step
 * avoids the fn-call + env frame that a mino-level walk would pay. */
static mino_val *realize_seq(mino_state *S, mino_val *coll)
{
    while (coll != NULL) {
        if (mino_type_of(coll) == MINO_LAZY) {
            coll = lazy_force(S, coll);
            if (coll == NULL) return NULL;
            continue;
        }
        if (mino_type_of(coll) == MINO_NIL) return coll;
        if (mino_type_of(coll) == MINO_CONS) {
            coll = coll->as.cons.cdr;
            continue;
        }
        if (mino_type_of(coll) == MINO_CHUNKED_CONS) {
            /* The chunk's slots are already values; the tail is the
             * part still to force. */
            coll = coll->as.chunked_cons.more;
            continue;
        }
        /* Non-lazy, non-cons collections are already fully realized. */
        return coll;
    }
    return mino_nil(S);
}

/* Bounded realization: force at most N steps of the spine, leaving
 * the rest lazy. Each step is one `next`, so N steps realize at most
 * N + 1 elements -- the same over-read the canonical bounded dorun's
 * (seq coll) probe performs on its final iteration. */
static mino_val *realize_seq_n(mino_state *S, mino_val *coll, long long n)
{
    while (coll != NULL) {
        if (mino_type_of(coll) == MINO_LAZY) {
            coll = lazy_force(S, coll);
            if (coll == NULL) return NULL;
            continue;
        }
        if (mino_type_of(coll) == MINO_NIL) return coll;
        if (n <= 0) return coll;
        if (mino_type_of(coll) == MINO_CONS) {
            coll = coll->as.cons.cdr;
            n--;
            continue;
        }
        if (mino_type_of(coll) == MINO_CHUNKED_CONS) {
            long long avail = (long long)(coll->as.chunked_cons.chunk
                                              ->as.chunk.len
                                          - coll->as.chunked_cons.off);
            if (avail > n) return coll;
            n -= avail;
            coll = coll->as.chunked_cons.more;
            continue;
        }
        return coll;
    }
    return mino_nil(S);
}

/* Shared arg parse for doall / dorun: 1-arity fills *coll_out only,
 * 2-arity also fills *n_out and sets *bounded. Returns 0 on success,
 * -1 after raising the arity / type error. */
static int doall_parse_args(mino_state *S, mino_val *args,
                            const char *who, mino_val **coll_out,
                            long long *n_out, int *bounded)
{
    size_t argc;
    char   msg[64];
    arg_count(S, args, &argc);
    *bounded = 0;
    if (argc == 1) {
        *coll_out = args->as.cons.car;
        return 0;
    }
    if (argc == 2) {
        if (!mino_to_int(args->as.cons.car, n_out)) {
            snprintf(msg, sizeof(msg), "%s: count must be an integer", who);
            (void)prim_throw_classified(S, "eval/type", "MTY001", msg);
            return -1;
        }
        *coll_out = args->as.cons.cdr->as.cons.car;
        *bounded  = 1;
        return 0;
    }
    snprintf(msg, sizeof(msg), "%s requires 1 or 2 arguments", who);
    (void)prim_throw_classified(S, "eval/arity", "MAR001", msg);
    return -1;
}

static mino_val *prim_doall(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll = NULL;
    long long   n    = 0;
    int         bounded;
    (void)env;
    if (doall_parse_args(S, args, "doall", &coll, &n, &bounded) != 0) {
        return NULL;
    }
    if (bounded) {
        if (realize_seq_n(S, coll, n) == NULL) return NULL;
        return coll;
    }
    if (realize_seq(S, coll) == NULL) return NULL;
    return coll;
}

/* Step fn for the no-accumulator consumers (dorun / doseq). The
 * walker drives elements through stages; this step does nothing
 * per element except propagate the continue signal. */
static int discard_step(mino_state *S, void *ctx, mino_val *elem,
                        mino_env *env)
{
    (void)S; (void)ctx; (void)elem; (void)env;
    return 0;
}

static mino_val *prim_dorun(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll = NULL;
    long long   n    = 0;
    int         bounded;
    if (doall_parse_args(S, args, "dorun", &coll, &n, &bounded) != 0) {
        return NULL;
    }
    if (bounded) {
        if (realize_seq_n(S, coll, n) == NULL) return NULL;
        return mino_nil(S);
    }
    if (coll_is_pipeline_head(coll)) {
        pipeline_stage_t stages[PIPELINE_MAX_STAGES];
        mino_val      *src = NULL;
        int ns = try_unwind_pipeline(
            coll, stages, PIPELINE_MAX_STAGES, &src);
        if (ns > 0) {
            if (pipeline_walk(S, src, stages, ns, discard_step,
                              NULL, env) < 0) {
                return NULL;
            }
            return mino_nil(S);
        }
    }
    if (realize_seq(S, coll) == NULL) return NULL;
    return mino_nil(S);
}

/* Eager range returning a vector. Avoids lazy thunk overhead for tight loops.
 * (rangev end) or (rangev start end) or (rangev start end step). */
static mino_val *prim_rangev(mino_state *S, mino_val *args, mino_env *env)
{
    long long start = 0, end = 0, step = 1, i;
    size_t n, len;
    mino_val **items;
    mino_val *result;
    (void)env;
    arg_count(S, args, &n);
    if (n == 1) {
        if (!mino_to_int(args->as.cons.car, &end)) {
            return prim_throw_classified(S, "eval/type", "MTY001", "rangev argument must be an integer");
        }
    } else if (n == 2) {
        if (!mino_to_int(args->as.cons.car, &start) ||
            !mino_to_int(args->as.cons.cdr->as.cons.car, &end)) {
            return prim_throw_classified(S, "eval/type", "MTY001", "rangev arguments must be integers");
        }
    } else if (n == 3) {
        if (!mino_to_int(args->as.cons.car, &start) ||
            !mino_to_int(args->as.cons.cdr->as.cons.car, &end) ||
            !mino_to_int(args->as.cons.cdr->as.cons.cdr->as.cons.car, &step)) {
            return prim_throw_classified(S, "eval/type", "MTY001", "rangev arguments must be integers");
        }
        if (step == 0) {
            return prim_throw_classified(S, "eval/bounds", "MBD001", "rangev step must not be zero");
        }
    } else {
        return prim_throw_classified(S, "eval/arity", "MAR001", "rangev requires 1, 2, or 3 arguments");
    }
    /* Compute length using unsigned arithmetic to avoid signed overflow when
     * end and start span the full signed range (e.g. LLONG_MIN..LLONG_MAX).
     * Negating a negative step via (unsigned long long)(~step)+1 avoids UB
     * for the LLONG_MIN edge case. */
    if (step > 0) {
        if (end > start) {
            unsigned long long uspan = (unsigned long long)end - (unsigned long long)start;
            unsigned long long ustep = (unsigned long long)step;
            len = (size_t)((uspan + ustep - 1) / ustep);
        } else {
            len = 0;
        }
    } else {
        if (start > end) {
            unsigned long long uspan = (unsigned long long)start - (unsigned long long)end;
            unsigned long long ustep = (unsigned long long)(~step) + 1ULL; /* -step without UB */
            len = (size_t)((uspan + ustep - 1) / ustep);
        } else {
            len = 0;
        }
    }
    items = malloc(len * sizeof(mino_val *));
    if (!items && len > 0) { return prim_throw_classified(S, "eval/bounds", "MBD001", "rangev: out of memory"); }
    /* C-heap items[] is invisible to the precise GC; suppress collection
     * so freshly minted ints cannot be swept before mino_vector rehomes
     * them into GC-visible storage. */
    mino_current_ctx(S)->gc_depth++;
    for (i = start, n = 0; n < len; i += step, n++) {
        items[n] = mino_int(S, i);
    }
    result = mino_vector(S, items, len);
    mino_current_ctx(S)->gc_depth--;
    free(items);
    return result;
}

/* Grow a PTRARR in place by reallocating into a larger GC block.
 * The new block replaces the old on the pin slot and becomes the
 * returned buffer; the old is left for the sweep. Returns NULL on
 * OOM (diag already set by gc_alloc_typed). */
static mino_val **ptrarr_grow(mino_state *S, mino_val **old,
                                size_t old_len, size_t new_cap, int pin_slot)
{
    mino_val **nb = (mino_val **)gc_alloc_typed(
        S, GC_T_PTRARR, new_cap * sizeof(mino_val *));
    size_t i;
    if (nb == NULL) return NULL;
    for (i = 0; i < old_len; i++) {
        nb[i] = old[i];
    }
    mino_current_ctx(S)->gc_save[pin_slot] = (mino_val *)nb;
    return nb;
}

/* Eager map returning a vector. (mapv f coll)
 *
 * Accumulate into a GC-tracked PTRARR pinned on the save stack so the
 * buffer and its contents stay marked across the apply_callable call.
 * A malloc'd items[] would be invisible to the precise collector, and a
 * blanket gc_depth++ would pin every user-code allocation for the
 * duration of the mapv. Pinning the accumulator lets GC continue normally
 * inside the fn while still preserving what we've produced so far. */
/* Transient-vector accumulator: the step fn for mapv / filterv /
 * into[vec] fast paths. `t` is a MINO_TRANSIENT vector; the step
 * conj-bangs each element and updates the slot to the returned
 * transient (transients may relocate). */
typedef struct {
    mino_val *t;
} tvec_ctx_t;

static int tvec_conj_step(mino_state *S, void *ctx_,
                          mino_val *elem, mino_env *env)
{
    tvec_ctx_t *ctx = (tvec_ctx_t *)ctx_;
    (void)env;
    mino_val *nt = mino_conj_bang(S, ctx->t, elem);
    if (nt == NULL) return -1;
    ctx->t = nt;
    return 0;
}

#define MAPV_MAX_COLLS 32

static mino_val *prim_mapv(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *fn, *coll;
    seq_iter_t  it;
    size_t      cap = 64, len = 0;
    mino_val **items;
    mino_val *result;
    int         pin_slot;
    size_t n;
    arg_count(S, args, &n);
    if (n < 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "mapv requires at least 2 arguments: function and one or more collections");
    }
    fn = args->as.cons.car;
    /* Multi-collection arity: walk N collections in parallel, calling
     * fn with one element from each per step; stop at the shortest. */
    if (n > 2) {
        size_t n_colls = n - 1;
        seq_iter_t   iters[MAPV_MAX_COLLS];
        mino_val  *argv[MAPV_MAX_COLLS];
        if (n_colls > MAPV_MAX_COLLS) {
            return prim_throw_classified(
                S, "eval/arity", "MAR001",
                "mapv: too many collections (max 32)");
        }
        {
            mino_val *p = args->as.cons.cdr;
            size_t i;
            for (i = 0; i < n_colls; i++) {
                mino_val *c = p->as.cons.car;
                if (c == NULL || mino_is_nil(c)) {
                    return mino_vector(S, NULL, 0);
                }
                seq_iter_init(S, &iters[i], c);
                p = p->as.cons.cdr;
            }
        }
        items = (mino_val **)gc_alloc_typed(S, GC_T_PTRARR,
                                              cap * sizeof(mino_val *));
        if (items == NULL) return NULL;
        pin_slot = mino_current_ctx(S)->gc_save_len;
        gc_pin((mino_val *)items);
        for (;;) {
            size_t i;
            for (i = 0; i < n_colls; i++) {
                if (seq_iter_done(&iters[i])) goto mapv_multi_done;
            }
            for (i = 0; i < n_colls; i++) {
                argv[i] = seq_iter_val(S, &iters[i]);
            }
            {
                mino_val *val = apply_callable_argv(
                    S, fn, argv, (int)n_colls, env);
                if (val == NULL) { gc_unpin(1); return NULL; }
                if (len >= cap) {
                    cap *= 2;
                    items = ptrarr_grow(S, items, len, cap, pin_slot);
                    if (items == NULL) { gc_unpin(1); return NULL; }
                }
                gc_valarr_set(S, items, len, val);
                len++;
            }
            for (i = 0; i < n_colls; i++) {
                seq_iter_next(S, &iters[i]);
            }
        }
        mapv_multi_done:
        result = mino_vector(S, items, len);
        gc_unpin(1);
        return result;
    }
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || mino_is_nil(coll)) {
        return mino_vector(S, NULL, 0);
    }
    /* Pipeline fast lane: when coll is a map/filter/take chain,
     * unwind the stages, prepend mapv's own fn as the outermost
     * map stage, and walk the bottom source through the fused
     * pipeline into a transient vector. Skips the lazy-seq cells
     * entirely. */
    if (coll_is_pipeline_head(coll)) {
        pipeline_stage_t stages[PIPELINE_MAX_STAGES];
        mino_val      *src = NULL;
        int ns = try_unwind_pipeline(
            coll, stages + 1, PIPELINE_MAX_STAGES - 1, &src);
        if (ns > 0) {
            stages[0].kind     = PIPELINE_STAGE_MAP;
            stages[0].callable = fn;
            stages[0].counter  = 0;
            tvec_ctx_t ctx = { mino_transient(S, mino_vector(S, NULL, 0)) };
            if (ctx.t == NULL) return NULL;
            int rc = pipeline_walk(S, src, stages, ns + 1,
                                   tvec_conj_step, &ctx, env);
            if (rc < 0) return NULL;
            return mino_persistent(S, ctx.t);
        }
    }
    items = (mino_val **)gc_alloc_typed(S, GC_T_PTRARR,
                                          cap * sizeof(mino_val *));
    if (items == NULL) return NULL;
    pin_slot = mino_current_ctx(S)->gc_save_len;
    gc_pin((mino_val *)items);
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val *elem = seq_iter_val(S, &it);
        mino_val *call_args = mino_cons(S, elem, mino_nil(S));
        mino_val *val = apply_callable(S, fn, call_args, env);
        if (val == NULL) { gc_unpin(1); return NULL; }
        if (len >= cap) {
            cap *= 2;
            items = ptrarr_grow(S, items, len, cap, pin_slot);
            if (items == NULL) { gc_unpin(1); return NULL; }
        }
        gc_valarr_set(S, items, len, val);
        len++;
        seq_iter_next(S, &it);
    }
    result = mino_vector(S, items, len);
    gc_unpin(1);
    return result;
}

/* Eager filter returning a vector. (filterv pred coll). Same precise-GC
 * caveat as prim_mapv: pin a GC-tracked accumulator on the save stack. */
static mino_val *prim_filterv(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *pred, *coll;
    seq_iter_t  it;
    size_t      cap = 64, len = 0;
    mino_val **items;
    mino_val *result;
    int         pin_slot;
    size_t n;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "filterv requires 2 arguments: predicate and collection");
    }
    pred = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || mino_is_nil(coll)) {
        return mino_vector(S, NULL, 0);
    }
    /* Pipeline fast lane (see prim_mapv for the same shape): prepend
     * filterv's predicate as the outermost filter stage. */
    if (coll_is_pipeline_head(coll)) {
        pipeline_stage_t stages[PIPELINE_MAX_STAGES];
        mino_val      *src = NULL;
        int ns = try_unwind_pipeline(
            coll, stages + 1, PIPELINE_MAX_STAGES - 1, &src);
        if (ns > 0) {
            stages[0].kind     = PIPELINE_STAGE_FILTER;
            stages[0].callable = pred;
            stages[0].counter  = 0;
            tvec_ctx_t ctx = { mino_transient(S, mino_vector(S, NULL, 0)) };
            if (ctx.t == NULL) return NULL;
            int rc = pipeline_walk(S, src, stages, ns + 1,
                                   tvec_conj_step, &ctx, env);
            if (rc < 0) return NULL;
            return mino_persistent(S, ctx.t);
        }
    }
    items = (mino_val **)gc_alloc_typed(S, GC_T_PTRARR,
                                          cap * sizeof(mino_val *));
    if (items == NULL) return NULL;
    pin_slot = mino_current_ctx(S)->gc_save_len;
    gc_pin((mino_val *)items);
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val *elem = seq_iter_val(S, &it);
        mino_val *call_args = mino_cons(S, elem, mino_nil(S));
        mino_val *test = apply_callable(S, pred, call_args, env);
        if (test == NULL) { gc_unpin(1); return NULL; }
        if (mino_is_truthy_inline(test)) {
            if (len >= cap) {
                cap *= 2;
                items = ptrarr_grow(S, items, len, cap, pin_slot);
                if (items == NULL) { gc_unpin(1); return NULL; }
            }
            gc_valarr_set(S, items, len, elem);
            len++;
        }
        seq_iter_next(S, &it);
    }
    result = mino_vector(S, items, len);
    gc_unpin(1);
    return result;
}

static mino_val *prim_into(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *to;
    mino_val *from;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "into requires two arguments");
    }
    to   = args->as.cons.car;
    from = args->as.cons.cdr->as.cons.car;
    if (from == NULL || mino_type_of(from) == MINO_NIL) {
        return to;
    }
    /* Conj each element of `from` into `to`. The type of `to` determines
     * the conj semantics (vector appends, list prepends, map/set merges). */
    if (to == NULL || mino_type_of(to) == MINO_NIL || mino_type_of(to) == MINO_EMPTY_LIST) {
        /* Into nil or empty-list: build a list (prepend each element). */
        mino_val *out = mino_nil(S);
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            out = mino_cons(S, seq_iter_val(S, &it), out);
            seq_iter_next(S, &it);
        }
        if (out == NULL || mino_type_of(out) == MINO_NIL) return mino_empty_list(S);
        return out;
    }
    if (mino_type_of(to) == MINO_VECTOR) {
        /* Pipeline fast lane: when `from` is a map/filter/take chain,
         * walk the bottom source through the unwound stages and
         * conj-bang each survivor into a transient copy of `to`. The
         * lazy-seq cells never get realized. */
        if (coll_is_pipeline_head(from)) {
            pipeline_stage_t stages[PIPELINE_MAX_STAGES];
            mino_val      *src = NULL;
            int ns = try_unwind_pipeline(
                from, stages, PIPELINE_MAX_STAGES, &src);
            if (ns > 0) {
                tvec_ctx_t ctx = { mino_transient(S, to) };
                if (ctx.t == NULL) return NULL;
                int rc = pipeline_walk(S, src, stages, ns,
                                       tvec_conj_step, &ctx, env);
                if (rc < 0) return NULL;
                return mino_persistent(S, ctx.t);
            }
        }
        /* Transient fast path for non-pipeline sources: conj-bang each
         * element onto an owner-tagged tail / trie, then seal. The
         * first conj! against a fresh transient clones the tail once;
         * subsequent conj!s within the same 32-slot chunk write a
         * single slot + bump the count with no allocation. Pinning is
         * the same -O2 register-scan defense as the map / set branches
         * below. */
        mino_val *t = mino_transient(S, to);
        mino_val *acc;
        if (t == NULL) return NULL;
        gc_pin(t);
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            if (mino_conj_bang(S, t, seq_iter_val(S, &it)) == NULL) {
                gc_unpin(1);
                return NULL;
            }
            seq_iter_next(S, &it);
        }
        acc = mino_persistent(S, t);
        gc_unpin(1);
        return acc;
    }
    if (mino_type_of(to) == MINO_MAP) {
        /* Transient fast path: assoc! each pair onto an owner-tagged
         * HAMT, then seal with persistent!. The first batch element
         * clones the spine once; subsequent elements reuse owner-
         * matching slots in place. The transient is pinned across
         * iterations -- at -O2 the local can live entirely in a
         * callee-saved register, invisible to the conservative C-stack
         * scan, so a long batch's mid-stride minor GC could otherwise
         * sweep the wrapping transient. */
        mino_val *t   = mino_transient(S, to);
        mino_val *acc;
        if (t == NULL) return NULL;
        gc_pin(t);
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            mino_val *item = seq_iter_val(S, &it);
            mino_val *ek, *ev;
            if (item != NULL && mino_type_of(item) == MINO_VECTOR
                && item->as.vec.len == 2) {
                ek = vec_nth(item, 0);
                ev = vec_nth(item, 1);
            } else if (item != NULL && mino_type_of(item) == MINO_MAP_ENTRY) {
                ek = item->as.map_entry.k;
                ev = item->as.map_entry.v;
            } else {
                gc_unpin(1);
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "into map: each element must be a map entry or 2-element vector");
            }
            if (mino_assoc_bang(S, t, ek, ev) == NULL) {
                gc_unpin(1);
                return NULL;
            }
            seq_iter_next(S, &it);
        }
        acc = mino_persistent(S, t);
        gc_unpin(1);
        return acc;
    }
    if (mino_type_of(to) == MINO_SET) {
        /* Transient fast path: conj! each element onto an owner-tagged
         * HAMT, then seal. The transient is pinned for the same -O2
         * register-scan reason as the map branch above. */
        mino_val *t = mino_transient(S, to);
        mino_val *acc;
        if (t == NULL) return NULL;
        gc_pin(t);
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            if (mino_conj_bang(S, t, seq_iter_val(S, &it)) == NULL) {
                gc_unpin(1);
                return NULL;
            }
            seq_iter_next(S, &it);
        }
        acc = mino_persistent(S, t);
        gc_unpin(1);
        return acc;
    }
    if (mino_type_of(to) == MINO_SORTED_MAP) {
        mino_val *acc = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            mino_val *item = seq_iter_val(S, &it);
            mino_val *ek, *ev;
            if (item != NULL && mino_type_of(item) == MINO_VECTOR
                && item->as.vec.len == 2) {
                ek = vec_nth(item, 0);
                ev = vec_nth(item, 1);
            } else if (item != NULL && mino_type_of(item) == MINO_MAP_ENTRY) {
                ek = item->as.map_entry.k;
                ev = item->as.map_entry.v;
            } else {
                return prim_throw_classified(S, "eval/type", "MTY001",
                    "into sorted-map: each element must be a map entry or 2-element vector");
            }
            acc = sorted_map_assoc1(S, acc, ek, ev);
            seq_iter_next(S, &it);
        }
        return acc;
    }
    if (mino_type_of(to) == MINO_SORTED_SET) {
        mino_val *acc = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            acc = sorted_set_conj1(S, acc, seq_iter_val(S, &it));
            seq_iter_next(S, &it);
        }
        return acc;
    }
    if (mino_type_of(to) == MINO_CONS) {
        mino_val *out = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            out = mino_cons(S, seq_iter_val(S, &it), out);
            seq_iter_next(S, &it);
        }
        return out;
    }
    if (mino_type_of(to) == MINO_QUEUE) {
        mino_val *out = to;
        seq_iter_init(S, &it, from);
        while (!seq_iter_done(&it)) {
            out = mino_queue_conj(S, out, seq_iter_val(S, &it));
            seq_iter_next(S, &it);
        }
        return out;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg),
                 "into: expected a list, vector, map, or set as target, got %s",
                 type_tag_str(to));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

static mino_val *prim_apply(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *fn;
    mino_val *last;
    mino_val *call_args;
    mino_val *p;
    size_t      n;
    int         lazy_safe;
    arg_count(S, args, &n);
    if (n < 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "apply requires a function and arguments");
    }
    fn = args->as.cons.car;
    /* `lazy_safe` is computed lazily -- only the paths that would
     * splice a LAZY tail check it (avoiding the fn-shape walk for
     * the very common `(apply f finite-list)` case where last is
     * CONS). When the callee is a single-arity fn with a rest-arg,
     * we can splice the final collection's seq directly into the
     * args spine instead of forcing every element via seq_iter.
     * apply_callable detects the lazy tail and routes such calls to
     * the tree-walker (whose bind_params walks lazy cells
     * incrementally and hands the rest-arg the still-lazy tail).
     * For unsafe callees -- prims that walk raw cons, multi-arity
     * fns whose dispatch counts args, and fixed-arity fns where
     * over-supply is an arity error anyway -- we keep the eager
     * seq_iter materialization. */
    lazy_safe = -1;
    if (n == 2) {
        /* (apply f coll) — spread coll as args. */
        last = args->as.cons.cdr->as.cons.car;
    } else {
        /* (apply f a b ... coll) — prepend individual args, then spread coll. */
        mino_val *head = mino_nil(S);
        mino_val *tail2 = NULL;
        p = args->as.cons.cdr;
        /* Collect all but the last arg as individual args. */
        while (mino_is_cons(p) && mino_is_cons(p->as.cons.cdr)) {
            mino_val *cell = mino_cons(S, p->as.cons.car, mino_nil(S));
            if (tail2 == NULL) { head = cell; } else { mino_cons_cdr_set(S, tail2, cell); }
            tail2 = cell;
            p = p->as.cons.cdr;
        }
        last = p->as.cons.car; /* the final collection argument */
        /* Lazy-safe splice: attach `last` (CONS or LAZY) directly as
         * the cdr of the prepend chain so an infinite tail flows
         * through to a rest-arg without materializing. */
        if (last != NULL
            && (mino_type_of(last) == MINO_LAZY
                || mino_type_of(last) == MINO_CONS)) {
            if (lazy_safe < 0) lazy_safe = fn_lazy_safe_rest(fn);
            if (lazy_safe) {
                if (tail2 == NULL) {
                    head = last;
                } else {
                    mino_cons_cdr_set(S, tail2, last);
                }
                return apply_callable(S, fn, head, env);
            }
        }
        /* Append elements from `last` collection. */
        if (last != NULL && mino_type_of(last) != MINO_NIL) {
            seq_iter_t it;
            seq_iter_init(S, &it, last);
            while (!seq_iter_done(&it)) {
                mino_val *cell = mino_cons(S, seq_iter_val(S, &it), mino_nil(S));
                if (tail2 == NULL) { head = cell; } else { mino_cons_cdr_set(S, tail2, cell); }
                tail2 = cell;
                seq_iter_next(S, &it);
            }
        }
        return apply_callable(S, fn, head, env);
    }
    /* (apply f coll) — convert coll to a cons arg list. */
    if (last == NULL || mino_type_of(last) == MINO_NIL
        || mino_type_of(last) == MINO_EMPTY_LIST) {
        return apply_callable(S, fn, mino_nil(S), env);
    }
    if (mino_type_of(last) == MINO_CONS) {
        return apply_callable(S, fn, last, env);
    }
    /* Lazy-safe pure-LAZY collection: hand the lazy seq directly. */
    if (mino_type_of(last) == MINO_LAZY) {
        if (lazy_safe < 0) lazy_safe = fn_lazy_safe_rest(fn);
        if (lazy_safe) {
            return apply_callable(S, fn, last, env);
        }
    }
    /* Convert non-list collection to cons list. */
    {
        mino_val *head = mino_nil(S);
        mino_val *tail2 = NULL;
        seq_iter_t it;
        seq_iter_init(S, &it, last);
        while (!seq_iter_done(&it)) {
            mino_val *cell = mino_cons(S, seq_iter_val(S, &it), mino_nil(S));
            if (tail2 == NULL) { head = cell; } else { mino_cons_cdr_set(S, tail2, cell); }
            tail2 = cell;
            seq_iter_next(S, &it);
        }
        call_args = head;
    }
    return apply_callable(S, fn, call_args, env);
}

static mino_val *prim_reverse(mino_state *S, mino_val *args, mino_env *env)
{
    /* Per Clojure, (reverse nil) and (reverse <empty>) return the
     * empty list (), not nil. Otherwise iterate the collection and
     * cons each element onto the running head (matching Clojure's
     * reverse contract: returns a sequence). Non-seqable inputs
     * (numbers, keywords, symbols, ...) throw via `prim_seq`'s
     * coercion check; the silent "treat as empty" path was wrong
     * because it suppressed the type error Clojure raises. */
    mino_val *coll;
    mino_val *seqd;
    mino_val *out = mino_empty_list(S);
    mino_val *seq_args;
    seq_iter_t  it;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "reverse requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_empty_list(S);
    }
    seq_args = mino_cons(S, coll, mino_nil(S));
    seqd = prim_seq(S, seq_args, env);
    if (seqd == NULL) return NULL;
    if (mino_type_of(seqd) == MINO_NIL) return mino_empty_list(S);
    seq_iter_init(S, &it, seqd);
    while (!seq_iter_done(&it)) {
        out = mino_cons(S, seq_iter_val(S, &it), out);
        seq_iter_next(S, &it);
    }
    return out;
}

/* val_compare is now in rbtree.c (shared with sorted map/set).
 * Sort comparator state: when sort_comp_fn is non-NULL, the merge sort
 * calls the user-supplied comparison function instead of val_compare. */

static int sort_compare(mino_state *S, const mino_val *a, const mino_val *b)
{
    if (S->sort_comp_fn != NULL) {
        mino_val *call_args = mino_cons(S, (mino_val *)a,
                                  mino_cons(S, (mino_val *)b, mino_nil(S)));
        mino_val *result = mino_call(S, S->sort_comp_fn, call_args, S->sort_comp_env);
        if (result == NULL) return 0;
        /* Numeric result: use sign directly (compare-style) */
        if (mino_val_int_p(result)) {
            return mino_val_int_get(result) < 0 ? -1 : mino_val_int_get(result) > 0 ? 1 : 0;
        }
        if (mino_type_of(result) == MINO_FLOAT) {
            return result->as.f < 0 ? -1 : result->as.f > 0 ? 1 : 0;
        }
        /* Boolean result: true means a < b, false means a >= b */
        return mino_is_truthy_inline(result) ? -1 : 1;
    }
    /* Default comparator: route through prim_compare so cross-type
     * pairs throw a typed error instead of silently ordering by tag.
     * (sort [1 []]) -> ClassCastException-shaped error matches Clojure. */
    {
        mino_val *args = mino_cons(S, (mino_val *)a,
                              mino_cons(S, (mino_val *)b, mino_nil(S)));
        mino_val *r = prim_compare(S, args, NULL);
        if (r == NULL) return 0;
        if (mino_val_int_p(r)) {
            return mino_val_int_get(r) < 0 ? -1 : mino_val_int_get(r) > 0 ? 1 : 0;
        }
        if (mino_type_of(r) == MINO_FLOAT) {
            return r->as.f < 0 ? -1 : r->as.f > 0 ? 1 : 0;
        }
        return 0;
    }
}

/* Merge sort for mino_val* arrays. */
static void merge_sort_vals(mino_state *S, mino_val **arr, mino_val **tmp, size_t len)
{
    size_t mid, i, j, k;
    if (len <= 1) return;
    mid = len / 2;
    merge_sort_vals(S, arr, tmp, mid);
    merge_sort_vals(S, arr + mid, tmp, len - mid);
    memcpy(tmp, arr, mid * sizeof(*tmp));
    i = 0; j = mid; k = 0;
    while (i < mid && j < len) {
        if (sort_compare(S, tmp[i], arr[j]) <= 0) {
            arr[k++] = tmp[i++];
        } else {
            arr[k++] = arr[j++];
        }
    }
    while (i < mid) { arr[k++] = tmp[i++]; }
}

/* (sort coll) or (sort comp coll) */
static mino_val *prim_sort(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    mino_val *comp = NULL;
    mino_val **arr;
    mino_val **tmp;
    mino_val *head = mino_nil(S);
    mino_val *tail = NULL;
    size_t      n_items, i;
    seq_iter_t  it;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "sort requires one or two arguments");
    }
    if (mino_is_cons(args->as.cons.cdr) &&
        !mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        /* Two args: (sort comp coll) */
        comp = args->as.cons.car;
        coll = args->as.cons.cdr->as.cons.car;
    } else if (!mino_is_cons(args->as.cons.cdr)) {
        /* One arg: (sort coll) */
        coll = args->as.cons.car;
    } else {
        return prim_throw_classified(S, "eval/arity", "MAR001", "sort requires one or two arguments");
    }
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_empty_list(S);
    }
    /* Validate seqability eagerly: non-coll inputs throw rather than
     * silently produce an empty result. Routes through prim_seq for the
     * shared coercion check. */
    {
        mino_val *seq_args = mino_cons(S, coll, mino_nil(S));
        mino_val *seqd = prim_seq(S, seq_args, env);
        if (seqd == NULL) return NULL;
        if (mino_type_of(seqd) == MINO_NIL) return mino_empty_list(S);
        coll = seqd;
    }
    /* Collect elements into an array. */
    n_items = 0;
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) { n_items++; seq_iter_next(S, &it); }
    if (n_items == 0) return mino_empty_list(S);
    if (n_items > SIZE_MAX / sizeof(*arr)) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
            "sort: collection too large");
    }
    arr = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, n_items * sizeof(*arr));
    tmp = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, n_items * sizeof(*tmp));
    i = 0;
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) { arr[i++] = seq_iter_val(S, &it); seq_iter_next(S, &it); }
    S->sort_comp_fn  = comp;
    S->sort_comp_env = env;
    merge_sort_vals(S, arr, tmp, n_items);
    S->sort_comp_fn  = NULL;
    S->sort_comp_env = NULL;
    for (i = 0; i < n_items; i++) {
        mino_val *cell = mino_cons(S, arr[i], mino_nil(S));
        if (tail == NULL) { head = cell; } else { mino_cons_cdr_set(S, tail, cell); }
        tail = cell;
    }
    return head;
}

static mino_val *prim_peek(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "peek requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL
        || mino_type_of(coll) == MINO_EMPTY_LIST) return mino_nil(S);
    if (mino_type_of(coll) == MINO_VECTOR) {
        if (coll->as.vec.len == 0) return mino_nil(S);
        return vec_nth(coll, coll->as.vec.len - 1);
    }
    /* Only list-shaped MINO_CONS cells are stack-eligible. The result
     * of a (cons x y) call has not_list=1 and falls through to throw,
     * matching JVM Clojure where Cons is not a stack. */
    if (mino_type_of(coll) == MINO_CONS && !coll->as.cons.not_list)
        return coll->as.cons.car;
    if (mino_type_of(coll) == MINO_QUEUE) {
        mino_val *first = mino_queue_peek(coll);
        return (first != NULL) ? first : mino_nil(S);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "peek: expected a vector, list, or queue, got %s",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

mino_val *prim_pop(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "pop requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return mino_nil(S);
    }
    if (mino_type_of(coll) == MINO_VECTOR) {
        if (coll->as.vec.len == 0) {
            return prim_throw_classified(S, "eval/bounds", "MBD001", "pop: cannot pop an empty vector");
        }
        return vec_pop(S, coll);
    }
    if (mino_type_of(coll) == MINO_EMPTY_LIST) {
        return prim_throw_classified(S, "eval/bounds", "MBD001",
            "pop: cannot pop an empty list");
    }
    if (mino_type_of(coll) == MINO_CONS && !coll->as.cons.not_list) {
        /* Single-element list -> empty list, not nil. Clojure-canon:
         * pop returns the collection minus its last element; the empty
         * collection of a list is `()`, which has type :list and is
         * `list?`-true, while `nil` is neither. Returning nil here used
         * to make (pop '(1)) silently differ from JVM/CLJS. */
        if (coll->as.cons.cdr == NULL
            || mino_type_of(coll->as.cons.cdr) == MINO_NIL) {
            return mino_empty_list(S);
        }
        return coll->as.cons.cdr;
    }
    if (mino_type_of(coll) == MINO_QUEUE) {
        /* Popping an empty queue yields the empty queue (queues are
         * the one collection where pop is total). */
        return mino_queue_pop(S, coll);
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "pop: expected a vector, list, or queue, got %s",
                 type_tag_str(coll));
        return prim_throw_classified(S, "eval/type", "MTY001", msg);
    }
}

static mino_val *prim_find(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *m;
    mino_val *k;
    mino_val *v;
    (void)env;
    if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)
        || mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "find requires two arguments");
    }
    m = args->as.cons.car;
    k = args->as.cons.cdr->as.cons.car;
    if (m == NULL || mino_type_of(m) == MINO_NIL) return mino_nil(S);
    /* Transient associatives delegate to their underlying persistent
     * collection, matching Clojure's semantics. */
    if (mino_type_of(m) == MINO_TRANSIENT) {
        if (!m->as.transient.valid)
            return prim_throw_classified(S, "eval/state", "MST001",
                "find: transient is no longer valid");
        m = m->as.transient.current;
        if (m == NULL || mino_type_of(m) == MINO_NIL) return mino_nil(S);
    }
    if (mino_type_of(m) == MINO_SORTED_MAP) {
        if (!rb_contains(S, m->as.sorted.root, k, m->as.sorted.comparator))
            return mino_nil(S);
        v = rb_get(S, m->as.sorted.root, k, m->as.sorted.comparator);
        return mino_map_entry(S, k, v);
    }
    if (mino_type_of(m) == MINO_VECTOR) {
        long long idx;
        if (!mino_val_int_p(k)) return mino_nil(S);
        idx = mino_val_int_get(k);
        if (idx < 0 || (size_t)idx >= m->as.vec.len) return mino_nil(S);
        return mino_map_entry(S, k, vec_nth(m, (size_t)idx));
    }
    if (mino_type_of(m) == MINO_RECORD) {
        int idx = record_field_index(m, k);
        if (idx >= 0) {
            mino_val *fields = m->as.record.type->as.record_type.fields;
            return mino_map_entry(S, vec_nth(fields, idx),
                                     m->as.record.vals[idx]);
        }
        if (m->as.record.ext != NULL) {
            v = map_get_val(m->as.record.ext, k);
            if (v != NULL) return mino_map_entry(S, k, v);
        }
        return mino_nil(S);
    }
    if (mino_type_of(m) != MINO_MAP) {
        return prim_throw_classified(S, "eval/type", "MTY001", "find: first argument must be an associative collection");
    }
    v = map_get_val(m, k);
    if (v == NULL) return mino_nil(S);
    return mino_map_entry(S, k, v);
}

static mino_val *prim_empty(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "empty requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) return mino_nil(S);
    {
        mino_val *r;
        switch (mino_type_of(coll)) {
        case MINO_VECTOR:
            r = mino_vector(S, NULL, 0);
            r->meta = coll->meta;
            return r;
        case MINO_MAP:
            r = mino_map(S, NULL, NULL, 0);
            r->meta = coll->meta;
            return r;
        case MINO_SET:
            r = mino_set(S, NULL, 0);
            r->meta = coll->meta;
            return r;
        case MINO_SORTED_MAP:
            r = mino_sorted_map(S, NULL, NULL, 0);
            r->meta = coll->meta;
            return r;
        case MINO_SORTED_SET:
            r = mino_sorted_set(S, NULL, 0);
            r->meta = coll->meta;
            return r;
        case MINO_CONS:
        case MINO_CHUNKED_CONS:
        case MINO_LAZY:
        case MINO_EMPTY_LIST:
            /* Per Clojure, (empty seq) is the empty list (). */
            return mino_empty_list(S);
        case MINO_QUEUE:
            r = mino_queue_empty(S);
            r->meta = coll->meta;
            return r;
        case MINO_BYTES:
            /* (empty bytes-value) returns a fresh empty bytes value. */
            r = mino_bytes(S, NULL, 0);
            if (r != NULL) r->meta = coll->meta;
            return r;
        default:
            return mino_nil(S);
        }
    }
}

static mino_val *prim_rseq(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    mino_val *out;
    size_t i;
    (void)env;
    if (!mino_is_cons(args) || mino_is_cons(args->as.cons.cdr)) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "rseq requires one argument");
    }
    coll = args->as.cons.car;
    if (coll == NULL || mino_type_of(coll) == MINO_NIL) {
        return prim_throw_classified(S, "eval/type", "MTY001", "rseq: argument must not be nil");
    }
    if (mino_type_of(coll) == MINO_SORTED_MAP || mino_type_of(coll) == MINO_SORTED_SET) {
        /* Reverse of sorted collection — build reverse cons list from
         * the in-order key list. */
        mino_val *keys = mino_nil(S);
        mino_val *kt   = NULL;
        rb_to_list(S, coll->as.sorted.root, &keys, &kt);
        out = mino_nil(S);
        if (mino_type_of(coll) == MINO_SORTED_MAP) {
            while (mino_is_cons(keys)) {
                mino_val *k = keys->as.cons.car;
                mino_val *v = rb_get(S, coll->as.sorted.root, k,
                                       coll->as.sorted.comparator);
                out = mino_cons(S, mino_map_entry(S, k, v), out);
                keys = keys->as.cons.cdr;
            }
        } else {
            while (mino_is_cons(keys)) {
                out = mino_cons(S, keys->as.cons.car, out);
                keys = keys->as.cons.cdr;
            }
        }
        return out;
    }
    if (mino_type_of(coll) != MINO_VECTOR) {
        return prim_throw_classified(S, "eval/type", "MTY001", "rseq: argument must be a vector or sorted collection");
    }
    if (coll->as.vec.len == 0) return mino_nil(S);
    out = mino_nil(S);
    for (i = 0; i < coll->as.vec.len; i++) {
        out = mino_cons(S, vec_nth(coll, i), out);
    }
    return out;
}

static mino_val *prim_sorted_map(mino_state *S, mino_val *args, mino_env *env)
{
    size_t n, pairs, i;
    mino_val **ks, **vs, *p;
    (void)env;
    arg_count(S, args, &n);
    if (n % 2 != 0) {
        return prim_throw_classified(S, "eval/arity", "MAR001", "sorted-map requires an even number of arguments");
    }
    pairs = n / 2;
    if (pairs == 0) return mino_sorted_map(S, NULL, NULL, 0);
    ks = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*ks));
    vs = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*vs));
    p = args;
    for (i = 0; i < pairs; i++) {
        ks[i] = p->as.cons.car; p = p->as.cons.cdr;
        vs[i] = p->as.cons.car; p = p->as.cons.cdr;
    }
    return mino_sorted_map(S, ks, vs, pairs);
}

static mino_val *prim_sorted_set(mino_state *S, mino_val *args, mino_env *env)
{
    size_t n, i;
    mino_val **tmp, *p;
    (void)env;
    arg_count(S, args, &n);
    if (n == 0) return mino_sorted_set(S, NULL, 0);
    tmp = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, n * sizeof(*tmp));
    p = args;
    for (i = 0; i < n; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_sorted_set(S, tmp, n);
}

static mino_val *prim_sorted_map_by(mino_state *S, mino_val *args, mino_env *env)
{
    size_t n, pairs, i;
    mino_val *comparator, **ks, **vs, *p;
    (void)env;
    arg_count(S, args, &n);
    if (n < 1 || (n - 1) % 2 != 0) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "sorted-map-by requires a comparator and an even number of keys and values");
    }
    comparator = args->as.cons.car;
    if (comparator == NULL
        || (mino_type_of(comparator) != MINO_FN && mino_type_of(comparator) != MINO_PRIM)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "sorted-map-by: comparator must be a function");
    }
    pairs = (n - 1) / 2;
    if (pairs == 0) return mino_sorted_map_by(S, comparator, NULL, NULL, 0);
    ks = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*ks));
    vs = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, pairs * sizeof(*vs));
    p = args->as.cons.cdr;
    for (i = 0; i < pairs; i++) {
        ks[i] = p->as.cons.car; p = p->as.cons.cdr;
        vs[i] = p->as.cons.car; p = p->as.cons.cdr;
    }
    return mino_sorted_map_by(S, comparator, ks, vs, pairs);
}

static mino_val *prim_sorted_set_by(mino_state *S, mino_val *args, mino_env *env)
{
    size_t n, items, i;
    mino_val *comparator, **tmp, *p;
    (void)env;
    arg_count(S, args, &n);
    if (n < 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "sorted-set-by requires a comparator");
    }
    comparator = args->as.cons.car;
    if (comparator == NULL
        || (mino_type_of(comparator) != MINO_FN && mino_type_of(comparator) != MINO_PRIM)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            "sorted-set-by: comparator must be a function");
    }
    items = n - 1;
    if (items == 0) return mino_sorted_set_by(S, comparator, NULL, 0);
    tmp = (mino_val **)gc_alloc_typed(S, GC_T_VALARR, items * sizeof(*tmp));
    p = args->as.cons.cdr;
    for (i = 0; i < items; i++) {
        tmp[i] = p->as.cons.car;
        p = p->as.cons.cdr;
    }
    return mino_sorted_set_by(S, comparator, tmp, items);
}

/* Classify one of the four comparison primitives (<, <=, >, >=) by
 * registered name. On success, *is_gt tells whether the test is a >
 * family test and *inclusive tells whether the boundary value itself
 * is included. Returns 0 if v is not one of the four accepted tests. */
static int classify_subseq_test(const mino_val *v, int *is_gt, int *inclusive)
{
    if (v == NULL || mino_type_of(v) != MINO_PRIM) return 0;
    const char *name = v->as.prim.name;
    if (name == NULL) return 0;
    if (strcmp(name, "<")  == 0) { *is_gt = 0; *inclusive = 0; return 1; }
    if (strcmp(name, "<=") == 0) { *is_gt = 0; *inclusive = 1; return 1; }
    if (strcmp(name, ">")  == 0) { *is_gt = 1; *inclusive = 0; return 1; }
    if (strcmp(name, ">=") == 0) { *is_gt = 1; *inclusive = 1; return 1; }
    return 0;
}

/* Shared body for subseq / rsubseq. reverse = 0 for subseq, 1 for rsubseq.
 *
 * Three-arg form: (subseq sc test key)
 *   A > or >= test means "entries whose key is > / >= key" (lower bound).
 *   A < or <= test means "entries whose key is < / <= key" (upper bound).
 *
 * Five-arg form: (subseq sc start-test start-key end-test end-key)
 *   Matches JVM Clojure: for subseq, scan ascending from start-key,
 *   drop the first element if start-test fails on it, then takeWhile
 *   end-test. For rsubseq, scan descending from end-key, drop the
 *   first element if end-test fails, then takeWhile start-test.
 *   start-test/end-test must each be one of <, <=, >, >=. */
static mino_val *subseq_impl(mino_state *S, mino_val *args, int reverse)
{
    size_t n;
    mino_val *sc;
    mino_val *head = mino_nil(S);
    mino_val *tail = NULL;
    int is_map;
    arg_count(S, args, &n);
    if (n != 3 && n != 5) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            reverse
              ? "rsubseq requires 3 or 5 arguments"
              : "subseq requires 3 or 5 arguments");
    }
    sc = args->as.cons.car;
    if (sc == NULL
        || (mino_type_of(sc) != MINO_SORTED_MAP && mino_type_of(sc) != MINO_SORTED_SET)) {
        return prim_throw_classified(S, "eval/type", "MTY001",
            reverse
              ? "rsubseq: first argument must be a sorted map or sorted set"
              : "subseq: first argument must be a sorted map or sorted set");
    }
    is_map = mino_type_of(sc) == MINO_SORTED_MAP;
    if (n == 3) {
        mino_val *test = args->as.cons.cdr->as.cons.car;
        mino_val *key  = args->as.cons.cdr->as.cons.cdr->as.cons.car;
        int is_gt, inclusive;
        int has_lo = 0, lo_inclusive = 0;
        int has_hi = 0, hi_inclusive = 0;
        mino_val *lo_key = NULL;
        mino_val *hi_key = NULL;
        if (!classify_subseq_test(test, &is_gt, &inclusive)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                reverse
                  ? "rsubseq: test must be <, <=, > or >="
                  : "subseq: test must be <, <=, > or >=");
        }
        if (is_gt) {
            has_lo = 1; lo_inclusive = inclusive; lo_key = key;
        } else {
            has_hi = 1; hi_inclusive = inclusive; hi_key = key;
        }
        if (sc->as.sorted.len == 0) return mino_nil(S);
        rb_bounded_seq(S, sc->as.sorted.root, is_map,
                       has_lo, lo_inclusive, lo_key,
                       has_hi, hi_inclusive, hi_key,
                       sc->as.sorted.comparator, reverse,
                       &head, &tail);
        return head;
    }
    {
        mino_val *p = args->as.cons.cdr;
        mino_val *start_test = p->as.cons.car; p = p->as.cons.cdr;
        mino_val *start_key  = p->as.cons.car; p = p->as.cons.cdr;
        mino_val *end_test   = p->as.cons.car; p = p->as.cons.cdr;
        mino_val *end_key    = p->as.cons.car;
        int start_is_gt, start_inclusive;
        int end_is_gt, end_inclusive;
        mino_val *raw_head = mino_nil(S);
        mino_val *raw_tail = NULL;
        mino_val *result_head = mino_nil(S);
        mino_val *result_tail = NULL;
        mino_val *pivot_key;
        int pivot_inclusive;
        int filter_is_gt, filter_inclusive;
        mino_val *filter_key;
        mino_val *cur;
        if (!classify_subseq_test(start_test, &start_is_gt, &start_inclusive)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                reverse
                  ? "rsubseq: start-test must be <, <=, > or >="
                  : "subseq: start-test must be <, <=, > or >=");
        }
        if (!classify_subseq_test(end_test, &end_is_gt, &end_inclusive)) {
            return prim_throw_classified(S, "eval/type", "MTY001",
                reverse
                  ? "rsubseq: end-test must be <, <=, > or >="
                  : "subseq: end-test must be <, <=, > or >=");
        }
        if (sc->as.sorted.len == 0) return mino_nil(S);
        /* Step 1: seqFrom(pivot, direction). For subseq the pivot is
         * start-key (ascending lower bound, inclusive); for rsubseq it's
         * end-key (descending upper bound, inclusive). The first cell may
         * be dropped in step 2 if the pivot-test fails on it. */
        if (!reverse) {
            rb_bounded_seq(S, sc->as.sorted.root, is_map,
                           1, 1, start_key,
                           0, 0, NULL,
                           sc->as.sorted.comparator, 0,
                           &raw_head, &raw_tail);
            pivot_key       = start_key;
            pivot_inclusive = start_inclusive;
            filter_is_gt    = end_is_gt;
            filter_inclusive= end_inclusive;
            filter_key      = end_key;
        } else {
            rb_bounded_seq(S, sc->as.sorted.root, is_map,
                           0, 0, NULL,
                           1, 1, end_key,
                           sc->as.sorted.comparator, 1,
                           &raw_head, &raw_tail);
            pivot_key       = end_key;
            pivot_inclusive = end_inclusive;
            filter_is_gt    = start_is_gt;
            filter_inclusive= start_inclusive;
            filter_key      = start_key;
        }
        cur = raw_head;
        /* Step 2: drop the first element if the pivot-test fails on it.
         * Only happens when first-key equals pivot-key and the pivot-test
         * is exclusive (> or <). */
        if (cur != NULL && mino_type_of(cur) == MINO_CONS && !pivot_inclusive) {
            mino_val *first_entry = cur->as.cons.car;
            mino_val *first_key   = is_map
                ? first_entry->as.map_entry.k
                : first_entry;
            int cmp = rb_compare(S, first_key, pivot_key,
                                 sc->as.sorted.comparator);
            if (cmp == 0) cur = cur->as.cons.cdr;
        }
        /* Step 3: takeWhile the other filter. */
        while (cur != NULL && mino_type_of(cur) == MINO_CONS) {
            mino_val *entry = cur->as.cons.car;
            mino_val *key   = is_map ? entry->as.map_entry.k : entry;
            int cmp = rb_compare(S, key, filter_key,
                                 sc->as.sorted.comparator);
            int keep;
            if (filter_is_gt) {
                keep = filter_inclusive ? (cmp >= 0) : (cmp > 0);
            } else {
                keep = filter_inclusive ? (cmp <= 0) : (cmp < 0);
            }
            if (!keep) break;
            {
                mino_val *cell = mino_cons(S, entry, mino_nil(S));
                if (result_tail == NULL) result_head = cell;
                else mino_cons_cdr_set(S, result_tail, cell);
                result_tail = cell;
            }
            cur = cur->as.cons.cdr;
        }
        /* JVM Clojure's 5-arg subseq/rsubseq returns nil only when
         * seqFrom yielded no elements (when-let binding failed). When
         * seqFrom was non-empty but takeWhile drained the result to
         * nothing, the result is `()`, not nil. */
        if (result_head == NULL || mino_type_of(result_head) == MINO_NIL) {
            if (raw_head != NULL && mino_type_of(raw_head) == MINO_CONS) {
                return mino_empty_list(S);
            }
            return mino_nil(S);
        }
        return result_head;
    }
}

static mino_val *prim_subseq(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    return subseq_impl(S, args, 0);
}

static mino_val *prim_rsubseq(mino_state *S, mino_val *args, mino_env *env)
{
    (void)env;
    return subseq_impl(S, args, 1);
}

/* (every? pred coll) — true iff (pred x) is truthy for every x in coll.
 * Empty / nil coll → true. Short-circuits on the first falsy result. */
static mino_val *prim_every_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *pred, *coll;
    seq_iter_t  it;
    size_t      n;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "every? requires 2 arguments: predicate and collection");
    }
    pred = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || mino_is_nil(coll)) return mino_true(S);
    /* Validate seqability eagerly so non-seqable inputs throw rather
     * than vacuously returning true. */
    {
        mino_val *seq_args = mino_cons(S, coll, mino_nil(S));
        mino_val *seqd = prim_seq(S, seq_args, env);
        if (seqd == NULL) return NULL;
        if (mino_type_of(seqd) == MINO_NIL) return mino_true(S);
        coll = seqd;
    }
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val *elem = seq_iter_val(S, &it);
        mino_val *call_args = mino_cons(S, elem, mino_nil(S));
        mino_val *test = apply_callable(S, pred, call_args, env);
        if (test == NULL) return NULL;
        if (!mino_is_truthy_inline(test)) return mino_false(S);
        seq_iter_next(S, &it);
    }
    return mino_true(S);
}

/* (some pred coll) — first truthy value of (pred x) for any x in coll,
 * else nil. Short-circuits on the first truthy result. */
static mino_val *prim_some(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *pred, *coll;
    seq_iter_t  it;
    size_t      n;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "some requires 2 arguments: predicate and collection");
    }
    pred = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    if (coll == NULL || mino_is_nil(coll)) return mino_nil(S);
    /* Validate seqability eagerly so non-seqable inputs throw rather
     * than silently returning nil. */
    {
        mino_val *seq_args = mino_cons(S, coll, mino_nil(S));
        mino_val *seqd = prim_seq(S, seq_args, env);
        if (seqd == NULL) return NULL;
        if (mino_type_of(seqd) == MINO_NIL) return mino_nil(S);
        coll = seqd;
    }
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val *elem = seq_iter_val(S, &it);
        mino_val *call_args = mino_cons(S, elem, mino_nil(S));
        mino_val *test = apply_callable(S, pred, call_args, env);
        if (test == NULL) return NULL;
        if (mino_is_truthy_inline(test)) return test;
        seq_iter_next(S, &it);
    }
    return mino_nil(S);
}

/* (not-any? pred coll) — true iff (pred x) is falsy for every x in coll. */
static mino_val *prim_not_any_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *r = prim_some(S, args, env);
    if (r == NULL) return NULL;
    return mino_is_truthy_inline(r) ? mino_false(S) : mino_true(S);
}

/* (not-every? pred coll) — true iff (pred x) is falsy for at least one x. */
static mino_val *prim_not_every_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *r = prim_every_p(S, args, env);
    if (r == NULL) return NULL;
    return mino_is_truthy_inline(r) ? mino_false(S) : mino_true(S);
}

/* Helper: build a [& args] params vector. */
static mino_val *params_amp_args(mino_state *S)
{
    mino_val *items[2];
    items[0] = mino_symbol(S, "&");
    items[1] = mino_symbol(S, "args");
    return mino_vector(S, items, 2);
}

/* (complement f) — returns a fn whose result is (not (apply f args)). */
static mino_val *prim_complement(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *f;
    mino_env *fn_env;
    mino_val *body;
    size_t n;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "complement requires 1 argument");
    }
    f = args->as.cons.car;
    fn_env = env_child(S, env);
    env_bind(S, fn_env, "f", f);
    /* body forms: ((not (apply f args))) */
    {
        mino_val *apply_form = mino_cons(S, mino_symbol(S, "apply"),
            mino_cons(S, mino_symbol(S, "f"),
                mino_cons(S, mino_symbol(S, "args"), mino_nil(S))));
        mino_val *not_form = mino_cons(S, mino_symbol(S, "not"),
            mino_cons(S, apply_form, mino_nil(S)));
        body = mino_cons(S, not_form, mino_nil(S));
    }
    return make_fn(S, params_amp_args(S), body, fn_env);
}

/* (comp) -> identity; (comp f) -> f; (comp f g ...) -> applied right to left.
 * Result is (fn [& args] (f (g (... (apply h args))))). */
static mino_val *prim_comp(mino_state *S, mino_val *args, mino_env *env)
{
    size_t n;
    mino_val *fs_vec;
    mino_env *fn_env;
    mino_val *body;
    arg_count(S, args, &n);
    if (n == 0) {
        mino_val *identity = var_find(S, "clojure.core", "identity");
        if (identity != NULL && mino_type_of(identity) == MINO_VAR) {
            return identity->as.var.root;
        }
        return mino_nil(S);
    }
    if (n == 1) return args->as.cons.car;
    /* Capture the fns as a vector in the closure env. The body's apply
     * chain reads from this vector by index so we don't allocate per
     * invocation. Build body as:
     *   (let [r (apply LAST args)]
     *     (PEN-2 (PEN-3 (... (FIRST r) ...))))
     * Simpler: build a recursive (reduce (fn [a g] (g a)) ((last fs) args) ...)
     * Even simpler: emit (apply (fn [...] (f1 (f2 (... (apply fk args))))) ...).
     * Most compact: use a Clojure form that reads fs from the env at
     * each call. */
    {
        mino_val **items = (mino_val **)malloc(n * sizeof(mino_val *));
        mino_val *cur;
        size_t i;
        if (items == NULL) {
            return prim_throw_classified(S, "eval/oom", "MOM001",
                "comp: out of memory");
        }
        /* Suppress GC while items[] (C-heap, GC-invisible) holds live pointers.
         * Mirrors the gc_depth guard in prim_rangev. */
        mino_current_ctx(S)->gc_depth++;
        cur = args;
        for (i = 0; i < n; i++) {
            items[i] = cur->as.cons.car;
            cur = cur->as.cons.cdr;
        }
        fs_vec = mino_vector(S, items, n);
        mino_current_ctx(S)->gc_depth--;
        free(items);
    }
    fn_env = env_child(S, env);
    env_bind(S, fn_env, "fs", fs_vec);
    /* body: ((reduce (fn [acc f] (f acc))
     *                (apply (peek fs) args)
     *                (rseq (pop fs)))) */
    {
        mino_val *sym_acc   = mino_symbol(S, "acc");
        mino_val *sym_f     = mino_symbol(S, "f");
        mino_val *sym_args  = mino_symbol(S, "args");
        mino_val *sym_fs    = mino_symbol(S, "fs");
        /* inner-fn params: [acc f] */
        mino_val *inner_params_items[2] = {sym_acc, sym_f};
        mino_val *inner_params = mino_vector(S, inner_params_items, 2);
        /* inner-fn body: ((f acc)) */
        mino_val *inner_call = mino_cons(S, sym_f,
            mino_cons(S, sym_acc, mino_nil(S)));
        mino_val *inner_body = mino_cons(S, inner_call, mino_nil(S));
        mino_val *inner_fn = mino_cons(S, mino_symbol(S, "fn"),
            mino_cons(S, inner_params,
                mino_cons(S, inner_call, mino_nil(S))));
        (void)inner_body;
        /* init form: (apply (peek fs) args) */
        mino_val *peek_call = mino_cons(S, mino_symbol(S, "peek"),
            mino_cons(S, sym_fs, mino_nil(S)));
        mino_val *init_form = mino_cons(S, mino_symbol(S, "apply"),
            mino_cons(S, peek_call,
                mino_cons(S, sym_args, mino_nil(S))));
        /* (rseq (pop fs)) */
        mino_val *pop_call = mino_cons(S, mino_symbol(S, "pop"),
            mino_cons(S, sym_fs, mino_nil(S)));
        mino_val *rseq_call = mino_cons(S, mino_symbol(S, "rseq"),
            mino_cons(S, pop_call, mino_nil(S)));
        /* (reduce inner-fn init-form rseq-call) */
        mino_val *reduce_form = mino_cons(S, mino_symbol(S, "reduce"),
            mino_cons(S, inner_fn,
                mino_cons(S, init_form,
                    mino_cons(S, rseq_call, mino_nil(S)))));
        body = mino_cons(S, reduce_form, mino_nil(S));
    }
    return make_fn(S, params_amp_args(S), body, fn_env);
}

/* (partial f) -> f; (partial f & args) -> fn that calls f with args prepended. */
static mino_val *prim_partial(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *f;
    mino_val *bound_args;
    mino_env *fn_env;
    mino_val *body;
    size_t n;
    arg_count(S, args, &n);
    if (n == 0) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "partial requires at least 1 argument");
    }
    f = args->as.cons.car;
    if (n == 1) return f;
    bound_args = args->as.cons.cdr;
    fn_env = env_child(S, env);
    env_bind(S, fn_env, "f", f);
    env_bind(S, fn_env, "bound", bound_args);
    /* body: ((apply f (concat bound args))) */
    {
        mino_val *concat_call = mino_cons(S, mino_symbol(S, "concat"),
            mino_cons(S, mino_symbol(S, "bound"),
                mino_cons(S, mino_symbol(S, "args"), mino_nil(S))));
        mino_val *apply_form = mino_cons(S, mino_symbol(S, "apply"),
            mino_cons(S, mino_symbol(S, "f"),
                mino_cons(S, concat_call, mino_nil(S))));
        body = mino_cons(S, apply_form, mino_nil(S));
    }
    return make_fn(S, params_amp_args(S), body, fn_env);
}

/* (juxt f g ...) — returns a fn that returns [(f a..) (g a..) ...] for its args. */
static mino_val *prim_juxt(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *fs;
    mino_env *fn_env;
    mino_val *body;
    size_t n;
    arg_count(S, args, &n);
    if (n == 0) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "juxt requires at least 1 argument");
    }
    fs = args; /* keep the cons list; bind it as 'fs' */
    fn_env = env_child(S, env);
    env_bind(S, fn_env, "fs", fs);
    /* body: ((mapv (fn [f] (apply f args)) fs)) */
    {
        mino_val *inner_params_items[1] = {mino_symbol(S, "f")};
        mino_val *inner_params = mino_vector(S, inner_params_items, 1);
        mino_val *apply_call = mino_cons(S, mino_symbol(S, "apply"),
            mino_cons(S, mino_symbol(S, "f"),
                mino_cons(S, mino_symbol(S, "args"), mino_nil(S))));
        mino_val *inner_fn = mino_cons(S, mino_symbol(S, "fn"),
            mino_cons(S, inner_params,
                mino_cons(S, apply_call, mino_nil(S))));
        mino_val *mapv_call = mino_cons(S, mino_symbol(S, "mapv"),
            mino_cons(S, inner_fn,
                mino_cons(S, mino_symbol(S, "fs"), mino_nil(S))));
        body = mino_cons(S, mapv_call, mino_nil(S));
    }
    return make_fn(S, params_amp_args(S), body, fn_env);
}

/* (distinct? x ...) — true iff no two args are equal. Empty arglist → true. */
static mino_val *prim_distinct_p(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *cur = args;
    mino_val *seen;
    size_t n_args = 0;
    (void)env;
    if (cur == NULL || mino_is_nil(cur) || !mino_is_cons(cur)) {
        return mino_true(S);
    }
    seen = mino_set(S, NULL, 0);
    while (mino_is_cons(cur)) {
        mino_val *x = cur->as.cons.car;
        seen = set_conj1(S, seen, x);
        if (seen == NULL) return NULL;
        n_args++;
        cur = cur->as.cons.cdr;
    }
    return seen->as.set.len == n_args ? mino_true(S) : mino_false(S);
}

/* (merge-with f m1 m2 ...) — merge maps, calling (f a b) to combine
 * values at shared keys. Returns nil if every map is nil. */
static mino_val *prim_merge_with(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *fn;
    mino_val *cur;
    mino_val *result = NULL;
    int any_non_nil = 0;
    size_t n_inputs = 0;
    if (!mino_is_cons(args)) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "merge-with requires at least 1 argument: combiner f");
    }
    fn  = args->as.cons.car;
    cur = args->as.cons.cdr;
    {
        mino_val *p = cur;
        while (mino_is_cons(p)) {
            n_inputs++;
            p = p->as.cons.cdr;
        }
    }
    while (mino_is_cons(cur)) {
        mino_val *m = cur->as.cons.car;
        cur = cur->as.cons.cdr;
        if (m == NULL || mino_is_nil(m)) continue;
        if (mino_type_of(m) != MINO_MAP) {
            /* JVM: `(merge-with f x)` with a single non-map input
             * returns x (it's `(reduce1 merge2 [x])` -> x). With
             * more than one input it throws. Mirror that so the
             * deep-merge / recursive idiom that bottoms out at a
             * non-map leaf works. */
            if (n_inputs == 1) return m;
            return prim_throw_classified(S, "eval/type", "MTY001",
                "merge-with: every input must be a map or nil");
        }
        any_non_nil = 1;
        if (result == NULL) {
            result = m;
            continue;
        }
        {
            seq_iter_t it;
            seq_iter_init(S, &it, m);
            while (!seq_iter_done(&it)) {
                mino_val *kv = seq_iter_val(S, &it);
                mino_val *k, *v;
                if (kv != NULL && mino_type_of(kv) == MINO_MAP_ENTRY) {
                    k = kv->as.map_entry.k;
                    v = kv->as.map_entry.v;
                } else if (kv != NULL && mino_type_of(kv) == MINO_VECTOR
                           && kv->as.vec.len >= 2) {
                    k = vec_nth(kv, 0);
                    v = vec_nth(kv, 1);
                } else {
                    seq_iter_next(S, &it);
                    continue;
                }
                {
                    mino_val *existing = mino_map_lookup(result, k);
                    if (existing == NULL) {
                        result = mino_map_assoc1(S, result, k, v);
                    } else {
                        mino_val *call_args =
                            mino_cons(S, existing,
                                mino_cons(S, v, mino_nil(S)));
                        mino_val *combined =
                            apply_callable(S, fn, call_args, env);
                        if (combined == NULL) return NULL;
                        result = mino_map_assoc1(S, result, k, combined);
                    }
                    if (result == NULL) return NULL;
                }
                seq_iter_next(S, &it);
            }
        }
    }
    return any_non_nil ? result : mino_nil(S);
}

/* (group-by f coll) — map of (f x) -> vector of items in coll where
 * f's result is the key. Preserves encounter order within each bucket. */
static mino_val *prim_group_by(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *fn, *coll;
    mino_val *result;
    seq_iter_t  it;
    size_t      n;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "group-by requires 2 arguments: f and collection");
    }
    fn   = args->as.cons.car;
    coll = args->as.cons.cdr->as.cons.car;
    result = mino_map(S, NULL, NULL, 0);
    if (coll == NULL || mino_is_nil(coll)) return result;
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val *elem = seq_iter_val(S, &it);
        mino_val *call_args = mino_cons(S, elem, mino_nil(S));
        mino_val *k = apply_callable(S, fn, call_args, env);
        mino_val *bucket;
        if (k == NULL) return NULL;
        bucket = mino_map_lookup(result, k);
        if (bucket == NULL || mino_is_nil(bucket)) {
            bucket = mino_vector(S, &elem, 1);
        } else {
            bucket = vec_conj1(S, bucket, elem);
        }
        if (bucket == NULL) return NULL;
        result = mino_map_assoc1(S, result, k, bucket);
        if (result == NULL) return NULL;
        seq_iter_next(S, &it);
    }
    return result;
}

/* (frequencies coll) — map of distinct items in coll to their count. */
static mino_val *prim_frequencies(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *coll;
    mino_val *result;
    seq_iter_t  it;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 1) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "frequencies requires 1 argument: collection");
    }
    coll = args->as.cons.car;
    result = mino_map(S, NULL, NULL, 0);
    if (coll == NULL || mino_is_nil(coll)) return result;
    seq_iter_init(S, &it, coll);
    while (!seq_iter_done(&it)) {
        mino_val *elem = seq_iter_val(S, &it);
        mino_val *cur  = mino_map_lookup(result, elem);
        long long   prev = 0;
        if (cur != NULL && !mino_is_nil(cur)) {
            (void)mino_to_int(cur, &prev);
        }
        result = mino_map_assoc1(S, result, elem, mino_int(S, prev + 1));
        if (result == NULL) return NULL;
        seq_iter_next(S, &it);
    }
    return result;
}

/* (zipmap ks vs) — map with keys ks paired with vals vs. Stops at the
 * shorter of the two collections. nil/empty pair returns {}. */
static mino_val *prim_zipmap(mino_state *S, mino_val *args, mino_env *env)
{
    mino_val *ks, *vs;
    mino_val *result;
    seq_iter_t  k_it, v_it;
    size_t      n;
    (void)env;
    arg_count(S, args, &n);
    if (n != 2) {
        return prim_throw_classified(S, "eval/arity", "MAR001",
            "zipmap requires 2 arguments: keys and vals");
    }
    ks = args->as.cons.car;
    vs = args->as.cons.cdr->as.cons.car;
    result = mino_map(S, NULL, NULL, 0);
    if (ks == NULL || mino_is_nil(ks)) return result;
    if (vs == NULL || mino_is_nil(vs)) return result;
    /* Validate seqability of both inputs eagerly so non-seqable keys or
     * vals throw rather than silently producing an empty map. */
    {
        mino_val *seq_args = mino_cons(S, ks, mino_nil(S));
        mino_val *seqd = prim_seq(S, seq_args, env);
        if (seqd == NULL) return NULL;
        if (mino_type_of(seqd) == MINO_NIL) return result;
        ks = seqd;
    }
    {
        mino_val *seq_args = mino_cons(S, vs, mino_nil(S));
        mino_val *seqd = prim_seq(S, seq_args, env);
        if (seqd == NULL) return NULL;
        if (mino_type_of(seqd) == MINO_NIL) return result;
        vs = seqd;
    }
    seq_iter_init(S, &k_it, ks);
    seq_iter_init(S, &v_it, vs);
    while (!seq_iter_done(&k_it) && !seq_iter_done(&v_it)) {
        mino_val *k = seq_iter_val(S, &k_it);
        mino_val *v = seq_iter_val(S, &v_it);
        result = mino_map_assoc1(S, result, k, v);
        if (result == NULL) return NULL;
        seq_iter_next(S, &k_it);
        seq_iter_next(S, &v_it);
    }
    return result;
}

const mino_prim_def k_prims_sequences[] = {
    {"reduce",   prim_reduce,
     "Reduces a collection using f. With no init, uses the first item."},
    {"reduced",  prim_reduced,
     "Wraps a value to signal early termination of reduce."},
    {"reduced?", prim_reduced_p,
     "Returns true if x is a reduced value."},
    {"into",     prim_into,
     "Returns a new collection with all items from the source conj'd in."},
    {"set",      prim_set,
     "Returns a set of the items in coll."},
    {"doall",    prim_doall,
     "Forces realization of a lazy sequence. With a leading count, forces at most that many steps. Returns coll."},
    {"dorun",    prim_dorun,
     "Forces realization of a lazy sequence. With a leading count, forces at most that many steps. Returns nil."},
    {"rangev",   prim_rangev,
     "Returns a vector of integers from start (inclusive) to end (exclusive)."},
    {"mapv",     prim_mapv,
     "Returns a vector of applying f to each item in one or more collections."},
    {"filterv",  prim_filterv,
     "Returns a vector of items in coll for which pred returns logical true."},
    {"apply",    prim_apply,
     "Applies f to the arguments, with the last argument spread as a sequence."},
    {"reverse",  prim_reverse,
     "Returns a sequence of the items in coll in reverse order."},
    {"sort",     prim_sort,
     "Returns a sorted sequence of the items in coll."},
    {"peek",     prim_peek,
     "Returns the first item of a list or last item of a vector."},
    {"pop",      prim_pop,
     "Returns a collection without the peek item."},
    {"find",     prim_find,
     "Returns the map entry for the key, or nil."},
    {"empty",    prim_empty,
     "Returns an empty collection of the same type."},
    {"rseq",     prim_rseq,
     "Returns a reverse sequence of a vector, or nil if empty."},
    {"sorted-map", prim_sorted_map,
     "Returns a new sorted map with the given key-value pairs."},
    {"sorted-set", prim_sorted_set,
     "Returns a new sorted set containing the arguments."},
    {"sorted-map-by", prim_sorted_map_by,
     "Returns a sorted map using the given comparator function."},
    {"sorted-set-by", prim_sorted_set_by,
     "Returns a sorted set using the given comparator function."},
    {"subseq",   prim_subseq,
     "Returns the entries of a sorted collection whose keys fall in the given range, ascending."},
    {"rsubseq",  prim_rsubseq,
     "Returns the entries of a sorted collection whose keys fall in the given range, descending."},
    {"seq",       prim_seq,
     "Returns a seq on the collection, or nil if empty."},
    {"realized?", prim_realized_p,
     "Returns true if the lazy value has been realized."},
    {"every?",    prim_every_p,
     "Returns true if (pred x) is truthy for every x in coll."},
    {"some",      prim_some,
     "Returns the first truthy value of (pred x) for any x in coll, else nil."},
    {"not-any?",  prim_not_any_p,
     "Returns true if (pred x) is falsy for every x in coll."},
    {"not-every?", prim_not_every_p,
     "Returns true if (pred x) is falsy for at least one x in coll."},
    {"zipmap",    prim_zipmap,
     "Returns a map with keys mapped to corresponding vals."},
    {"frequencies", prim_frequencies,
     "Returns a map from distinct items in coll to the number of times they appear."},
    {"group-by",  prim_group_by,
     "Returns a map of the items in coll grouped by the result of f."},
    {"distinct?", prim_distinct_p,
     "Returns true if no two of the arguments are equal."},
    {"merge-with", prim_merge_with,
     "Returns the merge of the given maps, calling f to combine values at shared keys."},
    {"complement", prim_complement,
     "Returns a function that returns the logical opposite of f."},
    {"comp",      prim_comp,
     "Returns a function that is the composition of the given functions."},
    {"partial",   prim_partial,
     "Returns a function that applies f with the given arguments prepended."},
    {"juxt",      prim_juxt,
     "Returns a function that returns a vector of applying each f to its args."},
};

const size_t k_prims_sequences_count =
    sizeof(k_prims_sequences) / sizeof(k_prims_sequences[0]);
