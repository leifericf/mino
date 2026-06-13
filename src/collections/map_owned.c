/*
 * map_owned.c -- owner-tagged (transient) HAMT walks.
 *
 * Extracted from map.c to keep that TU within the 1100-LOC limit.
 * Mirrors the vec_*_owned pattern: `owner` is the editing transient's
 * monotonic ID; nodes whose `owner` field matches are mutated in place,
 * others are cloned once with `owner` stamped and then mutated in place
 * by subsequent calls.
 *
 * GC barrier discipline: a slots[] write may install an OLD -> YOUNG
 * edge (the slots[] array has aged across a minor while the transient
 * batch still holds the wrapping transient on the stack). Every slot
 * mutation therefore routes through gc_write_barrier with the slots[]
 * array as container (matches the `gc_valarr_set` convention in
 * src/gc/barrier.c). When the slots[] array itself is reallocated we
 * stamp the new array's stores via direct assignment -- the fresh
 * YOUNG array makes the barrier a no-op for those writes.
 *
 * Public API: hamt_assoc_owned, hamt_dissoc_owned (declared in
 * collections/internal.h).
 */

#include "runtime/internal.h"

/* Write a single slot through the barrier so the remset captures any
 * OLD-array -> YOUNG-target edge. The barrier is a no-op for YOUNG
 * containers, so freshly cloned (still-young) slot arrays pay no
 * extra cost. */
static void hnode_slot_set(mino_state *S, void **slots, unsigned idx,
                            void *val)
{
    gc_write_barrier(S, slots, slots[idx], val);
    slots[idx] = val;
}

/* Install a fresh slots[] array onto an owner-tagged node. Routes
 * through the barrier so a long batch that promoted the node to OLD
 * across a mid-stride minor records the OLD-node -> YOUNG-slots edge
 * in the remset. Without this the next minor would skip the node
 * (OLD, not in remset), miss the YOUNG slots[] array entirely, and
 * sweep it -- the subsequent iteration's `n->slots[phys]` would then
 * read garbage. */
static void hnode_slots_install(mino_state *S, mino_hamt_node_t *node,
                                 void **slots)
{
    gc_write_barrier(S, node, node->slots, slots);
    node->slots = slots;
}

/* Stamp `node` so it belongs to the editing transient. Returns the
 * existing node when its owner already matches; otherwise clones the
 * node, copies the slots[] array (so the clone can mutate without
 * disturbing the persistent source), and stamps owner. */
static mino_hamt_node_t *hnode_ensure_owned(mino_state *S,
                                             const mino_hamt_node_t *node,
                                             unsigned slot_count,
                                             uintptr_t owner)
{
    mino_hamt_node_t *n;
    void            **slots;
    unsigned          k;
    if (node != NULL && node->owner == (uint32_t)owner) {
        return (mino_hamt_node_t *)node;
    }
    n = (mino_hamt_node_t *)gc_alloc_typed(S, GC_T_HAMT_NODE, sizeof(*n));
    if (n == NULL) return NULL;
    if (node != NULL) {
        n->bitmap          = node->bitmap;
        n->subnode_mask    = node->subnode_mask;
        n->collision_hash  = node->collision_hash;
        n->collision_count = node->collision_count;
        if (slot_count > 0) {
            gc_pin((mino_val *)n);
            slots = (void **)gc_alloc_typed(S, GC_T_PTRARR,
                                             slot_count * sizeof(*slots));
            gc_unpin(1);
            if (slots == NULL) return NULL;
            for (k = 0; k < slot_count; k++) slots[k] = node->slots[k];
            n->slots = slots;
        } else {
            n->slots = NULL;
        }
    }
    n->owner = (uint32_t)owner;
    return n;
}

/* Allocate a fresh slots[] array, copying `pop` entries from `src` and
 * leaving the remainder NULL. Used when an insert grows the slot
 * count; the fresh array is YOUNG so per-slot stores can skip the
 * barrier. */
