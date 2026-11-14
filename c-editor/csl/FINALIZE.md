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

When both sides of a boundary supply an ASCII space through affixes or
delimiters, one of those two spaces is removed. This is not general whitespace
normalization: two spaces joined to two spaces leave three, metadata whitespace
is retained, and tabs or nonbreaking spaces are not merged.

Node, person and quotation spans refer to the final UTF-8 bytes. Case expansion,
deletion and punctuation movement update these ranges. A span is a bounding
range and can overlap another span after punctuation movement. Use retained
typed name identities for comparison; ranges are not an instruction to delete
text during citation collapse. Display spans describe boundaries, not page
layout. Keep the source evaluation and its borrowed inputs alive until the
finalized result is freed.

`ce_csl_finalize_with_options` can omit a specified names node belonging to the
evaluation. It recomputes parent delimiters, case and group suppression while
retaining the original chosen branches and substitution consumption. A title
used as a name substitute does not reappear in a later field after the names
are omitted. No text is removed by guessing a formatted name's byte range.

The first-names getters retain an owned snapshot of the first emitted names
element, including its substitute and its own decorations. Whole-parent case
and period removal have already applied, but cross-boundary punctuation has
not. This gives grouping an exact field key even if a later delimiter moves
inside that field's closing quotation mark.

`ce_csl_finalize_join` accepts a planned sequence of citation evaluations and
optional names omissions. It applies the common citation layout envelope once
and performs punctuation cleanup once after joining all pieces. Generated
separators retain their origins; cite prefixes and suffixes are literal UTF-8,
without markup parsing or added spaces. Empty projected pieces do not emit
affixes or separators. Each evaluation uses the same layout and locale object.
Public spans preserve an opaque source-cite index for evaluation content; cite
affixes and the envelope have no public span. The caller retains whole-cite
coverage, including any entries hidden by collapse. Joined results borrow all
source evaluations and expose no standalone evaluation or first-names snapshot.

The result is replaced atomically. A shared 128 MiB allocation budget includes
the result, inline operations, case maps, span storage and punctuation maps.
Payload is bounded to 1 MiB and there are at most 262,144 temporary/public spans.
This budget excludes caller-owned source objects and the previous result, and
does not measure the C stack. Allocation failure preserves the old result.
Joins additionally bound the sum of evaluation node counts to 1,048,576,
counting repeated inputs again before projection validation. This bounds work
even when many large projected pieces produce no output.

This module does not plan citation grouping, inject implicit year suffixes,
decide author or number-range collapse, sort the bibliography, substitute
repeated bibliography authors, or integrate results with editor transactions.
Those require the document processor. A finalized individual entry is not proof
of complete APA, IEEE or GB/T behavior.
