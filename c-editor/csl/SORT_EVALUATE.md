# Typed CSL sort keys

`csl_sort_evaluate` evaluates one original `cs:key` against the immutable
environment used by `csl_evaluate`. `ce_csl_sort_evaluated_order` validates a
row-major key matrix and delegates to the existing stable binary sorter.
Directions come from each key. Empty values remain last in both directions;
complete ties retain their original row order. Citation numbers and other
generated fields must already be resolved by the caller.

The implementation follows the [CSL 1.0.2 sorting rules](https://docs.citationstyles.org/en/stable/specification.html#sorting)
and the [name-part sorting rules](https://docs.citationstyles.org/en/stable/specification.html#name-part-order).
The tests are independently written. Research against the pinned official test
suite informed selected-date and name-count cases; no suite fixture is copied.

## Supported keys

* Text variables lose recognized rich-text markup through the existing inline
  parser. Text-only macros use the actual finalizer output, including its
  punctuation, affixes and text-case behavior. Both use the documented
  [DUCET collation adapter](COLLATION.md): case-insensitive, accent-sensitive,
  with explicit default/Turkish/Azeri folding, and no CLDR locale tailoring.
* Direct name variables include the complete LONG list, without et-al
  truncation or local initialization. Macro names retain initialization,
  substitution, short/count form and truncation, with `cs:key` overrides.
  Name labels and et-al text do not contribute. Typed `CeCslNameSortView`
  components provide the family/particles/given/suffix ordering; names are
  never reconstructed from rendered text. Literal names lose an initial
  case-insensitive English `a`, `an` or `the` followed by ASCII whitespace.
* Each person supplies four independently collated tuple components. Literal
  names and text fallback supply their entire text as the first component and
  three empty components. Consequently an author's title substitute and a
  personal-name branch have compatible lexical keys. A title fallback does
  **not** lose its initial English article.
* Direct number variables, macro `cs:number`, and names in COUNT form use
  exact signed integer keys. Integer spelling is optional `+`/`-` followed by
  at most 4,096 ASCII digits. Roman/ordinal display forms on `cs:number` do not
  affect numeric sorting. Wholly nonnumeric values produce lexical keys.
  Ordinary `cs:text` in a macro remains text, even when its variable is numeric.
* Dates use shared typed source views, with macro masks taken from the actual
  evaluated date node. Year-only and year/month keys zero their unselected
  trailing parts and share a chronological domain with full dates. BC dates
  sort chronologically, seasons are ignored, and range endpoints break ties
  after their start, with single dates before ranges. Month-only, day-only,
  and month/day masks each have a separate compatible domain, independent of
  the original year.

## Explicit limitations

This is an initial typed-key profile, not general CSL sort conformance.
Unsupported cases fail and preserve the caller's prior output:

* Numeric ranges, prefixes, decimals and other mixed numeric/text values.
  A column containing both integer and wholly nonnumeric values is also
  rejected: no arbitrary cross-type order is invented.
* Literal/open-ended dates, year/day selections without month, and the date
  parser/renderer restrictions documented in `csl_date.h`. A column cannot
  mix distinct no-year date masks or a no-year mask with a chronological key.
* Macros with multiple active typed leaves, or a typed leaf mixed with other
  active text. A single typed leaf with outer affixes, quotation, text-case or
  period stripping is rejected rather than silently discarding decoration.
  Name-part formatting already represented by SortView remains supported.
* No document-level sort program, bibliography eligibility, numbering,
  disambiguation or locale selection is inferred by this module.

The order adapter checks domain, lexical collation language and direction
compatibility before any comparison. Callers bypassing that adapter must make
the same checks before using `ce_csl_sort_order` directly.

## Ownership and bounds

All outputs replace atomically and use per-object allocators; no global hooks
are changed. Each evaluation shares a 256 MiB allocation budget across its
child modules, excluding input objects, the prior result and final owned key
copy. Existing evaluator/finalizer/inline budgets also apply. Name components
have an aggregate 64 KiB source-text limit; all keys obey the existing 512 KiB
encoded-key cap. Collation retains its own normalization/combining limits and
its documented distinction between caller allocations and Mojibake internal
temporaries. Keys are binary and may contain NUL; semantic missingness is
independent of zero collation weights.

## Verification and integration

Native strict C11 with ASan/UBSan and actual Node-hosted WebAssembly tests cover
typed order, exact large integers, negative years, ranges, seasons, selected
parts, full direct name lists, short/count macro names, substitution, article
removal, text finalization, incompatible domains and 316 injected allocation
failures. A non-`malloc`-compatible allocator checks correct freeing and atomic
replacement, including the matrix adapter.

The pinned real APA style passes all four citation keys and all ten
bibliography keys over five independently authored records, including missing
author/title substitution, editor substitution, dates, volume/part number and
archive data. Real IEEE and GB numeric citation sort keys pass. This verifies
those concrete input/profile cases, not all metadata shapes accepted by each
style or their full rendered citation conformance.

For the standalone CMake owner: add `sort_evaluate` to `CSL_MODULES`, include its
test in the 1 MiB WebAssembly stack and resources-embed lists, and register the
real-profile test with the same three style and two locale paths passed to
`test_csl_evaluate`. The test accepts either no arguments for isolated cases or
exactly those five resource arguments.
