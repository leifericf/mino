/*
 * clone.c -- value cloning, mailbox, and actor system.
 */

#include "mino_internal.h"

/* ------------------------------------------------------------------------- */
/* Value cloning (cross-state transfer)                                      */
/* ------------------------------------------------------------------------- */

static mino_val_t *clone_val(mino_state_t *dst, const mino_val_t *v)
{
    if (v == NULL) return mino_nil(dst);

    switch (v->type) {
    case MINO_NIL:    return mino_nil(dst);
    case MINO_BOOL:   return v->as.b ? mino_true(dst) : mino_false(dst);
    case MINO_INT:    return mino_int(dst, v->as.i);
    case MINO_FLOAT:  return mino_float(dst, v->as.f);
    case MINO_STRING: return mino_string_n(dst, v->as.s.data, v->as.s.len);
    case MINO_SYMBOL: return mino_symbol_n(dst, v->as.s.data, v->as.s.len);
    case MINO_KEYWORD:return mino_keyword_n(dst, v->as.s.data, v->as.s.len);
    case MINO_CONS: {
        mino_val_t *car = clone_val(dst, v->as.cons.car);
        mino_val_t *cdr;
        mino_ref_t *rcar = mino_ref(dst, car);
        cdr = clone_val(dst, v->as.cons.cdr);
        car = mino_deref(rcar);
        mino_unref(dst, rcar);
        return mino_cons(dst, car, cdr);
    }
    case MINO_VECTOR: {
        size_t len = v->as.vec.len;
        size_t i;
        mino_val_t **items;
        mino_val_t *result;
        mino_ref_t **refs;
        if (len == 0) return mino_vector(dst, NULL, 0);
        items = (mino_val_t **)malloc(len * sizeof(*items));
        refs  = (mino_ref_t **)malloc(len * sizeof(*refs));
        if (items == NULL || refs == NULL) {
            free(items); free(refs);
            return NULL;
        }
        for (i = 0; i < len; i++) {
            items[i] = clone_val(dst, vec_nth(v, i));
            refs[i]  = mino_ref(dst, items[i]);
        }
        for (i = 0; i < len; i++) {
            items[i] = mino_deref(refs[i]);
        }
        result = mino_vector(dst, items, len);
        for (i = 0; i < len; i++) mino_unref(dst, refs[i]);
        free(items);
        free(refs);
        return result;
    }
    case MINO_MAP: {
        size_t len = v->as.map.len;
        size_t i;
        mino_val_t **keys, **vals;
        mino_ref_t **krefs, **vrefs;
        mino_val_t *result;
        if (len == 0) return mino_map(dst, NULL, NULL, 0);
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
            krefs[i] = mino_ref(dst, keys[i]);
            vals[i]  = clone_val(dst, src_val);
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
        return result;
    }
    case MINO_SET: {
        size_t len = v->as.set.len;
        size_t i;
        mino_val_t **items;
        mino_ref_t **refs;
        mino_val_t *result;
        if (len == 0) return mino_set(dst, NULL, 0);
        items = (mino_val_t **)malloc(len * sizeof(*items));
        refs  = (mino_ref_t **)malloc(len * sizeof(*refs));
        if (!items || !refs) { free(items); free(refs); return NULL; }
        for (i = 0; i < len; i++) {
            items[i] = clone_val(dst, vec_nth(v->as.set.key_order, i));
            refs[i]  = mino_ref(dst, items[i]);
        }
        for (i = 0; i < len; i++) {
            items[i] = mino_deref(refs[i]);
        }
        result = mino_set(dst, items, len);
        for (i = 0; i < len; i++) mino_unref(dst, refs[i]);
        free(items);
        free(refs);
        return result;
    }
    /* Non-transferable types. */
    case MINO_FN:
    case MINO_MACRO:
    case MINO_PRIM:
    case MINO_HANDLE:
    case MINO_ATOM:
    case MINO_LAZY:
    case MINO_RECUR:
    case MINO_TAIL_CALL:
        return NULL;
    }
    return NULL; /* unreachable */
}

