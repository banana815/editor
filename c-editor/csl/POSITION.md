# Derived citation positions

`ce_csl_positions_derive` consumes an immutable `CeCslInput` and returns position
bitmasks plus generated `first-reference-note-number` values. It changes neither
the input JSON nor its indexes. Position facts embedded in input are ignored.
The masks include the implications used by `csl_condition`: ibid-with-locator
includes ibid and subsequent, and near-note includes subsequent. Bibliography
evaluation must suppress all position tests, as `csl_condition` already does.

Optional cite permutations select rendering order within each cluster. They
contain original global flat cite indices, and each cluster's segment must
contain exactly that cluster's indices. Invalid, repeated, missing, or
cross-cluster indices fail. Results retain original input order, so an evaluator
can query a result by the original cluster and cite indexes after sorting.

## Note and adjacency policy

The CSL specification describes adjacency within a citation and across
singleton citations. Primary processor fixtures additionally establish separate
in-text and footnote histories and a stricter test across whole footnotes.
This module uses the following explicit policy:

- Zero `noteIndex` belongs to the in-text stream; positive values belong to the
  note stream. First/subsequent and ibid histories are independent. A note cite
  does not interrupt the in-text stream, or vice versa.
- Within a cluster, only the immediately preceding rendered cite can establish
  ibid. Between in-text clusters, the previous cluster must contain one cite.
- Within one positive note, clusters form a continuous sequence: the previous
  cluster's last rendered cite can establish ibid. Across notes, the numbers
  must be consecutive and the entire previous note must contain exactly one
  cite. Two singleton clusters in that previous note therefore prevent ibid.
- An otherwise adjacent match with a new or changed locator is ibid-with-locator.
  Identical locators give ibid. Removing a prior locator gives subsequent only.
  Locator and label are compared separately, byte-exactly; an absent/empty
  locator is absent, and an absent/empty label means `page`. Affixes and cite
  author-display flags do not alter identity.
- Near-note is available only in positive notes. It compares the latest prior
  note citing the same item with the current number. Distance defaults to five;
  an explicit zero permits repeats in the same note only.
- First-reference note numbers are generated only for a note style, only when
  an earlier note in this numbering section first cited the item. They are
  absent in the first note itself, including later cites in that same note.
  The style class does not change the other position facts.
- A decreasing positive note number returns an unsupported error by default.
  `allow_note_number_reset` explicitly opts into treating a decrease as a new
  section, clearing note history while retaining in-text history. Zero notes
  are not resets. Epoch tracking avoids repeatedly clearing the item array.

This is a derivation module, not the full citeproc-js integration API. Parallel
legal citations and special cluster modes such as author-only clusters are not
implemented. An application must validate or reject unsupported integration
features before evaluating them; ordinary cite-level author-display flags are
retained by input and do not redefine source identity here.

All allocations use the supplied per-call allocator (or malloc/free). No global
hooks are changed. Failure preserves the old result; success replaces it
atomically. Output has no borrowed input pointers and can outlive the input.
The input module's item/cluster/cite caps bound memory. Traversal is linear in
those counts plus locator comparisons; it is not a cancellation or time budget.

## Evidence and tests

The policy was checked against the [CSL specification's position conditions](https://docs.citationstyles.org/en/stable/specification.html#choose)
and [generated number variables](https://docs.citationstyles.org/en/stable/specification.html#number-variables).
Primary fixtures were inspected at test-suite commit
`6eefc5b07c6969ab8999e48542acbcc131cba864`:
[IbidInText](https://github.com/citation-style-language/test-suite/blob/6eefc5b07c6969ab8999e48542acbcc131cba864/processor-tests/machines/position_IbidInText.json),
[IbidWithMultipleSoloCitesInBackref](https://github.com/citation-style-language/test-suite/blob/6eefc5b07c6969ab8999e48542acbcc131cba864/processor-tests/machines/position_IbidWithMultipleSoloCitesInBackref.json),
[IbidSeparateCiteSameNote](https://github.com/citation-style-language/test-suite/blob/6eefc5b07c6969ab8999e48542acbcc131cba864/processor-tests/machines/position_IbidSeparateCiteSameNote.json),
and [NearNoteSameNote](https://github.com/citation-style-language/test-suite/blob/6eefc5b07c6969ab8999e48542acbcc131cba864/processor-tests/machines/position_NearNoteSameNote.json).
These fixtures remain research material and are not copied into this module.

An independent local black-box probe against the existing citeproc 2.4.63
research installation confirmed first-position separation: one item cited in
note sequence `0,1,0,2` produced `first,first,ibid,ibid`, with first-reference
note 1 on the last cite. This research engine is not a product dependency.

The newly written C tests cover both streams, multi-cluster notes, permutations,
locator identity, note-distance boundaries, explicit resets, maximum uint32
note numbers, immutable input, empty documents, all three allocation failure
points with a non-malloc-compatible allocator, and 3,000 numbering sections.
They run under native ASan/UBSan and actual WebAssembly.
