/*
 * print.c -- value printer.
 */

#include "mino_internal.h"

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

void mino_print_to(mino_state_t *S, FILE *out, const mino_val_t *v)
{
    S_ = S;
    if (v == NULL || v->type == MINO_NIL) {
        fputs("nil", out);
        return;
    }
    if (print_depth >= MINO_PRINT_DEPTH_MAX) {
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
    case MINO_FLOAT: {
        /*
         * Always include a decimal point so the printed form re-reads as a
         * float, not an int. %g may drop the dot for whole numbers.
         */
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "%g", v->as.f);
        int needs_dot = 1;
        int i;
        if (n < 0) {
            fputs("nan", out);
            return;
        }
        for (i = 0; i < n; i++) {
            if (buf[i] == '.' || buf[i] == 'e' || buf[i] == 'E'
                || buf[i] == 'n' || buf[i] == 'i') {
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
        print_depth++;
        while (p != NULL && p->type == MINO_CONS) {
            mino_print_to(S_, out, p->as.cons.car);
            p = p->as.cons.cdr;
            /* Force lazy tails so (cons x (lazy-seq ...)) prints as a list. */
            if (p != NULL && p->type == MINO_LAZY) {
                p = lazy_force((mino_val_t *)p);
            }
            if (p != NULL && p->type == MINO_CONS) {
                fputc(' ', out);
            } else if (p != NULL && p->type != MINO_NIL) {
                fputs(" . ", out);
                mino_print_to(S_, out, p);
                break;
            }
        }
        print_depth--;
        fputc(')', out);
        return;
    }
    case MINO_VECTOR: {
        size_t i;
        fputc('[', out);
        print_depth++;
        for (i = 0; i < v->as.vec.len; i++) {
            if (i > 0) {
                fputc(' ', out);
            }
            mino_print_to(S_, out, vec_nth(v, i));
        }
        print_depth--;
        fputc(']', out);
        return;
    }
    case MINO_MAP: {
        size_t i;
        fputc('{', out);
        print_depth++;
        for (i = 0; i < v->as.map.len; i++) {
            mino_val_t *key = vec_nth(v->as.map.key_order, i);
            if (i > 0) {
                fputs(", ", out);
            }
            mino_print_to(S_, out, key);
            fputc(' ', out);
            mino_print_to(S_, out, map_get_val(v, key));
        }
        print_depth--;
        fputc('}', out);
        return;
    }
    case MINO_SET: {
        size_t i;
        fputs("#{", out);
        print_depth++;
        for (i = 0; i < v->as.set.len; i++) {
            if (i > 0) {
                fputc(' ', out);
            }
            mino_print_to(S_, out, vec_nth(v->as.set.key_order, i));
        }
        print_depth--;
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
        print_depth++;
        mino_print_to(S_, out, v->as.atom.val);
        print_depth--;
        fputc(']', out);
        return;
    case MINO_LAZY: {
        /* Force the lazy seq and print the realized value. */
        mino_val_t *forced = lazy_force((mino_val_t *)v);
        mino_print_to(S_, out, forced);
        return;
    }
    case MINO_RECUR:
        /* Internal sentinel; should not escape to user-visible output. */
        fputs("#<recur>", out);
        return;
    case MINO_TAIL_CALL:
        fputs("#<tail-call>", out);
        return;
    }
}

void mino_print(mino_state_t *S, const mino_val_t *v)
{
    S_ = S;
    mino_print_to(S_, stdout, v);
}

void mino_println(mino_state_t *S, const mino_val_t *v)
{
    S_ = S;
    mino_print_to(S_, stdout, v);
    fputc('\n', stdout);
}

