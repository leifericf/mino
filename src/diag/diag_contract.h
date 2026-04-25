/*
 * diag/diag_contract.h -- internal severity classes for runtime errors.
 *
 * The diagnostic namespace (`:eval/...`, `:type/...`, `:io/...`, etc.,
 * defined in diag.c) is what user code sees.  This header introduces a
 * coarser internal taxonomy that drives control-flow policy: which
 * code paths catch, which propagate, which abort.
 *
 * Rule of thumb when reading code that emits an error:
 *
 *   MINO_ERR_RECOVERABLE -- the call site goes through prim_throw_*
 *                           or set_eval_diag.  User code can catch.
 *   MINO_ERR_HOST        -- the call site sets a diagnostic and
 *                           returns NULL; the embedder reads it via
 *                           mino_last_error.  No catch frame is
 *                           expected.  Used for I/O, OS failures,
 *                           and explicit host-side rejections.
 *   MINO_ERR_CORRUPT     -- the call site aborts.  Reaching it means
 *                           an invariant has been violated and no
 *                           recovery is meaningful.
 *
 * Per-subsystem internal headers list which classes they emit, where,
 * and why.  Search those headers for "Error classes emitted" to find
 * the contract for a given subsystem.
 */

#ifndef DIAG_CONTRACT_H
#define DIAG_CONTRACT_H

typedef enum {
    MINO_ERR_RECOVERABLE = 0,
    MINO_ERR_HOST        = 1,
    MINO_ERR_CORRUPT     = 2
} mino_err_class;

#endif /* DIAG_CONTRACT_H */
