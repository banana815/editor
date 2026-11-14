#ifndef CE_CSL_FINALIZE_H
#define CE_CSL_FINALIZE_H

#include "csl_evaluate.h"

#define CE_CSL_FINALIZE_MAX_BYTES CE_CSL_OUTPUT_MAX_BYTES
#define CE_CSL_FINALIZE_MAX_SPANS ((size_t)262144)
#define CE_CSL_FINALIZE_MAX_WORKSPACE ((size_t)128 * 1024 * 1024)

typedef struct CeCslFinalized CeCslFinalized;
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
void ce_csl_finalized_free(CeCslFinalized *result);
const CeCslInline *ce_csl_finalized_inline(const CeCslFinalized *result);
const CeCslOutput *ce_csl_finalized_output(const CeCslFinalized *result);
bool ce_csl_finalized_missing_term(const CeCslFinalized *result);
size_t ce_csl_finalized_span_count(const CeCslFinalized *result);
const CeCslFinalSpan *ce_csl_finalized_span(const CeCslFinalized *result, size_t index);

#endif