static void **hnode_slots_grow(mino_state *S, void **src, unsigned pop,
                                unsigned new_pop)
{
    void   **slots;
    unsigned k;
    slots = (void **)gc_alloc_typed(S, GC_T_PTRARR,
                                     new_pop * sizeof(*slots));
    if (slots == NULL) return NULL;
    for (k = 0; k < pop; k++) slots[k] = src[k];
    return slots;
}

static mino_hamt_node_t *merge_entries_owned(mino_state *S,
                                              hamt_entry_t *e1, uint32_t h1,
                                              hamt_entry_t *e2, uint32_t h2,
                                              unsigned shift, uintptr_t owner);

/* hamt_assoc_owned: same semantics as hamt_assoc but mutates owner-
 * tagged nodes in place. Returns the (possibly-new) subtree root for
 * this level. Caller writes the result into the parent slot. */
mino_hamt_node_t *hamt_assoc_owned(mino_state *S, mino_hamt_node_t *n,
                                    hamt_entry_t *new_entry, uint32_t h,
                                    unsigned shift, int *replaced,
                                    uintptr_t owner)
{
    if (n == NULL) {
        unsigned          i     = (unsigned)((h >> shift) & HAMT_MASK);
        void            **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR,
                                                           sizeof(*slots));
        mino_hamt_node_t *fresh;
        if (slots == NULL) return NULL;
        slots[0] = new_entry;
        gc_pin((mino_val *)slots);
        fresh = (mino_hamt_node_t *)gc_alloc_typed(S, GC_T_HAMT_NODE,
                                                    sizeof(*fresh));
        gc_unpin(1);
        if (fresh == NULL) return NULL;
        fresh->bitmap       = 1u << i;
        fresh->subnode_mask = 0u;
        fresh->slots        = slots;
        fresh->owner        = (uint32_t)owner;
        return fresh;
    }
    if (n->collision_count > 0) {
        unsigned cc = n->collision_count;
        if (h == n->collision_hash) {
            unsigned j;
            for (j = 0; j < cc; j++) {
                hamt_entry_t *e = (hamt_entry_t *)n->slots[j];
                if (mino_eq(e->key, new_entry->key)) {
                    mino_hamt_node_t *ed = hnode_ensure_owned(S, n, cc, owner);
                    if (ed == NULL) return NULL;
                    hnode_slot_set(S, ed->slots, j, new_entry);
                    *replaced = 1;
                    return ed;
                }
            }
            {
                /* Append: grow slots[]. Even when owner matches we
                 * have to allocate a wider array; reuse the node
                 * struct in place to save its alloc. */
                mino_hamt_node_t *ed;
                void            **slots = hnode_slots_grow(S, n->slots, cc, cc + 1u);
                if (slots == NULL) return NULL;
                slots[cc] = new_entry;
                gc_pin((mino_val *)slots);
                ed = hnode_ensure_owned(S, n, 0u, owner);
                gc_unpin(1);
                if (ed == NULL) return NULL;
                hnode_slots_install(S, ed, slots);
                ed->collision_count = cc + 1u;
                return ed;
            }
        }
        {
            /* Promote: wrap the collision bucket so it lives in one
             * slot of a bitmap node at this level, then insert. */
            unsigned ib = (unsigned)((n->collision_hash >> shift) & HAMT_MASK);
            unsigned in = (unsigned)((h               >> shift) & HAMT_MASK);
            if (ib == in) {
                mino_hamt_node_t *sub = hamt_assoc_owned(S, n, new_entry, h,
                                                          shift + HAMT_B,
                                                          replaced, owner);
                void            **slots;
                mino_hamt_node_t *fresh;
                if (sub == NULL) return NULL;
                gc_pin((mino_val *)sub);
                slots = (void **)gc_alloc_typed(S, GC_T_PTRARR,
                                                 sizeof(*slots));
                gc_unpin(1);
                if (slots == NULL) return NULL;
                slots[0] = sub;
                gc_pin((mino_val *)slots);
                fresh = (mino_hamt_node_t *)gc_alloc_typed(S, GC_T_HAMT_NODE,
                                                            sizeof(*fresh));
                gc_unpin(1);
                if (fresh == NULL) return NULL;
                fresh->bitmap       = 1u << ib;
                fresh->subnode_mask = 1u << ib;
                fresh->slots        = slots;
                fresh->owner        = (uint32_t)owner;
                return fresh;
            } else {
                void            **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR,
                                                                    2 * sizeof(*slots));
                mino_hamt_node_t *fresh;
                if (slots == NULL) return NULL;
                if (ib < in) { slots[0] = (void *)n; slots[1] = new_entry; }
                else         { slots[0] = new_entry; slots[1] = (void *)n; }
                gc_pin((mino_val *)slots);
                fresh = (mino_hamt_node_t *)gc_alloc_typed(S, GC_T_HAMT_NODE,
                                                            sizeof(*fresh));
                gc_unpin(1);
                if (fresh == NULL) return NULL;
                fresh->bitmap       = (1u << ib) | (1u << in);
                fresh->subnode_mask = 1u << ib;
                fresh->slots        = slots;
                fresh->owner        = (uint32_t)owner;
                return fresh;
            }
        }
    }
    {
        unsigned  i    = (unsigned)((h >> shift) & HAMT_MASK);
        uint32_t  bit  = 1u << i;
        unsigned  phys = popcount32(n->bitmap & (bit - 1u));
        unsigned  pop  = popcount32(n->bitmap);
        if ((n->bitmap & bit) == 0) {
            /* Empty slot: grow slots[] by one. Reuse the node struct
             * via hnode_ensure_owned so subsequent edits at this level
             * keep the same node. */
            mino_hamt_node_t *ed;
            void            **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR,
                                                                (pop + 1u) * sizeof(*slots));
            unsigned k;
            if (slots == NULL) return NULL;
            for (k = 0; k < phys; k++) slots[k]     = n->slots[k];
            slots[phys] = new_entry;
            for (k = phys; k < pop; k++) slots[k + 1] = n->slots[k];
            gc_pin((mino_val *)slots);
            ed = hnode_ensure_owned(S, n, 0u, owner);
            gc_unpin(1);
            if (ed == NULL) return NULL;
            hnode_slots_install(S, ed, slots);
            ed->bitmap       = n->bitmap | bit;
            ed->subnode_mask = n->subnode_mask;
            return ed;
        }
        if (n->subnode_mask & bit) {
            /* Child subtree: recurse, then update the slot in place
             * (no slots[] resize required). */
            mino_hamt_node_t *new_child = hamt_assoc_owned(S,
                (mino_hamt_node_t *)n->slots[phys], new_entry, h,
                shift + HAMT_B, replaced, owner);
            mino_hamt_node_t *ed;
            if (new_child == NULL) return NULL;
            gc_pin((mino_val *)new_child);
            ed = hnode_ensure_owned(S, n, pop, owner);
            gc_unpin(1);
            if (ed == NULL) return NULL;
            hnode_slot_set(S, ed->slots, phys, new_child);
            return ed;
        }
        {
            /* Leaf entry in slot. Same key → replace in place. Different
             * key → split into a subtree and stamp subnode_mask. */
            hamt_entry_t *existing = (hamt_entry_t *)n->slots[phys];
            if (mino_eq(existing->key, new_entry->key)) {
                mino_hamt_node_t *ed = hnode_ensure_owned(S, n, pop, owner);
                if (ed == NULL) return NULL;
                hnode_slot_set(S, ed->slots, phys, new_entry);
                *replaced = 1;
                return ed;
            }
            {
                uint32_t          eh  = hash_val(existing->key);
                mino_hamt_node_t *sub = merge_entries_owned(S, existing, eh,
                                                             new_entry, h,
                                                             shift + HAMT_B,
                                                             owner);
                mino_hamt_node_t *ed;
                if (sub == NULL) return NULL;
                gc_pin((mino_val *)sub);
                ed = hnode_ensure_owned(S, n, pop, owner);
                gc_unpin(1);
                if (ed == NULL) return NULL;
                hnode_slot_set(S, ed->slots, phys, sub);
                ed->subnode_mask = n->subnode_mask | bit;
                return ed;
            }
        }
    }
}

