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
    unsigned                 first_unknown_op; /* valid only when reason == UNKNOWN_OP */
    int                      compiled;
    size_t                   native_bytes;
    struct cpjit_stat_entry *next;
} cpjit_stat_entry_t;

static struct {
    int                  enabled;        /* tri-state: 0 untested, 1 on, -1 off */
    unsigned long        fns_attempted;
    unsigned long        fns_eligible;
    unsigned long        fns_compiled;
    unsigned long        reason_count[CPJIT_REASON__COUNT];
    unsigned long        op_reject_count[OP__COUNT];
    size_t               native_bytes_total;
    cpjit_stat_entry_t  *entries;
    int                  atexit_registered;
} g_cpjit_stats;

static void cpjit_stats_dump(void)
{
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
            fprintf(stderr, "  op=%-3d  %lu fns\n", op, n);
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
            char loc[160];
            if (e->reason == CPJIT_REASON_UNKNOWN_OP) {
                snprintf(loc, sizeof loc,
                         "%s:%d:%d  code_len=%zu  reason=%s(op=%u)  native=%zu",
                         base, e->line, e->column, e->code_len,
                         mino_jit_reason_name(e->reason),
                         e->first_unknown_op, e->native_bytes);
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
}

static int cpjit_stats_enabled(void)
{
    if (g_cpjit_stats.enabled == 0) {
        const char *e = getenv("MINO_CPJIT_STATS");
        g_cpjit_stats.enabled =
            (e != NULL && e[0] != '\0' && e[0] != '0') ? 1 : -1;
        if (g_cpjit_stats.enabled == 1 && !g_cpjit_stats.atexit_registered) {
            atexit(cpjit_stats_dump);
            g_cpjit_stats.atexit_registered = 1;
        }
    }
    return g_cpjit_stats.enabled == 1;
}

void mino_jit_stats_record(const mino_bc_fn_t *bc,
                            cpjit_reason_t reason,
                            unsigned first_unknown_op,
                            int compiled, size_t native_bytes)
{
    if (!cpjit_stats_enabled()) return;
    g_cpjit_stats.fns_attempted++;
    if (reason == CPJIT_REASON_OK) g_cpjit_stats.fns_eligible++;
    if (compiled) {
        g_cpjit_stats.fns_compiled++;
        g_cpjit_stats.native_bytes_total += native_bytes;
    }
    g_cpjit_stats.reason_count[reason]++;
    if (reason == CPJIT_REASON_UNKNOWN_OP && first_unknown_op < OP__COUNT) {
        g_cpjit_stats.op_reject_count[first_unknown_op]++;
    }
    cpjit_stat_entry_t *ent = (cpjit_stat_entry_t *)calloc(1, sizeof *ent);
    if (ent == NULL) return;
    ent->bc       = bc;
    ent->code_len = bc != NULL ? bc->code_len : 0;
    ent->reason   = reason;
    ent->first_unknown_op = first_unknown_op;
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
