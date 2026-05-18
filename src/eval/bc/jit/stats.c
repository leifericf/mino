/*
 * src/eval/bc/jit/stats.c -- MINO_CPJIT_STATS=1 attribution sink.
 *
 * The entry-point compile path calls `mino_jit_stats_record` for every
 * fn it inspects, regardless of whether the fn ended up JIT-compiled.
 * When the env var is unset this turns into a single tri-state check
 * and a return; when set, each call appends to a per-fn ring and the
 * atexit dump prints a summary plus per-fn detail to stderr.
 *
 * The stats state is process-global on purpose: the dump runs at
 * atexit when there is no live state to thread per-state stats
 * through, and the data is diagnostic-only (no production code reads
 * the counters).
 */

#include "internal.h"

#ifdef MINO_CPJIT_HOST

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct cpjit_stat_entry {
    const mino_bc_fn_t      *bc;
    const char              *file;       /* borrowed; lifetime matches state */
    int                      line;
    int                      column;
    size_t                   code_len;
    cpjit_reason_t           reason;
    unsigned                 first_unknown_op; /* valid only when reason == UNKNOWN_OP or OK_WITH_DEOPT */
    size_t                   first_unknown_pc; /* valid only when reason == UNKNOWN_OP or OK_WITH_DEOPT */
    int                      compiled;
    size_t                   native_bytes;
    struct cpjit_stat_entry *next;
} cpjit_stat_entry_t;

/* Mode tri-state for the MINO_CPJIT_STATS env-var sniff:
 *   0  -- not yet sniffed
 *  -1  -- off (default, env-var unset / "0")
 *   1  -- full dump (env-var "1" or any non-summary truthy value)
 *   2  -- one-line summary (env-var "summary"). Suppresses the
 *         per-fn ring entirely so a long-running JIT'd workload
 *         doesn't pay the allocation cost.
 *   3  -- tracing (env-var "tracing"). Full dump plus a
 *         "code-bytes blocked" aggregation per blocking op so the
 *         ranking surface reflects the total bytecode surface area
 *         lost to each unstenciled op rather than just fn count.
 *         A long fn carrying one unstenciled op loses more lane
 *         coverage than a tiny one. */
#define CPJIT_STATS_OFF      (-1)
#define CPJIT_STATS_FULL     1
#define CPJIT_STATS_SUMMARY  2
#define CPJIT_STATS_TRACING  3

static struct {
    int                  enabled;        /* see modes above */
    unsigned long        fns_attempted;
    unsigned long        fns_eligible;
    unsigned long        fns_compiled;
    unsigned long        reason_count[CPJIT_REASON__COUNT];
    unsigned long        op_reject_count[OP__COUNT];
    /* Per blocking op, sum of the code_len of every fn that was
     * rejected with that op as the first_unknown_op. Bytes-blocked
     * is a better lost-lane proxy than fn-count: a 300-op fn
     * carrying one OP_THROW loses 300 ops worth of native coverage,
     * while a 10-op leaf fn loses only 10. Populated by
     * mino_jit_stats_record on every recorded entry; surfaced by
     * the tracing dump. */
    unsigned long long   op_reject_code_bytes[OP__COUNT];
    size_t               native_bytes_total;
    cpjit_stat_entry_t  *entries;
    int                  atexit_registered;
} g_cpjit_stats;

/* One-line "JIT engagement" report intended for --jit=auto runs to
 * confirm the JIT is actually doing work without the per-fn ring's
 * verbosity. Top blockers are listed in op-number order so a stable
 * diff across runs is meaningful; the 5-entry cap keeps the line
 * scannable. */
static void cpjit_stats_dump_summary(void)
{
    int top_ops[5];
    unsigned long top_counts[5];
    int top_n = 0;
    for (int op = 0; op < OP__COUNT; op++) {
        unsigned long n = g_cpjit_stats.op_reject_count[op];
        if (n == 0) continue;
        int insert = top_n;
        for (int i = 0; i < top_n; i++) {
            if (n > top_counts[i]) { insert = i; break; }
        }
        if (insert < 5) {
            int end = top_n < 5 ? top_n : 4;
            for (int j = end; j > insert; j--) {
                top_ops[j]    = top_ops[j - 1];
                top_counts[j] = top_counts[j - 1];
            }
            top_ops[insert]    = op;
            top_counts[insert] = n;
            if (top_n < 5) top_n++;
        }
    }
    fprintf(stderr,
            "[cpjit-stats] compiled=%lu/%lu eligible=%lu native_bytes=%zu",
            g_cpjit_stats.fns_compiled,
            g_cpjit_stats.fns_attempted,
            g_cpjit_stats.fns_eligible,
            g_cpjit_stats.native_bytes_total);
    if (top_n > 0) {
        fprintf(stderr, " top_blockers=");
        for (int i = 0; i < top_n; i++) {
            fprintf(stderr, "%s%s(%lu)",
                    i == 0 ? "" : ",",
                    mino_bc_op_name((unsigned)top_ops[i]),
                    top_counts[i]);
        }
    }
    fprintf(stderr, "\n");
}

