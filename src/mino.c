/*
 * mino.c -- state management, GC, environment, evaluator, and REPL.
 */

#include "mino_internal.h"

/* Default global state instance and current-state pointer. */
mino_state_t  g_state;
int           g_state_ready;
mino_state_t *S_ = &g_state;

/* state_init and mino_state_free access struct fields directly by name,
 * which collides with the accessor macros.  Work around by saving and
 * restoring S_ so we can use the macros on `st` instead. */
static void state_init(mino_state_t *st)
{
    mino_state_t *saved = S_;
    memset(st, 0, sizeof(*st));
    S_ = st;
    gc_threshold        = 1u << 20;
    gc_stress           = -1;
    nil_singleton.type  = MINO_NIL;
    true_singleton.type = MINO_BOOL;
    true_singleton.as.b = 1;
    false_singleton.type = MINO_BOOL;
    reader_line         = 1;
    S_ = saved;
}

mino_state_t *mino_state_new(void)
{
    mino_state_t *st = (mino_state_t *)calloc(1, sizeof(*st));
    if (st == NULL) {
        abort();
    }
    state_init(st);
    return st;
}

void mino_state_free(mino_state_t *st)
{
    mino_state_t *saved = S_;
    root_env_t *r;
    root_env_t *rnext;
    gc_hdr_t   *h;
    gc_hdr_t   *hnext;
    size_t      i;
    if (st == NULL) {
        return;
    }
    S_ = st;
    for (r = gc_root_envs; r != NULL; r = rnext) {
        rnext = r->next;
        free(r);
    }
    {
        mino_ref_t *ref = S_->ref_roots;
        mino_ref_t *rnxt;
        while (ref != NULL) {
            rnxt = ref->next;
            free(ref);
            ref = rnxt;
        }
    }
    for (i = 0; i < module_cache_len; i++) {
        free(module_cache[i].name);
    }
    free(module_cache);
    for (i = 0; i < meta_table_len; i++) {
        free(meta_table[i].name);
        free(meta_table[i].docstring);
    }
    free(meta_table);
    free(sym_intern.entries);
    free(kw_intern.entries);
    free(gc_ranges);
    for (h = gc_all; h != NULL; h = hnext) {
        hnext = h->next;
        if (h->type_tag == GC_T_VAL) {
            mino_val_t *v = (mino_val_t *)(h + 1);
            if (v->type == MINO_HANDLE && v->as.handle.finalizer != NULL) {
                v->as.handle.finalizer(v->as.handle.ptr, v->as.handle.tag);
            }
        }
        free(h);
    }
    S_ = saved;
    if (st != &g_state) {
        free(st);
    }
}

mino_state_t *mino_current_state(void)
{
    return S_;
}

mino_ref_t *mino_ref(mino_state_t *S, mino_val_t *val)
{
    mino_ref_t *r = (mino_ref_t *)calloc(1, sizeof(*r));
    if (r == NULL) {
        abort();
    }
    r->val  = val;
    r->prev = NULL;
    r->next = S->ref_roots;
    if (S->ref_roots != NULL) {
        S->ref_roots->prev = r;
    }
    S->ref_roots = r;
    return r;
}

mino_val_t *mino_deref(const mino_ref_t *ref)
{
    if (ref == NULL) {
        return NULL;
    }
    return ref->val;
}

void mino_unref(mino_state_t *S, mino_ref_t *ref)
{
    if (ref == NULL) {
        return;
    }
    if (ref->prev != NULL) {
        ref->prev->next = ref->next;
    } else {
        S->ref_roots = ref->next;
    }
    if (ref->next != NULL) {
        ref->next->prev = ref->prev;
    }
    free(ref);
}

/* Look up a name in the dynamic binding stack.  Returns the value if
 * found, NULL otherwise. */
mino_val_t *dyn_lookup(const char *name)
{
    dyn_frame_t *f;
    dyn_binding_t *b;
    for (f = dyn_stack; f != NULL; f = f->prev) {
        for (b = f->bindings; b != NULL; b = b->next) {
            if (strcmp(b->name, name) == 0) return b->val;
        }
    }
    return NULL;
}

meta_entry_t *meta_find(const char *name)
{
    size_t i;
    for (i = 0; i < meta_table_len; i++) {
        if (strcmp(meta_table[i].name, name) == 0) {
            return &meta_table[i];
        }
    }
    return NULL;
}

void meta_set(const char *name, const char *doc, size_t doc_len,
                     mino_val_t *source)
{
    meta_entry_t *e = meta_find(name);
    if (e != NULL) {
        free(e->docstring);
        e->docstring = NULL;
        if (doc != NULL) {
            e->docstring = (char *)malloc(doc_len + 1);
            if (e->docstring != NULL) {
                memcpy(e->docstring, doc, doc_len);
                e->docstring[doc_len] = '\0';
            }
        }
        e->source = source;
        return;
    }
    if (meta_table_len == meta_table_cap) {
        size_t new_cap = meta_table_cap == 0 ? 32 : meta_table_cap * 2;
        meta_entry_t *ne = (meta_entry_t *)realloc(
            meta_table, new_cap * sizeof(*ne));
        if (ne == NULL) { return; }
        meta_table = ne;
        meta_table_cap = new_cap;
    }
    {
        size_t nlen = strlen(name);
        meta_table[meta_table_len].name = (char *)malloc(nlen + 1);
        if (meta_table[meta_table_len].name == NULL) { return; }
        memcpy(meta_table[meta_table_len].name, name, nlen + 1);
    }
    meta_table[meta_table_len].docstring = NULL;
    if (doc != NULL) {
        meta_table[meta_table_len].docstring = (char *)malloc(doc_len + 1);
        if (meta_table[meta_table_len].docstring != NULL) {
            memcpy(meta_table[meta_table_len].docstring, doc, doc_len);
            meta_table[meta_table_len].docstring[doc_len] = '\0';
        }
    }
    meta_table[meta_table_len].source = source;
    meta_table_len++;
}

void gc_collect(void);

/* Record a stack address from a host-called entry point so the collector's
 * conservative scan covers the entire host-to-mino call chain. We keep the
 * maximum address (shallowest frame on a downward-growing stack). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
void gc_note_host_frame(void *addr)
{
    if (gc_stack_bottom == NULL
        || (char *)addr > (char *)gc_stack_bottom) {
        gc_stack_bottom = addr;
    }
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

void *gc_alloc_typed(unsigned char tag, size_t size)
{
    gc_hdr_t *h;
    if (!g_state_ready) {
        state_init(S_);
        g_state_ready = 1;
    }
    if (gc_stress == -1) {
        const char *e = getenv("MINO_GC_STRESS");
        gc_stress = (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    if (gc_depth == 0 && gc_stack_bottom != NULL
        && (gc_stress || gc_bytes_alloc - gc_bytes_live > gc_threshold)) {
        gc_collect();
    }
    h = (gc_hdr_t *)calloc(1, sizeof(*h) + size);
    if (h == NULL) {
        abort();
    }
    h->type_tag      = tag;
    h->mark          = 0;
    h->size          = size;
    h->next          = gc_all;
    gc_all           = h;
    gc_bytes_alloc  += size;
    return (void *)(h + 1);
}

mino_val_t *alloc_val(mino_type_t type)
{
    mino_val_t *v = (mino_val_t *)gc_alloc_typed(GC_T_VAL, sizeof(*v));
    v->type = type;
    return v;
}

char *dup_n(const char *s, size_t len)
{
    char *out = (char *)gc_alloc_typed(GC_T_RAW, len + 1);
    if (len > 0) {
        memcpy(out, s, len);
    }
    out[len] = '\0';
    return out;
}


/* ------------------------------------------------------------------------- */
/* Error reporting                                                           */
/* ------------------------------------------------------------------------- */

const char *mino_last_error(mino_state_t *S)
{
    S_ = S;
    return error_buf[0] ? error_buf : NULL;
}

void set_error(const char *msg)
{
    size_t n = strlen(msg);
    if (n >= sizeof(error_buf)) {
        n = sizeof(error_buf) - 1;
    }
    memcpy(error_buf, msg, n);
    error_buf[n] = '\0';
}

void clear_error(void)
{
    error_buf[0] = '\0';
}

/* Location-aware error: prepend file:line when the form has source info. */
void set_error_at(const mino_val_t *form, const char *msg)
{
    if (form != NULL && form->type == MINO_CONS
        && form->as.cons.file != NULL && form->as.cons.line > 0) {
        char buf[2048];
        snprintf(buf, sizeof(buf), "%s:%d: %s",
                 form->as.cons.file, form->as.cons.line, msg);
        set_error(buf);
    } else {
        set_error(msg);
    }
}

