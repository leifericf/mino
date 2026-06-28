/*
 * image.c -- save-lisp-and-die (SLAD) image serializer.
 *
 * Saves the full runtime state (namespaces, vars, values, closures,
 * mutable refs) to a line-delimited text image file. The matching
 * deserializer lives in image_load.c. See ADR 12.
 *
 * Design: value-serialization with an identity table. A BFS from the
 * GC roots assigns each reachable value a stable integer ID; shared
 * references and cycles are handled naturally.
 */

#include "runtime/internal.h"
#include "runtime/image_internal.h"
#include "mino.h"
#include "values/internal.h"
#include "collections/internal.h"
#include "eval/internal.h"
#include "gc/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- CRC32 (IEEE 802.3 polynomial 0xEDB88320) -------------------- */

/* Table computed once on first use. Avoids the transcription-error risk
 * of a hand-typed 256-entry constant table. */
static uint32_t img_crc32_table[256];
static int img_crc32_table_ready = 0;

static void img_crc32_table_init(void)
{
    uint32_t i;
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        int j;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        img_crc32_table[i] = c;
    }
    img_crc32_table_ready = 1;
}

uint32_t img_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    if (!img_crc32_table_ready) img_crc32_table_init();
    crc = ~crc;
    for (i = 0; i < len; i++)
        crc = img_crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* --- ID table: pointer → uint32_t --------------------------------- */

typedef struct {
    mino_val *key;
    uint32_t  id;
} img_ht_entry;

typedef struct {
    img_ht_entry *entries;
    size_t         cap;
    size_t         count;
} img_ht;

typedef struct {
    /* Value ID table: mino_val* → ID */
    img_ht val_ht;
    /* Env ID table: mino_env* → ID */
    struct {
        mino_env **keys;
        uint32_t  *ids;
        size_t      cap;
        size_t      count;
    } env_ht;
    /* ID → value/env lookup (for serialization output) */
    mino_val **id_vals;
    mino_env **id_envs;
    uint32_t   id_count;
    uint32_t   id_cap;
    /* BFS queue: array of IDs pending serialization */
    uint32_t  *queue;
    uint32_t   q_head;
    uint32_t   q_tail;
    /* The clojure.core env — never followed during walk (reinstalled on load) */
    mino_env  *core_env;
} img_id_table;

static int img_ht_init(img_ht *h)
{
    h->cap    = IMG_HT_INIT;
    h->count  = 0;
    h->entries = (img_ht_entry *)calloc(h->cap, sizeof(img_ht_entry));
    return h->entries != NULL ? 0 : -1;
}

static void img_ht_free(img_ht *h)
{
    free(h->entries);
    h->entries = NULL;
    h->cap = 0;
    h->count = 0;
}

static uint32_t img_hash_ptr(const void *p)
{
    uintptr_t v = (uintptr_t)p;
    v = (v ^ (v >> 16)) * 0x45D9F3B;
    v = (v ^ (v >> 16)) * 0x45D9F3B;
    v = (v ^ (v >> 16));
    return (uint32_t)v;
}

/* Returns 1 if found (out_id set), 0 if not found. */
static int img_ht_lookup(img_ht *h, mino_val *key, uint32_t *out_id)
{
    size_t mask = h->cap - 1;
    size_t i    = img_hash_ptr(key) & mask;
    while (h->entries[i].key != NULL) {
        if (h->entries[i].key == key) {
            *out_id = h->entries[i].id;
            return 1;
        }
        i = (i + 1) & mask;
    }
    return 0;
}

static void img_ht_insert(img_ht *h, mino_val *key, uint32_t id)
{
    size_t mask, i;
    if (h->count * 100 >= h->cap * IMG_HT_LOAD) {
        size_t new_cap  = h->cap * 2;
        img_ht_entry *ne = (img_ht_entry *)calloc(new_cap, sizeof(img_ht_entry));
        size_t j;
        for (j = 0; j < h->cap; j++) {
            if (h->entries[j].key != NULL) {
                size_t ni = img_hash_ptr(h->entries[j].key) & (new_cap - 1);
                while (ne[ni].key != NULL)
                    ni = (ni + 1) & (new_cap - 1);
                ne[ni] = h->entries[j];
            }
        }
        free(h->entries);
        h->entries = ne;
        h->cap     = new_cap;
    }
    mask = h->cap - 1;
    i    = img_hash_ptr(key) & mask;
    while (h->entries[i].key != NULL)
        i = (i + 1) & mask;
    h->entries[i].key = key;
    h->entries[i].id  = id;
    h->count++;
}

/* --- env ID table ------------------------------------------------- */

static int img_env_ht_init(img_id_table *t)
{
    t->env_ht.cap   = IMG_HT_INIT;
    t->env_ht.count = 0;
    t->env_ht.keys  = (mino_env **)calloc(t->env_ht.cap, sizeof(mino_env *));
    t->env_ht.ids   = (uint32_t *)calloc(t->env_ht.cap, sizeof(uint32_t));
    return (t->env_ht.keys != NULL && t->env_ht.ids != NULL) ? 0 : -1;
}

