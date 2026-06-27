/*
 * image_load.c -- save-lisp-and-die (SLAD) image deserializer.
 *
 * Reads a text image file produced by image.c's mino_save_image and
 * reconstructs the runtime state: namespaces, vars, values, closures,
 * mutable refs. Two-pass load: allocate shells, then patch references.
 * See ADR 12.
 */

#include "runtime/internal.h"
#include "runtime/image_internal.h"
#include "runtime/var_module.h"
#include "mino.h"
#include "values/internal.h"
#include "collections/internal.h"
#include "eval/internal.h"
#include "gc/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (id >= r->id_count) return NULL;
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
                               ? intern_var_str(r->S, ns_str) : NULL;
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
                       ? intern_var_str(r->S, ns_str) : NULL;
        v->as.var.sym = (sym_str && strcmp(sym_str, "-") != 0)
                        ? intern_var_str(r->S, sym_str) : NULL;
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
                    /* Intern the name so it's stable for the state's
                     * lifetime (same pool as var ns/name strings). The
                     * malloc'd copy from img_parse_token is freed. */
                    e->bindings[e->len].name = (char *)intern_var_str(r->S, name);
                    free(name);
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
                    size_t j;
                    int found = 0;
                    /* Reconnect parent to clojure.core (skipped during
                     * serialization because core is reinstalled by bootstrap) */
                    if (new_env->parent == NULL)
                        new_env->parent = S->ns_vars.mino_core_env;
                    /* Register the env as a GC root */
                    root_env_t *rr = (root_env_t *)malloc(sizeof(*rr));
                    if (rr != NULL) {
                        rr->env = new_env;
                        rr->next = S->gc.root_envs;
                        S->gc.root_envs = rr;
                    }
                    /* Search for existing namespace entry to replace */
                    for (j = 0; j < S->ns_vars.ns_env_len; j++) {
                        if (S->ns_vars.ns_env_table[j].name != NULL &&
                            strcmp(S->ns_vars.ns_env_table[j].name,
                                   ns_name) == 0) {
                            S->ns_vars.ns_env_table[j].env = new_env;
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        /* Add new namespace entry */
                        ns_env_entry_t *ne;
                        if (S->ns_vars.ns_env_len >=
                            S->ns_vars.ns_env_cap) {
                            S->ns_vars.ns_env_cap *= 2;
                            S->ns_vars.ns_env_table =
                                (ns_env_entry_t *)realloc(
                                    S->ns_vars.ns_env_table,
                                    S->ns_vars.ns_env_cap *
                                    sizeof(ns_env_entry_t));
                        }
                        ne = &S->ns_vars.ns_env_table[
                            S->ns_vars.ns_env_len++];
                        ne->name = ns_name; /* takes ownership */
                        ne->env = new_env;
                        ne->meta = mino_nil(S);
                    } else {
                        free(ns_name);
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
                        /* ns/sym are already interned from the patch pass;
                         * just update to the canonical pointers and register */
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