/* Return a short human-readable label for a value's type. */
const char *type_tag_str(const mino_val_t *v)
{
    if (v == NULL) return "nil";
    switch (v->type) {
    case MINO_NIL:     return "nil";
    case MINO_BOOL:    return "bool";
    case MINO_INT:     return "int";
    case MINO_FLOAT:   return "float";
    case MINO_STRING:  return "string";
    case MINO_SYMBOL:  return "symbol";
    case MINO_KEYWORD: return "keyword";
    case MINO_CONS:    return "list";
    case MINO_VECTOR:  return "vector";
    case MINO_MAP:     return "map";
    case MINO_SET:     return "set";
    case MINO_PRIM:    return "fn";
    case MINO_FN:      return "fn";
    case MINO_MACRO:   return "macro";
    case MINO_HANDLE:  return "handle";
    case MINO_ATOM:    return "atom";
    case MINO_LAZY:    return "lazy-seq";
    case MINO_RECUR:     return "recur";
    case MINO_TAIL_CALL: return "tail-call";
    }
    return "unknown";
}

/* ------------------------------------------------------------------------- */
/* Call stack (for stack traces on error)                                     */
/* ------------------------------------------------------------------------- */

void push_frame(const char *name, const char *file, int line)
{
    if (call_depth < MAX_CALL_DEPTH) {
        call_stack[call_depth].name = name;
        call_stack[call_depth].file = file;
        call_stack[call_depth].line = line;
        call_depth++;
    }
}

void pop_frame(void)
{
    if (call_depth > 0) {
        call_depth--;
    }
}

/* Append the current call stack to error_buf. Called once per error. */
static void append_trace(void)
{
    size_t pos;
    int    i;
    if (trace_added || call_depth == 0) {
        return;
    }
    trace_added = 1;
    pos = strlen(error_buf);
    for (i = call_depth - 1; i >= 0 && pos + 80 < sizeof(error_buf); i--) {
        pos += (size_t)snprintf(
            error_buf + pos, sizeof(error_buf) - pos, "\n  in %s",
            call_stack[i].name ? call_stack[i].name : "<fn>");
        if (call_stack[i].file != NULL && pos + 40 < sizeof(error_buf)) {
            pos += (size_t)snprintf(
                error_buf + pos, sizeof(error_buf) - pos, " (%s:%d)",
                call_stack[i].file, call_stack[i].line);
        }
    }
}


/* ------------------------------------------------------------------------- */
/* Environment                                                               */
/* ------------------------------------------------------------------------- */

/*
 * Environment: a chain of frames. Each frame is a flat (name, value) array
 * with linear search. The root frame has parent == NULL and holds globals;
 * child frames are created by let, fn application, and loop. Lookup walks
 * parents; binding always writes to the current frame so that let and fn
 * parameters shadow rather than mutate outer bindings.
 *
 * Envs and their binding arrays are GC-managed. `mino_env_new` registers
 * the returned env as a persistent root so the whole chain (and everything
 * reachable from its bindings) survives collection; `mino_env_free` unroots
 * it, letting the next sweep reclaim the frame and any closures that were
 * only reachable through it.
 */

/* env_binding_t and mino_env defined in mino_internal.h */

static mino_env_t *env_alloc(mino_env_t *parent)
{
    mino_env_t *env = (mino_env_t *)gc_alloc_typed(GC_T_ENV, sizeof(*env));
    env->parent = parent;
    return env;
}

mino_env_t *mino_env_new(mino_state_t *S)
{
    volatile char probe = 0;
    S_ = S;
    mino_env_t   *env;
    root_env_t   *r;
    /* Record the host's stack frame: this is typically the earliest point
     * the host calls into mino, so it fixes a generous stack bottom before
     * any allocator runs. */
    gc_note_host_frame((void *)&probe);
    (void)probe;
    env = env_alloc(NULL);
    r   = (root_env_t *)malloc(sizeof(*r));
    if (r == NULL) {
        abort();
    }
    r->env       = env;
    r->next      = gc_root_envs;
    gc_root_envs = r;
    return env;
}

mino_env_t *env_child(mino_env_t *parent)
{
    return env_alloc(parent);
}