static void img_env_ht_free(img_id_table *t)
{
    free(t->env_ht.keys);
    free(t->env_ht.ids);
}

static int img_env_ht_lookup(img_id_table *t, mino_env *env, uint32_t *out_id)
{
    size_t i;
    for (i = 0; i < t->env_ht.count; i++) {
        if (t->env_ht.keys[i] == env) {
            *out_id = t->env_ht.ids[i];
            return 1;
        }
    }
    return 0;
}

static void img_env_ht_insert(img_id_table *t, mino_env *env, uint32_t id)
{
    if (t->env_ht.count >= t->env_ht.cap) {
        t->env_ht.cap *= 2;
        t->env_ht.keys = (mino_env **)realloc(t->env_ht.keys,
                                    t->env_ht.cap * sizeof(mino_env *));
        t->env_ht.ids  = (uint32_t *)realloc(t->env_ht.ids,
                                    t->env_ht.cap * sizeof(uint32_t));
    }
    t->env_ht.keys[t->env_ht.count] = env;
    t->env_ht.ids[t->env_ht.count]  = id;
    t->env_ht.count++;
}

/* --- ID table: init / assign / enqueue ---------------------------- */

static int img_idt_init(img_id_table *t)
{
    if (img_ht_init(&t->val_ht) != 0) return -1;
    if (img_env_ht_init(t) != 0) {
        img_ht_free(&t->val_ht);
        return -1;
    }
    t->id_count = 1;  /* ID 0 reserved for NULL sentinel */
    t->id_cap   = 256;
    t->id_vals  = (mino_val **)calloc(t->id_cap, sizeof(mino_val *));
    t->id_envs  = (mino_env **)calloc(t->id_cap, sizeof(mino_env *));
    t->queue    = (uint32_t *)calloc(t->id_cap, sizeof(uint32_t));
    if (t->id_vals == NULL || t->id_envs == NULL || t->queue == NULL) {
        img_env_ht_free(t);
        img_ht_free(&t->val_ht);
        return -1;
    }
    t->q_head   = 0;
    t->q_tail   = 0;
    t->core_env = NULL;
    return 0;
}

static void img_idt_free(img_id_table *t)
{
    img_ht_free(&t->val_ht);
    img_env_ht_free(t);
    free(t->id_vals);
    free(t->id_envs);
    free(t->queue);
}

static void img_idt_grow(img_id_table *t)
{
    uint32_t old_cap = t->id_cap;
    uint32_t i;
    t->id_cap *= 2;
    t->id_vals = (mino_val **)realloc(t->id_vals,
                                t->id_cap * sizeof(mino_val *));
    t->id_envs = (mino_env **)realloc(t->id_envs,
                                t->id_cap * sizeof(mino_env *));
    t->queue   = (uint32_t *)realloc(t->queue,
                                t->id_cap * sizeof(uint32_t));
    for (i = old_cap; i < t->id_cap; i++) {
        t->id_vals[i] = NULL;
        t->id_envs[i] = NULL;
    }
}

/* Assign an ID to a value if not already assigned. Returns the ID.
 * Enqueues the value for BFS traversal if newly assigned. */
static uint32_t img_idt_assign_val(img_id_table *t, mino_val *v)
{
    uint32_t id;
    if (v == NULL) return 0;  /* NULL maps to ID 0 (nil placeholder) */
    if (img_ht_lookup(&t->val_ht, v, &id)) return id;
    id = t->id_count++;
    if (t->id_count >= t->id_cap) img_idt_grow(t);
    t->id_vals[id] = v;
    img_ht_insert(&t->val_ht, v, id);
    t->queue[t->q_tail++] = id;
    return id;
}

/* Assign an ID to an env if not already assigned. */
static uint32_t img_idt_assign_env(img_id_table *t, mino_env *env)
{
    uint32_t id;
    if (env == NULL) return 0;
    if (img_env_ht_lookup(t, env, &id)) return id;
    id = t->id_count++;
    if (t->id_count >= t->id_cap) img_idt_grow(t);
    t->id_envs[id] = env;
    img_env_ht_insert(t, env, id);
    t->queue[t->q_tail++] = id;
    return id;
}

/* --- quiesce checks ----------------------------------------------- */

