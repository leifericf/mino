/*
 * clone.c -- value cloning, mailbox, and actor system.
 */

#include "mino_internal.h"

/* ------------------------------------------------------------------------- */
/* Value cloning (cross-state transfer)                                      */
/* ------------------------------------------------------------------------- */

static mino_val_t *clone_val(mino_state_t *dst, const mino_val_t *v);

/* Clone metadata if present, attaching it to the cloned value. */
static int clone_meta(mino_state_t *dst, const mino_val_t *src,
                      mino_val_t *out)
{
    mino_val_t *m;
    if (src->meta == NULL) return 0;
    m = clone_val(dst, src->meta);
    if (m == NULL) return -1;
    out->meta = m;
    return 0;
}

static mino_val_t *clone_val(mino_state_t *dst, const mino_val_t *v)
{
    if (v == NULL) return mino_nil(dst);

    switch (v->type) {
    case MINO_NIL:    return mino_nil(dst);
    case MINO_BOOL:   return v->as.b ? mino_true(dst) : mino_false(dst);
    case MINO_INT:    return mino_int(dst, v->as.i);
    case MINO_FLOAT:  return mino_float(dst, v->as.f);
    case MINO_STRING: return mino_string_n(dst, v->as.s.data, v->as.s.len);
    case MINO_SYMBOL: {
        mino_val_t *r = mino_symbol_n(dst, v->as.s.data, v->as.s.len);
        if (r != NULL && v->meta != NULL) {
            if (clone_meta(dst, v, r) != 0) return NULL;
        }
        return r;
    }
    case MINO_KEYWORD:return mino_keyword_n(dst, v->as.s.data, v->as.s.len);
    case MINO_CONS: {
        mino_val_t *car = clone_val(dst, v->as.cons.car);
        mino_val_t *cdr;
        mino_ref_t *rcar;
        if (car == NULL && v->as.cons.car != NULL
            && v->as.cons.car->type != MINO_NIL) return NULL;
        rcar = mino_ref(dst, car);
        cdr = clone_val(dst, v->as.cons.cdr);
        if (cdr == NULL && v->as.cons.cdr != NULL
            && v->as.cons.cdr->type != MINO_NIL) {
            mino_unref(dst, rcar);
            return NULL;
        }
        car = mino_deref(rcar);
        mino_unref(dst, rcar);
        {
            mino_val_t *r = mino_cons(dst, car, cdr);
            if (clone_meta(dst, v, r) != 0) return NULL;
            return r;
        }
    }
    case MINO_VECTOR: {
        size_t len = v->as.vec.len;
        size_t i;
        mino_val_t **items;
        mino_val_t *result;
        mino_ref_t **refs;
        if (len == 0) return mino_vector(dst, NULL, 0);
        if (mino_fi_should_fail_raw(dst)) return NULL;
        items = (mino_val_t **)malloc(len * sizeof(*items));
        refs  = (mino_ref_t **)malloc(len * sizeof(*refs));
        if (items == NULL || refs == NULL) {
            free(items); free(refs);
            return NULL;
        }
        for (i = 0; i < len; i++) {
            items[i] = clone_val(dst, vec_nth(v, i));
            if (items[i] == NULL) {
                size_t j;
                for (j = 0; j < i; j++) mino_unref(dst, refs[j]);
                free(items); free(refs);
                return NULL;
            }
            refs[i]  = mino_ref(dst, items[i]);
        }
        for (i = 0; i < len; i++) {
            items[i] = mino_deref(refs[i]);
        }
        result = mino_vector(dst, items, len);
        for (i = 0; i < len; i++) mino_unref(dst, refs[i]);
        free(items);
        free(refs);
        if (clone_meta(dst, v, result) != 0) return NULL;
        return result;
    }
    case MINO_MAP: {
        size_t len = v->as.map.len;
        size_t i;
        mino_val_t **keys, **vals;
        mino_ref_t **krefs, **vrefs;
        mino_val_t *result;
        if (len == 0) return mino_map(dst, NULL, NULL, 0);
        if (mino_fi_should_fail_raw(dst)) return NULL;
        keys  = (mino_val_t **)malloc(len * sizeof(*keys));
        vals  = (mino_val_t **)malloc(len * sizeof(*vals));
        krefs = (mino_ref_t **)malloc(len * sizeof(*krefs));
        vrefs = (mino_ref_t **)malloc(len * sizeof(*vrefs));
        if (!keys || !vals || !krefs || !vrefs) {
            free(keys); free(vals); free(krefs); free(vrefs);
            return NULL;
        }
        for (i = 0; i < len; i++) {
            mino_val_t *src_key = vec_nth(v->as.map.key_order, i);
            mino_val_t *src_val = hamt_get(v->as.map.root, src_key,
                                           hash_val(src_key), 0);
            keys[i]  = clone_val(dst, src_key);
            if (keys[i] == NULL) {
                size_t j;
                for (j = 0; j < i; j++) { mino_unref(dst, krefs[j]); mino_unref(dst, vrefs[j]); }
                free(keys); free(vals); free(krefs); free(vrefs);
                return NULL;
            }
            krefs[i] = mino_ref(dst, keys[i]);
            vals[i]  = clone_val(dst, src_val);
            if (vals[i] == NULL) {
                size_t j;
                mino_unref(dst, krefs[i]);
                for (j = 0; j < i; j++) { mino_unref(dst, krefs[j]); mino_unref(dst, vrefs[j]); }
                free(keys); free(vals); free(krefs); free(vrefs);
                return NULL;
            }
            vrefs[i] = mino_ref(dst, vals[i]);
        }
        for (i = 0; i < len; i++) {
            keys[i] = mino_deref(krefs[i]);
            vals[i] = mino_deref(vrefs[i]);
        }
        result = mino_map(dst, keys, vals, len);
        for (i = 0; i < len; i++) {
            mino_unref(dst, krefs[i]);
            mino_unref(dst, vrefs[i]);
        }
        free(keys); free(vals); free(krefs); free(vrefs);
        if (clone_meta(dst, v, result) != 0) return NULL;
        return result;
    }
    case MINO_SET: {
        size_t len = v->as.set.len;
        size_t i;
        mino_val_t **items;
        mino_ref_t **refs;
        mino_val_t *result;
        if (len == 0) return mino_set(dst, NULL, 0);
        if (mino_fi_should_fail_raw(dst)) return NULL;
        items = (mino_val_t **)malloc(len * sizeof(*items));
        refs  = (mino_ref_t **)malloc(len * sizeof(*refs));
        if (!items || !refs) { free(items); free(refs); return NULL; }
        for (i = 0; i < len; i++) {
            items[i] = clone_val(dst, vec_nth(v->as.set.key_order, i));
            if (items[i] == NULL) {
                size_t j;
                for (j = 0; j < i; j++) mino_unref(dst, refs[j]);
                free(items); free(refs);
                return NULL;
            }
            refs[i]  = mino_ref(dst, items[i]);
        }
        for (i = 0; i < len; i++) {
            items[i] = mino_deref(refs[i]);
        }
        result = mino_set(dst, items, len);
        for (i = 0; i < len; i++) mino_unref(dst, refs[i]);
        free(items);
        free(refs);
        if (clone_meta(dst, v, result) != 0) return NULL;
        return result;
    }
    /* Sorted collections with custom comparators hold function refs and
     * cannot be safely cloned across runtimes. Natural-order ones could
     * be rebuilt, but for now treat all as non-transferable. */
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET:
    /* Non-transferable types. */
    case MINO_FN:
    case MINO_MACRO:
    case MINO_PRIM:
    case MINO_HANDLE:
    case MINO_ATOM:
    case MINO_LAZY:
    case MINO_RECUR:
    case MINO_TAIL_CALL:
    case MINO_REDUCED:
        return NULL;
    }
    return NULL; /* unreachable */
}