static mino_hamt_node_t *merge_entries_owned(mino_state *S,
                                              hamt_entry_t *e1, uint32_t h1,
                                              hamt_entry_t *e2, uint32_t h2,
                                              unsigned shift, uintptr_t owner)
{
    if (h1 == h2 || shift >= 32u) {
        void            **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR,
                                                            2 * sizeof(*slots));
        mino_hamt_node_t *fresh;
        if (slots == NULL) return NULL;
        slots[0] = e1;
        slots[1] = e2;
        gc_pin((mino_val *)slots);
        fresh = (mino_hamt_node_t *)gc_alloc_typed(S, GC_T_HAMT_NODE,
                                                    sizeof(*fresh));
        gc_unpin(1);
        if (fresh == NULL) return NULL;
        fresh->collision_hash  = h1;
        fresh->collision_count = 2u;
        fresh->slots           = slots;
        fresh->owner           = (uint32_t)owner;
        return fresh;
    }
    {
        unsigned i1 = (unsigned)((h1 >> shift) & HAMT_MASK);
        unsigned i2 = (unsigned)((h2 >> shift) & HAMT_MASK);
        if (i1 == i2) {
            mino_hamt_node_t *child = merge_entries_owned(S, e1, h1, e2, h2,
                                                            shift + HAMT_B,
                                                            owner);
            void            **slots;
            mino_hamt_node_t *fresh;
            if (child == NULL) return NULL;
            gc_pin((mino_val *)child);
            slots = (void **)gc_alloc_typed(S, GC_T_PTRARR, sizeof(*slots));
            gc_unpin(1);
            if (slots == NULL) return NULL;
            slots[0] = child;
            gc_pin((mino_val *)slots);
            fresh = (mino_hamt_node_t *)gc_alloc_typed(S, GC_T_HAMT_NODE,
                                                        sizeof(*fresh));
            gc_unpin(1);
            if (fresh == NULL) return NULL;
            fresh->bitmap       = 1u << i1;
            fresh->subnode_mask = 1u << i1;
            fresh->slots        = slots;
            fresh->owner        = (uint32_t)owner;
            return fresh;
        } else {
            void            **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR,
                                                                2 * sizeof(*slots));
            mino_hamt_node_t *fresh;
            if (slots == NULL) return NULL;
            if (i1 < i2) { slots[0] = e1; slots[1] = e2; }
            else         { slots[0] = e2; slots[1] = e1; }
            gc_pin((mino_val *)slots);
            fresh = (mino_hamt_node_t *)gc_alloc_typed(S, GC_T_HAMT_NODE,
                                                        sizeof(*fresh));
            gc_unpin(1);
            if (fresh == NULL) return NULL;
            fresh->bitmap       = (1u << i1) | (1u << i2);
            fresh->subnode_mask = 0u;
            fresh->slots        = slots;
            fresh->owner        = (uint32_t)owner;
            return fresh;
        }
    }
}