static int img_check_quiesced(mino_state *S, const char **reason)
{
    mino_thread_ctx_t *ctx = mino_current_ctx(S);
    /* Note: try_depth, dyn_stack, and module load stack are not checked —
     * they are execution state, not heap state. The image captures the
     * current binding state regardless of in-flight execution. */
    if (ctx->current_tx != NULL) {
        *reason = "active STM transaction";
        return 0;
    }
    if (S->async.run_head != NULL) {
        *reason = "in-flight async operations";
        return 0;
    }
    /* Pending futures live on S->threading.future_list_head (they detach
     * when the worker resolves and GC sweeps); the async scheduler queue
     * above does not track them. Walk the list and refuse if any future
     * is still PENDING. Realized/failed/cancelled futures may still be
     * on the chain until GC sweeps them; those don't block the save. */
    /* Pending futures live on S->threading.future_list_head (they detach
     * when the worker resolves and GC sweeps); the async scheduler queue
     * above does not track them. Walk the list and refuse if any future
     * has a running worker that has not yet published. Promises share
     * the future struct but never spawn a worker (thread_started == 0);
     * an undelivered promise is not actively mutating state and does
     * not block the save. */
    {
        mino_future *impl = S->threading.future_list_head;
        while (impl != NULL) {
            if (impl->state_tag == MINO_FUTURE_PENDING
                && impl->thread_started) {
                *reason = "in-flight futures";
                return 0;
            }
            impl = impl->next_in_state;
        }
    }
    {
        int pi;
        for (pi = 0; pi < (int)(sizeof(S->agent.pool) / sizeof(S->agent.pool[0])); pi++) {
            if (S->agent.pool[pi].run_head != NULL) {
                *reason = "pending agent actions";
                return 0;
            }
        }
    }
    return 1;
}

/* --- child traversal (discover reachable values) ------------------- */

