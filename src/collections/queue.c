/*
 * queue.c -- PersistentQueue: a two-list persistent FIFO.
 *
 * Backed by two cons-spine lists:
 *   front -- elements in deque order (next-to-pop at the head)
 *   back  -- elements in REVERSE-deque order (most recent conj at the head)
 *
 * Invariant: when front is empty, back is also empty. The pop step
 * rotates back into front (by reversing back) when front would
 * become empty, so amortised conj / pop / peek are all O(1).
 *
 * The persistence rules mirror mino's vector / map: every mutating
 * op returns a fresh MINO_QUEUE cell whose front / back slots share
 * structure with the input. The cons cells themselves are immutable,
 * so the shared structure is safe.
 */

#include "runtime/internal.h"

mino_val *mino_queue_empty(mino_state *S)
{
    mino_val *q = (mino_val *)gc_alloc_typed(S, GC_T_VAL, sizeof(mino_val));
    q->type            = MINO_QUEUE;
    q->meta            = NULL;
    q->as.queue.front  = mino_empty_list(S);
    q->as.queue.back   = mino_empty_list(S);
    q->as.queue.len    = 0;
    return q;
}

static mino_val *queue_from_parts(mino_state *S,
                                    mino_val *front,
                                    mino_val *back,
                                    size_t len)
{
    mino_val *q = (mino_val *)gc_alloc_typed(S, GC_T_VAL, sizeof(mino_val));
    q->type            = MINO_QUEUE;
    q->meta            = NULL;
    q->as.queue.front  = (front != NULL) ? front : mino_empty_list(S);
    q->as.queue.back   = (back  != NULL) ? back  : mino_empty_list(S);
    q->as.queue.len    = len;
    return q;
}

int mino_is_queue(const mino_val *v)
{
    return v != NULL && mino_type_of(v) == MINO_QUEUE;
}

size_t mino_queue_count(const mino_val *q)
{
    if (q == NULL || mino_type_of(q) != MINO_QUEUE) return 0;
    return q->as.queue.len;
}

mino_val *mino_queue_conj(mino_state *S, mino_val *q, mino_val *v)
{
    mino_val *new_back;
    if (q == NULL || mino_type_of(q) != MINO_QUEUE) return NULL;
    new_back = mino_cons(S, v, q->as.queue.back);
    return queue_from_parts(S, q->as.queue.front, new_back,
                            q->as.queue.len + 1);
}

static mino_val *queue_reverse_list(mino_state *S, mino_val *xs)
{
    mino_val *out = mino_empty_list(S);
    while (xs != NULL && mino_is_cons(xs)) {
        out = mino_cons(S, xs->as.cons.car, out);
        xs  = xs->as.cons.cdr;
    }
    return out;
}

mino_val *mino_queue_peek(const mino_val *q)
{
    if (q == NULL || mino_type_of(q) != MINO_QUEUE) return NULL;
    if (q->as.queue.len == 0) return NULL;
    if (q->as.queue.front != NULL && mino_is_cons(q->as.queue.front)) {
        return q->as.queue.front->as.cons.car;
    }
    /* Defensive: front empty but back non-empty (invariant broken).
     * Walk back to find its tail, which is the next-to-pop element. */
    {
        mino_val *xs = q->as.queue.back;
        mino_val *last = NULL;
        while (xs != NULL && mino_is_cons(xs)) {
            last = xs;
            xs = xs->as.cons.cdr;
        }
        return last != NULL ? last->as.cons.car : NULL;
    }
}

mino_val *mino_queue_pop(mino_state *S, mino_val *q)
{
    mino_val *front;
    mino_val *new_front;
    if (q == NULL || mino_type_of(q) != MINO_QUEUE) return NULL;
    if (q->as.queue.len == 0) return q;
    front = q->as.queue.front;
    if (front == NULL || !mino_is_cons(front)) {
        front = queue_reverse_list(S, q->as.queue.back);
        if (front == NULL || !mino_is_cons(front)) return mino_queue_empty(S);
        return queue_from_parts(S, front->as.cons.cdr, mino_empty_list(S),
                                q->as.queue.len - 1);
    }
    new_front = front->as.cons.cdr;
    if (new_front == NULL || !mino_is_cons(new_front)) {
        new_front = queue_reverse_list(S, q->as.queue.back);
        return queue_from_parts(S, new_front, mino_empty_list(S),
                                q->as.queue.len - 1);
    }
    return queue_from_parts(S, new_front, q->as.queue.back,
                            q->as.queue.len - 1);
}