/* hamt_dissoc_owned: same semantics as the rebuild-from-key_order in
 * mino_map_dissoc1, but walks the trie directly and reuses owner-
 * tagged nodes in place. Returns the updated subtree root, NULL when
 * the subtree empties (caller drops the parent slot accordingly), or
 * `n` unchanged when the key is absent (with *removed left 0). */
mino_hamt_node_t *hamt_dissoc_owned(mino_state *S, mino_hamt_node_t *n,
                                     const mino_val *key, uint32_t h,
                                     unsigned shift, int *removed,
                                     uintptr_t owner)
{
    if (n == NULL) return NULL;
    if (n->collision_count > 0) {
        unsigned cc = n->collision_count;
        unsigned j;
        if (h != n->collision_hash) return n;
        for (j = 0; j < cc; j++) {
            hamt_entry_t *e = (hamt_entry_t *)n->slots[j];
            if (mino_eq(e->key, key)) {
                *removed = 1;
                if (cc <= 1u) return NULL;
                {
                    /* Shrink: build a new slots[] without index j. */
                    void            **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR,
                                                                        (cc - 1u) * sizeof(*slots));
                    mino_hamt_node_t *ed;
                    unsigned k, w;
                    if (slots == NULL) return NULL;
                    for (k = 0, w = 0; k < cc; k++) {
                        if (k == j) continue;
                        slots[w++] = n->slots[k];
                    }
                    gc_pin((mino_val *)slots);
                    ed = hnode_ensure_owned(S, n, 0u, owner);
                    gc_unpin(1);
                    if (ed == NULL) return NULL;
                    hnode_slots_install(S, ed, slots);
                    ed->collision_count = cc - 1u;
                    return ed;
                }
            }
        }
        return n;
    }
    {
        unsigned i    = (unsigned)((h >> shift) & HAMT_MASK);
        uint32_t bit  = 1u << i;
        unsigned phys = popcount32(n->bitmap & (bit - 1u));
        unsigned pop  = popcount32(n->bitmap);
        if ((n->bitmap & bit) == 0) return n;
        if (n->subnode_mask & bit) {
            mino_hamt_node_t *child = (mino_hamt_node_t *)n->slots[phys];
            mino_hamt_node_t *new_child = hamt_dissoc_owned(S, child, key, h,
                                                              shift + HAMT_B,
                                                              removed, owner);
            mino_hamt_node_t *ed;
            if (!*removed) return n;
            if (new_child != NULL) {
                ed = hnode_ensure_owned(S, n, pop, owner);
                if (ed == NULL) return NULL;
                hnode_slot_set(S, ed->slots, phys, new_child);
                return ed;
            }
            /* Child emptied: drop the slot. Shrink bitmap, subnode_mask,
             * slots[] by one. */
            if (pop == 1u) return NULL;
            {
                void            **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR,
                                                                    (pop - 1u) * sizeof(*slots));
                unsigned k, w;
                if (slots == NULL) return NULL;
                for (k = 0, w = 0; k < pop; k++) {
                    if (k == phys) continue;
                    slots[w++] = n->slots[k];
                }
                gc_pin((mino_val *)slots);
                ed = hnode_ensure_owned(S, n, 0u, owner);
                gc_unpin(1);
                if (ed == NULL) return NULL;
                hnode_slots_install(S, ed, slots);
                ed->bitmap       = n->bitmap       & ~bit;
                ed->subnode_mask = n->subnode_mask & ~bit;
                return ed;
            }
        }
        {
            hamt_entry_t *e = (hamt_entry_t *)n->slots[phys];
            if (!mino_eq(e->key, key)) return n;
            *removed = 1;
            if (pop == 1u) return NULL;
            {
                void            **slots = (void **)gc_alloc_typed(S, GC_T_PTRARR,
                                                                    (pop - 1u) * sizeof(*slots));
                mino_hamt_node_t *ed;
                unsigned k, w;
                if (slots == NULL) return NULL;
                for (k = 0, w = 0; k < pop; k++) {
                    if (k == phys) continue;
                    slots[w++] = n->slots[k];
                }
                gc_pin((mino_val *)slots);
                ed = hnode_ensure_owned(S, n, 0u, owner);
                gc_unpin(1);
                if (ed == NULL) return NULL;
                hnode_slots_install(S, ed, slots);
                ed->bitmap       = n->bitmap & ~bit;
                ed->subnode_mask = n->subnode_mask;
                return ed;
            }
        }
    }
}