/* Tracing dump: per-op bytes-blocked ranking (descending). Used to
 * answer "which unstenciled op, if covered, would unblock the most
 * bytecode body?" so eligibility work prioritizes by lost surface
 * area rather than rejection count. Bytes-blocked is the dominant
 * scalar; pairing with fn-count gives a coarse "average fn size
 * blocked" signal. */
static void cpjit_stats_dump_tracing_op_table(void)
{
    int   order[OP__COUNT];
    int   n = 0;
    int   i;
    int   j;
    fprintf(stderr,
            "\n[cpjit-stats] ---- bytes-blocked by op (tracing) ----\n");
    for (i = 0; i < OP__COUNT; i++) {
        if (g_cpjit_stats.op_reject_count[i] == 0) continue;
        order[n++] = i;
    }
    /* Insertion sort -- n is bounded by OP__COUNT (~70) and the
     * dump runs once at exit; a sort utility isn't worth the lib
     * surface. */
    for (i = 1; i < n; i++) {
        int                key = order[i];
        unsigned long long key_b = g_cpjit_stats.op_reject_code_bytes[key];
        j = i - 1;
        while (j >= 0
               && g_cpjit_stats.op_reject_code_bytes[order[j]] < key_b) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
    if (n == 0) {
        fprintf(stderr, "  (no rejected fns)\n");
        return;
    }
    for (i = 0; i < n; i++) {
        int op = order[i];
        fprintf(stderr,
                "  op=%-3d  %-30s  %llu bytes blocked  %lu fns\n",
                op, mino_bc_op_name((unsigned)op),
                g_cpjit_stats.op_reject_code_bytes[op],
                g_cpjit_stats.op_reject_count[op]);
    }
}

static void cpjit_stats_dump(void)
{
    if (g_cpjit_stats.enabled == CPJIT_STATS_SUMMARY) {
        cpjit_stats_dump_summary();
        return;
    }
    fprintf(stderr, "\n[cpjit-stats] ---- summary ----\n");
    fprintf(stderr, "  fns_attempted    %lu\n", g_cpjit_stats.fns_attempted);
    fprintf(stderr, "  fns_eligible     %lu (%.1f%%)\n",
            g_cpjit_stats.fns_eligible,
            g_cpjit_stats.fns_attempted
                ? 100.0 * (double)g_cpjit_stats.fns_eligible
                          / (double)g_cpjit_stats.fns_attempted
                : 0.0);
    fprintf(stderr, "  fns_compiled     %lu (%.1f%%)\n",
            g_cpjit_stats.fns_compiled,
            g_cpjit_stats.fns_attempted
                ? 100.0 * (double)g_cpjit_stats.fns_compiled
                          / (double)g_cpjit_stats.fns_attempted
                : 0.0);
    fprintf(stderr, "  native_bytes     %zu\n",
            g_cpjit_stats.native_bytes_total);
    fprintf(stderr, "\n[cpjit-stats] ---- by reason ----\n");
    for (int i = 0; i < CPJIT_REASON__COUNT; i++) {
        if (g_cpjit_stats.reason_count[i] == 0) continue;
        fprintf(stderr, "  %-16s %lu\n",
                mino_jit_reason_name((cpjit_reason_t)i),
                g_cpjit_stats.reason_count[i]);
    }
    fprintf(stderr, "\n[cpjit-stats] ---- unknown-op breakdown ----\n");
    {
        int any = 0;
        for (int op = 0; op < OP__COUNT; op++) {
            unsigned long n = g_cpjit_stats.op_reject_count[op];
            if (n == 0) continue;
            /* Self-describe each row: id and symbolic name together so
             * downstream parsers (mino-bench histogram) don't need a
             * private opcode-name table that drifts from the C enum. */
            fprintf(stderr, "  op=%-3d  %-30s  %lu fns\n",
                    op, mino_bc_op_name((unsigned)op), n);
            any = 1;
        }
        if (!any) fprintf(stderr, "  (none)\n");
    }
    fprintf(stderr, "\n[cpjit-stats] ---- per-fn ----\n");
    {
        size_t n = 0;
        for (const cpjit_stat_entry_t *e = g_cpjit_stats.entries;
             e != NULL; e = e->next) {
            const char *file = e->file ? e->file : "?";
            const char *base = strrchr(file, '/');
            base = base ? base + 1 : file;
            char loc[200];
            if (e->reason == CPJIT_REASON_UNKNOWN_OP
                || e->reason == CPJIT_REASON_OK_WITH_DEOPT) {
                snprintf(loc, sizeof loc,
                         "%s:%d:%d  code_len=%zu  reason=%s(op=%u@pc=%zu)  native=%zu",
                         base, e->line, e->column, e->code_len,
                         mino_jit_reason_name(e->reason),
                         e->first_unknown_op, e->first_unknown_pc,
                         e->native_bytes);
            } else {
                snprintf(loc, sizeof loc,
                         "%s:%d:%d  code_len=%zu  reason=%-16s  %s native=%zu",
                         base, e->line, e->column, e->code_len,
                         mino_jit_reason_name(e->reason),
                         e->compiled ? "compiled" : "        ",
                         e->native_bytes);
            }
            fprintf(stderr, "  %s\n", loc);
            n++;
        }
        fprintf(stderr, "[cpjit-stats] ---- %zu fns tracked ----\n", n);
    }
    if (g_cpjit_stats.enabled == CPJIT_STATS_TRACING) {
        cpjit_stats_dump_tracing_op_table();
    }
}

static int cpjit_stats_enabled(void)
{
    if (g_cpjit_stats.enabled == 0) {
        const char *e = getenv("MINO_CPJIT_STATS");
        if (e == NULL || e[0] == '\0' || e[0] == '0') {
            g_cpjit_stats.enabled = CPJIT_STATS_OFF;
        } else if (strcmp(e, "summary") == 0) {
            g_cpjit_stats.enabled = CPJIT_STATS_SUMMARY;
        } else if (strcmp(e, "tracing") == 0) {
            g_cpjit_stats.enabled = CPJIT_STATS_TRACING;
        } else {
            g_cpjit_stats.enabled = CPJIT_STATS_FULL;
        }
        if (g_cpjit_stats.enabled != CPJIT_STATS_OFF
            && !g_cpjit_stats.atexit_registered) {
            atexit(cpjit_stats_dump);
            g_cpjit_stats.atexit_registered = 1;
        }
    }
    return g_cpjit_stats.enabled != CPJIT_STATS_OFF;
}

void mino_jit_stats_record(const mino_bc_fn_t *bc,
                            cpjit_reason_t reason,
                            unsigned first_unknown_op,
                            size_t first_unknown_pc,
                            int compiled, size_t native_bytes)
{
    if (!cpjit_stats_enabled()) return;
    g_cpjit_stats.fns_attempted++;
    if (reason == CPJIT_REASON_OK
        || reason == CPJIT_REASON_OK_WITH_DEOPT) {
        g_cpjit_stats.fns_eligible++;
    }
    if (compiled) {
        g_cpjit_stats.fns_compiled++;
        g_cpjit_stats.native_bytes_total += native_bytes;
    }
    g_cpjit_stats.reason_count[reason]++;
    /* Both UNKNOWN_OP (no native prefix) and OK_WITH_DEOPT (some prefix
     * compileable, tail blocked at first_unknown_pc) credit the
     * blocking op so the bytes-blocked table ranks both pools. The
     * tracing dump distinguishes the two reasons inline. */
    if ((reason == CPJIT_REASON_UNKNOWN_OP
         || reason == CPJIT_REASON_OK_WITH_DEOPT)
        && first_unknown_op < OP__COUNT) {
        size_t code_len = bc != NULL ? bc->code_len : 0;
        g_cpjit_stats.op_reject_count[first_unknown_op]++;
        g_cpjit_stats.op_reject_code_bytes[first_unknown_op] +=
            (unsigned long long)code_len;
    }
    /* Summary mode skips the per-fn ring: the one-line dump only
     * reads the aggregate counters, so the alloc + the borrowed
     * file-string copy would be pure overhead in a long run.
     * Tracing mode behaves like full -- the ring carries the
     * code_len needed for the bytes-blocked aggregation. */
    if (g_cpjit_stats.enabled != CPJIT_STATS_FULL
        && g_cpjit_stats.enabled != CPJIT_STATS_TRACING) return;
    cpjit_stat_entry_t *ent = (cpjit_stat_entry_t *)calloc(1, sizeof *ent);
    if (ent == NULL) return;
    ent->bc       = bc;
    ent->code_len = bc != NULL ? bc->code_len : 0;
    ent->reason   = reason;
    ent->first_unknown_op = first_unknown_op;
    ent->first_unknown_pc = first_unknown_pc;
    ent->compiled    = compiled;
    ent->native_bytes = native_bytes;
    if (bc != NULL && bc->source_map.positions != NULL
        && bc->source_map.len > 0) {
        /* strdup the filename: the borrowed pointer's pool lives in the
         * mino_state_t and is freed before atexit handlers fire. */
        if (bc->source_map.file != NULL) {
            size_t n = strlen(bc->source_map.file);
            char  *copy = (char *)malloc(n + 1);
            if (copy != NULL) {
                memcpy(copy, bc->source_map.file, n + 1);
                ent->file = copy;
            }
        }
        ent->line   = bc->source_map.positions[0].line;
        ent->column = bc->source_map.positions[0].column;
    }
    ent->next = g_cpjit_stats.entries;
    g_cpjit_stats.entries = ent;
}

#endif /* MINO_CPJIT_HOST */

/* Keep this TU non-empty under -Werror=pedantic when the gate above
 * is false. */
typedef int mino_jit_stats_tu_marker;