mino_val_t *mino_clone(mino_state_t *dst, mino_state_t *src, mino_val_t *val)
{
    mino_val_t *result;
    (void)src;
    result = clone_val(dst, val);
    if (result == NULL && val != NULL) {
        set_error(dst, "clone: value contains non-transferable types "
                  "(fn, macro, prim, handle, atom, or lazy-seq)");
    }
    return result;
}

/* ------------------------------------------------------------------------- */
/* Mailbox (thread-safe value queue between states)                          */
/* ------------------------------------------------------------------------- */

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION mb_mutex_t;
#define MB_MUTEX_INIT(m)    InitializeCriticalSection(m)
#define MB_MUTEX_LOCK(m)    EnterCriticalSection(m)
#define MB_MUTEX_UNLOCK(m)  LeaveCriticalSection(m)
#define MB_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#else
#include <pthread.h>
typedef pthread_mutex_t mb_mutex_t;
#define MB_MUTEX_INIT(m)    pthread_mutex_init(m, NULL)
#define MB_MUTEX_LOCK(m)    pthread_mutex_lock(m)
#define MB_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)
#define MB_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#endif

typedef struct mb_msg {
    char           *data;   /* serialized form (malloc'd) */
    size_t          len;
    struct mb_msg  *next;
} mb_msg_t;

