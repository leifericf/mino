/*
 * image.c -- save-lisp-and-die (SLAD) image serialization.
 *
 * Saves the full runtime state (namespaces, vars, values, closures,
 * mutable refs) to a line-delimited text image file. On load, the
 * image is read into a fresh state initialized via mino_state_new +
 * mino_install_all.
 *
 * Design: value-serialization with an identity table. A BFS from the
 * GC roots assigns each reachable value a stable integer ID; shared
 * references and cycles are handled naturally. See ADR 12.
 *
 * Limitations (v1):
 *   - Functions are saved with params+body+env; bytecode is recompiled
 *     on load via the compiler. JIT state is dropped.
 *   - clojure.core is skipped (reinstalled by bootstrap).
 *   - Futures, channels, agents must be quiesced (checked, then dropped).
 *   - Sorted collections revert to default comparator on load.
 *   - Host handles, regex, bytes, uuid, records: error on save.
 */

#include "runtime/internal.h"
#include "mino.h"
#include "values/internal.h"
#include "collections/internal.h"
#include "eval/internal.h"
#include "gc/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- constants ---------------------------------------------------- */

#define IMG_MAGIC       "MINO-IMAGE/1"
#define IMG_VERSION     1
#define IMG_HT_INIT     256
#define IMG_HT_LOAD     75

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

static uint32_t img_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    if (!img_crc32_table_ready) img_crc32_table_init();
    crc = ~crc;
    for (i = 0; i < len; i++)
        crc = img_crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* Duplicate a NUL-terminated C string into malloc'd storage. */
static char *img_dup_str(const char *s)
{
    size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (out != NULL) memcpy(out, s, len + 1);
    return out;
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
static int img_is_stdlib_ns(const char *name)
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

/* --- deserializer ------------------------------------------------- */

/* Parse state for the image reader. */
typedef struct {
    /* ID → allocated value/env */
    mino_val **id_vals;
    mino_env **id_envs;
    uint32_t   id_count;
    uint32_t   id_cap;
    /* Raw lines for each ID (for the patch pass) */
    char     **id_lines;
    /* Type tag for each ID */
    int       *id_types; /* 0=unallocated, 1=val, 2=env */
    mino_state *S;
    mino_env   *env;
} img_reader;

static void img_reader_init(img_reader *r, mino_state *S, mino_env *env)
{
    r->S = S;
    r->env = env;
    r->id_count = 0;
    r->id_cap = 256;
    r->id_vals = (mino_val **)calloc(r->id_cap, sizeof(mino_val *));
    r->id_envs = (mino_env **)calloc(r->id_cap, sizeof(mino_env *));
    r->id_lines = (char **)calloc(r->id_cap, sizeof(char *));
    r->id_types = (int *)calloc(r->id_cap, sizeof(int));
}

static void img_reader_free(img_reader *r)
{
    uint32_t i;
    for (i = 0; i < r->id_count; i++)
        free(r->id_lines[i]);
    free(r->id_vals);
    free(r->id_envs);
    free(r->id_lines);
    free(r->id_types);
}

static void img_reader_grow(img_reader *r)
{
    r->id_cap *= 2;
    r->id_vals = (mino_val **)realloc(r->id_vals, r->id_cap * sizeof(mino_val *));
    r->id_envs = (mino_env **)realloc(r->id_envs, r->id_cap * sizeof(mino_env *));
    r->id_lines = (char **)realloc(r->id_lines, r->id_cap * sizeof(char *));
    r->id_types = (int *)realloc(r->id_types, r->id_cap * sizeof(int));
}

/* Skip whitespace, return pointer to next non-space char. */
static const char *img_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Parse a uint32 from string, advance pointer. Returns 0 on failure. */
static int img_parse_u32(const char **pp, uint32_t *out)
{
    const char *p = img_skip_ws(*pp);
    char *end;
    unsigned long val = strtoul(p, &end, 10);
    if (end == p) return 0;
    *out = (uint32_t)val;
    *pp = end;
    return 1;
}

/* Parse a long long from string, advance pointer. */
static int img_parse_ll(const char **pp, long long *out)
{
    const char *p = img_skip_ws(*pp);
    char *end;
    long long val = strtoll(p, &end, 10);
    if (end == p) return 0;
    *out = val;
    *pp = end;
    return 1;
}

/* Parse a double from string, advance pointer. */
static int img_parse_d(const char **pp, double *out)
{
    const char *p = img_skip_ws(*pp);
    char *end;
    double val = strtod(p, &end);
    if (end == p) return 0;
    *out = val;
    *pp = end;
    return 1;
}

/* Parse a quoted escaped string from the image. Advances pp past
 * the closing quote. Returns malloc'd string (caller frees). */
static char *img_parse_str(const char **pp)
{
    const char *p = img_skip_ws(*pp);
    size_t cap = 64, len = 0;
    char *out = (char *)malloc(cap);
    if (*p != '"') { free(out); return NULL; }
    p++;
    while (*p != '"' && *p != '\0') {
        char c;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case '"': c = '"';  break;
            case '\\': c = '\\'; break;
            default: c = *p;    break;
            }
            p++;
        } else {
            c = *p++;
        }
        if (len + 1 >= cap) {
            cap *= 2;
            out = (char *)realloc(out, cap);
        }
        out[len++] = c;
    }
    if (*p == '"') p++;
    out[len] = '\0';
    *pp = p;
    return out;
}

