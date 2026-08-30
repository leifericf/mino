/*
 * ranges.c -- the generation-split range index (ADR 30).
 *
 * Maps payload address spans to owning headers for the conservative
 * stack scan and interior-pointer mark. Each generation owns a sorted
 * main array plus a pending buffer: the young pair minors maintain
 * alone, the old pair folds at index rebuild. Lookup never reads
 * generation from an entry, so a header resolves from whichever
 * buffer holds its entry at every instant: a new allocation appends
 * to the young pending buffer, merges into the young array at the
 * next minor top, rides there through the sweep that promotes it, and
 * routes to the sorted old pending buffer at the next compact. The
 * old pending buffer folds into the old array at the next full
 * rebuild, or early once it rivals the old array (hard-capped).
 */

#include "runtime/internal.h"

/* Hard cap on the old pending buffer between folds: one million
 * entries. Promotion volume per minor is nursery-bounded and majors
 * fire on old-gen growth, so reaching it means the fold must not wait
 * for the rebuild. The soft threshold below folds far earlier
 * (once pending rivals a fraction of the old array), which keeps the
 * per-compact sort small and the fold cost amortized constant per
 * promoted entry even in collection-dense regimes (stress mode, tight
 * nurseries) where every survivor promotes. */
#define GC_RANGES_OLD_PENDING_CAP  ((size_t)1 << 20)
#define GC_RANGES_OLD_PENDING_SOFT ((size_t)64)

static int gc_range_cmp(const void *a, const void *b);

/* Update heap_min / heap_max from the extents of the three sorted
 * buffers. Called at every point a sorted buffer changes; the young
 * pending buffer is uncovered by design (the fast reject is skipped
 * while it is nonempty). */
static void gc_heap_bounds_update(mino_state *S)
{
    uintptr_t lo = UINTPTR_MAX;
    uintptr_t hi = 0;
    if (S->gc.ranges_y_len > 0) {
        if (S->gc.ranges_y[0].start < lo) {
            lo = S->gc.ranges_y[0].start;
        }
        if (S->gc.ranges_y[S->gc.ranges_y_len - 1].end > hi) {
            hi = S->gc.ranges_y[S->gc.ranges_y_len - 1].end;
        }
    }
    if (S->gc.ranges_o_len > 0) {
        if (S->gc.ranges_o[0].start < lo) {
            lo = S->gc.ranges_o[0].start;
        }
        if (S->gc.ranges_o[S->gc.ranges_o_len - 1].end > hi) {
            hi = S->gc.ranges_o[S->gc.ranges_o_len - 1].end;
        }
    }
    if (S->gc.ranges_o_pending_len > 0) {
        if (S->gc.ranges_o_pending[0].start < lo) {
            lo = S->gc.ranges_o_pending[0].start;
        }
        if (S->gc.ranges_o_pending[S->gc.ranges_o_pending_len - 1].end > hi) {
            hi = S->gc.ranges_o_pending[S->gc.ranges_o_pending_len - 1].end;
        }
    }
    S->gc.heap_min = (lo == UINTPTR_MAX) ? 0 : lo;
    S->gc.heap_max = hi;
}

static int gc_range_cmp(const void *a, const void *b)
{
    const gc_range_t *ra = (const gc_range_t *)a;
    const gc_range_t *rb = (const gc_range_t *)b;
    if (ra->start < rb->start) return -1;
    if (ra->start > rb->start) return 1;
    return 0;
}

/* Grow *buf to hold at least need entries. Returns 0 on failure (the
 * caller invalidates the index so the next collection rebuilds from
 * gc_all), 1 on success. Overflow in the capacity computation is a
 * Class I abort: inside the GC there is no recovery path. */
static int gc_ranges_grow(gc_range_t **buf, size_t *cap, size_t need)
{
    size_t      new_cap;
    gc_range_t *nr;
    if (need <= *cap) {
        return 1;
    }
    if (need > (SIZE_MAX - 16) / 2) {
        abort(); /* Class I: overflow computing range index capacity */
    }
    new_cap = need * 2 + 16;
    nr      = (gc_range_t *)realloc(*buf, new_cap * sizeof(*nr));
    if (nr == NULL) {
        return 0;
    }
    *buf = nr;
    *cap = new_cap;
    return 1;
}