/* Visit all mino_val* children of v, calling assign for each. */
static void img_visit_val_children(img_id_table *t, mino_val *v)
{
    if (v == NULL) return;
    /* Visit metadata (applies to all heap-allocated values) */
    if (MINO_IS_PTR(v) && MINO_IS_PTR(v->meta))
        img_idt_assign_val(t, v->meta);
    switch (mino_type_of(v)) {
    case MINO_NIL:
    case MINO_BOOL:
    case MINO_INT:
    case MINO_FLOAT:
    case MINO_FLOAT32:
    case MINO_CHAR:
    case MINO_STRING:
    case MINO_SYMBOL:
    case MINO_KEYWORD:
    case MINO_EMPTY_LIST:
    case MINO_UUID:
    case MINO_BYTES:
    case MINO_BIGINT:
    case MINO_PRIM:
        break;
    case MINO_CONS:
        img_idt_assign_val(t, v->as.cons.car);
        img_idt_assign_val(t, v->as.cons.cdr);
        break;
    case MINO_VECTOR: {
        size_t i;
        for (i = 0; i < v->as.vec.len; i++) {
            mino_val *e = vec_nth(v, i);
            img_idt_assign_val(t, e);
        }
        break;
    }
    case MINO_MAP: {
        size_t i;
        if (MINO_IS_PTR(v->as.map.key_order) &&
            MINO_IS_PTR(v->as.map.val_order)) {
            for (i = 0; i < v->as.map.len; i++) {
                mino_val *k = vec_nth(v->as.map.key_order, i);
                mino_val *val = vec_nth(v->as.map.val_order, i);
                img_idt_assign_val(t, k);
                img_idt_assign_val(t, val);
            }
        }
        break;
    }
    case MINO_SET: {
        size_t i;
        if (MINO_IS_PTR(v->as.set.key_order)) {
            for (i = 0; i < v->as.set.len; i++)
                img_idt_assign_val(t, vec_nth(v->as.set.key_order, i));
        }
        break;
    }
    case MINO_MAP_ENTRY:
        img_idt_assign_val(t, v->as.map_entry.k);
        img_idt_assign_val(t, v->as.map_entry.v);
        break;
    case MINO_FN:
    case MINO_MACRO:
        img_idt_assign_val(t, v->as.fn.params);
        img_idt_assign_val(t, v->as.fn.body);
        img_idt_assign_env(t, v->as.fn.env);
        if (MINO_IS_PTR(v->as.fn.wraps_prim))
            img_idt_assign_val(t, v->as.fn.wraps_prim);
        if (MINO_IS_PTR(v->as.fn.template_fn))
            img_idt_assign_val(t, v->as.fn.template_fn);
        break;
    case MINO_ATOM:
        img_idt_assign_val(t, v->as.atom.val);
        if (MINO_IS_PTR(v->as.atom.watches))
            img_idt_assign_val(t, v->as.atom.watches);
        if (MINO_IS_PTR(v->as.atom.validator))
            img_idt_assign_val(t, v->as.atom.validator);
        break;
    case MINO_VOLATILE:
        img_idt_assign_val(t, v->as.volatile_.val);
        break;
    case MINO_VAR:
        if (MINO_IS_PTR(v->as.var.root))
            img_idt_assign_val(t, v->as.var.root);
        if (MINO_IS_PTR(v->as.var.watches))
            img_idt_assign_val(t, v->as.var.watches);
        if (MINO_IS_PTR(v->as.var.validator))
            img_idt_assign_val(t, v->as.var.validator);
        break;
    case MINO_RATIO:
        img_idt_assign_val(t, v->as.ratio.num);
        img_idt_assign_val(t, v->as.ratio.denom);
        break;
    case MINO_STORE:
        img_idt_assign_val(t, v->as.store.val);
        if (MINO_IS_PTR(v->as.store.watches))
            img_idt_assign_val(t, v->as.store.watches);
        break;
    case MINO_BIGDEC:
        img_idt_assign_val(t, v->as.bigdec.unscaled);
        break;
    case MINO_REGEX:
        if (MINO_IS_PTR(v->as.regex.source))
            img_idt_assign_val(t, v->as.regex.source);
        break;
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET: {
        const mino_rb_node_t *stack[64];
        int sp = 0;
        const mino_rb_node_t *node = v->as.sorted.root;
        if (MINO_IS_PTR(v->as.sorted.comparator))
            img_idt_assign_val(t, v->as.sorted.comparator);
        while (node != NULL || sp > 0) {
            while (node != NULL) {
                if (sp < 64) stack[sp++] = node;
                node = node->left;
            }
            if (sp > 0) {
                node = stack[--sp];
                img_idt_assign_val(t, node->key);
                if (node->val != NULL)
                    img_idt_assign_val(t, node->val);
                node = node->right;
            }
        }
        break;
    }
    case MINO_LAZY:
        if (v->as.lazy.realized == 1) {
            /* Realized: visit cached value */
            if (MINO_IS_PTR(v->as.lazy.cached))
                img_idt_assign_val(t, v->as.lazy.cached);
        } else if (v->as.lazy.c_thunk == NULL) {
            /* Unrealized Clojure lazy: visit body + env */
            if (MINO_IS_PTR(v->as.lazy.body))
                img_idt_assign_val(t, v->as.lazy.body);
            if (v->as.lazy.env != NULL)
                img_idt_assign_env(t, v->as.lazy.env);
        }
        /* C-thunk lazies: no children, will error on emit */
        break;
    case MINO_QUEUE:
        if (MINO_IS_PTR(v->as.queue.front))
            img_idt_assign_val(t, v->as.queue.front);
        if (MINO_IS_PTR(v->as.queue.back))
            img_idt_assign_val(t, v->as.queue.back);
        break;
    case MINO_CHUNK: {
        unsigned i;
        for (i = 0; i < v->as.chunk.len; i++)
            img_idt_assign_val(t, v->as.chunk.vals[i]);
        break;
    }
    case MINO_CHUNKED_CONS:
        if (MINO_IS_PTR(v->as.chunked_cons.chunk))
            img_idt_assign_val(t, v->as.chunked_cons.chunk);
        if (MINO_IS_PTR(v->as.chunked_cons.more))
            img_idt_assign_val(t, v->as.chunked_cons.more);
        break;
    case MINO_TYPE:
        if (MINO_IS_PTR(v->as.record_type.fields))
            img_idt_assign_val(t, v->as.record_type.fields);
        break;
    case MINO_RECORD: {
        unsigned i, nfields;
        if (MINO_IS_PTR(v->as.record.type))
            img_idt_assign_val(t, v->as.record.type);
        nfields = 0;
        if (MINO_IS_PTR(v->as.record.type) &&
            mino_type_of(v->as.record.type) == MINO_TYPE &&
            MINO_IS_PTR(v->as.record.type->as.record_type.fields))
            nfields = (unsigned)v->as.record.type->as.record_type.fields->as.vec.len;
        for (i = 0; i < nfields; i++)
            img_idt_assign_val(t, v->as.record.vals[i]);
        if (MINO_IS_PTR(v->as.record.ext))
            img_idt_assign_val(t, v->as.record.ext);
        break;
    }
    case MINO_TX_REF:
        img_idt_assign_val(t, v->as.tx_ref.val);
        if (MINO_IS_PTR(v->as.tx_ref.watches))
            img_idt_assign_val(t, v->as.tx_ref.watches);
        if (MINO_IS_PTR(v->as.tx_ref.validator))
            img_idt_assign_val(t, v->as.tx_ref.validator);
        break;
    case MINO_TRANSIENT:
        if (MINO_IS_PTR(v->as.transient.current))
            img_idt_assign_val(t, v->as.transient.current);
        break;
    case MINO_HANDLE:
    case MINO_FUTURE:
    case MINO_CHAN:
    case MINO_AGENT:
    case MINO_HOST_ARRAY:
    case MINO_RECUR:
    case MINO_TAIL_CALL:
    case MINO_REDUCED:
        break;
    default:
        break;
    }
}

/* Visit all children of an env (binding values + parent).
 * Skips the parent if it's the core env — core is reinstalled on load. */
static void img_visit_env_children(img_id_table *t, mino_env *env)
{
    size_t i;
    if (env == NULL) return;
    for (i = 0; i < env->len; i++)
        img_idt_assign_val(t, env->bindings[i].val);
    if (env->parent != NULL && env->parent != t->core_env)
        img_idt_assign_env(t, env->parent);
}

/* Check if a namespace name belongs to the standard library (reinstalled
 * by mino_install_all and should not be saved in the image). */
int img_is_stdlib_ns(const char *name)
{
    if (name == NULL) return 1;
    if (strcmp(name, "clojure.core") == 0) return 1;
    if (strncmp(name, "clojure.", 8) == 0) return 1;
    if (strncmp(name, "mino.", 5) == 0) return 1;
    return 0;
}

/* --- BFS root walk ------------------------------------------------ */

