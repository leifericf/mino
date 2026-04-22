/*
 * runtime_gc_major.c -- full-heap sweep for the mark-and-sweep collector.
 *
 * Split out of runtime_gc.c as a pure refactor; call graph unchanged.
 * Called from gc_major_collect after mark has finished. Frees every allocation
 * whose mark bit is clear, resets the mark bit on survivors, and grows
 * the next cycle's threshold so amortized collection cost stays bounded.
 */

#include "mino_internal.h"

void gc_sweep(mino_state_t *S)
{
    gc_hdr_t **pp   = &S->gc_all;
    size_t     live = 0;
    while (*pp != NULL) {
        gc_hdr_t *h = *pp;
        if (h->mark) {
            h->mark = 0;
            live += h->size;
            pp = &h->next;
        } else {
            /* Call finalizer for handles being collected. */
            if (h->type_tag == GC_T_VAL) {
                mino_val_t *v = (mino_val_t *)(h + 1);
                if (v->type == MINO_HANDLE && v->as.handle.finalizer != NULL) {
                    v->as.handle.finalizer(v->as.handle.ptr,
                                           v->as.handle.tag);
                }
            }
            *pp = h->next;
            {
                int fc = gc_freelist_class(h->size);
                if (fc >= 0) {
                    h->next = S->gc_freelists[fc];
                    S->gc_freelists[fc] = h;
                } else {
                    free(h);
                }
            }
        }
    }
    S->gc_total_freed += S->gc_bytes_alloc - live;
    S->gc_bytes_live  = live;
    S->gc_bytes_alloc = live;
    /* Next cycle triggers after another threshold's worth of growth above
     * the live set; threshold grows to keep collection amortized. */
    if (live * 2 > S->gc_threshold) {
        S->gc_threshold = live * 2;
    }
}