void mino_env_free(mino_state_t *S, mino_env_t *env)
{
    S_ = S;
    /* Unroot the env. Its memory, along with any closures and bindings
     * reachable only through it, is reclaimed at the next collection. */
    root_env_t **pp = &gc_root_envs;
    if (env == NULL) {
        return;
    }
    while (*pp != NULL) {
        if ((*pp)->env == env) {
            root_env_t *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

env_binding_t *env_find_here(mino_env_t *env, const char *name)
{
    size_t i;
    for (i = 0; i < env->len; i++) {
        if (strcmp(env->bindings[i].name, name) == 0) {
            return &env->bindings[i];
        }
    }
    return NULL;
}

void env_bind(mino_env_t *env, const char *name, mino_val_t *val)
{
    env_binding_t *b = env_find_here(env, name);
    if (b != NULL) {
        b->val = val;
        return;
    }
    if (env->len == env->cap) {
        size_t         new_cap = env->cap == 0 ? 16 : env->cap * 2;
        env_binding_t *nb      = (env_binding_t *)gc_alloc_typed(
            GC_T_RAW, new_cap * sizeof(*nb));
        if (env->bindings != NULL && env->len > 0) {
            memcpy(nb, env->bindings, env->len * sizeof(*nb));
        }
        env->bindings = nb;
        env->cap      = new_cap;
    }
    env->bindings[env->len].name = dup_n(name, strlen(name));
    env->bindings[env->len].val  = val;
    env->len++;
}

mino_env_t *env_root(mino_env_t *env)
{
    while (env->parent != NULL) {
        env = env->parent;
    }
    return env;
}

mino_env_t *mino_env_clone(mino_state_t *S, mino_env_t *env)
{
    S_ = S;
    if (env == NULL) return NULL;

    /* Allocate a new root env and copy all bindings from the source. */
    mino_env_t *clone = mino_env_new(S);
    size_t i;
    for (i = 0; i < env->len; i++) {
        env_bind(clone, env->bindings[i].name, env->bindings[i].val);
    }
    return clone;
}

void mino_env_set(mino_state_t *S, mino_env_t *env, const char *name, mino_val_t *val)
{
    S_ = S;
    env_bind(env, name, val);
}

mino_val_t *mino_env_get(mino_env_t *env, const char *name)
{
    while (env != NULL) {
        env_binding_t *b = env_find_here(env, name);
        if (b != NULL) {
            return b->val;
        }
        env = env->parent;
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Garbage collector: mark-and-sweep                                         */
/* ------------------------------------------------------------------------- */
/*
 * Marking is driven by three root sources: the registry of user-held envs
 * (`gc_root_envs`), the symbol and keyword intern tables, and a conservative
 * scan of the C stack between `gc_stack_bottom` (saved on the first
 * mino_eval call) and the collector's own frame.
 *
 * The marker traces each object according to its type tag, following every
 * owned pointer it knows about. Conservative stack scan treats every aligned
 * machine word as a candidate pointer and consults a sorted range index
 * built from the allocation registry to check, in O(log n), whether the
 * word falls inside any managed payload. Interior pointers (into the middle
 * of a payload) count; this keeps the scan correct under normal compiler
 * optimizations that may retain base+offset forms in registers.
 *
 * Sweep walks the registry in place, freeing every allocation whose mark
 * bit is clear and resetting the mark bit on survivors. The live-byte tally
 * becomes the next cycle's baseline; the threshold grows to 2× live so the
 * collector's amortized cost stays bounded under steady-state programs.
 */

static int gc_range_cmp(const void *a, const void *b)
{
    const gc_range_t *ra = (const gc_range_t *)a;
    const gc_range_t *rb = (const gc_range_t *)b;
    if (ra->start < rb->start) return -1;
    if (ra->start > rb->start) return 1;
    return 0;
}

static void gc_build_range_index(void)
{
    gc_hdr_t *h;
    size_t    n = 0;
    for (h = gc_all; h != NULL; h = h->next) {
        n++;
    }
    if (n > gc_ranges_cap) {
        size_t      new_cap = n * 2 + 16;
        gc_range_t *nr      = (gc_range_t *)realloc(
            gc_ranges, new_cap * sizeof(*nr));
        if (nr == NULL) {
            abort();
        }
        gc_ranges     = nr;
        gc_ranges_cap = new_cap;
    }
    gc_ranges_len = 0;
    for (h = gc_all; h != NULL; h = h->next) {
        gc_ranges[gc_ranges_len].start = (uintptr_t)(h + 1);
        gc_ranges[gc_ranges_len].end   = (uintptr_t)(h + 1) + h->size;
        gc_ranges[gc_ranges_len].h     = h;
        gc_ranges_len++;
    }
    qsort(gc_ranges, gc_ranges_len, sizeof(*gc_ranges), gc_range_cmp);
}

static gc_hdr_t *gc_find_header_for_ptr(const void *p)
{
    uintptr_t u  = (uintptr_t)p;
    size_t    lo = 0;
    size_t    hi = gc_ranges_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (u < gc_ranges[mid].start) {
            hi = mid;
        } else if (u >= gc_ranges[mid].end) {
            lo = mid + 1;
        } else {
            return gc_ranges[mid].h;
        }
    }
    return NULL;
}

static void gc_mark_header(gc_hdr_t *h);

void gc_mark_interior(const void *p)
{
    gc_hdr_t *h;
    if (p == NULL) {
        return;
    }
    h = gc_find_header_for_ptr(p);
    if (h != NULL) {
        gc_mark_header(h);
    }
}

static void gc_mark_val(mino_val_t *v)
{
    if (v == NULL) {
        return;
    }
    switch (v->type) {
    case MINO_STRING:
    case MINO_SYMBOL:
    case MINO_KEYWORD:
        gc_mark_interior(v->as.s.data);
        break;
    case MINO_CONS:
        gc_mark_interior(v->as.cons.car);
        gc_mark_interior(v->as.cons.cdr);
        break;
    case MINO_VECTOR:
        gc_mark_interior(v->as.vec.root);
        gc_mark_interior(v->as.vec.tail);
        break;
    case MINO_MAP:
        gc_mark_interior(v->as.map.root);
        gc_mark_interior(v->as.map.key_order);
        break;
    case MINO_SET:
        gc_mark_interior(v->as.set.root);
        gc_mark_interior(v->as.set.key_order);
        break;
    case MINO_FN:
    case MINO_MACRO:
        gc_mark_interior(v->as.fn.params);
        gc_mark_interior(v->as.fn.body);
        gc_mark_interior(v->as.fn.env);
        break;
    case MINO_ATOM:
        gc_mark_interior(v->as.atom.val);
        break;
    case MINO_LAZY:
        if (v->as.lazy.realized) {
            gc_mark_interior(v->as.lazy.cached);
        } else {
            gc_mark_interior(v->as.lazy.body);
            gc_mark_interior(v->as.lazy.env);
        }
        break;
    case MINO_RECUR:
        gc_mark_interior(v->as.recur.args);
        break;
    case MINO_TAIL_CALL:
        gc_mark_interior(v->as.tail_call.fn);
        gc_mark_interior(v->as.tail_call.args);
        break;
    default:
        /* NIL, BOOL, INT, FLOAT, PRIM, HANDLE: no owned children. prim.name
         * and handle.tag are static/host-owned C strings. */
        break;
    }
}

static void gc_mark_env(mino_env_t *env)
{
    size_t i;
    if (env == NULL) {
        return;
    }
    gc_mark_interior(env->parent);
    if (env->bindings != NULL) {
        gc_mark_interior(env->bindings);
        for (i = 0; i < env->len; i++) {
            gc_mark_interior(env->bindings[i].name);
            gc_mark_interior(env->bindings[i].val);
        }
    }
}

static void gc_mark_vec_node(mino_vec_node_t *n)
{
    unsigned i;
    if (n == NULL) {
        return;
    }
    /* Leaf slots hold mino_val_t*; branch slots hold mino_vec_node_t*.
     * gc_mark_interior dispatches on the header's type tag either way. */
    for (i = 0; i < n->count; i++) {
        gc_mark_interior(n->slots[i]);
    }
}

static void gc_mark_hamt_node(mino_hamt_node_t *n)
{
    unsigned count;
    unsigned i;
    if (n == NULL) {
        return;
    }
    gc_mark_interior(n->slots);
    count = (n->collision_count > 0) ? n->collision_count
                                     : popcount32(n->bitmap);
    if (n->slots != NULL) {
        for (i = 0; i < count; i++) {
            gc_mark_interior(n->slots[i]);
        }
    }
}

static void gc_mark_hamt_entry(hamt_entry_t *e)
{
    if (e == NULL) {
        return;
    }
    gc_mark_interior(e->key);
    gc_mark_interior(e->val);
}

static void gc_mark_ptr_array(void **arr, size_t bytes)
{
    size_t n = bytes / sizeof(*arr);
    size_t i;
    if (arr == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        gc_mark_interior(arr[i]);
    }
}

static void gc_mark_header(gc_hdr_t *h)
{
    if (h == NULL || h->mark) {
        return;
    }
    h->mark = 1;
    switch (h->type_tag) {
    case GC_T_VAL:
        gc_mark_val((mino_val_t *)(h + 1));
        break;
    case GC_T_ENV:
        gc_mark_env((mino_env_t *)(h + 1));
        break;
    case GC_T_VEC_NODE:
        gc_mark_vec_node((mino_vec_node_t *)(h + 1));
        break;
    case GC_T_HAMT_NODE:
        gc_mark_hamt_node((mino_hamt_node_t *)(h + 1));
        break;
    case GC_T_HAMT_ENTRY:
        gc_mark_hamt_entry((hamt_entry_t *)(h + 1));
        break;
    case GC_T_VALARR:
    case GC_T_PTRARR:
        gc_mark_ptr_array((void **)(h + 1), h->size);
        break;
    case GC_T_RAW:
    default:
        /* Leaf allocation: no children. */
        break;
    }
}

static void gc_scan_stack(void)
{
    volatile char probe = 0;
    char         *lo;
    char         *hi;
    char         *cur;
    if (gc_stack_bottom == NULL) {
        return;
    }
    if ((char *)&probe < (char *)gc_stack_bottom) {
        lo = (char *)&probe;
        hi = (char *)gc_stack_bottom;
    } else {
        lo = (char *)gc_stack_bottom;
        hi = (char *)&probe;
    }
    while (((uintptr_t)lo % sizeof(void *)) != 0 && lo < hi) {
        lo++;
    }
    for (cur = lo; cur + sizeof(void *) <= hi; cur += sizeof(void *)) {
        void *word;
        memcpy(&word, cur, sizeof(word));
        gc_mark_interior(word);
    }
    (void)probe;
}

static void gc_mark_intern_table(const intern_table_t *tbl)
{
    size_t i;
    for (i = 0; i < tbl->len; i++) {
        gc_mark_interior(tbl->entries[i]);
    }
}

static void gc_mark_roots(void)
{
    root_env_t *r;
    int i;
    for (r = gc_root_envs; r != NULL; r = r->next) {
        gc_mark_interior(r->env);
    }
    gc_mark_intern_table(&sym_intern);
    gc_mark_intern_table(&kw_intern);
    /* Pin try/catch exception values and module cache results. */
    for (i = 0; i < try_depth; i++) {
        gc_mark_interior(try_stack[i].exception);
    }
    {
        size_t mi;
        for (mi = 0; mi < module_cache_len; mi++) {
            gc_mark_interior(module_cache[mi].value);
        }
    }
    /* Pin metadata source forms. */
    {
        size_t mi;
        for (mi = 0; mi < meta_table_len; mi++) {
            gc_mark_interior(meta_table[mi].source);
        }
    }
    /* Pin host-retained refs. */
    {
        mino_ref_t *ref;
        for (ref = S_->ref_roots; ref != NULL; ref = ref->next) {
            gc_mark_interior(ref->val);
        }
    }
    /* Pin dynamic binding values. */
    {
        dyn_frame_t *f;
        dyn_binding_t *b;
        for (f = dyn_stack; f != NULL; f = f->prev) {
            for (b = f->bindings; b != NULL; b = b->next) {
                gc_mark_interior(b->val);
            }
        }
    }
    /* Pin sort comparator if active. */
    gc_mark_interior(sort_comp_fn);
    /* Pin values on the GC save stack. */
    {
        int si;
        for (si = 0; si < gc_save_len; si++) {
            gc_mark_interior(gc_save[si]);
        }
    }
}

static void gc_sweep(void)
{
    gc_hdr_t **pp   = &gc_all;
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
            free(h);
        }
    }
    gc_bytes_live  = live;
    gc_bytes_alloc = live;
    /* Next cycle triggers after another threshold's worth of growth above
     * the live set; threshold grows to keep collection amortized. */
    if (live * 2 > gc_threshold) {
        gc_threshold = live * 2;
    }
}

void gc_collect(void)
{
    jmp_buf jb;
    if (gc_depth > 0) {
        return;
    }
    gc_depth++;
    /* setjmp spills callee-saved registers into jb, which lives in this
     * stack frame. gc_scan_stack scans from a deeper frame up through ours,
     * so any pointer that was register-resident at entry is visible. */
    if (setjmp(jb) != 0) {
        /* We never longjmp; a nonzero return indicates a jump from
         * somewhere unexpected. Surface the bug. */
        abort();
    }
    (void)jb;
    gc_build_range_index();
    gc_mark_roots();
    gc_scan_stack();
    gc_sweep();
    gc_depth--;
}


/* ------------------------------------------------------------------------- */
/* Evaluator                                                                 */
/* ------------------------------------------------------------------------- */

int sym_eq(const mino_val_t *v, const char *s)
{
    size_t n;
    if (v == NULL || v->type != MINO_SYMBOL) {
        return 0;
    }
    n = strlen(s);
    return v->as.s.len == n && memcmp(v->as.s.data, s, n) == 0;
}

/*
 * macroexpand1: if `form` is a call whose head resolves to a macro in env,
 * expand it once and return the new form. If not a macro call, return the
 * input unchanged and set *expanded = 0.
 */
mino_val_t *macroexpand1(mino_val_t *form, mino_env_t *env,
                                 int *expanded)
{
    char        buf[256];
    size_t      n;
    mino_val_t *head;
    mino_val_t *mac;
    *expanded = 0;
    if (!mino_is_cons(form)) {
        return form;
    }
    head = form->as.cons.car;
    if (head == NULL || head->type != MINO_SYMBOL) {
        return form;
    }
    n = head->as.s.len;
    if (n >= sizeof(buf)) {
        return form;
    }
    memcpy(buf, head->as.s.data, n);
    buf[n] = '\0';
    mac = mino_env_get(env, buf);
    if (mac == NULL || mac->type != MINO_MACRO) {
        return form;
    }
    *expanded = 1;
    return apply_callable(mac, form->as.cons.cdr, env);
}

/* Expand repeatedly until `form` is no longer a macro call at the top. */
mino_val_t *macroexpand_all(mino_val_t *form, mino_env_t *env)
{
    for (;;) {
        int         expanded = 0;
        mino_val_t *next     = macroexpand1(form, env, &expanded);
        if (next == NULL) {
            return NULL;
        }
        if (!expanded) {
            return form;
        }
        form = next;
    }
}

/*
 * quasiquote_expand: walk `form` as a template. Sublists, subvectors, and
 * submaps are recursed into; (unquote x) evaluates x and uses its value;
 * (unquote-splicing x) evaluates x (expected to yield a list) and splices
 * its elements into the enclosing list.
 */
static mino_val_t *quasiquote_expand(mino_val_t *form, mino_env_t *env)
{
    if (form != NULL && form->type == MINO_VECTOR) {
        size_t       nn  = form->as.vec.len;
        mino_val_t **tmp;
        size_t       i;
        if (nn == 0) { return form; }
        /* GC_T_VALARR: scratch buffer whose slots the collector traces as
         * mino_val_t*, so partial fills survive allocation mid-loop. */
        tmp = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, nn * sizeof(*tmp));
        for (i = 0; i < nn; i++) {
            mino_val_t *e = quasiquote_expand(vec_nth(form, i), env);
            if (e == NULL) { return NULL; }
            tmp[i] = e;
        }
        return mino_vector(S_, tmp, nn);
    }
    if (form != NULL && form->type == MINO_MAP) {
        size_t        nn = form->as.map.len;
        mino_val_t  **ks;
        mino_val_t  **vs;
        size_t        i;
        if (nn == 0) { return form; }
        ks = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, nn * sizeof(*ks));
        vs = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, nn * sizeof(*vs));
        for (i = 0; i < nn; i++) {
            mino_val_t *key = vec_nth(form->as.map.key_order, i);
            mino_val_t *val = map_get_val(form, key);
            mino_val_t *kk  = quasiquote_expand(key, env);
            mino_val_t *vv;
            if (kk == NULL) { return NULL; }
            vv = quasiquote_expand(val, env);
            if (vv == NULL) { return NULL; }
            ks[i] = kk; vs[i] = vv;
        }
        return mino_map(S_, ks, vs, nn);
    }
    if (!mino_is_cons(form)) {
        return form;
    }
    {
        mino_val_t *head = form->as.cons.car;
        if (sym_eq(head, "unquote")) {
            mino_val_t *arg = form->as.cons.cdr;
            if (!mino_is_cons(arg)) {
                set_error("unquote requires one argument");
                return NULL;
            }
            return eval_value(arg->as.cons.car, env);
        }
        if (sym_eq(head, "unquote-splicing")) {
            set_error("unquote-splicing must appear inside a list");
            return NULL;
        }
    }
    {
        mino_val_t *out  = mino_nil(S_);
        mino_val_t *tail = NULL;
        mino_val_t *p    = form;
        while (mino_is_cons(p)) {
            mino_val_t *elem = p->as.cons.car;
            if (mino_is_cons(elem)
                && sym_eq(elem->as.cons.car, "unquote-splicing")) {
                mino_val_t *arg = elem->as.cons.cdr;
                mino_val_t *spliced;
                mino_val_t *sp;
                if (!mino_is_cons(arg)) {
                    set_error("unquote-splicing requires one argument");
                    return NULL;
                }
                spliced = eval_value(arg->as.cons.car, env);
                if (spliced == NULL) { return NULL; }
                sp = spliced;
                while (mino_is_cons(sp)) {
                    mino_val_t *cell = mino_cons(S_, sp->as.cons.car, mino_nil(S_));
                    if (tail == NULL) { out = cell; } else { tail->as.cons.cdr = cell; }
                    tail = cell;
                    sp = sp->as.cons.cdr;
                }
            } else {
                mino_val_t *expanded = quasiquote_expand(elem, env);
                mino_val_t *cell;
                if (expanded == NULL) { return NULL; }
                cell = mino_cons(S_, expanded, mino_nil(S_));
                if (tail == NULL) { out = cell; } else { tail->as.cons.cdr = cell; }
                tail = cell;
            }
            p = p->as.cons.cdr;
        }
        return out;
    }
}