/*
 * Rebuild both sorted arrays from scratch by walking gc_all. Called
 * when the index is invalid: at the top of a collection after a major
 * sweep dropped entries, or as the fallback when pending growth or a
 * merge hit memory pressure. This is the fold point for the old
 * pending buffer: the old array is rebuilt from gc_all_old, which
 * already contains every promoted header, so both pending buffers
 * reset to empty.
 */
void gc_build_range_index(mino_state *S)
{
    gc_hdr_t *h;
    size_t    n_y = 0;
    size_t    n_o = 0;
    for (h = S->gc.all_young; h != NULL; h = h->next) n_y++;
    for (h = S->gc.all_old;   h != NULL; h = h->next) n_o++;
    if (!gc_ranges_grow(&S->gc.ranges_y, &S->gc.ranges_y_cap, n_y)) {
        abort(); /* Class I: inside GC; no safe recovery path */
    }
    if (!gc_ranges_grow(&S->gc.ranges_o, &S->gc.ranges_o_cap, n_o)) {
        abort(); /* Class I: inside GC; no safe recovery path */
    }
    S->gc.ranges_y_len = 0;
    for (h = S->gc.all_young; h != NULL; h = h->next) {
        S->gc.ranges_y[S->gc.ranges_y_len].start = (uintptr_t)(h + 1);
        S->gc.ranges_y[S->gc.ranges_y_len].end   = (uintptr_t)(h + 1) + h->size;
        S->gc.ranges_y[S->gc.ranges_y_len].h     = h;
        S->gc.ranges_y_len++;
    }
    S->gc.ranges_o_len = 0;
    for (h = S->gc.all_old; h != NULL; h = h->next) {
        S->gc.ranges_o[S->gc.ranges_o_len].start = (uintptr_t)(h + 1);
        S->gc.ranges_o[S->gc.ranges_o_len].end   = (uintptr_t)(h + 1) + h->size;
        S->gc.ranges_o[S->gc.ranges_o_len].h     = h;
        S->gc.ranges_o_len++;
    }
    /* Skip the sort when a generation is empty: gc_ranges_grow leaves the
     * array pointer NULL until it first grows, and qsort declares its base
     * argument non-null, so qsort(NULL, 0, ...) is undefined behavior a
     * generation with no live headers would otherwise trigger. */
    if (S->gc.ranges_y_len > 0) {
        qsort(S->gc.ranges_y, S->gc.ranges_y_len,
              sizeof(*S->gc.ranges_y), gc_range_cmp);
    }
    if (S->gc.ranges_o_len > 0) {
        qsort(S->gc.ranges_o, S->gc.ranges_o_len,
              sizeof(*S->gc.ranges_o), gc_range_cmp);
    }
    S->gc.ranges_valid          = 1;
    S->gc.ranges_y_pending_len  = 0;
    S->gc.ranges_o_pending_len  = 0;
    gc_heap_bounds_update(S);
}

/*
 * Buffer a newly allocated header for the next collection. New
 * allocations are young, so this appends to the young pending buffer;
 * the next minor merges it into the sorted young array.
 */
void gc_range_insert(mino_state *S, gc_hdr_t *h)
{
    gc_range_t entry;

    if (!S->gc.ranges_valid) {
        return;
    }

    if (S->gc.ranges_y_pending_len == S->gc.ranges_y_pending_cap) {
        if (!gc_ranges_grow(&S->gc.ranges_y_pending,
                            &S->gc.ranges_y_pending_cap,
                            S->gc.ranges_y_pending_len + 1)) {
            /* Fallback to the invalidate path so mutation can continue
             * even under memory pressure. Next collection rebuilds from
             * gc_all. */
            S->gc.ranges_valid = 0;
            return;
        }
    }

    entry.start = (uintptr_t)(h + 1);
    entry.end   = (uintptr_t)(h + 1) + h->size;
    entry.h     = h;
    S->gc.ranges_y_pending[S->gc.ranges_y_pending_len] = entry;
    S->gc.ranges_y_pending_len++;
}

/* Append one entry to the old pending buffer. Returns 0 on growth
 * failure (caller invalidates), 1 on success. Sorted later by the
 * compact that routes the batch. */
