# Citation grouping and collapse

`csl_cluster` builds one citation from already sorted, disambiguated semantic
evaluations and their normal standalone finalizations. It does not assign
numbers, derive positions, sort bibliography rows or solve document-wide
ambiguity. These operations belong to the document processor. If regrouping
changes position-dependent rendering, the processor must reevaluate positions
and grouping with a bounded convergence policy; this module does not silently
reinterpret the supplied trees.

The entry array is the starting order. Each entry carries its original cite,
evaluation, matching unprojected finalization, exact generated citation number,
and caller source index. The source index is distinct from the entry-array
index and item identity; `SIZE_MAX` is reserved for the layout envelope.

## Grouping and author-date collapse

The [CSL 1.0.2 grouping rule](https://docs.citationstyles.org/en/stable/specification.html#cite-grouping)
uses the first emitted names element, including its successful substitution.
The finalizer provides an owned field snapshot after parent text-case and
period stripping, before cross-boundary punctuation cleanup. Grouping compares
the snapshot's exact plain UTF-8 bytes. It does not casefold, normalize Unicode,
compare font styling or infer personal identity. Empty fields never group.

A stable merge sort identifies equal fields. Members retain their original
relative order, and each group moves to its earliest original member. Only the
first names field participates; a later editor list does not prevent grouping.
This follows the specification even where the isolated citeproc-js oracle
showed different behavior for second names, substitutions or nonadjacent cites.

`cite-group-delimiter` enables grouping without name omission. With `year`
collapse, subsequent members project away their first names node. Projection
leaves original chosen branches and substitution consumption intact, then
recomputes visible parent groups and delimiters. Thus a consumed substitute
does not reappear elsewhere. Locators, other names, and cite affixes remain.
An empty projected entry contributes no separator or affixes.

Within a group, the default separator is `, `. Between groups, the layout
delimiter applies; after a collapsed visible group, `after-collapse-delimiter`
applies, defaulting to the layout delimiter. Entirely empty groups do not alter
the delimiter selected for the next visible group.

The `year-suffix` and `year-suffix-ranged` modes fall back to `year` only when
`disambiguate-add-year-suffix` is disabled, as specified. Enabled suffix collapse
is currently an explicit `UNSUPPORTED` result.

## Numeric collapse

Numeric collapse scans the supplied sequence without sorting or deduplicating
it. At least three consecutive, strictly increasing exact `uint64_t` numbers
form a range; pairs remain separate. Repeated numbers, descending sequences,
gaps and overflow boundaries terminate a run. The generated number must match
the actual evaluated citation-number variable.

The first release supports entries whose visible content consists of a single
citation-number leaf and its style containers/affixes. Other visible variable
or literal leaves interrupt a range. The number's semantic source/branch path
must match between endpoints, preventing a conditional middle prefix from
being lost. This is a conservative supported profile: a separate constant
`<text value="Ref "/>` keeps the citations uncollapsed, while a number's
`prefix="Ref "` can remain on both complete endpoints.

A prefix is permitted on the first range member; later prefixes and any suffix
terminate a run. A locator is a barrier only when it actually renders. Merely
having a locator in input does not prevent a range when the style omits it.
The complete first and last semantic entries remain, joined by U+2013 as
required by [CSL range delimiters](https://docs.citationstyles.org/en/stable/specification.html#range-delimiters).
This preserves both `[1]–[3]` for individual number affixes and `[1–3]` for a
single surrounding layout envelope. No nonstandard range-delimiter term is
introduced. The supplied IEEE style has no collapse option and stays separate.

## Affixes, flags, and provenance

`suppress-author` uses the same first-names projection. `author-only` is always
explicitly unsupported in this first release; a grouping key is not used as a
replacement for the actual author-only rendering. Cluster-level extension
properties such as `mode`/`composite` belong to document orchestration and must
not be silently ignored there.

Cite prefixes and suffixes are literal input bytes. They are not parsed as
markup, trimmed, or padded with spaces. Callers must provide intended spacing.
This deliberately does not emulate citeproc-js's extra affix-spacing rules.

The finalizer rebuilds planned semantic pieces, keeps generated separators and
quote origins through one punctuation pass, and applies the citation layout
envelope once. No final output string is reverse-parsed or cut to remove names.
Final spans include the caller's source cite index. Group records cover every
member in the stable permutation, including numeric interior members hidden
by a range and entries that become empty. Piece records identify rendered
endpoints and omission decisions. Consumers can associate a displayed range
with all its covered original entries without inventing an intermediate cite.

## Ownership, limits, and tests

Results own the final output and all planning arrays. Evaluations and their
borrowed input/style/locale sources must remain alive while semantic span
views are used. The entry array and baseline finalizations are borrowed only
during the build. Caller allocators are per object; no process-global hooks
change. Every error preserves the previous live result and all source data.

Limits are 100,000 input entries, 64 KiB per grouping field, 16 MiB aggregate
grouping fields, 128 MiB compared bytes, 16 million work units, and 256 MiB live
workspace. Options can lower these limits. Finalizer limits additionally apply
across the entire joined citation, including its 1 MiB output cap. Cancellation
is checked while planning and between finalizer calls, not inside one bounded
finalizer invocation. Child rendering errors remain distinct from planner
validation, unsupported features, limits, OOM and cancellation.

Native strict C11 ASan/UBSan and actual WebAssembly tests exercise 70 independent
cases, 336 injected allocation failures, cancellation after final rendering,
and a 4,096-entry equal-name group. Allocation failure tests use a nonstandard
allocator prefix to verify matching frees, no leaks, and unchanged old results.
The real bundled styles produce these verified clusters:

* APA: `(Doe, 2019, 2020; Roe, 2021)`.
* IEEE: `[1], [2], [3]`.
* GB: `[1–3]`, with superscript formatting retained throughout.

These are concrete profile tests, not a claim of full CSL or whole-document
conformance. Official fixtures were read for research only; tracked tests and
black-box query inputs are independently authored. The JavaScript oracle stays
in ignored research files and is never linked into the pure C implementation.

For the CMake owner: add `cluster` to `CSL_MODULES`, its test to the 1 MiB
WebAssembly stack/resource embedding lists, and a real-profile test with the
three style and two locale resource arguments used by `test_csl_evaluate`.
