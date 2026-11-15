#ifndef CE_CSL_FINALIZE_H
#define CE_CSL_FINALIZE_H

#include "csl_evaluate.h"

#define CE_CSL_FINALIZE_MAX_BYTES CE_CSL_OUTPUT_MAX_BYTES
#define CE_CSL_FINALIZE_MAX_SPANS ((size_t)262144)
#define CE_CSL_FINALIZE_MAX_WORKSPACE ((size_t)128 * 1024 * 1024)
#define CE_CSL_FINALIZE_MAX_JOIN_NODES ((size_t)1048576)

typedef struct CeCslFinalized CeCslFinalized;
typedef struct CeCslFinalizeOptions {
    /* Optional names node from THIS evaluation to omit during projection.
     * The original evaluation, substitutions and chosen branches remain
     * unchanged. Parent delimiters, case and group suppression are recomputed.
     * Intended for citation-group year collapse, not general source filtering. */
    const CeCslEvalNode *omit_names;
} CeCslFinalizeOptions;
typedef struct CeCslFinalizeJoinEntry {
    const CeCslEvaluation *evaluation;
    const CeCslEvalNode *omit_names;
    const char *prefix, *suffix; /* Cite data: literal bytes, no added whitespace. */
    const char *before_delimiter; /* Generated separator; ignored for first output. */
    size_t source_cite_index;
} CeCslFinalizeJoinEntry;
typedef enum CeCslFinalSpanKind {
    CE_CSL_FINAL_NODE, CE_CSL_FINAL_PERSON,
    CE_CSL_FINAL_OPEN_QUOTE, CE_CSL_FINAL_CLOSE_QUOTE
} CeCslFinalSpanKind;
typedef struct CeCslFinalSpan {
    const CeCslEvalNode *node;
    CeCslFinalSpanKind kind;
    size_t offset, length; /* Bounding range in the final plain UTF-8 bytes. */
    /* PERSON: index into this node's NamesResult rendered-person metadata.
     * Other kinds use SIZE_MAX. Never reverse-parse a person's output text. */
    size_t person_index;
    CeCslEvalDisplay display;
    size_t source_cite_index; /* SIZE_MAX for standalone output/layout envelope. */
} CeCslFinalSpan;

/* Finalizes ONE semantic evaluation using its retained environment. Rebuilds
 * from leaf content and node decorations, not already-decorated previews.
 * Keeps protected text/font runs through whole-parent case changes, resolves
 * nested locale quotation marks, and applies semantic boundary punctuation.
 * Display boundaries are returned as spans; this does not lay out a page.
 * Citation layout envelopes and input cite prefix/suffix belong to the later
 * cluster assembler and are not added here.
 *
 * Spans are bounding ranges, not a disjoint ownership partition: moving a
 * delimiter inside quotation marks may make source-node ranges overlap.
 * Name/source identities remain on the borrowed evaluation tree. Keep that
 * evaluation (and its documented source inputs) alive until this result is
 * freed. Result text/runs/spans are owned independently; no source is mutated.
 *
 * Initialize *out to NULL or a live result. Success atomically replaces it;
 * allocation/validation/limit failure preserves it. NULL allocator uses
 * malloc/free; its supplied context must outlive the result. All operation
 * allocations, including retained temporary inline transformations, share
 * MAX_WORKSPACE; the caller's existing input/result are excluded. This is not
 * a document processor, sort-key generator or disambiguation driver. */
bool ce_csl_finalize(const CeCslEvaluation *evaluation,
    const CeCslOutputAllocator *allocator, CeCslFinalized **out,
    char *error, size_t capacity);
bool ce_csl_finalize_with_options(const CeCslEvaluation *evaluation,
    const CeCslFinalizeOptions *options, const CeCslOutputAllocator *allocator,
    CeCslFinalized **out, char *error, size_t capacity);
/* Joins already planned citation pieces, preserving semantic quote and
 * generated-separator origins until ONE final punctuation pass. Every input
 * must use the same citation layout XML and locale object; bibliography and
 * macro-entry evaluations are rejected. The layout envelope is applied once.
 * Empty pieces emit no affixes/delimiters. Prefix/suffix are literal cite data
 * and never parsed as markup or automatically spaced. Count is 1..MAX_CITES.
 * source_cite_index is an opaque caller mapping (SIZE_MAX is reserved).
 * All input evaluations remain borrowed until result free. No sorting,
 * grouping, collapsing or source-index deduplication is performed here.
 * First-names getters are NULL on a joined result. Limits/atomicity are the
 * same as standalone finalization, across the ENTIRE joined citation. The
 * sum of evaluation node counts may not exceed MAX_JOIN_NODES (duplicates
 * count again), bounding projection validation and materialization work.
 * Public spans map evaluation content; literal cite affixes/envelope do not
 * have public source spans. The caller retains whole-cite coverage metadata. */
bool ce_csl_finalize_join(const CeCslFinalizeJoinEntry *entries, size_t count,
    const CeCslOutputAllocator *allocator, CeCslFinalized **out,
    char *error, size_t capacity);
void ce_csl_finalized_free(CeCslFinalized *result);
const CeCslInline *ce_csl_finalized_inline(const CeCslFinalized *result);
const CeCslOutput *ce_csl_finalized_output(const CeCslFinalized *result);
const CeCslEvaluation *ce_csl_finalized_evaluation(const CeCslFinalized *result); /* NULL for joins. */
/* First emitted names element, including substitution, with whole-parent case
 * and strip already applied. The owned field snapshot precedes cross-boundary
 * punctuation cleanup, excluding external separators even when they later move
 * inside a quote. This semantic grouping key is not a final bounding-span slice.
 * NULL means no names element emitted content (including after projection). */
const CeCslEvalNode *ce_csl_finalized_first_names(const CeCslFinalized *result);
const CeCslInline *ce_csl_finalized_first_names_inline(const CeCslFinalized *result);
bool ce_csl_finalized_missing_term(const CeCslFinalized *result);
size_t ce_csl_finalized_span_count(const CeCslFinalized *result);
const CeCslFinalSpan *ce_csl_finalized_span(const CeCslFinalized *result, size_t index);

#endif