/*
 * Evaluate `form` for its value. Any MINO_RECUR escaping here is a
 * non-tail recur and is rejected. Use plain eval() in positions where
 * a recur is legitimately in tail position (if branches, implicit-do
 * trailing expression, fn/loop body through the trampoline).
 */
mino_val_t *eval_value(mino_val_t *form, mino_env_t *env)
{
    mino_val_t *v = eval(form, env);
    if (v == NULL) {
        return NULL;
    }
    if (v->type == MINO_RECUR) {
        set_error("recur must be in tail position");
        return NULL;
    }
    if (v->type == MINO_TAIL_CALL) {
        set_error("tail call in non-tail position");
        return NULL;
    }
    return v;
}

mino_val_t *eval_implicit_do_impl(mino_val_t *body, mino_env_t *env,
                                         int tail)
{
    if (!mino_is_cons(body)) {
        return mino_nil(S_);
    }
    for (;;) {
        mino_val_t *rest = body->as.cons.cdr;
        if (!mino_is_cons(rest)) {
            /* Last expression: tail position, propagate recur/tail-call. */
            return eval_impl(body->as.cons.car, env, tail);
        }
        if (eval_value(body->as.cons.car, env) == NULL) {
            return NULL;
        }
        body = rest;
    }
}

mino_val_t *eval_implicit_do(mino_val_t *body, mino_env_t *env)
{
    return eval_implicit_do_impl(body, env, 0);
}

/*
 * Force a lazy sequence: evaluate the body in the captured environment,
 * cache the result, and release the thunk for GC. Iteratively unwraps
 * nested lazy seqs to avoid stack overflow.
 */
mino_val_t *lazy_force(mino_val_t *v)
{
    if (v->as.lazy.realized) {
        return v->as.lazy.cached;
    }
    {
        mino_val_t *result = eval_implicit_do(v->as.lazy.body, v->as.lazy.env);
        if (result == NULL) return NULL;
        /* Iteratively unwrap nested lazy seqs. */
        while (result != NULL && result->type == MINO_LAZY) {
            if (result->as.lazy.realized) {
                result = result->as.lazy.cached;
            } else {
                mino_val_t *inner = eval_implicit_do(
                    result->as.lazy.body, result->as.lazy.env);
                result->as.lazy.cached  = inner;
                result->as.lazy.realized = 1;
                result->as.lazy.body    = NULL;
                result->as.lazy.env     = NULL;
                result = inner;
                if (result == NULL) return NULL;
            }
        }
        v->as.lazy.cached   = result;
        v->as.lazy.realized = 1;
        v->as.lazy.body     = NULL;
        v->as.lazy.env      = NULL;
        return result;
    }
}

