/*
 * chunk.c -- chunked-seq value support: MINO_CHUNK fixed-cap value
 * buffer + MINO_CHUNKED_CONS seq cell. Mirrors canon's chunk-buffer /
 * chunk / chunk-cons / chunk-first / chunk-rest family. Sweep paths
 * for the malloc-owned vals[] array live in gc/minor.c and gc/major.c
 * alongside MINO_RECORD's slots[] array.
 */

#include "mino.h"
#include "mino_internal.h"
#include "gc/internal.h"
#include "runtime/value_assert.h"

#include <stdlib.h>

mino_val *mino_chunk_buffer(mino_state *S, unsigned cap)
{
    mino_val   *v;
    mino_val  **slots;
    if (cap == 0) cap = 1;
    slots = (mino_val **)malloc((size_t)cap * sizeof(*slots));
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

int mino_chunk_append(mino_val *buf, mino_val *elem)
{
    if (buf == NULL || mino_type_of(buf) != MINO_CHUNK) return 0;
    if (buf->as.chunk.sealed) return 0;
    if (buf->as.chunk.len >= buf->as.chunk.cap) return 0;
    buf->as.chunk.vals[buf->as.chunk.len++] = elem;
    return 1;
}

mino_val *mino_chunk_seal(mino_val *buf)
{
    if (buf == NULL || mino_type_of(buf) != MINO_CHUNK) return NULL;
    buf->as.chunk.sealed = 1;
    return buf;
}

mino_val *mino_chunked_cons(mino_state *S, mino_val *chunk,
                              mino_val *more)
{
    mino_val *cell;
    if (chunk == NULL || mino_type_of(chunk) != MINO_CHUNK) return NULL;
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
mino_val *mino_chunked_cons_advance(mino_state *S, const mino_val *cs)
{
    const mino_val *ch;
    mino_val       *cell;
    unsigned          next;
    if (cs == NULL || mino_type_of(cs) != MINO_CHUNKED_CONS) return NULL;
    ch   = cs->as.chunked_cons.chunk;
    next = cs->as.chunked_cons.off + 1;
    if (next >= ch->as.chunk.len) return cs->as.chunked_cons.more;
    cell = alloc_val(S, MINO_CHUNKED_CONS);
    if (cell == NULL) return NULL;
    cell->as.chunked_cons.chunk = (mino_val *)ch;
    cell->as.chunked_cons.more  = cs->as.chunked_cons.more;
    cell->as.chunked_cons.off   = next;
    return cell;
}