/* Parse a non-quoted token (terminated by whitespace or EOL). */
static char *img_parse_token(const char **pp)
{
    const char *p = img_skip_ws(*pp);
    const char *start = p;
    size_t len;
    char *out;
    while (*p != ' ' && *p != '\t' && *p != '\0' && *p != '\n' && *p != '\r')
        p++;
    len = (size_t)(p - start);
    out = (char *)malloc(len + 1);
    memcpy(out, start, len);
    out[len] = '\0';
    *pp = p;
    return out;
}

/* Allocate a value for the given ID based on its type tag.
 * Stores the raw line for the patch pass. */
static int img_alloc_one(img_reader *r, uint32_t id, const char *type_tag,
                           const char *line_rest)
{
    mino_state *S = r->S;
    mino_val *v = NULL;
    mino_env *e = NULL;
    char *line_copy;

    if (id >= r->id_cap) {
        while (id >= r->id_cap) img_reader_grow(r);
    }
    if (id >= r->id_count) r->id_count = id + 1;

    /* Save the line for the patch pass */
    {
        size_t rlen = strlen(type_tag) + 1 + strlen(line_rest) + 1;
        line_copy = (char *)malloc(rlen);
        snprintf(line_copy, rlen, "%s %s", type_tag, line_rest);
    }
    r->id_lines[id] = line_copy;

    if (strcmp(type_tag, "E") == 0) {
        /* Environment: allocate via GC */
        const char *p = line_rest;
        char *parent_tok;
        e = env_alloc(S, NULL);  /* parent set in patch pass */
        r->id_envs[id] = e;
        r->id_types[id] = 2;
        /* Skip parent token (could be "-" for NULL or an ID) */
        parent_tok = img_parse_token(&p);
        free(parent_tok);
        /* Parse binding count to pre-allocate */
        {
            long long n;
            if (img_parse_ll(&p, &n) && n > 0) {
                e->cap = (size_t)n;
                e->bindings = (env_binding_t *)calloc((size_t)n,
                                              sizeof(env_binding_t));
            }
        }
        return 1;
    }

    /* It's a value — allocate based on type tag */
    r->id_types[id] = 1;

    if (strcmp(type_tag, "N") == 0) {
        v = mino_nil(S);
    } else if (strcmp(type_tag, "B") == 0) {
        long long b;
        const char *p = line_rest;
        if (!img_parse_ll(&p, &b)) return 0;
        v = b ? mino_true(S) : mino_false(S);
    } else if (strcmp(type_tag, "I") == 0) {
        long long i;
        const char *p = line_rest;
        if (!img_parse_ll(&p, &i)) return 0;
        v = mino_int(S, i);
    } else if (strcmp(type_tag, "D") == 0) {
        double d;
        const char *p = line_rest;
        if (!img_parse_d(&p, &d)) return 0;
        v = mino_float(S, d);
    } else if (strcmp(type_tag, "DF") == 0) {
        double d;
        const char *p = line_rest;
        if (!img_parse_d(&p, &d)) return 0;
        v = mino_float32(S, (float)d);
    } else if (strcmp(type_tag, "C") == 0) {
        long long c;
        const char *p = line_rest;
        if (!img_parse_ll(&p, &c)) return 0;
        v = mino_char(S, (int)c);
    } else if (strcmp(type_tag, "S") == 0) {
        char *s;
        const char *p = line_rest;
        s = img_parse_str(&p);
        if (s == NULL) return 0;
        v = mino_string_n(S, s, strlen(s));
        free(s);
    } else if (strcmp(type_tag, "Y") == 0) {
        char *s;
        const char *p = line_rest;
        s = img_parse_str(&p);
        if (s == NULL) return 0;
        v = mino_symbol(S, s);
        free(s);
    } else if (strcmp(type_tag, "K") == 0) {
        char *s;
        const char *p = line_rest;
        s = img_parse_str(&p);
        if (s == NULL) return 0;
        v = mino_keyword(S, s);
        free(s);
    } else if (strcmp(type_tag, "EL") == 0) {
        v = mino_empty_list(S);
    } else {
        /* Types that need the patch pass: L, V, M, SE, ME, FN, A, VL,
         * VAR, RT, ST. Allocate the shell now; fill in patch pass. */
        if (strcmp(type_tag, "L") == 0) {
            v = alloc_val(S, MINO_CONS);
        } else if (strcmp(type_tag, "V") == 0) {
            v = alloc_val(S, MINO_VECTOR);
            v->as.vec.root = NULL;
            v->as.vec.tail = NULL;
            v->as.vec.len = 0;
            v->as.vec.tail_len = 0;
            v->as.vec.shift = 0;
        } else if (strcmp(type_tag, "M") == 0) {
            v = alloc_val(S, MINO_MAP);
        } else if (strcmp(type_tag, "SE") == 0) {
            v = alloc_val(S, MINO_SET);
        } else if (strcmp(type_tag, "ME") == 0) {
            v = alloc_val(S, MINO_MAP_ENTRY);
        } else if (strcmp(type_tag, "FN") == 0) {
            v = alloc_val(S, MINO_FN);
        } else if (strcmp(type_tag, "A") == 0) {
            v = alloc_val(S, MINO_ATOM);
        } else if (strcmp(type_tag, "VL") == 0) {
            v = alloc_val(S, MINO_VOLATILE);
        } else if (strcmp(type_tag, "VAR") == 0) {
            v = alloc_val(S, MINO_VAR);
        } else if (strcmp(type_tag, "RT") == 0) {
            v = alloc_val(S, MINO_RATIO);
        } else if (strcmp(type_tag, "ST") == 0) {
            v = alloc_val(S, MINO_STORE);
        } else {
            return 0; /* unsupported type */
        }
    }

    r->id_vals[id] = v;
    return 1;
}