static void img_walk_roots(mino_state *S, img_id_table *t)
{
    size_t i;

    /* Namespace envs: each binding value is a root.
     * Skip standard library namespaces (reinstalled on load). */
    for (i = 0; i < S->ns_vars.ns_env_len; i++) {
        ns_env_entry_t *ne = &S->ns_vars.ns_env_table[i];
        if (ne->name == NULL) continue;
        if (img_is_stdlib_ns(ne->name)) continue;
        if (ne->env != NULL)
            img_idt_assign_env(t, ne->env);
    }

    /* Var registry: each var's root is a root.
     * Skip standard library vars. */
    for (i = 0; i < S->ns_vars.var_registry_len; i++) {
        var_entry_t *ve = &S->ns_vars.var_registry[i];
        if (ve->ns == NULL) continue;
        if (img_is_stdlib_ns(ve->ns)) continue;
        if (ve->var != NULL)
            img_idt_assign_val(t, ve->var);
    }

    /* BFS: drain the queue, visiting children of each value/env */
    while (t->q_head < t->q_tail) {
        uint32_t id = t->queue[t->q_head++];
        if (t->id_vals[id] != NULL)
            img_visit_val_children(t, t->id_vals[id]);
        else if (t->id_envs[id] != NULL)
            img_visit_env_children(t, t->id_envs[id]);
    }
}

/* --- serializer --------------------------------------------------- */

static void img_emit_escaped(FILE *f, const char *s, size_t len)
{
    size_t i;
    fputc('"', f);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"') { fputc('\\', f); fputc('"', f); }
        else if (c == '\\') { fputc('\\', f); fputc('\\', f); }
        else if (c == '\n') { fputc('\\', f); fputc('n', f); }
        else if (c == '\r') { fputc('\\', f); fputc('r', f); }
        else if (c == '\t') { fputc('\\', f); fputc('t', f); }
        else fputc(c, f);
    }
    fputc('"', f);
}

/* Forward declarations for emit-with-ids */
static void img_emit_val_id(FILE *f, img_id_table *t, mino_val *v);
static void img_emit_env(FILE *f, img_id_table *t, mino_env *env, uint32_t id);

static void img_emit_val_id(FILE *f, img_id_table *t, mino_val *v)
{
    uint32_t id;
    if (v == NULL) { fprintf(f, "0"); return; }
    if (img_ht_lookup(&t->val_ht, v, &id)) {
        fprintf(f, "%u", id);
    } else {
        fprintf(f, "0");
    }
}

static void img_emit_env_id(FILE *f, img_id_table *t, mino_env *env)
{
    uint32_t id;
    if (env == NULL) { fprintf(f, "-"); return; }
    if (img_env_ht_lookup(t, env, &id)) {
        fprintf(f, "%u", id);
    } else {
        fprintf(f, "-");
    }
}

