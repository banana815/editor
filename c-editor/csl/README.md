# C citation processor — development module

This is the beginning of an original C implementation of CSL processing. It is
not connected to the editor and is not yet a complete document citation processor. The existing
editor continues to label its output as approximate. No JavaScript interpreter
or third-party JavaScript formatter will be part of this implementation.

The current modules provide:

- Namespace-aware XML loading with size/depth limits, preserving affix spaces
  and Unicode. DTDs and external entities are rejected; no file/network lookup.
- Independent-style validation and indexed macros, including missing references
  and cycles in unused branches. This is not a complete schema validator.
- Locale fallback, explicit empty terms, plural/gender forms, ordinal sets,
  localized date templates and independently inherited locale options.
- Unicode 18 NFC/NFD and full upper/lower/casefold mappings, including contextual
  Turkish/Azeri/Lithuanian and Greek sigma behavior, plus full scalar title
  mappings and source-to-output maps for expanding/deleting case conversions.
- Owned formatted text runs, bounded output and escaped plain/HTML exports.
- Ordered document citation clusters and CSL items, with exact identity checks,
  preserved typed metadata, resolved references and atomic input replacement.
- Locale-based scalar number forms, numeric-field extraction and exact decimal
  page-range pairs, including distinct Chicago 15/16 shortening rules.
- Inherited formatting, distinct light/oblique runs, nonempty-only affixes and
  condition/branch evaluation with explicit cite-position context.
- CSL plain-text case conversion with pinned stop words and source maps, and
  date rendering with locale templates, ranges, seasons and preserved formatting.
- Structured personal/institutional names, initials, particles, name-list
  truncation, role labels and retained source identities for later disambiguation.
  Pinned Unicode Script data supplies CJK name-order classification.
- Shared typed variable lookup, explicit empty/suppressed values, long/short
  fallback, generated-number slots and structured editor/translator equality.
- Stable precomputed-key sorting, exact signed integer and chronological date
  keys, and bounded full-casefold/NFD Unicode DUCET collation in C.
- Document-derived first/subsequent/ibid and near-note positions, with explicit
  note-number reset policy and source-index queries after citation reordering.
- Bibliography eligibility and citation numbering: cited items by default, plus
  explicitly included items, with stable initial order and an external sort
  permutation. Importing a library alone never includes every item in a document.
- Rich metadata fields with controlled font tags and protected case, scalar
  slicing and source maps through period removal and whole-parent case changes.
- Semantic style evaluation with transactional substitution, typed leaf values,
  macro-call occurrences, group suppression and retained name/date identities.
- Entry finalization with nested locale quotation marks, generated punctuation
  boundaries and mapped name/display ranges. [FINALIZE.md](FINALIZE.md) documents
  the supported cleanup rules and the remaining document-level work.
- A bounded document-wide disambiguation driver with typed rendering callbacks,
  primary-name initials, added names, conditional branches and ordered year
  suffixes. [DISAMBIGUATION.md](DISAMBIGUATION.md) describes the initial supported
  rule and the required real-renderer integration; mock-provider checks do not
  establish citation-format conformance.
- Typed stylesheet sort-key evaluation and stable matrix ordering, including
  the pinned profiles' keys, structured name components and selected date parts.
  [SORT_EVALUATE.md](SORT_EVALUATE.md) lists unsupported mixed-key cases and
  encoding checks; keys are not recovered by parsing formatted names or dates.

These components still need document-wide disambiguation integration, citation
collapse and document integration. Date parsing currently rejects
mixed-precision intervals and unsupported raw date syntax; Roman numerals are
limited to 1–3999. Automatic name-particle parsing is not implemented; explicit
`parse-names=true` is rejected. The default collation does not provide Chinese
pinyin or other CLDR language tailoring. Accepting an APA stylesheet
does not establish that its citation or bibliography output is implemented.

```sh
cmake -S c-editor/csl -B c-editor/build-csl -DCSL_SANITIZERS=ON
cmake --build c-editor/build-csl --parallel 4
ctest --test-dir c-editor/build-csl --output-on-failure
```

Dependencies are pinned in `../cmake/CSLDependencies.cmake`: Expat 2.10.0 for
XML parsing and utf8proc 2.12.0 for Unicode primitives. Both are C libraries.
Mojibake 0.7.0 supplies Unicode collation through a verified C-only source subset;
[COLLATION.md](COLLATION.md) documents the normalization workaround, limits,
allocator boundary and exact profile. Its MIT notice and pinned source manifest
are included under `licenses/` and `data/`.
The input model reuses the editor's pinned cJSON 1.7.19 from
`../cmake/Dependencies.cmake`. Expat/utf8proc licenses are in `licenses/`,
cJSON's notice is in `../licenses/cJSON.txt`, and generated Unicode data records
its upstream hashes and license under `data/`. The CSL schema's stop-word list
has its own source manifest and MIT notice in `licenses/CSL-Schema.txt`.
Unicode normalization/casing and default DUCET collation do not implement
locale-specific collation tailoring. [POSITION.md](POSITION.md) records the
separate in-text/note histories and the supported position policy.

Processing must operate on the whole document's ordered citation clusters,
not merely format each bibliography item independently. The planned result
includes per-cluster citation runs and the ordered bibliography, with structured
formatting and separate plain-text/HTML serializations. The document adapter
will apply a successful result as one undoable transaction; unsupported styles
or failed processing must not silently become approximate output.

The implementation is guided by the [CSL 1.0.2 specification](https://docs.citationstyles.org/en/stable/specification.html).
Matching a few bibliography examples will not establish full conformance.

The native suite runs with ASan/UBSan when enabled; the same C fixtures also
compile and execute as WebAssembly. Allocation-failure tests cover each module's
documented hooks, not every transitive library allocation. The input model's
numeric identity checks use original decimal tokens before conversion to double;
ordinary metadata numbers remain cJSON doubles and are not lossless JSON text.

The profile integration suite checks 36 representative citation/bibliography
texts and 540 typed evaluation/finalization combinations against the pinned
styles. Its test adapter wraps a single citation layout and represents aligned
bibliography fields with a space; the application document adapter is still
pending. One recorded processor difference is intentional: a family name-part
case rule does not uppercase a non-personal literal name. Multi-citation sorting,
disambiguation and collapse are not established by these single-entry examples.

The dedicated `C citation processor` workflow runs this independent module on
Windows, macOS, Linux and WebAssembly, plus mobile cross-compilation. Mobile
library compilation is not mobile runtime conformance. Until this library is
connected to the application, its changes are tested separately from the SDL
editor workflow; shared dependency changes continue to exercise both workflows.
