/*
 * print.c -- value printer.
 */

#include "runtime/internal.h"
#include "runtime/host_threads.h"

/* ------------------------------------------------------------------------- */
/* Printer                                                                   */
/* ------------------------------------------------------------------------- */

static void print_string_escaped(FILE *out, const char *s, size_t len)
{
    size_t i;
    fputc('"', out);
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n",  out); break;
        case '\t': fputs("\\t",  out); break;
        case '\r': fputs("\\r",  out); break;
        case '\0': fputs("\\0",  out); break;
        default:   fputc((int)c, out); break;
        }
    }
    fputc('"', out);
}

/*
 * Cycle-safe print depth: when recursion exceeds this limit, the printer
 * emits #<...> instead of descending further. This prevents stack overflow
 * on deeply nested or self-referential structures (possible through mutable
 * cons tails in internal data, though the user-facing API is immutable).
 */
#define MINO_PRINT_DEPTH_MAX 128

static void print_rb_inorder(mino_state_t *S, FILE *out,
                             const mino_rb_node_t *n, int is_map, int *first)
{
    if (n == NULL) return;
    print_rb_inorder(S, out, n->left, is_map, first);
    if (!*first) fputs(is_map ? ", " : " ", out);
    *first = 0;
    mino_print_to(S, out, n->key);
    if (is_map) { fputc(' ', out); mino_print_to(S, out, n->val); }
    print_rb_inorder(S, out, n->right, is_map, first);
}