mino_val_t *eval_args(mino_val_t *args, mino_env_t *env)
{
    mino_val_t *head = mino_nil(S_);
    mino_val_t *tail = NULL;
    while (mino_is_cons(args)) {
        mino_val_t *v = eval_value(args->as.cons.car, env);
        mino_val_t *cell;
        if (v == NULL) {
            return NULL;
        }
        gc_pin(v);
        cell = mino_cons(S_, v, mino_nil(S_));
        gc_unpin(1);
        if (tail == NULL) {
            head = cell;
        } else {
            tail->as.cons.cdr = cell;
        }
        tail = cell;
        args = args->as.cons.cdr;
    }
    return head;
}

/*
 * Bind a list of parameter symbols to a list of values in `env`.
 * Returns 1 on success, 0 on arity mismatch or over-long name (with error set).
 */
static int bind_params(mino_env_t *env, mino_val_t *params, mino_val_t *args,
                       const char *ctx)
{
    while (mino_is_cons(params)) {
        mino_val_t *name = params->as.cons.car;
        char        buf[256];
        size_t      n;
        /* `&` marks a rest-parameter: the next symbol binds to the remainder
         * of args as a list (possibly empty). */
        if (sym_eq(name, "&")) {
            mino_val_t *rest_param;
            params = params->as.cons.cdr;
            if (!mino_is_cons(params)
                || params->as.cons.car == NULL
                || params->as.cons.car->type != MINO_SYMBOL) {
                set_error("& must be followed by a single parameter name");
                return 0;
            }
            rest_param = params->as.cons.car;
            if (mino_is_cons(params->as.cons.cdr)) {
                set_error("& parameter must be last");
                return 0;
            }
            n = rest_param->as.s.len;
            if (n >= sizeof(buf)) {
                set_error("parameter name too long");
                return 0;
            }
            memcpy(buf, rest_param->as.s.data, n);
            buf[n] = '\0';
            env_bind(env, buf, args);  /* args is the remainder (may be nil) */
            return 1;
        }
        if (!mino_is_cons(args)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "%s arity mismatch", ctx);
            set_error(msg);
            return 0;
        }
        n = name->as.s.len;
        if (n >= sizeof(buf)) {
            set_error("parameter name too long");
            return 0;
        }
        memcpy(buf, name->as.s.data, n);
        buf[n] = '\0';
        env_bind(env, buf, args->as.cons.car);
        params = params->as.cons.cdr;
        args   = args->as.cons.cdr;
    }
    if (mino_is_cons(args)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s arity mismatch", ctx);
        set_error(msg);
        return 0;
    }
    return 1;
}

