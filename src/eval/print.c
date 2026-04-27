/*
 * print.c -- value printer.
 */

#include "runtime/internal.h"

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
    if (v == NULL || v->type == MINO_NIL) {
        fputs("nil", out);
        return;
    }
    if (S->print_depth >= MINO_PRINT_DEPTH_MAX) {
        fputs("#<...>", out);
        return;
    }
    switch (v->type) {
    case MINO_NIL:
        fputs("nil", out);
        return;
    case MINO_BOOL:
        fputs(v->as.b ? "true" : "false", out);
        return;
    case MINO_INT:
        fprintf(out, "%lld", v->as.i);
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
    case MINO_FLOAT: {
        char buf[64];
        int n, needs_dot, i;
        if (isnan(v->as.f)) { fputs("##NaN", out); return; }
        if (isinf(v->as.f)) {
            fputs(v->as.f > 0 ? "##Inf" : "##-Inf", out);
            return;
        }
        /*
         * Always include a decimal point so the printed form re-reads as a
         * float, not an int. %g may drop the dot for whole numbers.
         */
        n = snprintf(buf, sizeof(buf), "%g", v->as.f);
        needs_dot = 1;
        if (n < 0) { fputs("0.0", out); return; }
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
        int cp = v->as.ch;
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
        while (p != NULL && p->type == MINO_CONS) {
            mino_print_to(S, out, p->as.cons.car);
            p = p->as.cons.cdr;
            /* Force lazy tails so (cons x (lazy-seq ...)) prints as a list. */
            if (p != NULL && p->type == MINO_LAZY) {
                p = lazy_force(S,(mino_val_t *)p);
            }
            if (p != NULL && p->type == MINO_CONS) {
                fputc(' ', out);
            } else if (p != NULL && p->type != MINO_NIL) {
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
    case MINO_LAZY: {
        /* Force the lazy seq and print the realized value. */
        mino_val_t *forced = lazy_force(S,(mino_val_t *)v);
        mino_print_to(S, out, forced);
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
            switch (v->as.transient.current->type) {
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