/* Emit a value with its full payload, using IDs for references. */
static void img_emit_val_full(FILE *f, img_id_table *t, mino_val *v, uint32_t id)
{
    if (v == NULL) {
        fprintf(f, "%u N\n", id);
        return;
    }
    switch (mino_type_of(v)) {
    case MINO_NIL:
        fprintf(f, "%u N\n", id);
        break;
    case MINO_BOOL:
        fprintf(f, "%u B %d\n", id, mino_val_bool_get(v));
        break;
    case MINO_INT:
        fprintf(f, "%u I %lld\n", id, mino_val_int_get(v));
        break;
    case MINO_FLOAT:
        fprintf(f, "%u D %.17g\n", id, v->as.f);
        break;
    case MINO_FLOAT32:
        fprintf(f, "%u DF %.8g\n", id, v->as.f);
        break;
    case MINO_CHAR:
        fprintf(f, "%u C %d\n", id, mino_val_char_get(v));
        break;
    case MINO_STRING:
        fprintf(f, "%u S ", id);
        img_emit_escaped(f, v->as.s.data, v->as.s.len);
        fputc('\n', f);
        break;
    case MINO_SYMBOL:
        fprintf(f, "%u Y ", id);
        img_emit_escaped(f, v->as.s.data, v->as.s.len);
        fputc('\n', f);
        break;
    case MINO_KEYWORD:
        fprintf(f, "%u K ", id);
        img_emit_escaped(f, v->as.s.data, v->as.s.len);
        fputc('\n', f);
        break;
    case MINO_EMPTY_LIST:
        fprintf(f, "%u EL\n", id);
        break;
    case MINO_CONS:
        fprintf(f, "%u L ", id);
        img_emit_val_id(f, t, v->as.cons.car);
        fputc(' ', f);
        img_emit_val_id(f, t, v->as.cons.cdr);
        fputc('\n', f);
        break;
    case MINO_VECTOR: {
        size_t i;
        fprintf(f, "%u V %zu", id, v->as.vec.len);
        for (i = 0; i < v->as.vec.len; i++) {
            mino_val *e = vec_nth(v, i);
            fputc(' ', f);
            img_emit_val_id(f, t, e);
        }
        fputc('\n', f);
        break;
    }
    case MINO_MAP: {
        size_t i;
        fprintf(f, "%u M %zu", id, v->as.map.len);
        if (MINO_IS_PTR(v->as.map.key_order) &&
            MINO_IS_PTR(v->as.map.val_order)) {
            for (i = 0; i < v->as.map.len; i++) {
                fputc(' ', f);
                img_emit_val_id(f, t, vec_nth(v->as.map.key_order, i));
                fputc(' ', f);
                img_emit_val_id(f, t, vec_nth(v->as.map.val_order, i));
            }
        }
        fputc('\n', f);
        break;
    }
    case MINO_SET: {
        size_t i;
        fprintf(f, "%u SE %zu", id, v->as.set.len);
        if (MINO_IS_PTR(v->as.set.key_order)) {
            for (i = 0; i < v->as.set.len; i++) {
                fputc(' ', f);
                img_emit_val_id(f, t, vec_nth(v->as.set.key_order, i));
            }
        }
        fputc('\n', f);
        break;
    }
    case MINO_MAP_ENTRY:
        fprintf(f, "%u ME ", id);
        img_emit_val_id(f, t, v->as.map_entry.k);
        fputc(' ', f);
        img_emit_val_id(f, t, v->as.map_entry.v);
        fputc('\n', f);
        break;
    case MINO_FN:
    case MINO_MACRO:
        fprintf(f, "%u FN ", id);
        img_emit_val_id(f, t, v->as.fn.params);
        fputc(' ', f);
        img_emit_val_id(f, t, v->as.fn.body);
        fputc(' ', f);
        img_emit_env_id(f, t, v->as.fn.env);
        fprintf(f, " %s %d\n",
                v->as.fn.defining_ns ? v->as.fn.defining_ns : "-",
                v->as.fn.shape);
        break;
    case MINO_ATOM:
        fprintf(f, "%u A ", id);
        img_emit_val_id(f, t, v->as.atom.val);
        fputc(' ', f);
        img_emit_val_id(f, t, v->as.atom.watches);
        fputc(' ', f);
        img_emit_val_id(f, t, v->as.atom.validator);
        fputc('\n', f);
        break;
    case MINO_VOLATILE:
        fprintf(f, "%u VL ", id);
        img_emit_val_id(f, t, v->as.volatile_.val);
        fputc('\n', f);
        break;
    case MINO_VAR:
        fprintf(f, "%u VAR %s %s %d %d %d %u ",
                id,
                v->as.var.ns ? v->as.var.ns : "-",
                v->as.var.sym ? v->as.var.sym : "-",
                v->as.var.dynamic,
                v->as.var.bound,
                v->as.var.is_private,
                v->as.var.version);
        img_emit_val_id(f, t, v->as.var.root);
        fputc(' ', f);
        img_emit_val_id(f, t, v->as.var.watches);
        fputc(' ', f);
        img_emit_val_id(f, t, v->as.var.validator);
        fputc('\n', f);
        break;
    case MINO_RATIO:
        fprintf(f, "%u RT ", id);
        img_emit_val_id(f, t, v->as.ratio.num);
        fputc(' ', f);
        img_emit_val_id(f, t, v->as.ratio.denom);
        fputc('\n', f);
        break;
    case MINO_STORE: {
        const char *sp = mino_store_path(v);
        fprintf(f, "%u ST ", id);
        img_emit_val_id(f, t, v->as.store.val);
        fprintf(f, " %s\n", sp ? sp : "-");
        break;
    }
    case MINO_BIGINT: {
        char buf[256];
        size_t written = 0;
        if (mino_to_bigint_str(v, buf, sizeof buf, &written) && written > 0)
            fprintf(f, "%u BI %s\n", id, buf);
        else
            fprintf(f, "%u N\n", id);
        break;
    }
    case MINO_BIGDEC:
        fprintf(f, "%u BD ", id);
        img_emit_val_id(f, t, v->as.bigdec.unscaled);
        fprintf(f, " %d\n", v->as.bigdec.scale);
        break;
    case MINO_UUID: {
        unsigned char ub[16];
        if (mino_to_uuid_bytes(v, ub)) {
            fprintf(f, "%u UI ", id);
            {
                int bi;
                for (bi = 0; bi < 16; bi++)
                    fprintf(f, "%02x", ub[bi]);
            }
            fputc('\n', f);
        } else {
            fprintf(f, "%u N\n", id);
        }
        break;
    }
    case MINO_REGEX:
        fprintf(f, "%u RX ", id);
        img_emit_val_id(f, t, v->as.regex.source);
        fputc('\n', f);
        break;
    case MINO_BYTES: {
        size_t blen = mino_bytes_len(v);
        const unsigned char *data = mino_bytes_data(v);
        fprintf(f, "%u BY %zu %u ", id, blen, v->as.bytes.bit_tail);
        {
            size_t bi;
            for (bi = 0; bi < blen; bi++)
                fprintf(f, "%02x", data[bi]);
        }
        fputc('\n', f);
        break;
    }
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET: {
        int is_map = (mino_type_of(v) == MINO_SORTED_MAP);
        fprintf(f, "%u %s %zu ", id, is_map ? "SM" : "SS",
                v->as.sorted.len);
        img_emit_val_id(f, t, v->as.sorted.comparator);
        /* Iterative in-order RB tree traversal */
        {
            const mino_rb_node_t *stack[64];
            int sp = 0;
            const mino_rb_node_t *node = v->as.sorted.root;
            while (node != NULL || sp > 0) {
                while (node != NULL) {
                    if (sp < 64) stack[sp++] = node;
                    node = node->left;
                }
                if (sp > 0) {
                    node = stack[--sp];
                    fputc(' ', f);
                    img_emit_val_id(f, t, node->key);
                    if (is_map) {
                        fputc(' ', f);
                        img_emit_val_id(f, t, node->val);
                    }
                    node = node->right;
                }
            }
        }
        fputc('\n', f);
        break;
    }
    case MINO_LAZY:
        if (v->as.lazy.realized == 1) {
            fprintf(f, "%u LZ R ", id);
            img_emit_val_id(f, t, v->as.lazy.cached);
            fputc('\n', f);
        } else if (v->as.lazy.c_thunk == NULL) {
            fprintf(f, "%u LZ U ", id);
            img_emit_val_id(f, t, v->as.lazy.body);
            fputc(' ', f);
            img_emit_env_id(f, t, v->as.lazy.env);
            fprintf(f, " %s\n",
                    v->as.lazy.defining_ns ? v->as.lazy.defining_ns : "-");
        } else {
            fprintf(f, "%u N\n", id);
        }
        break;
    case MINO_QUEUE:
        fprintf(f, "%u QU ", id);
        img_emit_val_id(f, t, v->as.queue.front);
        fputc(' ', f);
        img_emit_val_id(f, t, v->as.queue.back);
        fprintf(f, " %zu\n", v->as.queue.len);
        break;
    case MINO_CHUNK: {
        unsigned ci;
        fprintf(f, "%u CH %u", id, v->as.chunk.len);
        for (ci = 0; ci < v->as.chunk.len; ci++) {
            fputc(' ', f);
            img_emit_val_id(f, t, v->as.chunk.vals[ci]);
        }
        fputc('\n', f);
        break;
    }
    case MINO_CHUNKED_CONS:
        fprintf(f, "%u CC ", id);
        img_emit_val_id(f, t, v->as.chunked_cons.chunk);
        fputc(' ', f);
        img_emit_val_id(f, t, v->as.chunked_cons.more);
        fprintf(f, " %u\n", v->as.chunked_cons.off);
        break;
    case MINO_TYPE:
        fprintf(f, "%u TY %s %s ", id,
                v->as.record_type.ns ? v->as.record_type.ns : "-",
                v->as.record_type.name ? v->as.record_type.name : "-");
        img_emit_val_id(f, t, v->as.record_type.fields);
        fputc('\n', f);
        break;
    case MINO_RECORD: {
        unsigned i, nfields = 0;
        if (MINO_IS_PTR(v->as.record.type) &&
            mino_type_of(v->as.record.type) == MINO_TYPE &&
            MINO_IS_PTR(v->as.record.type->as.record_type.fields))
            nfields = (unsigned)v->as.record.type->as.record_type.fields->as.vec.len;
        fprintf(f, "%u RC ", id);
        img_emit_val_id(f, t, v->as.record.type);
        fprintf(f, " %u", nfields);
        for (i = 0; i < nfields; i++) {
            fputc(' ', f);
            img_emit_val_id(f, t, v->as.record.vals[i]);
        }
        fputc(' ', f);
        img_emit_val_id(f, t, v->as.record.ext);
        fputc('\n', f);
        break;
    }
    case MINO_PRIM:
        fprintf(f, "%u PR %s\n", id,
                v->as.prim.name ? v->as.prim.name : "-");
        break;
    case MINO_TX_REF:
        fprintf(f, "%u TX ", id);
        img_emit_val_id(f, t, v->as.tx_ref.val);
        fputc(' ', f);
        img_emit_val_id(f, t, v->as.tx_ref.watches);
        fputc(' ', f);
        img_emit_val_id(f, t, v->as.tx_ref.validator);
        fprintf(f, " %llu %llu\n",
                (unsigned long long)v->as.tx_ref.version,
                (unsigned long long)v->as.tx_ref.ref_id);
        break;
    case MINO_TRANSIENT:
        fprintf(f, "%u TR ", id);
        img_emit_val_id(f, t, v->as.transient.current);
        fputc('\n', f);
        break;
    default:
        /* Unserializable types (handle, future, chan, agent, host-array,
         * recur/tail-call/reduced sentinels): substitute nil so the load
         * doesn't fail. The host should re-create these after loading. */
        fprintf(f, "%u N\n", id);
        break;
    }
}