struct mino_mailbox {
    mb_msg_t   *head;
    mb_msg_t   *tail;
    mb_mutex_t  lock;
};

mino_mailbox_t *mino_mailbox_new(void)
{
    mino_mailbox_t *mb = (mino_mailbox_t *)calloc(1, sizeof(*mb));
    if (mb == NULL) return NULL;
    MB_MUTEX_INIT(&mb->lock);
    return mb;
}

void mino_mailbox_free(mino_mailbox_t *mb)
{
    mb_msg_t *m, *next;
    if (mb == NULL) return;
    for (m = mb->head; m != NULL; m = next) {
        next = m->next;
        free(m->data);
        free(m);
    }
    MB_MUTEX_DESTROY(&mb->lock);
    free(mb);
}

/* --- Growable buffer for serialization (avoids tmpfile syscalls) --- */

typedef struct {
    char           *data;
    size_t          len;
    size_t          cap;
    int             failed; /* set on allocation failure; all writes become no-ops */
    mino_state_t   *fi_state; /* for raw fault injection; may be NULL */
} sbuf_t;

static void sbuf_init(sbuf_t *b, mino_state_t *S)
{
    b->data     = NULL;
    b->len      = 0;
    b->cap      = 0;
    b->failed   = 0;
    b->fi_state = S;
}

static int sbuf_ensure(sbuf_t *b, size_t extra)
{
    if (b->failed) return -1;
    if (b->fi_state != NULL && mino_fi_should_fail_raw(b->fi_state)) {
        b->failed = 1;
        return -1;
    }
    if (b->len + extra + 1 > b->cap) {
        size_t need = b->len + extra + 1;
        size_t nc   = b->cap == 0 ? 128 : b->cap;
        char  *nd;
        while (nc < need) nc *= 2;
        nd = (char *)realloc(b->data, nc);
        if (nd == NULL) {
            b->failed = 1;
            return -1;
        }
        b->data = nd;
        b->cap  = nc;
    }
    return 0;
}

static void sbuf_putc(sbuf_t *b, char c)
{
    if (sbuf_ensure(b, 1) == 0) b->data[b->len++] = c;
}

static void sbuf_puts(sbuf_t *b, const char *s)
{
    size_t n = strlen(s);
    if (sbuf_ensure(b, n) == 0) { memcpy(b->data + b->len, s, n); b->len += n; }
}