mino_val_t *mino_clone(mino_state_t *dst, mino_state_t *src, mino_val_t *val)
{
    mino_val_t *result;
    mino_state_t *saved = S_;
    (void)src;
    result = clone_val(dst, val);
    S_ = saved;
    if (result == NULL && val != NULL) {
        S_ = dst;
        set_error("clone: value contains non-transferable types "
                  "(fn, macro, prim, handle, atom, or lazy-seq)");
        S_ = saved;
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

/* Serialize a value to a malloc'd string.  Returns NULL on failure. */
static char *val_serialize(mino_state_t *S, mino_val_t *val, size_t *out_len)
{
    FILE *f = tmpfile();
    long  n;
    char *buf;
    if (f == NULL) return NULL;
    mino_print_to(S, f, val);
    n = ftell(f);
    if (n < 0) n = 0;
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) { fclose(f); return NULL; }
    if (n > 0) {
        size_t got = fread(buf, 1, (size_t)n, f);
        (void)got;
    }
    buf[n] = '\0';
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

int mino_mailbox_send(mino_mailbox_t *mb, mino_state_t *S, mino_val_t *val)
{
    char   *data;
    size_t  len;
    mb_msg_t *msg;
    if (mb == NULL || val == NULL) return -1;
    data = val_serialize(S, val, &len);
    if (data == NULL) return -1;
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
mino_val_t *prim_send_bang(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *handle, *val;
    mino_actor_t *a;
    (void)env;
    if (args == NULL || args->type != MINO_CONS ||
        args->as.cons.cdr == NULL || args->as.cons.cdr->type != MINO_CONS) {
        set_error("send! requires 2 arguments: actor and value");
        return NULL;
    }
    handle = args->as.cons.car;
    val    = args->as.cons.cdr->as.cons.car;
    if (handle == NULL || handle->type != MINO_HANDLE ||
        strcmp(handle->as.handle.tag, "actor") != 0) {
        set_error("send! first argument must be an actor handle");
        return NULL;
    }
    a = (mino_actor_t *)handle->as.handle.ptr;
    mino_actor_send(a, S_, val);
    return mino_nil(S_);
}

/* Primitive: (receive) — receive from the current state's actor mailbox.
 * The host must set up the "self" binding before ticking the actor.
 * Returns nil if the mailbox is empty. */
mino_val_t *prim_receive(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *self;
    mino_actor_t *a;
    mino_val_t *msg;
    (void)args;
    self = mino_env_get(env, "*self*");
    if (self == NULL || self->type != MINO_HANDLE ||
        strcmp(self->as.handle.tag, "actor") != 0) {
        set_error("receive: no actor context (*self* not bound)");
        return NULL;
    }
    a = (mino_actor_t *)self->as.handle.ptr;
    msg = mino_actor_recv(a);
    return msg != NULL ? msg : mino_nil(S_);
}

/* Primitive: (spawn src) — create a new actor, eval src string in it,
 * return a handle. The src typically defines a handler function. */
mino_val_t *prim_spawn(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *src_val;
    const char *src;
    mino_actor_t *a;
    mino_val_t *self_handle;
    char buf[256];
    (void)env;
    if (args == NULL || args->type != MINO_CONS) {
        set_error("spawn requires 1 argument: init source string");
        return NULL;
    }
    src_val = args->as.cons.car;
    if (src_val == NULL || src_val->type != MINO_STRING) {
        set_error("spawn argument must be a string");
        return NULL;
    }
    src = src_val->as.s.data;
    a = mino_actor_new();
    if (a == NULL) {
        set_error("spawn: failed to create actor");
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
        set_error(buf);
        mino_actor_free(a);
        return NULL;
    }
    /* Return a handle in the caller's state. */
    return mino_handle(S_, a, "actor");
}

