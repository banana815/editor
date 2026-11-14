# Semantic entry finalization

`ce_csl_finalize` turns one evaluated tree into owned inline text and controlled
output runs. It rebuilds from leaf bodies and retained decorations, preserving
protected case, font changes, and source identities. It does not parse the
evaluator's preview back into names or dates.

Child output is assembled before a parent's case or period-stripping operation.
The operation applies to the complete text, including Unicode context, while
`nocase` protects case only. The current node's affixes are added afterwards.
Style-generated affixes use the inherited font; a bibliography layout uses its
effective font. A citation layout's envelope belongs to the cluster assembler.
`choose` retains the enclosing delimiter; groups and macros keep their own
boundaries. Substitution uses only the winner already selected by evaluation.

Quotation terms alternate between outer and inner locale terms by semantic
nesting depth. Missing terms remain missing and set a diagnostic; no English
quotation mark is invented. `punctuation-in-quote` moves an immediately following
style-generated comma or period before adjacent closing quotation marks.
Whitespace prevents the move. A comma belonging to the next metadata field
keeps its position. Question marks, exclamation marks, semicolons and colons are
not relocated by this implementation.

Punctuation cleanup is restricted to generated structural boundaries. Matching
ASCII punctuation is coalesced; a following period is suppressed after a colon,
semicolon, question mark or exclamation mark. Comma coalescing requires an affix
or delimiter origin on at least one side. Literal internal repetitions such as
ellipses are preserved, and U+2026 is not treated as three ASCII periods. These
are explicitly bounded cleanup rules, not a claim to reproduce every historical
processor's punctuation rearrangement.

Node, person and quotation spans refer to the final UTF-8 bytes. Case expansion,
deletion and punctuation movement update these ranges. A span is a bounding
range and can overlap another span after punctuation movement. Use retained
typed name identities for comparison; ranges are not an instruction to delete
text during citation collapse. Display spans describe boundaries, not page
layout. Keep the source evaluation and its borrowed inputs alive until the
finalized result is freed.

The result is replaced atomically. A shared 128 MiB allocation budget includes
the result, inline operations, case maps, span storage and punctuation maps.
Payload is bounded to 1 MiB and there are at most 262,144 temporary/public spans.
This budget excludes caller-owned source objects and the previous result, and
does not measure the C stack. Allocation failure preserves the old result.

This module does not assemble citation clusters, inject implicit year suffixes,
collapse repeated authors or number ranges, sort the bibliography, substitute
repeated bibliography authors, or integrate results with editor transactions.
Those require the document processor. A finalized individual entry is not proof
of complete APA, IEEE or GB/T behavior.