mino_val_t *eval_impl(mino_val_t *form, mino_env_t *env, int tail)
{
    if (limit_exceeded) {
        return NULL;
    }
    if (interrupted) {
        limit_exceeded = 1;
        set_error("interrupted");
        return NULL;
    }
    if (limit_steps > 0 && ++eval_steps > limit_steps) {
        limit_exceeded = 1;
        set_error("step limit exceeded");
        return NULL;
    }
    if (limit_heap > 0 && gc_bytes_alloc > limit_heap) {
        limit_exceeded = 1;
        set_error("heap limit exceeded");
        return NULL;
    }
    if (form == NULL) {
        return mino_nil(S_);
    }
    switch (form->type) {
    case MINO_NIL:
    case MINO_BOOL:
    case MINO_INT:
    case MINO_FLOAT:
    case MINO_STRING:
    case MINO_KEYWORD:
    case MINO_PRIM:
    case MINO_FN:
    case MINO_MACRO:
    case MINO_HANDLE:
    case MINO_ATOM:
    case MINO_LAZY:
    case MINO_RECUR:
    case MINO_TAIL_CALL:
        return form;
    case MINO_SYMBOL: {
        char buf[256];
        size_t n = form->as.s.len;
        mino_val_t *v;
        if (n >= sizeof(buf)) {
            set_error_at(eval_current_form, "symbol name too long");
            return NULL;
        }
        memcpy(buf, form->as.s.data, n);
        buf[n] = '\0';
        v = dyn_lookup(buf);
        if (v == NULL) v = mino_env_get(env, buf);
        if (v == NULL) {
            char msg[300];
            snprintf(msg, sizeof(msg), "unbound symbol: %s", buf);
            set_error_at(eval_current_form, msg);
            return NULL;
        }
        return v;
    }
    case MINO_VECTOR: {
        /* Vector literals evaluate each element in order, producing a new
         * vector whose shape matches the source. */
        size_t i;
        size_t n = form->as.vec.len;
        mino_val_t **tmp;
        if (n == 0) {
            return form;
        }
        tmp = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n * sizeof(*tmp));
        for (i = 0; i < n; i++) {
            mino_val_t *ev = eval_value(vec_nth(form, i), env);
            if (ev == NULL) {
                return NULL;
            }
            tmp[i] = ev;
        }
        return mino_vector(S_, tmp, n);
    }
    case MINO_MAP: {
        /* Map literals evaluate keys and values in read order; the
         * constructor handles duplicate-key resolution. */
        size_t i;
        size_t n = form->as.map.len;
        mino_val_t **ks;
        mino_val_t **vs;
        if (n == 0) {
            return form;
        }
        ks = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n * sizeof(*ks));
        vs = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n * sizeof(*vs));
        for (i = 0; i < n; i++) {
            mino_val_t *form_key = vec_nth(form->as.map.key_order, i);
            mino_val_t *form_val = map_get_val(form, form_key);
            mino_val_t *k = eval_value(form_key, env);
            mino_val_t *v;
            if (k == NULL) { return NULL; }
            v = eval_value(form_val, env);
            if (v == NULL) { return NULL; }
            ks[i] = k;
            vs[i] = v;
        }
        return mino_map(S_, ks, vs, n);
    }
    case MINO_SET: {
        /* Set literals evaluate each element in order. */
        size_t i;
        size_t n = form->as.set.len;
        mino_val_t **tmp;
        if (n == 0) {
            return form;
        }
        tmp = (mino_val_t **)gc_alloc_typed(GC_T_VALARR, n * sizeof(*tmp));
        for (i = 0; i < n; i++) {
            mino_val_t *ev = eval_value(vec_nth(form->as.set.key_order, i), env);
            if (ev == NULL) {
                return NULL;
            }
            tmp[i] = ev;
        }
        return mino_set(S_, tmp, n);
    }
    case MINO_CONS: {
        mino_val_t *head = form->as.cons.car;
        mino_val_t *args = form->as.cons.cdr;
        eval_current_form = form;

        /* Special forms. */
        if (sym_eq(head, "quote")) {
            if (!mino_is_cons(args)) {
                set_error_at(form, "quote requires one argument");
                return NULL;
            }
            return args->as.cons.car;
        }
        if (sym_eq(head, "quasiquote")) {
            if (!mino_is_cons(args)) {
                set_error_at(form, "quasiquote requires one argument");
                return NULL;
            }
            return quasiquote_expand(args->as.cons.car, env);
        }
        if (sym_eq(head, "unquote")
            || sym_eq(head, "unquote-splicing")) {
            set_error_at(form, "unquote outside of quasiquote");
            return NULL;
        }
        if (sym_eq(head, "defmacro")) {
            mino_val_t *name_form;
            mino_val_t *params;
            mino_val_t *body;
            mino_val_t *mac;
            mino_val_t *p;
            const char *doc     = NULL;
            size_t      doc_len = 0;
            char        buf[256];
            size_t      n;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_error_at(form, "defmacro requires a name, parameters, and body");
                return NULL;
            }
            name_form = args->as.cons.car;
            if (name_form == NULL || name_form->type != MINO_SYMBOL) {
                set_error_at(form, "defmacro name must be a symbol");
                return NULL;
            }
            /* Optional docstring: (defmacro name "doc" (params) body) */
            {
                mino_val_t *after_name = args->as.cons.cdr;
                mino_val_t *maybe_doc  = after_name->as.cons.car;
                if (maybe_doc != NULL && maybe_doc->type == MINO_STRING
                    && mino_is_cons(after_name->as.cons.cdr)) {
                    doc     = maybe_doc->as.s.data;
                    doc_len = maybe_doc->as.s.len;
                    params  = after_name->as.cons.cdr->as.cons.car;
                    body    = after_name->as.cons.cdr->as.cons.cdr;
                } else {
                    params = after_name->as.cons.car;
                    body   = after_name->as.cons.cdr;
                }
            }
            if (!mino_is_cons(params) && !mino_is_nil(params)) {
                set_error_at(form, "defmacro parameter list must be a list");
                return NULL;
            }
            for (p = params; mino_is_cons(p); p = p->as.cons.cdr) {
                mino_val_t *pn = p->as.cons.car;
                if (pn == NULL || pn->type != MINO_SYMBOL) {
                    set_error_at(form, "defmacro parameter must be a symbol");
                    return NULL;
                }
            }
            mac = alloc_val(MINO_MACRO);
            mac->as.fn.params = params;
            mac->as.fn.body   = body;
            mac->as.fn.env    = env;
            n = name_form->as.s.len;
            if (n >= sizeof(buf)) {
                set_error_at(form, "defmacro name too long");
                return NULL;
            }
            memcpy(buf, name_form->as.s.data, n);
            buf[n] = '\0';
            gc_pin(mac);
            env_bind(env_root(env), buf, mac);
            gc_unpin(1);
            meta_set(buf, doc, doc_len, form);
            return mac;
        }
        if (sym_eq(head, "def")) {
            mino_val_t *name_form;
            mino_val_t *value_form;
            mino_val_t *value;
            const char *doc     = NULL;
            size_t      doc_len = 0;
            char buf[256];
            size_t n;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_error_at(form, "def requires a name and a value");
                return NULL;
            }
            name_form  = args->as.cons.car;
            if (name_form == NULL || name_form->type != MINO_SYMBOL) {
                set_error_at(form, "def name must be a symbol");
                return NULL;
            }
            /* Optional docstring: (def name "doc" value) */
            if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
                mino_val_t *maybe_doc = args->as.cons.cdr->as.cons.car;
                if (maybe_doc != NULL && maybe_doc->type == MINO_STRING) {
                    doc       = maybe_doc->as.s.data;
                    doc_len   = maybe_doc->as.s.len;
                    value_form = args->as.cons.cdr->as.cons.cdr->as.cons.car;
                } else {
                    value_form = args->as.cons.cdr->as.cons.car;
                }
            } else {
                value_form = args->as.cons.cdr->as.cons.car;
            }
            n = name_form->as.s.len;
            if (n >= sizeof(buf)) {
                set_error_at(form, "def name too long");
                return NULL;
            }
            memcpy(buf, name_form->as.s.data, n);
            buf[n] = '\0';
            value = eval_value(value_form, env);
            if (value == NULL) {
                return NULL;
            }
            gc_pin(value);
            env_bind(env_root(env), buf, value);
            gc_unpin(1);
            meta_set(buf, doc, doc_len, form);
            return value;
        }
        if (sym_eq(head, "if")) {
            mino_val_t *cond_form;
            mino_val_t *then_form;
            mino_val_t *else_form = mino_nil(S_);
            mino_val_t *cond;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_error_at(form, "if requires a condition and a then-branch");
                return NULL;
            }
            cond_form = args->as.cons.car;
            then_form = args->as.cons.cdr->as.cons.car;
            if (mino_is_cons(args->as.cons.cdr->as.cons.cdr)) {
                else_form = args->as.cons.cdr->as.cons.cdr->as.cons.car;
            }
            cond = eval_value(cond_form, env);
            if (cond == NULL) {
                return NULL;
            }
            /* Branch is tail position: propagate recur/tail-call. */
            return eval_impl(mino_is_truthy(cond) ? then_form : else_form,
                             env, tail);
        }
        if (sym_eq(head, "do")) {
            return eval_implicit_do_impl(args, env, tail);
        }
        if (sym_eq(head, "let")) {
            mino_val_t *bindings;
            mino_val_t *body;
            mino_env_t *local;
            if (!mino_is_cons(args)) {
                set_error_at(form, "let requires a binding list and body");
                return NULL;
            }
            bindings = args->as.cons.car;
            body     = args->as.cons.cdr;
            if (!mino_is_cons(bindings) && !mino_is_nil(bindings)) {
                set_error_at(form, "let bindings must be a list");
                return NULL;
            }
            local = env_child(env);
            while (mino_is_cons(bindings)) {
                mino_val_t *name_form = bindings->as.cons.car;
                mino_val_t *rest_pair = bindings->as.cons.cdr;
                mino_val_t *val;
                char        buf[256];
                size_t      n;
                if (name_form == NULL || name_form->type != MINO_SYMBOL) {
                    set_error_at(form, "let binding name must be a symbol");
                    return NULL;
                }
                if (!mino_is_cons(rest_pair)) {
                    set_error_at(form, "let binding missing value");
                    return NULL;
                }
                n = name_form->as.s.len;
                if (n >= sizeof(buf)) {
                    set_error_at(form, "let name too long");
                    return NULL;
                }
                memcpy(buf, name_form->as.s.data, n);
                buf[n] = '\0';
                val = eval_value(rest_pair->as.cons.car, local);
                if (val == NULL) {
                    return NULL;
                }
                gc_pin(val);
                env_bind(local, buf, val);
                gc_unpin(1);
                bindings = rest_pair->as.cons.cdr;
            }
            return eval_implicit_do_impl(body, local, tail);
        }
        if (sym_eq(head, "fn")) {
            mino_val_t *params;
            mino_val_t *body;
            mino_val_t *p;
            if (!mino_is_cons(args)) {
                set_error_at(form, "fn requires a parameter list");
                return NULL;
            }
            params = args->as.cons.car;
            body   = args->as.cons.cdr;
            if (!mino_is_cons(params) && !mino_is_nil(params)) {
                set_error_at(form, "fn parameter list must be a list");
                return NULL;
            }
            for (p = params; mino_is_cons(p); p = p->as.cons.cdr) {
                mino_val_t *name = p->as.cons.car;
                if (name == NULL || name->type != MINO_SYMBOL) {
                    set_error_at(form, "fn parameter must be a symbol");
                    return NULL;
                }
            }
            return make_fn(params, body, env);
        }
        if (sym_eq(head, "recur")) {
            mino_val_t *evaled = eval_args(args, env);
            mino_val_t *r;
            if (evaled == NULL && mino_last_error(S_) != NULL) {
                return NULL;
            }
            r = alloc_val(MINO_RECUR);
            r->as.recur.args = evaled;
            return r;
        }
        if (sym_eq(head, "loop")) {
            mino_val_t *bindings;
            mino_val_t *body;
            mino_val_t *params      = mino_nil(S_);
            mino_val_t *params_tail = NULL;
            mino_env_t *local;
            if (!mino_is_cons(args)) {
                set_error_at(form, "loop requires a binding list and body");
                return NULL;
            }
            bindings = args->as.cons.car;
            body     = args->as.cons.cdr;
            if (!mino_is_cons(bindings) && !mino_is_nil(bindings)) {
                set_error_at(form, "loop bindings must be a list");
                return NULL;
            }
            local = env_child(env);
            while (mino_is_cons(bindings)) {
                mino_val_t *name_form = bindings->as.cons.car;
                mino_val_t *rest_pair = bindings->as.cons.cdr;
                mino_val_t *val;
                char        buf[256];
                size_t      n;
                mino_val_t *cell;
                if (name_form == NULL || name_form->type != MINO_SYMBOL) {
                    set_error_at(form, "loop binding name must be a symbol");
                    return NULL;
                }
                if (!mino_is_cons(rest_pair)) {
                    set_error_at(form, "loop binding missing value");
                    return NULL;
                }
                n = name_form->as.s.len;
                if (n >= sizeof(buf)) {
                    set_error_at(form, "loop name too long");
                    return NULL;
                }
                memcpy(buf, name_form->as.s.data, n);
                buf[n] = '\0';
                val = eval_value(rest_pair->as.cons.car, local);
                if (val == NULL) {
                    return NULL;
                }
                gc_pin(val);
                env_bind(local, buf, val);
                gc_unpin(1);
                cell = mino_cons(S_, name_form, mino_nil(S_));
                if (params_tail == NULL) {
                    params = cell;
                } else {
                    params_tail->as.cons.cdr = cell;
                }
                params_tail = cell;
                bindings = rest_pair->as.cons.cdr;
            }
            for (;;) {
                mino_val_t *result = eval_implicit_do_impl(body, local, tail);
                if (result == NULL) {
                    return NULL;
                }
                if (result->type != MINO_RECUR) {
                    return result;
                }
                if (!bind_params(local, params, result->as.recur.args,
                                 "recur")) {
                    return NULL;
                }
            }
        }

        /*
         * try / catch: (try body (catch e handler...))
         * Script-level exceptions thrown by `throw` are caught; fatal
         * runtime errors (NULL returns without longjmp) propagate to host.
         */
        if (sym_eq(head, "try")) {
            mino_val_t *body_form;
            mino_val_t *catch_clause;
            mino_val_t *catch_var;
            mino_val_t *catch_body;
            char        var_buf[256];
            size_t      var_len;
            int         saved_try   = try_depth;
            int         saved_call  = call_depth;
            int         saved_trace = trace_added;
            dyn_frame_t *saved_dyn  = dyn_stack;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.cdr)) {
                set_error_at(form, "try requires a body and a (catch e ...) clause");
                return NULL;
            }
            body_form    = args->as.cons.car;
            catch_clause = args->as.cons.cdr->as.cons.car;
            /* Parse (catch e handler-body ...) */
            if (!mino_is_cons(catch_clause)
                || !sym_eq(catch_clause->as.cons.car, "catch")
                || !mino_is_cons(catch_clause->as.cons.cdr)) {
                set_error_at(form, "try: second form must be (catch e ...)");
                return NULL;
            }
            catch_var = catch_clause->as.cons.cdr->as.cons.car;
            if (catch_var == NULL || catch_var->type != MINO_SYMBOL) {
                set_error_at(form, "catch binding must be a symbol");
                return NULL;
            }
            var_len = catch_var->as.s.len;
            if (var_len >= sizeof(var_buf)) {
                set_error_at(form, "catch variable name too long");
                return NULL;
            }
            memcpy(var_buf, catch_var->as.s.data, var_len);
            var_buf[var_len] = '\0';
            catch_body = catch_clause->as.cons.cdr->as.cons.cdr;
            if (try_depth >= MAX_TRY_DEPTH) {
                set_error_at(form, "try nesting too deep");
                return NULL;
            }
            try_stack[try_depth].exception = NULL;
            if (setjmp(try_stack[try_depth].buf) == 0) {
                mino_val_t *result;
                try_depth++;
                result = eval(body_form, env);
                try_depth = saved_try;
                if (result == NULL) {
                    /* Fatal runtime error — propagate to host. */
                    return NULL;
                }
                return result;
            } else {
                /* longjmp'd from throw. */
                mino_val_t *ex = try_stack[saved_try].exception;
                mino_env_t *local;
                try_depth   = saved_try;
                call_depth  = saved_call;
                trace_added = saved_trace;
                /* Unwind dynamic binding frames pushed between the
                 * try entry and the throw.  Each frame's bindings
                 * were malloc'd; free them to avoid leaks. */
                while (dyn_stack != saved_dyn) {
                    dyn_frame_t *f = dyn_stack;
                    dyn_binding_t *b = f->bindings;
                    dyn_stack = f->prev;
                    while (b) {
                        dyn_binding_t *next = b->next;
                        free(b);
                        b = next;
                    }
                    /* The frame itself is a stack-local variable in
                     * the binding special form; do NOT free it. */
                }
                clear_error();
                local = env_child(env);
                env_bind(local, var_buf, ex != NULL ? ex : mino_nil(S_));
                return eval_implicit_do(catch_body, local);
            }
        }

        /*
         * binding: (binding (name1 val1 name2 val2 ...) body...)
         * Pushes a dynamic binding frame, evaluates body forms, then pops
         * the frame (even on error, via setjmp/longjmp cleanup).
         */
        if (sym_eq(head, "binding")) {
            mino_val_t *pairs, *body, *result;
            dyn_frame_t frame;
            dyn_binding_t *bhead = NULL;
            if (!mino_is_cons(args) || !mino_is_cons(args->as.cons.car)) {
                set_error_at(form, "binding requires a binding list and body");
                return NULL;
            }
            pairs = args->as.cons.car;
            body  = args->as.cons.cdr;
            /* Parse binding pairs. */
            while (pairs != NULL && pairs->type == MINO_CONS) {
                mino_val_t *sym_v, *val_form, *val;
                dyn_binding_t *b;
                char nbuf[256];
                size_t nlen;
                sym_v = pairs->as.cons.car;
                if (sym_v == NULL || sym_v->type != MINO_SYMBOL) {
                    set_error_at(form, "binding: names must be symbols");
                    /* Free any allocated bindings. */
                    while (bhead) { dyn_binding_t *n = bhead->next; free(bhead); bhead = n; }
                    return NULL;
                }
                nlen = sym_v->as.s.len;
                if (nlen >= sizeof(nbuf)) {
                    set_error_at(form, "binding: name too long");
                    while (bhead) { dyn_binding_t *n = bhead->next; free(bhead); bhead = n; }
                    return NULL;
                }
                memcpy(nbuf, sym_v->as.s.data, nlen);
                nbuf[nlen] = '\0';
                pairs = pairs->as.cons.cdr;
                if (pairs == NULL || pairs->type != MINO_CONS) {
                    set_error_at(form, "binding: odd number of forms in binding list");
                    while (bhead) { dyn_binding_t *n = bhead->next; free(bhead); bhead = n; }
                    return NULL;
                }
                val_form = pairs->as.cons.car;
                pairs    = pairs->as.cons.cdr;
                val = eval(val_form, env);
                if (val == NULL) {
                    while (bhead) { dyn_binding_t *n = bhead->next; free(bhead); bhead = n; }
                    return NULL;
                }
                b = (dyn_binding_t *)malloc(sizeof(*b));
                b->name = mino_symbol(S_, nbuf)->as.s.data; /* interned */
                b->val  = val;
                b->next = bhead;
                bhead   = b;
            }
            /* Push frame. */
            frame.bindings = bhead;
            frame.prev     = dyn_stack;
            dyn_stack      = &frame;
            result = eval_implicit_do(body, env);
            /* Pop frame. */
            dyn_stack = frame.prev;
            while (bhead) { dyn_binding_t *n = bhead->next; free(bhead); bhead = n; }
            return result;
        }

        if (sym_eq(head, "lazy-seq")) {
            mino_val_t *lz = alloc_val(MINO_LAZY);
            lz->as.lazy.body     = args;
            lz->as.lazy.env      = env;
            lz->as.lazy.cached   = NULL;
            lz->as.lazy.realized = 0;
            return lz;
        }

        /* Function or macro application. */
        {
            mino_val_t *fn = eval_value(head, env);
            mino_val_t *evaled;
            if (fn == NULL) {
                return NULL;
            }
            /* Pin fn: eval_args allocates, and the conservative stack
             * scanner may miss fn if the compiler keeps it in a register. */
            gc_pin(fn);
            if (fn->type == MINO_MACRO) {
                /* Expand with unevaluated args; re-eval the resulting form
                 * in the caller's environment. */
                mino_val_t *expanded = apply_callable(fn, args, env);
                gc_unpin(1);
                if (expanded == NULL) {
                    return NULL;
                }
                return eval_impl(expanded, env, tail);
            }
            if (fn->type != MINO_PRIM && fn->type != MINO_FN) {
                gc_unpin(1);
                {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "not a function (got %s)",
                             type_tag_str(fn));
                    set_error_at(form, msg);
                }
                return NULL;
            }
            evaled = eval_args(args, env);
            gc_unpin(1);
            if (evaled == NULL && mino_last_error(S_) != NULL) {
                return NULL;
            }
            /* Proper tail calls: in tail position, return a trampoline
             * sentinel instead of growing the C stack. */
            if (tail && fn->type == MINO_FN) {
                mino_val_t *tc = alloc_val(MINO_TAIL_CALL);
                tc->as.tail_call.fn   = fn;
                tc->as.tail_call.args = evaled;
                return tc;
            }
            return apply_callable(fn, evaled, env);
        }
    }
    }
    set_error("eval: unknown value type");
    return NULL;
}