/* Resolve an ID to a value. */
static mino_val *img_resolve_val(img_reader *r, uint32_t id)
{
    if (id >= r->id_count) return mino_nil(r->S);
    return r->id_vals[id] != NULL ? r->id_vals[id] : mino_nil(r->S);
}

/* Resolve an ID to an env. */
static mino_env *img_resolve_env(img_reader *r, uint32_t id)
{
    if (id == 0 || id >= r->id_count) return NULL;
    return r->id_envs[id];
}

/* Patch a single value's references now that all values are allocated. */
static int img_patch_one(img_reader *r, uint32_t id)
{
    mino_state *S = r->S;
    const char *line = r->id_lines[id];
    const char *p;
    char *tag;

    if (line == NULL) return 1;
    p = line;
    tag = img_parse_token(&p);

    if (strcmp(tag, "L") == 0) {
        uint32_t car_id, cdr_id;
        mino_val *v = r->id_vals[id];
        if (!img_parse_u32(&p, &car_id) || !img_parse_u32(&p, &cdr_id)) {
            free(tag); return 0;
        }
        v->as.cons.car = img_resolve_val(r, car_id);
        v->as.cons.cdr = img_resolve_val(r, cdr_id);
    } else if (strcmp(tag, "V") == 0) {
        uint32_t count, i;
        if (!img_parse_u32(&p, &count)) { free(tag); return 0; }
        {
            mino_vec_builder *vb = mino_vector_builder_new(S);
            for (i = 0; i < count; i++) {
                uint32_t eid;
                if (!img_parse_u32(&p, &eid)) { free(tag); return 0; }
                mino_vector_builder_push(vb, img_resolve_val(r, eid));
            }
            {
                mino_val *nv = mino_vector_builder_finish(vb);
                mino_val *v = r->id_vals[id];
                v->as.vec = nv->as.vec;
                v->type = MINO_VECTOR;
            }
        }
    } else if (strcmp(tag, "M") == 0) {
        uint32_t count, i;
        if (!img_parse_u32(&p, &count)) { free(tag); return 0; }
        {
            mino_map_builder *mb = mino_map_builder_new(S);
            for (i = 0; i < count; i++) {
                uint32_t kid, vid;
                if (!img_parse_u32(&p, &kid) || !img_parse_u32(&p, &vid)) {
                    free(tag); return 0;
                }
                mino_map_builder_put(mb, img_resolve_val(r, kid),
                                    img_resolve_val(r, vid));
            }
            {
                mino_val *nv = mino_map_builder_finish(mb);
                mino_val *v = r->id_vals[id];
                v->as.map = nv->as.map;
                v->type = MINO_MAP;
            }
        }
    } else if (strcmp(tag, "SE") == 0) {
        uint32_t count, i;
        if (!img_parse_u32(&p, &count)) { free(tag); return 0; }
        {
            /* Build set from vector of elements */
            mino_val *elems[count > 0 ? count : 1];
            for (i = 0; i < count; i++) {
                uint32_t eid;
                if (!img_parse_u32(&p, &eid)) { free(tag); return 0; }
                elems[i] = img_resolve_val(r, eid);
            }
            {
                mino_val *nv = mino_set(S, elems, count);
                mino_val *v = r->id_vals[id];
                v->as.set = nv->as.set;
                v->type = MINO_SET;
            }
        }
    } else if (strcmp(tag, "ME") == 0) {
        uint32_t kid, vid;
        mino_val *v = r->id_vals[id];
        if (!img_parse_u32(&p, &kid) || !img_parse_u32(&p, &vid)) {
            free(tag); return 0;
        }
        v->as.map_entry.k = img_resolve_val(r, kid);
        v->as.map_entry.v = img_resolve_val(r, vid);
    } else if (strcmp(tag, "FN") == 0) {
        uint32_t pid, bid, eid;
        char *ns_str;
        long long shape;
        mino_val *v = r->id_vals[id];
        if (!img_parse_u32(&p, &pid) || !img_parse_u32(&p, &bid) ||
            !img_parse_u32(&p, &eid)) { free(tag); return 0; }
        ns_str = img_parse_token(&p);
        if (!img_parse_ll(&p, &shape)) { free(ns_str); free(tag); return 0; }
        v->as.fn.params = img_resolve_val(r, pid);
        v->as.fn.body = img_resolve_val(r, bid);
        v->as.fn.env = img_resolve_env(r, eid);
        v->as.fn.defining_ns = (ns_str && strcmp(ns_str, "-") != 0)
                               ? img_dup_str(ns_str) : NULL;
        v->as.fn.shape = (int)shape;
        v->as.fn.bc = NULL;
        v->as.fn.wraps_prim = NULL;
        v->as.fn.template_fn = NULL;
        free(ns_str);
    } else if (strcmp(tag, "A") == 0) {
        uint32_t vid, wid, valid;
        mino_val *v = r->id_vals[id];
        if (!img_parse_u32(&p, &vid) || !img_parse_u32(&p, &wid) ||
            !img_parse_u32(&p, &valid)) { free(tag); return 0; }
        v->as.atom.val = img_resolve_val(r, vid);
        v->as.atom.watches = wid == 0 ? NULL : img_resolve_val(r, wid);
        v->as.atom.validator = valid == 0 ? NULL : img_resolve_val(r, valid);
    } else if (strcmp(tag, "VL") == 0) {
        uint32_t vid;
        mino_val *v = r->id_vals[id];
        if (!img_parse_u32(&p, &vid)) { free(tag); return 0; }
        v->as.volatile_.val = img_resolve_val(r, vid);
    } else if (strcmp(tag, "VAR") == 0) {
        char *ns_str, *sym_str;
        long long dyn, bound, priv, ver;
        uint32_t rid, wid, valid;
        mino_val *v = r->id_vals[id];
        ns_str = img_parse_token(&p);
        sym_str = img_parse_token(&p);
        if (!img_parse_ll(&p, &dyn) || !img_parse_ll(&p, &bound) ||
            !img_parse_ll(&p, &priv) || !img_parse_ll(&p, &ver) ||
            !img_parse_u32(&p, &rid) || !img_parse_u32(&p, &wid) ||
            !img_parse_u32(&p, &valid)) {
            free(ns_str); free(sym_str); free(tag); return 0;
        }
        v->as.var.ns = (ns_str && strcmp(ns_str, "-") != 0)
                       ? img_dup_str(ns_str) : NULL;
        v->as.var.sym = (sym_str && strcmp(sym_str, "-") != 0)
                        ? img_dup_str(sym_str) : NULL;
        v->as.var.dynamic = (int)dyn;
        v->as.var.bound = (int)bound;
        v->as.var.is_private = (int)priv;
        v->as.var.version = (unsigned)ver;
        v->as.var.root = img_resolve_val(r, rid);
        v->as.var.watches = wid == 0 ? NULL : img_resolve_val(r, wid);
        v->as.var.validator = valid == 0 ? NULL : img_resolve_val(r, valid);
        free(ns_str);
        free(sym_str);
    } else if (strcmp(tag, "RT") == 0) {
        uint32_t nid, did;
        mino_val *v = r->id_vals[id];
        if (!img_parse_u32(&p, &nid) || !img_parse_u32(&p, &did)) {
            free(tag); return 0;
        }
        v->as.ratio.num = img_resolve_val(r, nid);
        v->as.ratio.denom = img_resolve_val(r, did);
    } else if (strcmp(tag, "ST") == 0) {
        uint32_t dbid;
        char *path_str;
        mino_val *v = r->id_vals[id];
        if (!img_parse_u32(&p, &dbid)) { free(tag); return 0; }
        path_str = img_parse_token(&p);
        v->as.store.val = img_resolve_val(r, dbid);
        v->as.store.watches = NULL;
        v->as.store.handle = NULL;
        v->as.store.store_id = (uint64_t)(uintptr_t)v;
        v->as.store.owning_state = S;
        if (path_str && strcmp(path_str, "-") != 0) {
            /* Create a proper store handle via the public constructor */
            mino_val *tmp = mino_store_val(S, v->as.store.val, path_str,
                                            NULL, NULL);
            if (tmp != NULL) {
                v->as.store.handle = tmp->as.store.handle;
                tmp->as.store.handle = NULL; /* prevent double-free */
            }
        }
        free(path_str);
    } else if (strcmp(tag, "E") == 0) {
        /* Patch environment: parent + bindings */
        char *parent_tok;
        long long n;
        mino_env *e = r->id_envs[id];
        parent_tok = img_parse_token(&p);
        if (parent_tok != NULL && strcmp(parent_tok, "-") != 0) {
            uint32_t pid = (uint32_t)strtoul(parent_tok, NULL, 10);
            e->parent = img_resolve_env(r, pid);
        }
        free(parent_tok);
        if (!img_parse_ll(&p, &n)) { free(tag); return 0; }
        e->len = 0;
        {
            long long j;
            for (j = 0; j < n; j++) {
                char *name;
                uint32_t vid;
                name = img_parse_token(&p);
                if (!img_parse_u32(&p, &vid)) { free(name); free(tag); return 0; }
                if (name != NULL && e->len < e->cap) {
                    e->bindings[e->len].name = name; /* takes ownership */
                    e->bindings[e->len].val = img_resolve_val(r, vid);
                    e->len++;
                } else {
                    free(name);
                }
            }
        }
    }

    free(tag);
    return 1;
}