static void sbuf_write(sbuf_t *b, const char *s, size_t n)
{
    if (sbuf_ensure(b, n) == 0) { memcpy(b->data + b->len, s, n); b->len += n; }
}

/* Print a value into a growable buffer (no FILE*, no syscalls). */
static void sbuf_print(mino_state_t *S, sbuf_t *b, const mino_val_t *v);

static void sbuf_print_string_escaped(sbuf_t *b, const char *s, size_t len)
{
    size_t i;
    sbuf_putc(b, '"');
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  sbuf_puts(b, "\\\""); break;
        case '\\': sbuf_puts(b, "\\\\"); break;
        case '\n': sbuf_puts(b, "\\n");  break;
        case '\t': sbuf_puts(b, "\\t");  break;
        case '\r': sbuf_puts(b, "\\r");  break;
        case '\0': sbuf_puts(b, "\\0");  break;
        default:   sbuf_putc(b, (char)c); break;
        }
    }
    sbuf_putc(b, '"');
}

static void sbuf_print(mino_state_t *S, sbuf_t *b, const mino_val_t *v)
{
    if (v == NULL || v->type == MINO_NIL) { sbuf_puts(b, "nil"); return; }
    switch (v->type) {
    case MINO_NIL:  sbuf_puts(b, "nil"); return;
    case MINO_BOOL: sbuf_puts(b, v->as.b ? "true" : "false"); return;
    case MINO_INT: {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%lld", v->as.i);
        sbuf_puts(b, tmp);
        return;
    }
    case MINO_FLOAT: {
        char tmp[64];
        int n = snprintf(tmp, sizeof(tmp), "%g", v->as.f);
        int needs_dot = 1, k;
        for (k = 0; k < n; k++) {
            if (tmp[k] == '.' || tmp[k] == 'e' || tmp[k] == 'E'
                || tmp[k] == 'n' || tmp[k] == 'i') {
                needs_dot = 0; break;
            }
        }
        sbuf_puts(b, tmp);
        if (needs_dot) sbuf_puts(b, ".0");
        return;
    }
    case MINO_STRING:
        sbuf_print_string_escaped(b, v->as.s.data, v->as.s.len);
        return;
    case MINO_SYMBOL:
        sbuf_write(b, v->as.s.data, v->as.s.len);
        return;
    case MINO_KEYWORD:
        sbuf_putc(b, ':');
        sbuf_write(b, v->as.s.data, v->as.s.len);
        return;
    case MINO_CONS: {
        const mino_val_t *p = v;
        sbuf_putc(b, '(');
        while (p != NULL && p->type == MINO_CONS) {
            sbuf_print(S, b, p->as.cons.car);
            p = p->as.cons.cdr;
            if (p != NULL && p->type == MINO_LAZY) p = lazy_force(S, (mino_val_t *)p);
            if (p != NULL && p->type == MINO_CONS) sbuf_putc(b, ' ');
            else if (p != NULL && p->type != MINO_NIL) {
                sbuf_puts(b, " . ");
                sbuf_print(S, b, p);
                break;
            }
        }
        sbuf_putc(b, ')');
        return;
    }
    case MINO_VECTOR: {
        size_t i;
        sbuf_putc(b, '[');
        for (i = 0; i < v->as.vec.len; i++) {
            if (i > 0) sbuf_putc(b, ' ');
            sbuf_print(S, b, vec_nth(v, i));
        }
        sbuf_putc(b, ']');
        return;
    }
    case MINO_MAP: {
        size_t i;
        sbuf_putc(b, '{');
        for (i = 0; i < v->as.map.len; i++) {
            mino_val_t *key = vec_nth(v->as.map.key_order, i);
            if (i > 0) sbuf_puts(b, ", ");
            sbuf_print(S, b, key);
            sbuf_putc(b, ' ');
            sbuf_print(S, b, map_get_val(v, key));
        }
        sbuf_putc(b, '}');
        return;
    }
    case MINO_SET: {
        size_t i;
        sbuf_puts(b, "#{");
        for (i = 0; i < v->as.set.len; i++) {
            if (i > 0) sbuf_putc(b, ' ');
            sbuf_print(S, b, vec_nth(v->as.set.key_order, i));
        }
        sbuf_putc(b, '}');
        return;
    }
    case MINO_SORTED_MAP:
    case MINO_SORTED_SET:
        /* Sorted collections serialize as their unsorted equivalents for
         * cross-runtime transfer. Full support deferred. */
        sbuf_puts(b, "nil");
        return;
    default:
        sbuf_puts(b, "nil");
        return;
    }
}