mino_val_t *eval(mino_val_t *form, mino_env_t *env)
{
    return eval_impl(form, env, 0);
}

/*
 * Invoke `fn` with an already-evaluated argument list. Used both by the
 * evaluator's function-call path and by primitives (e.g. update) that
 * need to call back into user-defined code.
 */
mino_val_t *apply_callable(mino_val_t *fn, mino_val_t *args,
                                  mino_env_t *env)
{
    if (fn == NULL) {
        set_error("cannot apply null");
        return NULL;
    }
    if (fn->type == MINO_PRIM) {
        const char *file = NULL;
        int         line = 0;
        mino_val_t *result;
        if (eval_current_form != NULL
            && eval_current_form->type == MINO_CONS) {
            file = eval_current_form->as.cons.file;
            line = eval_current_form->as.cons.line;
        }
        push_frame(fn->as.prim.name, file, line);
        result = fn->as.prim.fn(args, env);
        if (result == NULL) {
            return NULL; /* leave frame for trace */
        }
        pop_frame();
        return result;
    }
    if (fn->type == MINO_FN || fn->type == MINO_MACRO) {
        const char *tag       = fn->type == MINO_MACRO ? "macro" : "fn";
        mino_env_t *local     = env_child(fn->as.fn.env);
        mino_val_t *call_args = args;
        const char *file      = NULL;
        int         line      = 0;
        mino_val_t *result;
        if (eval_current_form != NULL
            && eval_current_form->type == MINO_CONS) {
            file = eval_current_form->as.cons.file;
            line = eval_current_form->as.cons.line;
        }
        push_frame(tag, file, line);
        for (;;) {
            if (!bind_params(local, fn->as.fn.params, call_args, tag)) {
                return NULL; /* leave frame for trace */
            }
            result = eval_implicit_do_impl(fn->as.fn.body, local, 1);
            if (result == NULL) {
                return NULL; /* leave frame for trace */
            }
            if (result->type == MINO_RECUR) {
                /* Self-recursion: rebind params and loop. */
                call_args = result->as.recur.args;
                continue;
            }
            if (result->type == MINO_TAIL_CALL) {
                /* Proper tail call: switch to the target function. */
                fn        = result->as.tail_call.fn;
                call_args = result->as.tail_call.args;
                local     = env_child(fn->as.fn.env);
                continue;
            }
            pop_frame();
            return result;
        }
    }
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "not a function (got %s)",
                 type_tag_str(fn));
        set_error(msg);
    }
    return NULL;
}

