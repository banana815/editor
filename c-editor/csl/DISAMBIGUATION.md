# Typed disambiguation driver

This module plans document-level name adjustments, conditional disambiguation,
and year suffixes. It is not a citation renderer. Its provider must supply exact
collision keys after the real evaluator and finalizer, plus typed name metadata.
`CeCslEvaluation` previews must not be used as finalized keys. The independent
mock-provider tests validate the driver algorithm, not APA output or full CSL
conformance. The separate [evaluator-backed provider](DISAMBIGUATION_RENDER.md)
now supplies real finalized keys and has its own pinned-APA integration tests.
The library is not yet connected to the editor application.

The registry determines eligibility and final bibliography order. Explicitly
included uncited entries receive invisible citation probes and participate in
disambiguation. Caller-selected FIRST and SUBSEQUENT probes cover canonical
forms without locator, affix, author-only or ibid context noise. The latter is
needed when a subsequent et-al form can collide even though first cites differ.
BIBLIOGRAPHY probes receive the same per-item branch and suffix state, plus
source-variable/person-index name adjustments needed by the citation forms.

The first supported given-name rule is `primary-name-with-initials`. It groups
typed first-name identities and actual display keys across eligible items, even
when different years already distinguish the cites. Only the first rendered
name is a target. Repeating that person in a secondary position does not acquire
the primary override. Initials that cannot distinguish different identities do
not expand the names; full given names are not substituted as a fallback.
`can_initials` must reflect the resolved name options, including `initialize`
and the presence of `initialize-with`. The other four rule enums return
UNSUPPORTED when given-name expansion is enabled. An inactive default `by-cite`
option does not prevent other disambiguation methods.

Method order is given names, added names, conditional disambiguation, suffixes.
Names are added to every member of an ambiguous set using the provider's next
effective prefix count. Successful partitions are retained; later ineffective
trials roll back to that state. Source identities are never inferred from output
strings. The initial implementation explicitly rejects an added-name trial that
merges previously separate collision groups; solving those non-monotone cases
is outside this profile. It does not silently choose a string approximation.

Remaining sets receive suffixes in final registry row order, independently for
each set: a through z, then aa, ab, and so on. The provider must actually render
the generated suffix. Reprobing detects ineffective branches or invisible
suffixes. Remaining collisions are exposed as unresolved groups; an otherwise
valid style that cannot distinguish its entries is not reported as resolved.
Bibliography keys also participate in the final unresolved diagnostic.

Every build starts from the original style state. A later build after deleting
an item can remove initials, extra names, branches and suffixes. An empty suffix
is explicitly resolved and must override stale source metadata in the variable
resolver. Result adjustments use the evaluator's XML source plus complete macro
call path, with owned copies of the paths. Source nodes remain borrowed from the
same immutable style revision. Results must not be applied to a different input
or style revision.

All driver and provider heap allocations share a counting allocator. The hard
workspace limit is 64 MiB, including allocation headers and live probe objects.
Collision/name keys are at most 64 KiB. Cumulative limits separately bound
200,000 render calls, 500,000 inspected name occurrences, one million state
changes, 128 MiB of emitted metadata, 128 MiB of compared key/identity bytes,
and 16 million driver work units. The work counter includes empty-key sorting
and traversal. Limits may be lowered; zero selects the default. Cancellation is
checked during driver work. A provider must use the supplied allocator and
independently bound its internal work and check the shared cancellation signal;
the driver cannot preempt arbitrary callback code. No global hooks are changed.

Output replacement is atomic on success, including when the new result has
unresolved groups. Invalid options, unsupported features, limits, cancellation,
provider errors, and allocation failure retain the previous result. The C tests
cover global initials, secondary-name exclusion, typed macro occurrences,
bibliography propagation, useful and ineffective name expansion, branch/suffix
ordering, invisible entries, subsequent forms, 54 suffixes, reversible rebuilds,
all allocator failures, and bounded error paths. They run under native
ASan/UBSan and WebAssembly.

The behavior was designed against the
[CSL 1.0.2 disambiguation specification](https://docs.citationstyles.org/en/stable/specification.html#disambiguation)
and primary processor fixtures inspected at
[test-suite commit 6eefc5b](https://github.com/citation-style-language/test-suite/tree/6eefc5b07c6969ab8999e48542acbcc131cba864).
Fixture source was not copied into these tests. The evaluator-backed provider
also verifies full-context initial keys, explicit year suffixes, bibliography
propagation and reversible deletion rebuilds using real C rendering. Its limits
and unsupported behavior are recorded in [DISAMBIGUATION_RENDER.md](DISAMBIGUATION_RENDER.md).
Remaining given-name rules, implicit suffix insertion, full document processing
and editor integration require their own implementation and end-to-end verification.