void mino_print_to(mino_state_t *S, FILE *out, const mino_val_t *v)
{
    if (v == NULL || (MINO_IS_PTR(v) && mino_type_of(v) == MINO_NIL)) {
        fputs("nil", out);
        return;
    }
    if (S->print_depth >= MINO_PRINT_DEPTH_MAX) {
        fputs("#<...>", out);
        return;
    }
    switch (mino_type_of(v)) {
    case MINO_NIL:
        fputs("nil", out);
        return;
    case MINO_EMPTY_LIST:
        fputs("()", out);
        return;
    case MINO_BOOL:
        fputs(mino_val_bool_get(v) ? "true" : "false", out);
        return;
    case MINO_INT:
        fprintf(out, "%lld", mino_val_int_get(v));
        return;
    case MINO_BIGINT:
        mino_bigint_print(S, v, out);
        return;
    case MINO_RATIO:
        mino_ratio_print(S, v, out);
        return;
    case MINO_BIGDEC:
        mino_bigdec_print(S, v, out);
        return;
    case MINO_FLOAT:
    case MINO_FLOAT32: {
        char buf[64];
        int n, needs_dot, i, p;
        double x = v->as.f;
        if (isnan(x)) { fputs("##NaN", out); return; }
        if (isinf(x)) {
            fputs(x > 0 ? "##Inf" : "##-Inf", out);
            return;
        }
        /* Shortest round-trippable representation. JVM Double.toString
         * uses fixed notation in [1e-3, 1e7) (signed magnitude) and
         * scientific elsewhere; pick the shortest precision (after the
         * decimal for fixed, significand digits for scientific) that
         * still re-parses to the same double. */
        {
            double absx = (x < 0.0) ? -x : x;
            int use_sci = (x != 0.0) && (absx < 1e-3 || absx >= 1e7);
            n = 0;
            if (use_sci) {
                for (p = 0; p <= 17; p++) {
                    double back;
                    n = snprintf(buf, sizeof(buf), "%.*e", p, x);
                    if (n < 0 || n >= (int)sizeof(buf)) { n = -1; break; }
                    back = strtod(buf, NULL);
                    if (back == x) break;
                }
            } else {
                /* Fixed: %f precision is fractional-digit count. */
                for (p = 0; p <= 17; p++) {
                    double back;
                    n = snprintf(buf, sizeof(buf), "%.*f", p, x);
                    if (n < 0 || n >= (int)sizeof(buf)) { n = -1; break; }
                    back = strtod(buf, NULL);
                    if (back == x) break;
                }
            }
        }
        if (n < 0) { fputs("0.0", out); return; }
        /* Always include a decimal point so the printed form re-reads
         * as a float. For fixed, p=0 has no point; for scientific,
         * the format is like "1e+05" or "1.5e+02" — we want "1.0E5" /
         * "1.5E2"-style. Rewrite e[+]?digits → E<digits> for JVM-ish
         * surface; leading-zero scrubbing keeps small exponents clean. */
        needs_dot = 1;
        for (i = 0; i < n; i++) {
            if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E') {
                needs_dot = 0;
                break;
            }
        }
        fputs(buf, out);
        if (needs_dot) {
            fputs(".0", out);
        }
        return;
    }
    case MINO_CHAR: {
        int cp = mino_val_char_get(v);
        switch (cp) {
        case ' ':  fputs("\\space",     out); return;
        case '\n': fputs("\\newline",   out); return;
        case '\t': fputs("\\tab",       out); return;
        case '\r': fputs("\\return",    out); return;
        case '\b': fputs("\\backspace", out); return;
        case '\f': fputs("\\formfeed",  out); return;
        default:
            if (cp >= 0x21 && cp <= 0x7E) {
                fputc('\\', out);
                fputc(cp, out);
            } else {
                /* Out-of-ASCII codepoints print as \uXXXX. Codepoints
                 * above 0xFFFF are uncommon; surface them with a full
                 * width that re-reads correctly. */
                fprintf(out, "\\u%04X", (unsigned)cp);
            }
            return;
        }
    }
    case MINO_STRING:
        print_string_escaped(out, v->as.s.data, v->as.s.len);
        return;
    case MINO_SYMBOL:
        fwrite(v->as.s.data, 1, v->as.s.len, out);
        return;
    case MINO_KEYWORD:
        fputc(':', out);
        fwrite(v->as.s.data, 1, v->as.s.len, out);
        return;
    case MINO_CONS: {
        const mino_val_t *p = v;
        fputc('(', out);
        S->print_depth++;
        while (p != NULL && mino_type_of(p) == MINO_CONS) {
            mino_print_to(S, out, p->as.cons.car);
            p = p->as.cons.cdr;
            /* Force lazy tails so (cons x (lazy-seq ...)) prints as a list. */
            if (p != NULL && mino_type_of(p) == MINO_LAZY) {
                p = lazy_force(S,(mino_val_t *)p);
            }
            if (p != NULL && mino_type_of(p) == MINO_CONS) {
                fputc(' ', out);
            } else if (p != NULL && mino_type_of(p) != MINO_NIL
                       && mino_type_of(p) != MINO_EMPTY_LIST) {
                fputs(" . ", out);
                mino_print_to(S, out, p);
                break;
            }
        }
        S->print_depth--;
        fputc(')', out);
        return;
    }
    case MINO_VECTOR: {
        size_t i;
        fputc('[', out);
        S->print_depth++;
        for (i = 0; i < v->as.vec.len; i++) {
            if (i > 0) {
                fputc(' ', out);
            }
            mino_print_to(S, out, vec_nth(v, i));
        }
        S->print_depth--;
        fputc(']', out);
        return;
    }
    case MINO_MAP: {
        size_t i;
        fputc('{', out);
        S->print_depth++;
        for (i = 0; i < v->as.map.len; i++) {
            mino_val_t *key = vec_nth(v->as.map.key_order, i);
            if (i > 0) {
                fputs(", ", out);
            }
            mino_print_to(S, out, key);
            fputc(' ', out);
            mino_print_to(S, out, map_get_val(v, key));
        }
        S->print_depth--;
        fputc('}', out);
        return;
    }
    case MINO_SET: {
        size_t i;
        fputs("#{", out);
        S->print_depth++;
        for (i = 0; i < v->as.set.len; i++) {
            if (i > 0) {
                fputc(' ', out);
            }
            mino_print_to(S, out, vec_nth(v->as.set.key_order, i));
        }
        S->print_depth--;
        fputc('}', out);
        return;
    }
    case MINO_SORTED_MAP: {
        int first = 1;
        fputc('{', out);
        S->print_depth++;
        print_rb_inorder(S, out, v->as.sorted.root, 1, &first);
        S->print_depth--;
        fputc('}', out);
        return;
    }
    case MINO_SORTED_SET: {
        int first = 1;
        fputs("#{", out);
        S->print_depth++;
        print_rb_inorder(S, out, v->as.sorted.root, 0, &first);
        S->print_depth--;
        fputc('}', out);
        return;
    }
    case MINO_PRIM:
        fputs("#<prim ", out);
        if (v->as.prim.name != NULL) {
            fputs(v->as.prim.name, out);
        }
        fputc('>', out);
        return;
    case MINO_FN:
        fputs("#<fn>", out);
        return;
    case MINO_MACRO:
        fputs("#<macro>", out);
        return;
    case MINO_HANDLE:
        fputs("#<handle", out);
        if (v->as.handle.tag != NULL) {
            fputc(':', out);
            fputs(v->as.handle.tag, out);
        }
        fputc('>', out);
        return;
    case MINO_ATOM:
        fputs("#atom[", out);
        S->print_depth++;
        mino_print_to(S, out, v->as.atom.val);
        S->print_depth--;
        fputc(']', out);
        return;
    case MINO_VOLATILE:
        fputs("#volatile[", out);
        S->print_depth++;
        mino_print_to(S, out, v->as.volatile_.val);
        S->print_depth--;
        fputc(']', out);
        return;
    case MINO_LAZY: {
        /* Force the lazy seq and print the realized value. A lazy seq
         * that resolves to nil is the canonical empty seq — print as
         * () so cross-type seq equality and printer agree. */
        mino_val_t *forced = lazy_force(S,(mino_val_t *)v);
        if (forced == NULL || mino_type_of(forced) == MINO_NIL
            || mino_type_of(forced) == MINO_EMPTY_LIST) {
            fputs("()", out);
            return;
        }
        mino_print_to(S, out, forced);
        return;
    }
    case MINO_CHUNK: {
        /* Internal seq leaf; surface as `#chunk[…]` rather than
         * pretending to be a vector. */
        unsigned k;
        fputs("#chunk[", out);
        S->print_depth++;
        for (k = 0; k < v->as.chunk.len; k++) {
            if (k > 0) fputc(' ', out);
            mino_print_to(S, out, v->as.chunk.vals[k]);
        }
        S->print_depth--;
        fputc(']', out);
        return;
    }
    case MINO_CHUNKED_CONS: {
        /* Print as a list. Walk the chunk from off..len-1, then
         * recurse into the more pointer (which may be cons / lazy /
         * another chunked-cons). */
        const mino_val_t *cur = v;
        fputc('(', out);
        S->print_depth++;
        while (cur != NULL && mino_type_of(cur) == MINO_CHUNKED_CONS) {
            const mino_val_t *ch = cur->as.chunked_cons.chunk;
            unsigned k;
            for (k = cur->as.chunked_cons.off; k < ch->as.chunk.len; k++) {
                if (k > cur->as.chunked_cons.off
                    || cur != v) fputc(' ', out);
                mino_print_to(S, out, ch->as.chunk.vals[k]);
            }
            cur = cur->as.chunked_cons.more;
            if (cur != NULL && mino_type_of(cur) == MINO_LAZY) {
                cur = lazy_force(S, (mino_val_t *)cur);
            }
        }
        if (cur != NULL && mino_type_of(cur) == MINO_CONS) {
            fputc(' ', out);
            /* Reuse the cons walker by printing the tail inline. */
            while (cur != NULL && mino_type_of(cur) == MINO_CONS) {
                mino_print_to(S, out, cur->as.cons.car);
                cur = cur->as.cons.cdr;
                if (cur != NULL && mino_type_of(cur) == MINO_LAZY) {
                    cur = lazy_force(S, (mino_val_t *)cur);
                }
                if (cur != NULL && mino_type_of(cur) == MINO_CONS) fputc(' ', out);
            }
        }
        S->print_depth--;
        fputc(')', out);
        return;
    }
    case MINO_RECUR:
        /* Internal sentinel; should not escape to user-visible output. */
        fputs("#<recur>", out);
        return;
    case MINO_TAIL_CALL:
        fputs("#<tail-call>", out);
        return;
    case MINO_REDUCED:
        fputs("#<reduced ", out);
        mino_print_to(S, out, v->as.reduced.val);
        fputc('>', out);
        return;
    case MINO_VAR:
        fputs("#'", out);
        if (v->as.var.ns != NULL) {
            fputs(v->as.var.ns, out);
            fputc('/', out);
        }
        fputs(v->as.var.sym, out);
        return;
    case MINO_TRANSIENT:
        fputs("#<transient", out);
        if (v->as.transient.current != NULL) {
            fputc(':', out);
            switch (mino_type_of(v->as.transient.current)) {
            case MINO_VECTOR: fputs("vector", out); break;
            case MINO_MAP:    fputs("map",    out); break;
            case MINO_SET:    fputs("set",    out); break;
            default: fputs("?", out); break;
            }
        }
        if (!v->as.transient.valid) fputs(" sealed", out);
        fputc('>', out);
        return;
    case MINO_TYPE:
        if (v->as.record_type.ns != NULL) {
            fputs(v->as.record_type.ns, out);
            fputc('.', out);
        }
        if (v->as.record_type.name != NULL) {
            fputs(v->as.record_type.name, out);
        }
        return;
    case MINO_RECORD: {
        /* #ns.Name{:f1 v1, :f2 v2, ...[ext...]} -- declared fields
         * first in declared order, then ext entries in insertion
         * order. */
        const mino_val_t *t      = v->as.record.type;
        mino_val_t       *fields = t->as.record_type.fields;
        size_t            i, n_fields;
        int               first  = 1;
        fputc('#', out);
        if (t->as.record_type.ns != NULL) {
            fputs(t->as.record_type.ns, out);
            fputc('.', out);
        }
        if (t->as.record_type.name != NULL) {
            fputs(t->as.record_type.name, out);
        }
        fputc('{', out);
        S->print_depth++;
        n_fields = (fields != NULL) ? fields->as.vec.len : 0;
        for (i = 0; i < n_fields; i++) {
            if (!first) fputs(", ", out);
            first = 0;
            mino_print_to(S, out, vec_nth(fields, i));
            fputc(' ', out);
            mino_print_to(S, out, v->as.record.vals[i]);
        }
        if (v->as.record.ext != NULL) {
            const mino_val_t *e = v->as.record.ext;
            size_t k;
            for (k = 0; k < e->as.map.len; k++) {
                mino_val_t *key = vec_nth(e->as.map.key_order, k);
                if (!first) fputs(", ", out);
                first = 0;
                mino_print_to(S, out, key);
                fputc(' ', out);
                mino_print_to(S, out, map_get_val(e, key));
            }
        }
        S->print_depth--;
        fputc('}', out);
        return;
    }
    case MINO_FUTURE: {
        const mino_future_t *impl = v->as.future.impl;
        const char *st = "pending";
        if (impl != NULL) {
            switch (impl->state_tag) {
            case MINO_FUTURE_RESOLVED:  st = "resolved"; break;
            case MINO_FUTURE_FAILED:    st = "failed";   break;
            case MINO_FUTURE_CANCELLED: st = "cancelled"; break;
            default:                    st = "pending";  break;
            }
        }
        fputs("#<future:", out);
        fputs(st, out);
        fputc('>', out);
        return;
    }
    case MINO_UUID: {
        /* Canonical lowercase 8-4-4-4-12 form prefixed with #uuid so
         * the printed form roundtrips through the reader. */
        const unsigned char *b = v->as.uuid.bytes;
        char buf[64];
        snprintf(buf, sizeof(buf),
            "#uuid \"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
            "%02x%02x%02x%02x%02x%02x\"",
            b[0],  b[1],  b[2],  b[3],  b[4],  b[5],  b[6],  b[7],
            b[8],  b[9],  b[10], b[11], b[12], b[13], b[14], b[15]);
        fputs(buf, out);
        return;
    }
    case MINO_REGEX: {
        /* Print as `#"source"` so the regex round-trips through the
         * reader. The source bytes pass through unescaped -- the
         * reader's regex literal accepts the same form. */
        const mino_val_t *src = v->as.regex.source;
        fputs("#\"", out);
        if (src != NULL && mino_type_of(src) == MINO_STRING) {
            fwrite(src->as.s.data, 1, src->as.s.len, out);
        }
        fputc('"', out);
        return;
    }
    case MINO_MAP_ENTRY: {
        /* Print as `[k v]` so map-entry round-trips through equality
         * with a 2-vector and matches Clojure's pr/print form. */
        fputc('[', out);
        mino_print_to(S, out, v->as.map_entry.k);
        fputc(' ', out);
        mino_print_to(S, out, v->as.map_entry.v);
        fputc(']', out);
        return;
    }
    case MINO_TX_REF:
        /* `#ref[ID VAL]` -- not round-trippable, but consistent with
         * the rest of mino's identity-cell prints (atom, volatile). */
        fprintf(out, "#ref[0x%llx ",
                (unsigned long long)v->as.tx_ref.ref_id);
        S->print_depth++;
        mino_print_to(S, out, v->as.tx_ref.val);
        S->print_depth--;
        fputc(']', out);
        return;
    case MINO_AGENT:
        /* `#agent[ID VAL]` -- not round-trippable, but ID gives
         * each agent a stable identity in the print form so two
         * distinct agents holding the same value are still
         * distinguishable. Mirrors `#ref[ID VAL]`. */
        fprintf(out, "#agent[0x%llx ",
                (unsigned long long)v->as.agent.agent_id);
        S->print_depth++;
        mino_print_to(S, out, v->as.agent.val);
        S->print_depth--;
        fputc(']', out);
        return;
    case MINO_HOST_ARRAY: {
        /* Mirror Clojure JVM's #object[...] form for arrays since
         * arrays don't round-trip through the reader. */
        static const char *kinds[] = {
            "Object", "int", "long", "short", "byte",
            "float",  "double", "char", "boolean"
        };
        unsigned k = v->as.host_array.element_kind;
        if (k >= sizeof(kinds) / sizeof(kinds[0])) k = 0;
        fprintf(out, "#object[\"[L%s;\" 0x%p {", kinds[k], (const void *)v);
        {
            size_t i;
            for (i = 0; i < v->as.host_array.len; i++) {
                if (i > 0) fputs(", ", out);
                mino_print_to(S, out, v->as.host_array.vals[i]);
            }
        }
        fputs("}]", out);
        return;
    }
    }
}

void mino_print(mino_state_t *S, const mino_val_t *v)
{
    mino_print_to(S, stdout, v);
}

void mino_println(mino_state_t *S, const mino_val_t *v)
{
    mino_print_to(S, stdout, v);
    fputc('\n', stdout);
}

int mino_print_to_buf(mino_state_t *S, const mino_val_t *v,
                      char *buf, size_t n)
{
    FILE *out;
    size_t written;
    int rc = -1;
    if (buf == NULL || n == 0) return -1;
    out = tmpfile();
    if (out == NULL) {
        buf[0] = '\0';
        return -1;
    }
    mino_print_to(S, out, v);
    fflush(out);
    rewind(out);
    written = fread(buf, 1, n - 1, out);
    buf[written] = '\0';
    rc = (int)written;
    fclose(out);
    return rc;
}