/* --- public API: load --------------------------------------------- */

int mino_load_image_into(mino_state *S, const char *path)
{
    FILE *f;
    long sz;
    char *buf;
    char *line;
    img_reader r;
    uint32_t i;
    int in_values = 0;
    int rc = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        set_eval_diag(S, NULL, "io", "MIO001",
                       "load-image: cannot open file");
        return -1;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) { fclose(f); return -1; }
    {
        size_t got = fread(buf, 1, (size_t)sz, f);
        if (got != (size_t)sz) { free(buf); fclose(f); return -1; }
    }
    buf[sz] = '\0';
    fclose(f);

    /* Verify magic */
    if (strncmp(buf, IMG_MAGIC, strlen(IMG_MAGIC)) != 0) {
        set_eval_diag(S, NULL, "io", "MIO001",
                       "load-image: bad magic");
        free(buf);
        return -1;
    }

    /* Verify CRC32: find the trailer, compute CRC over everything before it */
    {
        char       *crc_pos = NULL;
        char       *search;
        uint32_t    stored_crc = 0;
        uint32_t    computed_crc;
        /* Find the last "CRC32 " line */
        for (search = buf; (search = strstr(search, "\nCRC32 ")) != NULL; )
            crc_pos = ++search;
        if (crc_pos != NULL) {
            /* crc_pos points right after the \n before CRC32 */
            stored_crc = (uint32_t)strtoul(crc_pos + 5, NULL, 16);
            /* Compute CRC over everything from start to the \n before CRC32 */
            *crc_pos = '\0';  /* truncate at the \n */
            computed_crc = img_crc32_update(0, buf, (size_t)(crc_pos - buf));
            if (computed_crc != stored_crc) {
                char diag[128];
                snprintf(diag, sizeof diag,
                    "load-image: CRC mismatch (stored %08x, computed %08x)",
                    stored_crc, computed_crc);
                set_eval_diag(S, NULL, "io", "MIO001", diag);
                free(buf);
                return -1;
            }
            /* Restore the \n for line parsing */
            *crc_pos = '\n';
        }
        /* If no CRC32 line found, proceed without verification (v0 compat) */
    }

    img_reader_init(&r, S, ns_env_ensure(S, "clojure.core"));

    /* First pass: allocate all values */
    line = buf;
    while (*line != '\0') {
        char *nl = strchr(line, '\n');
        char saved = '\0';
        if (nl) { saved = *nl; *nl = '\0'; }

        /* Skip header lines */
        if (strncmp(line, IMG_MAGIC, strlen(IMG_MAGIC)) == 0 ||
            line[0] == '#' || line[0] == '\0' || line[0] == '\n') {
            if (nl) { *nl = saved; line = nl + 1; } else break;
            continue;
        }
        if (strcmp(line, "VALUES") == 0) {
            in_values = 1;
            if (nl) { *nl = saved; line = nl + 1; } else break;
            continue;
        }
        if (strcmp(line, "ROOTS") == 0 || strncmp(line, "ROOTS", 5) == 0) {
            in_values = 0;
            if (nl) { *nl = saved; line = nl + 1; } else break;
            continue;
        }
        if (strncmp(line, "CRC32", 5) == 0) {
            break;
        }

        if (in_values) {
            /* Parse: <id> <type-tag> <payload...> */
            const char *p = line;
            uint32_t id;
            char *tag;
            const char *rest;
            if (!img_parse_u32(&p, &id)) goto done_line;
            p = img_skip_ws(p);
            tag = img_parse_token(&p);
            rest = p;
            if (tag != NULL) {
                if (!img_alloc_one(&r, id, tag, rest)) {
                    free(tag);
                    rc = -1;
                    goto cleanup;
                }
                free(tag);
            }
        }

      done_line:
        if (nl) { *nl = saved; line = nl + 1; } else break;
    }

    /* Second pass: patch all references */
    for (i = 0; i < r.id_count; i++) {
        if (r.id_lines[i] != NULL) {
            if (!img_patch_one(&r, i)) {
                rc = -1;
                goto cleanup;
            }
        }
    }

    /* Splice: rebuild namespace envs and var registry */
    {
        char *l = buf;
        int saw_roots = 0;
        while (*l != '\0') {
            char *nl = strchr(l, '\n');
            char saved2 = '\0';
            if (nl) { saved2 = *nl; *nl = '\0'; }

            if (strcmp(l, "ROOTS") == 0 || strncmp(l, "ROOTS", 5) == 0) {
                saw_roots = 1;
            }
            if (saw_roots && strncmp(l, "NS ", 3) == 0) {
                const char *p = l + 3;
                char *ns_name = img_parse_token(&p);
                long long eid;
                mino_env *new_env = NULL;
                if (img_parse_ll(&p, &eid) && eid >= 0) {
                    new_env = img_resolve_env(&r, (uint32_t)eid);
                }
                if (ns_name != NULL && new_env != NULL) {
                    /* Register the env as a root and add to ns_env_table */
                    root_env_t *rr = (root_env_t *)malloc(sizeof(*rr));
                    if (rr != NULL) {
                        rr->env = new_env;
                        rr->next = S->gc.root_envs;
                        S->gc.root_envs = rr;
                    }
                    /* Add namespace entry */
                    {
                        ns_env_entry_t *ne;
                        if (S->ns_vars.ns_env_len >= S->ns_vars.ns_env_cap) {
                            S->ns_vars.ns_env_cap *= 2;
                            S->ns_vars.ns_env_table =
                                (ns_env_entry_t *)realloc(S->ns_vars.ns_env_table,
                                S->ns_vars.ns_env_cap * sizeof(ns_env_entry_t));
                        }
                        ne = &S->ns_vars.ns_env_table[S->ns_vars.ns_env_len++];
                        ne->name = ns_name; /* takes ownership */
                        ne->env = new_env;
                        ne->meta = mino_nil(S);
                    }
                } else {
                    free(ns_name);
                }
            }
            if (saw_roots && strncmp(l, "CURSOR ", 7) == 0) {
                const char *cp = l + 7;
                char *ns_name = img_parse_token(&cp);
                if (ns_name != NULL) {
                    S->ns_vars.current_ns = ns_name;
                }
            }
            if (saw_roots && strncmp(l, "VREG ", 5) == 0) {
                const char *cp = l + 5;
                char *ns_str = img_parse_token(&cp);
                char *name_str = img_parse_token(&cp);
                uint32_t vid;
                if (ns_str != NULL && name_str != NULL &&
                    img_parse_u32(&cp, &vid) && vid < r.id_count) {
                    mino_val *var = r.id_vals[vid];
                    if (var != NULL && mino_type_of(var) == MINO_VAR) {
                        const char *i_ns = intern_var_str(S, ns_str);
                        const char *i_name = intern_var_str(S, name_str);
                        /* Replace malloc'd strings with interned versions */
                        free((char *)var->as.var.ns);
                        free((char *)var->as.var.sym);
                        var->as.var.ns = i_ns;
                        var->as.var.sym = i_name;
                        var_registry_add(S, i_ns, i_name, var);
                    }
                }
                free(ns_str);
                free(name_str);
            }

            if (nl) { *nl = saved2; l = nl + 1; } else break;
        }
    }

cleanup:
    img_reader_free(&r);
    free(buf);
    return rc;
}