static void img_emit_env(FILE *f, img_id_table *t, mino_env *env, uint32_t id)
{
    size_t i;
    fprintf(f, "%u E ", id);
    img_emit_env_id(f, t, env->parent);
    fprintf(f, " %zu", env->len);
    for (i = 0; i < env->len; i++) {
        fprintf(f, " %s ", env->bindings[i].name);
        img_emit_val_id(f, t, env->bindings[i].val);
    }
    fputc('\n', f);
}

/* --- public API: save --------------------------------------------- */

int mino_save_image(mino_state *S, const char *path)
{
    const char *quiesce_reason;
    FILE *f;
    img_id_table idt;
    uint32_t i;
    uint32_t crc = 0;
    long file_end;
    char *buf;
    size_t buf_len;

    if (!img_check_quiesced(S, &quiesce_reason)) {
        char diag[128];
        snprintf(diag, sizeof diag,
                 "save-image: runtime not quiesced (%s)", quiesce_reason);
        set_eval_diag(S, NULL, "contract", "MCT002", diag);
        return -1;
    }

    if (img_idt_init(&idt) != 0) {
        set_eval_diag(S, NULL, "internal", "MIN001",
                       "save-image: out of memory");
        return -1;
    }
    idt.core_env = S->ns_vars.mino_core_env;
    img_walk_roots(S, &idt);

    /* Open read-write so we can read the file back for CRC verification */
    f = fopen(path, "wb+");
    if (f == NULL) {
        img_idt_free(&idt);
        set_eval_diag(S, NULL, "io", "MIO001",
                       "save-image: cannot open file for writing");
        return -1;
    }

    /* Header */
    fprintf(f, "%s\n", IMG_MAGIC);
    fprintf(f, "# created %lld\n", (long long)(mino_monotonic_ns() / 1000000));
    fprintf(f, "# current-ns %s\n",
            S->ns_vars.current_ns ? S->ns_vars.current_ns : "user");
    fputc('\n', f);

    /* Values section */
    fprintf(f, "VALUES\n");
    for (i = 1; i < idt.id_count; i++) {
        if (idt.id_vals[i] != NULL)
            img_emit_val_full(f, &idt, idt.id_vals[i], i);
        else if (idt.id_envs[i] != NULL)
            img_emit_env(f, &idt, idt.id_envs[i], i);
    }

    /* Meta section: values with non-null metadata */
    fprintf(f, "\nMETA\n");
    for (i = 1; i < idt.id_count; i++) {
        mino_val *mv = idt.id_vals[i];
        if (mv != NULL && MINO_IS_PTR(mv) && MINO_IS_PTR(mv->meta)) {
            fprintf(f, "%u ", i);
            img_emit_val_id(f, &idt, mv->meta);
            fputc('\n', f);
        }
    }

    /* Roots section: namespaces and vars */
    fprintf(f, "\nROOTS\n");
    {
        size_t j;
        for (j = 0; j < S->ns_vars.ns_env_len; j++) {
            ns_env_entry_t *ne = &S->ns_vars.ns_env_table[j];
            if (ne->name == NULL) continue;
            if (img_is_stdlib_ns(ne->name)) continue;
            fprintf(f, "NS %s ", ne->name);
            img_emit_env_id(f, &idt, ne->env);
            fputc(' ', f);
            img_emit_val_id(f, &idt, ne->meta);
            fputc('\n', f);
        }
        /* Var registry: emit (ns, name, var-id) for each non-stdlib var */
        for (j = 0; j < S->ns_vars.var_registry_len; j++) {
            var_entry_t *ve = &S->ns_vars.var_registry[j];
            uint32_t vid;
            if (ve->ns == NULL || ve->name == NULL) continue;
            if (img_is_stdlib_ns(ve->ns)) continue;
            if (!img_ht_lookup(&idt.val_ht, ve->var, &vid)) continue;
            fprintf(f, "VREG %s %s %u\n", ve->ns, ve->name, vid);
        }
        for (j = 0; j < S->ns_vars.ns_env_len; j++) {
            ns_env_entry_t *ne = &S->ns_vars.ns_env_table[j];
            size_t a;
            if (ne->name == NULL) continue;
            if (img_is_stdlib_ns(ne->name)) continue;
            for (a = 0; a < S->ns_vars.ns_alias_len; a++) {
                ns_alias_t *al = &S->ns_vars.ns_aliases[a];
                if (al->owning_ns != NULL && ne->name != NULL &&
                    strcmp(al->owning_ns, ne->name) == 0) {
                    fprintf(f, "ALIAS %s %s %s\n",
                            al->owning_ns, al->alias, al->full_name);
                }
            }
        }
    }
    fprintf(f, "CURSOR %s\n",
            S->ns_vars.current_ns ? S->ns_vars.current_ns : "user");

    /* CRC32: compute over everything written so far */
    fflush(f);
    fseek(f, 0, SEEK_END);
    file_end = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf_len = (size_t)file_end;
    buf = (char *)malloc(buf_len);
    if (buf != NULL) {
        size_t got = fread(buf, 1, buf_len, f);
        if (got == buf_len)
            crc = img_crc32_update(0, buf, buf_len);
        free(buf);
    }
    fseek(f, 0, SEEK_END);
    fprintf(f, "CRC32 %08x\n", crc);

    fclose(f);
    img_idt_free(&idt);
    return 0;
}
