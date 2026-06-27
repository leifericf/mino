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

static void img_ht_init(img_ht *h)
{
    h->cap    = IMG_HT_INIT;
    h->count  = 0;
    h->entries = (img_ht_entry *)calloc(h->cap, sizeof(img_ht_entry));
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

static void img_env_ht_init(img_id_table *t)
{
    t->env_ht.cap   = IMG_HT_INIT;
    t->env_ht.count = 0;
    t->env_ht.keys  = (mino_env **)calloc(t->env_ht.cap, sizeof(mino_env *));
    t->env_ht.ids   = (uint32_t *)calloc(t->env_ht.cap, sizeof(uint32_t));
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

static void img_idt_init(img_id_table *t)
{
    img_ht_init(&t->val_ht);
    img_env_ht_init(t);
    t->id_count = 0;
    t->id_cap   = 256;
    t->id_vals  = (mino_val **)calloc(t->id_cap, sizeof(mino_val *));
    t->id_envs  = (mino_env **)calloc(t->id_cap, sizeof(mino_env *));
    t->queue    = (uint32_t *)calloc(t->id_cap, sizeof(uint32_t));
    t->q_head   = 0;
    t->q_tail   = 0;
    t->core_env = NULL;
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
    case MINO_REGEX:
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
        if (v->as.map.key_order != NULL) {
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
        if (v->as.set.key_order != NULL) {
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
        if (v->as.fn.wraps_prim != NULL)
            img_idt_assign_val(t, v->as.fn.wraps_prim);
        if (v->as.fn.template_fn != NULL)
            img_idt_assign_val(t, v->as.fn.template_fn);
        break;
    case MINO_ATOM:
        img_idt_assign_val(t, v->as.atom.val);
        if (v->as.atom.watches != NULL)
            img_idt_assign_val(t, v->as.atom.watches);
        if (v->as.atom.validator != NULL)
            img_idt_assign_val(t, v->as.atom.validator);
        break;
    case MINO_VOLATILE:
        img_idt_assign_val(t, v->as.volatile_.val);
        break;
    case MINO_VAR:
        if (v->as.var.root != NULL)
            img_idt_assign_val(t, v->as.var.root);
        if (v->as.var.watches != NULL)
            img_idt_assign_val(t, v->as.var.watches);
        if (v->as.var.validator != NULL)
            img_idt_assign_val(t, v->as.var.validator);
        break;
    case MINO_RATIO:
        img_idt_assign_val(t, v->as.ratio.num);
        img_idt_assign_val(t, v->as.ratio.denom);
        break;
    case MINO_STORE:
        img_idt_assign_val(t, v->as.store.val);
        if (v->as.store.watches != NULL)
            img_idt_assign_val(t, v->as.store.watches);
        break;
    case MINO_BIGINT:
    case MINO_BIGDEC:
    case MINO_BYTES:
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET:
    case MINO_TYPE:
    case MINO_RECORD:
    case MINO_LAZY:
    case MINO_CHUNK:
    case MINO_CHUNKED_CONS:
    case MINO_HANDLE:
    case MINO_FUTURE:
    case MINO_CHAN:
    case MINO_QUEUE:
    case MINO_TX_REF:
    case MINO_AGENT:
    case MINO_TRANSIENT:
    case MINO_PRIM:
    case MINO_RECUR:
    case MINO_TAIL_CALL:
    case MINO_REDUCED:
    case MINO_HOST_ARRAY:
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
        if (v->as.map.key_order != NULL) {
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
        if (v->as.set.key_order != NULL) {
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
    default:
        fprintf(f, "%u UNSUPPORTED %d\n", id, (int)mino_type_of(v));
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

    img_idt_init(&idt);
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
    for (i = 0; i < idt.id_count; i++) {
        if (idt.id_vals[i] != NULL)
            img_emit_val_full(f, &idt, idt.id_vals[i], i);
        else if (idt.id_envs[i] != NULL)
            img_emit_env(f, &idt, idt.id_envs[i], i);
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
