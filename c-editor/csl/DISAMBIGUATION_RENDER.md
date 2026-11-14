# Evaluator-backed disambiguation provider

`csl_disambiguation_render` connects the typed [driver](DISAMBIGUATION.md) to
`csl_evaluate` and `csl_finalize`. It is entirely C. It supplies real finalized
collision keys and structured person identities; it does not parse names out of
citation strings. This is the bounded driver profile, not full CSL conformance
or application integration.

`ce_csl_disambiguation_render_provider(config)` creates an allocation-free
adapter. The configuration borrows one immutable style, locale, input and
matching registry. Optional abbreviation and cancellation callbacks are read
only. FIRST and SUBSEQUENT use an empty cite without user locator or affixes;
BIBLIOGRAPHY uses no cite. Citation numbers come from the registry,
first-reference-note-number is resolved absent, and year-suffix is always
resolved, including the empty value that overrides stale source metadata.

Each probe retains its own evaluation and finalization. A names occurrence is
identified by its source XML node and complete macro call path. The provider
copies the resolved initialization capability and call path during evaluation;
it never retains the evaluator's temporary XML option view. Every source binding
and original person index remains available, including placeholders, omitted
people and collapsed editor/translator roles. Only the first nonempty rendered
person in the whole probe is primary.

STYLE keys come from typed final PERSON spans. An INITIALS candidate reevaluates
and finalizes the entire entry with the same context, so parent case conversion,
period stripping, Unicode context and formatting remain in effect. Already
initialized visible names reuse their actual key. Hidden people keep empty
keys; a temporary increased prefix tests their capability without changing the
main probe. A candidate that still has no initialized given name, including
uninitializable CJK names, does not advertise initial expansion. Added-name
trials use `ce_csl_names_result_next_minimum`, preserving every currently visible
name: first-plus-last `A,C` advances directly to `A,B,C`.

A nonempty generated suffix requires an explicit `text variable="year-suffix"`
reachable from the selected section, including macros and unchosen branches,
excluding sort keys. Implicit suffix insertion is UNSUPPORTED. A reference in
an unchosen branch may still leave the actual output ambiguous; the driver
reports that unresolved result. The driver currently supports
`primary-name-with-initials`; this provider does not enable its other four rules.
Unimplemented evaluator/name/inline features remain errors. PERSON spans are
bounding ranges for collision keys within the supported rendering profile;
they must not be used to remove a names field from a citation cluster.

All retained and temporary allocations use the driver-supplied allocator,
including candidate evaluations. They share the driver's 64 MiB workspace;
the provider also enforces 64 MiB per probe. Other per-probe hard defaults are:

| Resource | Limit |
| --- | ---: |
| Final citation or person key | 64 KiB |
| Candidate reevaluations, excluding the main render | 256 |
| Cumulative retained evaluation nodes across renders | 262,144 |
| Source people, including placeholders and repeated occurrences | 16,384 |
| Allocation/traversal/comparison work units | 1,048,576 |

The four configurable counters can only be lowered; zero selects the default.
Child evaluators additionally bound expanded speculative nodes and payloads.
Cancellation is checked during provider work; synchronous child operations
remain bounded by their own limits. Failure returns no partial probe. The driver
preserves its previous result if rebuilding fails.

Release frees finalization before evaluation and does not read the old request,
state, configuration or callback contexts. Immutable source objects and the
allocator context must remain alive until release. Exported strings from the
borrowed finalized-output getter inherit the probe allocator and must be freed
before the probe. Direct rendering requires an initially NULL output pointer.

`test_csl_disambiguation_render` covers full-context Unicode keys, repeated macro
occurrences, original indices, hidden CJK/Latin capability, collapsed roles,
count form, first/subsequent et-al, last-name preservation, explicitly included
uncited items, bibliography propagation, limits, cancellation and every reached
allocation-failure point. Its two-argument mode reads the pinned `apa.csl` and
`locales-en-US.xml`, testing different initials, same-initial year suffixes and
removal of expansions/suffixes after deletion. Native ASan/UBSan and WebAssembly
both pass, including 229 provider and 765 combined driver/provider failure
points in the reviewed snapshot. These are original regression cases, not a
claim that the full upstream conformance suite has passed.