static int gc_ranges_old_pending_push(mino_state *S, const gc_range_t *e)
{
    if (S->gc.ranges_o_pending_len == S->gc.ranges_o_pending_cap) {
        if (!gc_ranges_grow(&S->gc.ranges_o_pending,
                            &S->gc.ranges_o_pending_cap,
                            S->gc.ranges_o_pending_len + 1)) {
            return 0;
        }
    }
    S->gc.ranges_o_pending[S->gc.ranges_o_pending_len] = *e;
    S->gc.ranges_o_pending_len++;
    return 1;
}

/* Merge the sorted old pending buffer into the sorted old array and
 * empty it. The rebuild path folds by reconstruction; this is the
 * between-majors fold, fired from compact when pending crosses its
 * soft threshold or hard cap. Returns 0 on growth failure. */
static int gc_ranges_old_pending_fold(mino_state *S)
{
    size_t K, N, need, i, j, k;
    gc_range_t *merged;

    K = S->gc.ranges_o_pending_len;
    if (K == 0) {
        return 1;
    }
    N = S->gc.ranges_o_len;
    if (K > SIZE_MAX - N) {
        abort(); /* Class I: overflow computing fold need */
    }
    need = N + K;
    if (!gc_ranges_grow(&S->gc.ranges_o, &S->gc.ranges_o_cap, need)) {
        return 0;
    }
    /* In-place back merge, same shape as the young merge. */
    merged = S->gc.ranges_o;
    i = N;
    j = K;
    k = need;
    while (j > 0) {
        if (i > 0 && merged[i - 1].start
                > S->gc.ranges_o_pending[j - 1].start) {
            merged[k - 1] = merged[i - 1];
            i--;
        } else {
            merged[k - 1] = S->gc.ranges_o_pending[j - 1];
            j--;
        }
        k--;
    }
    S->gc.ranges_o_len        = need;
    S->gc.ranges_o_pending_len = 0;
    gc_heap_bounds_update(S);
    return 1;
}

/*
 * Sort the young pending buffer and merge it into the sorted young
 * array. Called at the top of a minor collection, before any code
 * that does ptr->header lookups. Walks young data only: the old pair
 * is untouched.
 *
 * Cost: O(K log K) sort of pending plus O(young + K) back merge into
 * the young array, where K is the number of allocations since the
 * last collection.
 */
void gc_range_merge_pending(mino_state *S)
{
    size_t K, N, need, i, j, k;
    gc_range_t *merged;

    if (!S->gc.ranges_valid) {
        return;
    }
    K = S->gc.ranges_y_pending_len;
    if (K == 0) {
        return;
    }
    qsort(S->gc.ranges_y_pending, K,
          sizeof(*S->gc.ranges_y_pending), gc_range_cmp);

    N = S->gc.ranges_y_len;
    if (K > SIZE_MAX - N) {
        abort(); /* Class I: overflow computing merge need */
    }
    need = N + K;
    if (!gc_ranges_grow(&S->gc.ranges_y, &S->gc.ranges_y_cap, need)) {
        /* Merge-buffer growth failed under memory pressure. The young
         * array still holds its pre-merge sorted entries and the
         * pending entries are intact; invalidate so the next
         * collection rebuilds from gc_all rather than leaving a
         * half-merged, inconsistent state. */
        S->gc.ranges_valid = 0;
        return;
    }
    /* In-place merge from the back to avoid a scratch buffer. */
    merged = S->gc.ranges_y;
    i = N;
    j = K;
    k = need;
    while (j > 0) {
        if (i > 0 && merged[i - 1].start
                > S->gc.ranges_y_pending[j - 1].start) {
            merged[k - 1] = merged[i - 1];
            i--;
        } else {
            merged[k - 1] = S->gc.ranges_y_pending[j - 1];
            j--;
        }
        k--;
        S->gc_range_walk_entries++;
    }
    S->gc.ranges_y_len         = need;
    S->gc.ranges_y_pending_len = 0;
    gc_heap_bounds_update(S);
}

/*
 * Compact the young array after a minor mark. Three fates per entry:
 * live young (marked) stays, dead young (unmarked; the sweep right
 * after frees the header) drops, and entries whose header flipped
 * OLD at a previous sweep route to the old pending buffer -- the
 * promotion ride is exactly one cycle because promotion happens in
 * the sweep that follows this compact. The old arrays are untouched:
 * minor does not free OLD, so no old entry can be stale here.
 *
 * Call site: gc_minor_collect, after the drain loops and before
 * gc_minor_sweep, while mark bits still indicate YOUNG liveness.
 */