/* Build a seq view of the queue: front list followed by the reversed
 * back list. Returns the empty-list singleton for an empty queue. */
mino_val *mino_queue_seq(mino_state *S, const mino_val *q)
{
    mino_val *back_rev;
    mino_val *head;
    mino_val *tail;
    mino_val *cur;
    int back_is_cons;
    if (q == NULL || mino_type_of(q) != MINO_QUEUE) return mino_empty_list(S);
    if (q->as.queue.len == 0) return mino_empty_list(S);
    back_is_cons = q->as.queue.back != NULL && mino_is_cons(q->as.queue.back);
    if (!back_is_cons) {
        return (q->as.queue.front != NULL && mino_is_cons(q->as.queue.front))
            ? q->as.queue.front
            : mino_empty_list(S);
    }
    back_rev = queue_reverse_list(S, q->as.queue.back);
    if (q->as.queue.front == NULL
        || !mino_is_cons(q->as.queue.front)) {
        return back_rev;
    }
    head = mino_nil(S);
    tail = NULL;
    cur  = q->as.queue.front;
    while (cur != NULL && mino_is_cons(cur)) {
        mino_val *cell = mino_cons(S, cur->as.cons.car, mino_nil(S));
        if (tail == NULL) head = cell;
        else mino_cons_cdr_set(S, tail, cell);
        tail = cell;
        cur  = cur->as.cons.cdr;
    }
    if (tail != NULL) {
        mino_cons_cdr_set(S, tail, back_rev);
    } else {
        head = back_rev;
    }
    return head;
}

/* List length helper (cons-spine, ignores non-cons tails). */
static size_t cons_len(const mino_val *xs)
{
    size_t n = 0;
    while (xs != NULL && mino_is_cons(xs)) { n++; xs = xs->as.cons.cdr; }
    return n;
}

/* Get the kth element of the reverse of `xs`. xs has length `n`.
 * The kth-from-front of reverse(xs) is the (n-1-k)th-from-front of xs.
 * Returns NULL if out of bounds. */
static mino_val *back_nth_reversed(const mino_val *xs, size_t n, size_t k)
{
    size_t steps;
    if (k >= n) return NULL;
    steps = n - 1 - k;
    while (steps > 0 && xs != NULL && mino_is_cons(xs)) {
        xs = xs->as.cons.cdr;
        steps--;
    }
    if (xs == NULL || !mino_is_cons(xs)) return NULL;
    return xs->as.cons.car;
}

/* Get the ith element of queue in deque order. O(n) for back access. */
static mino_val *queue_nth(const mino_val *q, size_t i, size_t front_len)
{
    if (i < front_len) {
        const mino_val *p = q->as.queue.front;
        while (i > 0 && p != NULL && mino_is_cons(p)) {
            p = p->as.cons.cdr;
            i--;
        }
        if (p == NULL || !mino_is_cons(p)) return NULL;
        return p->as.cons.car;
    }
    {
        size_t back_len = q->as.queue.len - front_len;
        return back_nth_reversed(q->as.queue.back, back_len, i - front_len);
    }
}

int mino_queue_eq(const mino_val *a, const mino_val *b)
{
    size_t n, i, a_fl, b_fl;
    if (a == NULL || b == NULL) return 0;
    if (mino_type_of(a) != MINO_QUEUE || mino_type_of(b) != MINO_QUEUE) return 0;
    if (a->as.queue.len != b->as.queue.len) return 0;
    n    = a->as.queue.len;
    if (n == 0) return 1;
    a_fl = cons_len(a->as.queue.front);
    b_fl = cons_len(b->as.queue.front);
    for (i = 0; i < n; i++) {
        if (!mino_eq(queue_nth(a, i, a_fl), queue_nth(b, i, b_fl))) {
            return 0;
        }
    }
    return 1;
}

/* Hash matching the sequence-hash contract (so two queues with the
 * same elements in the same order share a hash and equal queues hash
 * the same). Uses Clojure's hash-ordered-coll algorithm. */
uint32_t mino_queue_hash(const mino_val *q)
{
    size_t n, i, fl;
    uint32_t h = 1u;
    if (q == NULL || mino_type_of(q) != MINO_QUEUE) return 0u;
    n  = q->as.queue.len;
    if (n == 0) return 0u;
    fl = cons_len(q->as.queue.front);
    for (i = 0; i < n; i++) {
        h = 31u * h + (uint32_t)mino_hash(queue_nth(q, i, fl));
    }
    /* mix the count in like clojure.lang.Util/hashOrdered does */
    h ^= (uint32_t)n;
    h *= 0x9e3779b9u;
    return h;
}
