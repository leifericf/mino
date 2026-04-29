/*
 * chunk.c -- chunked-seq value support: MINO_CHUNK fixed-cap value
 * buffer + MINO_CHUNKED_CONS seq cell. Mirrors canon's chunk-buffer /
 * chunk / chunk-cons / chunk-first / chunk-rest family. Sweep paths
 * for the malloc-owned vals[] array live in gc/minor.c and gc/major.c
 * alongside MINO_RECORD's slots[] array.
 */

#include "runtime/internal.h"

mino_val_t *mino_chunk_buffer(mino_state_t *S, unsigned cap)
{
    mino_val_t   *v;
    mino_val_t  **slots;
    if (cap == 0) cap = 1;
    slots = (mino_val_t **)malloc((size_t)cap * sizeof(*slots));
    if (slots == NULL) return NULL;
    {
        unsigned i;
        for (i = 0; i < cap; i++) slots[i] = NULL;
    }
    v = alloc_val(S, MINO_CHUNK);
    if (v == NULL) {
        free(slots);
        return NULL;
    }
    v->as.chunk.vals   = slots;
    v->as.chunk.cap    = cap;
    v->as.chunk.len    = 0;
    v->as.chunk.sealed = 0;
    return v;
}

int mino_chunk_append(mino_val_t *buf, mino_val_t *elem)
{
    if (buf == NULL || buf->type != MINO_CHUNK) return 0;
    if (buf->as.chunk.sealed) return 0;
    if (buf->as.chunk.len >= buf->as.chunk.cap) return 0;
    buf->as.chunk.vals[buf->as.chunk.len++] = elem;
    return 1;
}

mino_val_t *mino_chunk_seal(mino_val_t *buf)
{
    if (buf == NULL || buf->type != MINO_CHUNK) return NULL;
    buf->as.chunk.sealed = 1;
    return buf;
}

mino_val_t *mino_chunked_cons(mino_state_t *S, mino_val_t *chunk,
                              mino_val_t *more)
{
    mino_val_t *cell;
    if (chunk == NULL || chunk->type != MINO_CHUNK) return NULL;
    if (chunk->as.chunk.len == 0) {
        /* Empty chunk degenerates to its tail. */
        return more;
    }
    cell = alloc_val(S, MINO_CHUNKED_CONS);
    if (cell == NULL) return NULL;
    cell->as.chunked_cons.chunk = chunk;
    cell->as.chunked_cons.more  = more;
    cell->as.chunked_cons.off   = 0;
    return cell;
}

/* Build a new chunked-cons cell with the same chunk/more but the off
 * advanced by one. Returns the more pointer if the offset would
 * exceed chunk.len. */
mino_val_t *mino_chunked_cons_advance(mino_state_t *S, const mino_val_t *cs)
{
    const mino_val_t *ch;
    mino_val_t       *cell;
    unsigned          next;
    if (cs == NULL || cs->type != MINO_CHUNKED_CONS) return NULL;
    ch   = cs->as.chunked_cons.chunk;
    next = cs->as.chunked_cons.off + 1;
    if (next >= ch->as.chunk.len) return cs->as.chunked_cons.more;
    cell = alloc_val(S, MINO_CHUNKED_CONS);
    if (cell == NULL) return NULL;
    cell->as.chunked_cons.chunk = (mino_val_t *)ch;
    cell->as.chunked_cons.more  = cs->as.chunked_cons.more;
    cell->as.chunked_cons.off   = next;
    return cell;
}