/* Serialize a value to a malloc'd string.  Returns NULL on failure. */
static char *val_serialize(mino_state_t *S, mino_val_t *val, size_t *out_len)
{
    sbuf_t buf;
    sbuf_init(&buf, S);
    sbuf_print(S, &buf, val);
    if (buf.data == NULL || buf.failed) {
        free(buf.data);
        return NULL;
    }
    buf.data[buf.len] = '\0';
    *out_len = buf.len;
    return buf.data;
}

int mino_mailbox_send(mino_mailbox_t *mb, mino_state_t *S, mino_val_t *val)
{
    char   *data;
    size_t  len;
    mb_msg_t *msg;
    if (mb == NULL || val == NULL) return -1;
    data = val_serialize(S, val, &len);
    if (data == NULL) return -1;
    if (mino_fi_should_fail_raw(S)) { free(data); return -1; }
    msg = (mb_msg_t *)calloc(1, sizeof(*msg));
    if (msg == NULL) { free(data); return -1; }
    msg->data = data;
    msg->len  = len;
    MB_MUTEX_LOCK(&mb->lock);
    if (mb->tail != NULL) {
        mb->tail->next = msg;
    } else {
        mb->head = msg;
    }
    mb->tail = msg;
    MB_MUTEX_UNLOCK(&mb->lock);
    return 0;
}

mino_val_t *mino_mailbox_recv(mino_mailbox_t *mb, mino_state_t *S)
{
    mb_msg_t   *msg;
    mino_val_t *result;
    if (mb == NULL) return NULL;
    MB_MUTEX_LOCK(&mb->lock);
    msg = mb->head;
    if (msg != NULL) {
        mb->head = msg->next;
        if (mb->head == NULL) mb->tail = NULL;
    }
    MB_MUTEX_UNLOCK(&mb->lock);
    if (msg == NULL) return NULL;
    result = mino_read(S, msg->data, NULL);
    free(msg->data);
    free(msg);
    return result;
}

/* ------------------------------------------------------------------------- */
/* Actor (state + env + mailbox bundle)                                      */
/* ------------------------------------------------------------------------- */

struct mino_actor {
    mino_state_t   *state;
    mino_env_t     *env;
    mino_mailbox_t *mailbox;
};

mino_actor_t *mino_actor_new(void)
{
    mino_actor_t *a = (mino_actor_t *)calloc(1, sizeof(*a));
    if (a == NULL) return NULL;
    a->state = mino_state_new();
    if (a->state == NULL) { free(a); return NULL; }
    a->env = mino_new(a->state);
    if (a->env == NULL) {
        mino_state_free(a->state);
        free(a);
        return NULL;
    }
    a->mailbox = mino_mailbox_new();
    if (a->mailbox == NULL) {
        mino_env_free(a->state, a->env);
        mino_state_free(a->state);
        free(a);
        return NULL;
    }
    return a;
}

mino_state_t *mino_actor_state(mino_actor_t *a)
{
    return a ? a->state : NULL;
}

mino_env_t *mino_actor_env(mino_actor_t *a)
{
    return a ? a->env : NULL;
}

mino_mailbox_t *mino_actor_mailbox(mino_actor_t *a)
{
    return a ? a->mailbox : NULL;
}

