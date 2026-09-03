# ADR 48: The core bootstrap keeps its bare-string throws

Date: 2026-09-03

## Context

ADR 38 bans bare-string throws in library code as "the worst of both
worlds: opaque and unstructured". That premise is imported from canon,
where a thrown string is an alien object: it carries no error class, no
structured data, and generic tooling cannot present it. A review of
`src/core.clj` flagged about twenty bare-string throws mixed in with
`ex-info` sites and asked whether they should convert.

In mino the premise does not hold. `(throw "msg")` produces a complete
diagnostic map:

```clojure
{:mino/kind :user, :mino/code "MUS001", :mino/phase :eval,
 :mino/message "msg", :mino/data "msg",
 :mino/location {:file "...", :line 1, :column 6}}
```

It is classified (`:mino/kind :user`), catchable by kind (ADR 37), and
`ex-message` returns the string. The core sites are programmer-error
validations on bootstrap forms (`num`, `realized?`, `if-let`,
`when-let`, `set!`, and kin), each with a specific message. No caller
dispatches on a domain kind for any of them; a domain kind would name a
category nobody branches on.

## Decision

The core bootstrap keeps its bare-string throws. This narrows ADR 38's
"no bare-string throws" clause to its actual motivation: a raise must be
classified, structured, and specific. In mino a bare-string throw
already is, as `:mino/kind :user`. The ban therefore applies where
callers dispatch by kind and need a domain `:mino/kind` (as already
converted in `mino.cli`, `mino.deps`, `mino.http`, and `clojure.zip`),
not to bootstrap validations whose only consumer is a human reading the
message. New core validations may throw a bare string when the message
is specific and no caller branches on the kind.

## Consequences

No churn across the bootstrap for zero caller benefit. The message
quality bar of ADR 38 still binds every site: a vague bare-string throw
remains a finding. The `:user` kind stays a catch-all, so the day a
caller wants a domain-specific catch on a core validation, that site
converts then, as an ordinary change. Reviewers citing ADR 38 against
`src/core.clj` bare-string throws should cite this record instead.

## Alternatives

**Convert all core throws to domain kinds.** Uniformity: one raise
shape across the whole surface, and future kind-dispatch needs are
pre-paid. Rejected: it renames `:user` to bespoke kinds nobody catches,
touches the bootstrap that everything loads through, and buys nothing a
caller can observe today.

**Ban bare strings but allow `:user` via explicit maps.** Same runtime
value, more ceremony at every site. Rejected: the thrown-string form is
the shorthand for exactly this diagnostic; forbidding the shorthand
while keeping the meaning is style churn.