void gc_range_compact_after_minor_mark(mino_state *S)
{
    size_t dst = 0, src;
    size_t routed = 0;
    if (!S->gc.ranges_valid) {
        return;
    }
    for (src = 0; src < S->gc.ranges_y_len; src++) {
        gc_hdr_t *h = S->gc.ranges_y[src].h;
        if (h->gen == GC_GEN_OLD) {
            if (!gc_ranges_old_pending_push(S, &S->gc.ranges_y[src])) {
                /* Growth failure mid-walk: young array and pending are
                 * inconsistent (routed entries exist in both). Nothing
                 * reads the index before the next collection's
                 * rebuild; invalidate and let gc_build_range_index
                 * reconstruct from gc_all. */
                S->gc.ranges_valid = 0;
                return;
            }
            routed++;
        } else if (h->mark) {
            S->gc.ranges_y[dst++] = S->gc.ranges_y[src];
        }
        S->gc_range_walk_entries++;
    }
    S->gc.ranges_y_len = dst;
    if (routed > 0) {
        qsort(S->gc.ranges_o_pending, S->gc.ranges_o_pending_len,
              sizeof(*S->gc.ranges_o_pending), gc_range_cmp);
        /* Soft fold: once pending rivals a quarter of the old array
         * (or 64 entries against a small one), merge it in. Each fold
         * costs O(old + pending) and buys pending/old of headroom, so
         * the amortized cost per promoted entry is constant; the hard
         * cap remains the absolute backstop. */
        if (S->gc.ranges_o_pending_len >= GC_RANGES_OLD_PENDING_CAP
            || (S->gc.ranges_o_pending_len >= GC_RANGES_OLD_PENDING_SOFT
                && S->gc.ranges_o_pending_len * 4
                   >= S->gc.ranges_o_len)) {
            if (!gc_ranges_old_pending_fold(S)) {
                S->gc.ranges_valid = 0;
                return;
            }
        }
    }
    gc_heap_bounds_update(S);
}

/* Binary search one sorted buffer for the entry containing u. */
static gc_hdr_t *gc_range_bsearch(const gc_range_t *arr, size_t len,
                                  uintptr_t u)
{
    size_t lo = 0;
    size_t hi = len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (u < arr[mid].start) {
            hi = mid;
        } else if (u >= arr[mid].end) {
            lo = mid + 1;
        } else {
            return arr[mid].h;
        }
    }
    return NULL;
}

/*
 * Resolve p to its owning header, or NULL if p is not within any live
 * payload. Handles interior pointers (word lands in the middle of an
 * allocation). Fast-rejects words outside [heap_min, heap_max) when no
 * young pending inserts are in flight: the three sorted buffers are
 * covered by the bounds, the unsorted young pending buffer is not, so
 * the reject waits until it drains.
 */
gc_hdr_t *gc_find_header_for_ptr(mino_state *S, const void *p)
{
    uintptr_t u  = (uintptr_t)p;
    gc_hdr_t *h;
    size_t    i;
    /* Fast reject for stack words outside the heap — the conservative
     * scan examines every aligned machine word, and most of them are
     * not pointers into the managed heap. */
    if ((u < S->gc.heap_min || u >= S->gc.heap_max)
        && S->gc.ranges_y_pending_len == 0) {
        return NULL;
    }
    h = gc_range_bsearch(S->gc.ranges_y, S->gc.ranges_y_len, u);
    if (h != NULL) {
        return h;
    }
    h = gc_range_bsearch(S->gc.ranges_o, S->gc.ranges_o_len, u);
    if (h != NULL) {
        return h;
    }
    h = gc_range_bsearch(S->gc.ranges_o_pending,
                         S->gc.ranges_o_pending_len, u);
    if (h != NULL) {
        return h;
    }
    for (i = 0; i < S->gc.ranges_y_pending_len; i++) {
        if (u >= S->gc.ranges_y_pending[i].start
            && u < S->gc.ranges_y_pending[i].end) {
            return S->gc.ranges_y_pending[i].h;
        }
    }
    return NULL;
}
