# Immutable CSL document revisions

`csl_document.h` composes the C modules into one bounded document operation.
It accepts a parsed style, locale and `CeCslInput`, and returns owned citation
output and bibliography entries. It does not read files, access Zotero, mutate
editor state, execute JavaScript or start network requests.

## Processing order

1. Build eligibility from the first occurrence of each cited item in input
   cluster/cite order. Explicit `include_items` append uncited items in caller
   order, with duplicates removed. Other library items remain ineligible.
2. Evaluate bibliography sort keys over those rows. Stable sorting determines
   their one-based `uint64_t` citation numbers; absent numbers are zero.
3. Run document disambiguation against actual semantic evaluation and finalizer
   output for first, subsequent and, when present, bibliography contexts. Name
   overrides, disambiguation branches and year suffixes are retained per item.
   Year suffix order follows final bibliography order.
4. Evaluate each citation's style sort keys using the assigned numbers. Save
   this original sorted order independently of the eventual grouping order.
5. Derive positions from the current grouped order; render each citation from
   the saved style-sorted order; then group/collapse its semantic entries.
   Repeat until the grouped order is unchanged, up to four passes. This allows
   a group to separate again if position-dependent names change. A cycle or
   exhaustion fails explicitly instead of retaining an inconsistent result.
6. Stream bibliography evaluations into independently owned row/cell entries.
   Empty entries remain eligible and keep their number and source item index,
   but have no visible rows. No citation is required for explicitly included
   bibliography items.

Citation sorting never feeds back into first-use numbering. Each citation's
`cite_order` contains **original within-cluster indices**, including every cite
hidden inside a numeric range. It is not a bibliography row index or item ID.
Thus an input `[C, B, A]` can return `[1, 2, 0]` after first-use numbering and
numeric sorting, while still covering all three original citations.

## Ownership and replacement

Initialize `CeCslDocument *result = NULL`, then call `ce_csl_document_build`.
Success installs a complete new revision and frees the previous result.
Any unsupported construct, budget failure, cancellation or allocation failure
leaves the old result and all input data unchanged. Rebuild from original
immutable input after edits; do not patch old suffixes or numbers into new item
data. Removing a citation can remove eligibility, undo name expansion, clear
suffixes and renumber later items even if the removed item remains in the
library.

Keep the source input, style and locale alive and immutable until their document
result is freed. When replacing a revision, retain the old sources until the
build call has succeeded and freed the old result. Exported citation views and
bibliography views are borrowed from the document. Copy/export them before
freeing it. The bibliography entries themselves contain owned controlled
output and numeric provenance, with no transient evaluation pointers.

The supplied allocator and its context must outlive the result. Direct
`ce_csl_output_plain/html` exports from borrowed citation outputs or bibliography
cells inherit an internal tracking allocator: release those strings **before
freeing the document**, even when the outer allocator is malloc/free.
`ce_csl_document_citation_plain/html` instead accepts an independent allocator;
its default-allocated strings can outlive the document. Aggregate bibliography
plain/HTML exports likewise accept an independent allocator.
Options, include arrays and callback
contexts need survive only the build call. Callbacks must be read-only and
deterministic for that revision. No process-global allocator is changed.

## Positions and limits

Positive note numbers and in-text citations use the independent history rules
documented in `POSITION.md`. Input `position`, `near-note`, citation numbers,
year suffixes and first-reference-note numbers do not override generated
document state. Note-number decreases fail by default;
`allow_note_number_reset = true` explicitly starts a new note epoch.

Hard defaults, which a caller may lower but not raise:

| Budget | Default |
| --- | ---: |
| Shared operation workspace | 256 MiB |
| Retained citation and bibliography text | 64 MiB |
| Sort key / disambiguation probe / citation / bibliography evaluations | 100,000 |
| Total sort cells across bibliography and citations | 1,000,000 |
| Position/grouping passes | 4 |

Input, an old live result, and dependency exceptions documented by the individual
modules are outside the workspace cap. In particular, the default Mojibake
allocator's internal temporary allocations are not intercepted. The provider's
initials variants and cluster finalization/projections have additional module
bounds; they are not each counted as a document evaluation. Cancellation is
checked between bounded component calls, including before committing the final
result, not inside arbitrary caller callbacks. Completed failed passes consume
the evaluation budget; output bytes count the retained final pass plus the
bibliography.

## Explicit support boundaries

This composes the supported profiles of the modules; it does not claim full CSL
1.0.2 support. See `DISAMBIGUATION.md`, `SORT_EVALUATE.md`, `CLUSTER.md`, and the
evaluator/finalizer/bibliography headers for their exact restrictions.

- Only `primary-name-with-initials` is currently implemented when given-name
  disambiguation is enabled. A disabled option does not require that rule.
- Implicit year-suffix insertion is not implemented. An active suffix needs a
  reachable explicit `text variable="year-suffix"` in each affected section.
- Sort keys depending on positions, first-reference-note numbers, disambiguation
  tests or year suffixes are rejected, including indirect macro dependencies.
  Bibliography sorting by the numbers it would itself reassign is conservatively
  rejected. Citation sorting by assigned citation number is supported. Unrelated
  macros outside the reachable sort graph do not trigger this check.
- Bibliography `subsequent-author-substitute` is not implemented. Author-only
  citations and cluster `properties.mode` / enabled `properties.unsorted`
  extensions are rejected explicitly. Suppress-author uses semantic omission.
- Numeric and year collapsing follow `CLUSTER.md`; unsupported active
  year-suffix collapse and nonconverging position/grouping interactions fail.
- Style-specific unsupported sorting, rendering, display or name behavior also
  fails without approximate text sorting, string deletion or silent fallback.

## Verification

`tests/test_csl_document.c` contains independently written regressions for
eligibility, stable numbering and sort mappings, explicit inclusion, empty
bibliography output, suffix deletion, global initials, note streams and resets,
position-sensitive grouping convergence and a cycle, source immutability,
allocator ownership, limits, and cancellation/failed-replacement atomicity.
Its failure injector visits every allocation in a representative complete
revision, and its cancellation test visits every observed callback checkpoint.

The optional five resource arguments are, in order, the pinned APA, IEEE and
GB/T 7714 numeric styles followed by the English and Chinese locale XML files.
Those tests exercise real whole-document citation output, bibliography exports,
source rows and original-cite mappings. They are distinct from mocked provider
tests and do not establish conformance for arbitrary CSL styles.

The standalone CMake test names are `csl_document` and `csl_real_document`.
Run them in the native ASan/UBSan build and the actual Emscripten/Node build;
the WASM build embeds the pinned resource directory and uses a 1 MiB stack,
as do the existing real-profile integration tests.

```sh
ctest --test-dir c-editor/build-csl -R '^(csl_document|csl_real_document)$' --output-on-failure
ctest --test-dir c-editor/build-csl-wasm -R '^(csl_document|csl_real_document)$' --output-on-failure
```

The reviewed snapshot passes 60 owned cases in both runtimes, including 6,515
injected allocation failures and 6,162 individually cancelled checkpoints.
This includes independently allocated citation exports that remain readable
and releasable after the document and all source objects have been freed.