mino_val_t *mino_eval(mino_state_t *S, mino_val_t *form, mino_env_t *env)
{
    S_ = S;
    volatile char probe = 0;
    mino_val_t   *v;
    gc_note_host_frame((void *)&probe);
    (void)probe;
    eval_steps     = 0;
    limit_exceeded = 0;
    interrupted    = 0;
    trace_added    = 0;
    call_depth     = 0;
    v = eval(form, env);
    if (v == NULL) {
        append_trace();
        call_depth = 0;
        return NULL;
    }
    if (v->type == MINO_RECUR) {
        set_error("recur must be in tail position");
        call_depth = 0;
        return NULL;
    }
    if (v->type == MINO_TAIL_CALL) {
        set_error("tail call escaped to top level");
        call_depth = 0;
        return NULL;
    }
    call_depth = 0;
    return v;
}

mino_val_t *mino_eval_string(mino_state_t *S, const char *src, mino_env_t *env)
{
    S_ = S;
    volatile char   probe = 0;
    mino_val_t     *last  = mino_nil(S_);
    const char     *saved_file = reader_file;
    int             saved_line = reader_line;
    gc_note_host_frame((void *)&probe);
    (void)probe;
    eval_steps     = 0;
    limit_exceeded = 0;
    interrupted    = 0;
    if (reader_file == NULL) {
        reader_file = intern_filename("<string>");
    }
    reader_line = 1;
    while (*src != '\0') {
        const char *end  = NULL;
        mino_val_t *form = mino_read(S_, src, &end);
        if (form == NULL) {
            if (mino_last_error(S_) != NULL) {
                reader_file = saved_file;
                reader_line = saved_line;
                return NULL;
            }
            break; /* EOF */
        }
        last = mino_eval(S_, form, env);
        if (last == NULL) {
            reader_file = saved_file;
            reader_line = saved_line;
            return NULL;
        }
        src = end;
    }
    reader_file = saved_file;
    reader_line = saved_line;
    return last;
}

mino_val_t *mino_load_file(mino_state_t *S, const char *path, mino_env_t *env)
{
    S_ = S;
    FILE  *f;
    char  *buf;
    long   sz;
    size_t rd;
    mino_val_t    *result;
    const char    *saved_file;
    if (path == NULL || env == NULL) {
        set_error("mino_load_file: NULL argument");
        return NULL;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        char msg[300];
        snprintf(msg, sizeof(msg), "cannot open file: %s", path);
        set_error(msg);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        set_error("cannot determine file size");
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        set_error("out of memory loading file");
        return NULL;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    saved_file  = reader_file;
    reader_file = intern_filename(path);
    result = mino_eval_string(S_, buf, env);
    reader_file = saved_file;
    free(buf);
    return result;
}

mino_env_t *mino_new(mino_state_t *S)
{
    S_ = S;
    mino_env_t *env = mino_env_new(S_);
    mino_install_core(S_, env);
    mino_install_io(S_, env);
    return env;
}

void mino_register_fn(mino_state_t *S, mino_env_t *env, const char *name, mino_prim_fn fn)
{
    S_ = S;
    mino_env_set(S_, env, name, mino_prim(S_, name, fn));
}

mino_val_t *mino_call(mino_state_t *S, mino_val_t *fn, mino_val_t *args, mino_env_t *env)
{
    S_ = S;
    volatile char probe = 0;
    gc_note_host_frame((void *)&probe);
    (void)probe;
    return apply_callable(fn, args, env);
}

int mino_pcall(mino_state_t *S, mino_val_t *fn, mino_val_t *args, mino_env_t *env,
               mino_val_t **out)
{
    S_ = S;
    mino_val_t *result = mino_call(S_, fn, args, env);
    if (out != NULL) {
        *out = result;
    }
    return result == NULL ? -1 : 0;
}

void mino_set_limit(mino_state_t *S, int kind, size_t value)
{
    S_ = S;
    switch (kind) {
    case MINO_LIMIT_STEPS: limit_steps = value; break;
    case MINO_LIMIT_HEAP:  limit_heap  = value; break;
    default: break;
    }
}

void mino_interrupt(mino_state_t *S)
{
    /* Write directly to avoid S_ (may be in use by another thread). */
#undef interrupted
    S->interrupted = 1;
#define interrupted (S_->interrupted)
}


/* ------------------------------------------------------------------------- */
/* In-process REPL handle                                                    */
/* ------------------------------------------------------------------------- */

struct mino_repl {
    mino_state_t *state;
    mino_env_t   *env;
    char         *buf;
    size_t        len;
    size_t        cap;
};

mino_repl_t *mino_repl_new(mino_state_t *S, mino_env_t *env)
{
    S_ = S;
    mino_repl_t *r = (mino_repl_t *)malloc(sizeof(*r));
    if (r == NULL) { return NULL; }
    r->state = S;
    r->env   = env;
    r->buf   = NULL;
    r->len   = 0;
    r->cap   = 0;
    return r;
}

static int repl_is_whitespace(const char *s)
{
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',') {
            return 0;
        }
    }
    return 1;
}

int mino_repl_feed(mino_repl_t *repl, const char *line, mino_val_t **out)
{
    size_t         add;
    const char    *cursor;
    const char    *end;
    mino_val_t    *form;
    mino_val_t    *result;

    if (out != NULL) { *out = NULL; }
    if (repl == NULL) { return MINO_REPL_ERROR; }
    S_ = repl->state;

    /* Append the line to the buffer. */
    add = (line != NULL) ? strlen(line) : 0;
    if (repl->len + add + 1 > repl->cap) {
        size_t new_cap = repl->cap == 0 ? 256 : repl->cap;
        char  *nb;
        while (new_cap < repl->len + add + 1) { new_cap *= 2; }
        nb = (char *)realloc(repl->buf, new_cap);
        if (nb == NULL) {
            set_error("repl: out of memory");
            return MINO_REPL_ERROR;
        }
        repl->buf = nb;
        repl->cap = new_cap;
    }
    if (add > 0) {
        memcpy(repl->buf + repl->len, line, add);
    }
    repl->len += add;
    repl->buf[repl->len] = '\0';

    /* If buffer is only whitespace, need more input. */
    if (repl_is_whitespace(repl->buf)) {
        return MINO_REPL_MORE;
    }

    /* Try to read a form. */
    cursor = repl->buf;
    end    = repl->buf;
    form   = mino_read(S_, cursor, &end);
    if (form == NULL) {
        const char *err = mino_last_error(S_);
        if (err != NULL && strstr(err, "unterminated") != NULL) {
            return MINO_REPL_MORE;
        }
        /* Hard parse error — reset buffer. */
        repl->len = 0;
        repl->buf[0] = '\0';
        return MINO_REPL_ERROR;
    }

    /* Shift remaining bytes to the front. */
    {
        size_t consumed  = (size_t)(end - repl->buf);
        size_t remaining = repl->len - consumed;
        memmove(repl->buf, end, remaining + 1);
        repl->len = remaining;
    }

    /* Evaluate the form. */
    result = mino_eval(S_, form, repl->env);
    if (result == NULL) {
        return MINO_REPL_ERROR;
    }
    if (out != NULL) { *out = result; }
    return MINO_REPL_OK;
}

void mino_repl_free(mino_repl_t *repl)
{
    if (repl == NULL) { return; }
    free(repl->buf);
    free(repl);
}

