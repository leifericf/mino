# ADR 58: Protected eval entry points publish the last error; pcall stays the swallow

Date: 2026-09-07

## Context

The C embedding API offered two error-observation models depending on
which call the host made. The unsuffixed eval entry points
(`mino_eval`, `mino_eval_string`, `mino_load_file`) publish a caught
throw to the observable last-error slot: after a failing call,
`mino_last_error(S)` returns the diagnostic. Their protected `_ex`
twins (`mino_eval_ex`, `mino_eval_string_ex`, `mino_load_file_ex`) did
not; the header documented this ("The _ex variants do NOT publish to
mino_last_error on a caught throw"). So an embedder that switched from
`mino_eval_string` to `mino_eval_string_ex` to disambiguate a genuine
nil result from an error silently lost `mino_last_error` as an
observation channel.

Separately, `mino_pcall`'s header claimed "The error message is
available via mino_last_error()", but the implementation deliberately
does not publish, and a long comment in the catch path explains why:
`mino_pcall` is the runtime's own swallow-and-capture primitive.
Validators (`ref_publish.c`), watch dispatch, agent actions
(`agent.c`), STM retries (`stm.c`), delay forcing (`stateful.c`), and
signal hooks (`signal.c`) all call it to run user code that is allowed
to throw, recover the raw payload through `out_ex`, and continue. If
`mino_pcall` published, a validator rejection during `swap!` would
leave a spurious last-error that the next embedder observation would
misread.

## Decision

Error observation is made uniform across the eval family, and the
low-level swallow is documented honestly.

1. The three `_ex` eval entry points publish the caught throw to the
   last-error slot exactly as their unsuffixed twins do, guarded by the
   same `mino_last_error(S) == NULL` check so an inner diagnostic is
   preserved. A shared `error_publish_caught` helper carries the
   normalize-then-`set_eval_diag` logic so the eval family has one
   publish path. After any failing eval entry point, suffixed or not,
   `mino_last_error(S)` is populated with the same diagnostic and
   `*out_ex` additionally carries the raw payload.

2. `mino_pcall` stays non-publishing: it is the arbitrary-callable
   swallow the runtime's own reference-type machinery is built on, and
   publishing would corrupt those flows. Its header is corrected to say
   so plainly: observation is through the `-1` return and `*out_ex`,
   not the last-error slot; a host that wants a diagnostic published
   after `mino_pcall` calls the eval entry points or sets one itself.

## Consequences

The embedder-facing eval surface now has one error model: every
`mino_eval*` call publishes to `mino_last_error`, and the `_ex` forms
add payload recovery on top rather than trading the slot away.
`mino_pcall` keeps the behavior its internal callers depend on, and its
documentation no longer contradicts its code. The publish helper
removes a copy of the normalize-and-set-diag block; the two
pre-existing inner publish sites keep their file-context-specific
wording and are out of scope for this change.

## Alternatives

Publish from `mino_pcall` too, for total uniformity. Rejected: it would
regress every internal reference-type flow that runs user callbacks
through `mino_pcall` and relies on a clean last-error slot afterward,
turning a documentation bug into a behavior bug across atoms, refs,
agents, STM, delays, and signal hooks.

Leave the `_ex` family non-publishing and document the split as
intended. Rejected: it keeps the exact asymmetry the embedder hits when
moving to `_ex` for nil disambiguation, which is the defect this change
exists to remove.