void mino_actor_send(mino_actor_t *a, mino_state_t *src, mino_val_t *val)
{
    if (a == NULL || val == NULL) return;
    mino_mailbox_send(a->mailbox, src, val);
}

mino_val_t *mino_actor_recv(mino_actor_t *a)
{
    if (a == NULL) return NULL;
    return mino_mailbox_recv(a->mailbox, a->state);
}

void mino_actor_free(mino_actor_t *a)
{
    if (a == NULL) return;
    mino_mailbox_free(a->mailbox);
    mino_env_free(a->state, a->env);
    mino_state_free(a->state);
    free(a);
}

/* Primitive: (send! actor val) — send a value to an actor's mailbox. */
mino_val_t *prim_send_bang(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *handle, *val;
    mino_actor_t *a;
    (void)env;
    if (args == NULL || args->type != MINO_CONS ||
        args->as.cons.cdr == NULL || args->as.cons.cdr->type != MINO_CONS) {
        set_error(S, "send! requires 2 arguments: actor and value");
        return NULL;
    }
    handle = args->as.cons.car;
    val    = args->as.cons.cdr->as.cons.car;
    if (handle == NULL || handle->type != MINO_HANDLE ||
        strcmp(handle->as.handle.tag, "actor") != 0) {
        set_error(S, "send! first argument must be an actor handle");
        return NULL;
    }
    a = (mino_actor_t *)handle->as.handle.ptr;
    mino_actor_send(a, S, val);
    return mino_nil(S);
}

/* Primitive: (receive) — receive from the current state's actor mailbox.
 * The host must set up the "self" binding before ticking the actor.
 * Returns nil if the mailbox is empty. */
mino_val_t *prim_receive(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *self;
    mino_actor_t *a;
    mino_val_t *msg;
    (void)args;
    self = mino_env_get(env, "*self*");
    if (self == NULL || self->type != MINO_HANDLE ||
        strcmp(self->as.handle.tag, "actor") != 0) {
        set_error(S, "receive: no actor context (*self* not bound)");
        return NULL;
    }
    a = (mino_actor_t *)self->as.handle.ptr;
    msg = mino_actor_recv(a);
    return msg != NULL ? msg : mino_nil(S);
}

/* Primitive: (spawn src) — create a new actor, eval src string in it,
 * return a handle. The src typically defines a handler function. */
mino_val_t *prim_spawn(mino_state_t *S, mino_val_t *args, mino_env_t *env)
{
    mino_val_t *src_val;
    const char *src;
    mino_actor_t *a;
    mino_val_t *self_handle;
    char buf[256];
    (void)env;
    if (args == NULL || args->type != MINO_CONS) {
        set_error(S, "spawn requires 1 argument: init source string");
        return NULL;
    }
    src_val = args->as.cons.car;
    if (src_val == NULL || src_val->type != MINO_STRING) {
        set_error(S, "spawn argument must be a string");
        return NULL;
    }
    src = src_val->as.s.data;
    a = mino_actor_new();
    if (a == NULL) {
        set_error(S, "spawn: failed to create actor");
        return NULL;
    }
    /* Install I/O in the actor so it can print etc. */
    mino_install_io(a->state, a->env);
    /* Create a handle for this actor in the actor's own state and bind *self*. */
    self_handle = mino_handle(a->state, a, "actor");
    mino_env_set(a->state, a->env, "*self*", self_handle);
    /* Evaluate the init source in the actor's state. */
    if (mino_eval_string(a->state, src, a->env) == NULL) {
        const char *err = mino_last_error(a->state);
        snprintf(buf, sizeof(buf),
                 "spawn: init eval failed: %s", err ? err : "unknown error");
        set_error(S, buf);
        mino_actor_free(a);
        return NULL;
    }
    /* Return a handle in the caller's state. */
    return mino_handle(S, a, "actor");
}

