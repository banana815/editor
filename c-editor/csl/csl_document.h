#ifndef CE_CSL_DOCUMENT_H
#define CE_CSL_DOCUMENT_H

#include "csl_cluster.h"
#include "csl_disambiguation_render.h"
#include "csl_position.h"
#include "csl_sort_evaluate.h"
#include "csl_bibliography.h"

#define CE_CSL_DOCUMENT_MAX_WORKSPACE ((size_t)256 * 1024 * 1024)
#define CE_CSL_DOCUMENT_MAX_OUTPUT_BYTES ((size_t)64 * 1024 * 1024)
#define CE_CSL_DOCUMENT_MAX_EVALUATIONS ((size_t)100000)
#define CE_CSL_DOCUMENT_MAX_SORT_CELLS ((size_t)1000000)
#define CE_CSL_DOCUMENT_MAX_PASSES ((size_t)4)

typedef struct CeCslDocument CeCslDocument;
typedef struct CeCslDocumentOptions {
    const size_t *include_items;
    size_t include_count;
    CeCslCollationLanguage collation_language;
    bool allow_note_number_reset;
    void *short_context;
    CeCslVariableStatus (*short_value)(void *context, const char *canonical_name,
        const cJSON *long_value, CeCslVariableShortValue *value, char *error, size_t capacity);
    void *cancel_context;
    bool (*cancelled)(void *context);
    /* Zero uses the named hard default; positive limits may only lower it. */
    /* Evaluation count includes each sort key, disambiguation probe, citation
     * and bibliography evaluation. A probe's internal initials variants and a
     * cluster's projection/finalization work use their separate module bounds. */
    size_t max_workspace_bytes, max_output_bytes, max_evaluations;
    size_t max_sort_cells, max_passes;
} CeCslDocumentOptions;
typedef struct CeCslDocumentCitation {
    size_t cluster_index;
    const CeCslOutput *output;
    /* Original within-cluster indices in final grouped order, including cites
     * hidden inside numeric ranges. No item identities are deduplicated here. */
    const size_t *cite_order;
    size_t cite_count;
    bool missing_term, literal_markup, malformed_markup;
} CeCslDocumentCitation;
typedef struct CeCslDocumentBibliography {
    size_t item_index;
    uint64_t citation_number;
    const CeCslBibliographyEntry *entry;
    bool visible, missing_term, literal_markup, malformed_markup;
} CeCslDocumentBibliography;

/* Processes one immutable document revision. Builds initial cited eligibility,
 * bibliography sort/numbering, actual-renderer disambiguation, citation sort,
 * positions and semantic grouping/collapse. Each position/grouping pass starts
 * from the original stylesheet sort, preserving the ability to ungroup after
 * a key changes. A non-converging order fails explicitly after MAX_PASSES.
 * Final outputs own their runs and index maps: temporary evaluations are freed
 * per citation, and no evaluation/source span escapes into these output views.
 *
 * The style, locale and input must remain immutable and alive until result
 * free (retained disambiguation occurrences reference the style). Options and
 * callback contexts survive only this call; callbacks must be read-only and
 * deterministic for this input revision. Source objects and a previous live
 * result are excluded from MAX_WORKSPACE; all component allocation hooks share
 * it, subject to their documented dependency exceptions. Cancellation occurs
 * between bounded component calls, not inside arbitrary client callbacks.
 *
 * Initialize *out to NULL/live. Any error, unsupported behavior, limit or OOM
 * preserves it and all inputs. No approximate fallback and no editor mutation.
 * Rebuild from the original revision on every edit, so deleted citations can
 * remove suffixes, name expansion and numbers. This is a bounded CSL profile,
 * not a declaration of complete support for every valid stylesheet. */
bool ce_csl_document_build(const CeCslStyle *style, const CeCslLocale *locale,
    const CeCslInput *input, const CeCslDocumentOptions *options,
    const CeCslOutputAllocator *allocator, CeCslDocument **out,
    char *error, size_t capacity);
void ce_csl_document_free(CeCslDocument *document);
size_t ce_csl_document_citation_count(const CeCslDocument *document);
const CeCslDocumentCitation *ce_csl_document_citation(const CeCslDocument *document, size_t index);
/* Export through an explicitly chosen allocator. With NULL, these strings can
 * outlive the document. Direct ce_csl_output_plain/html on a borrowed citation
 * output instead inherits the document's tracking allocator: such strings MUST
 * be released before the document, even if its outer allocator is malloc/free.
 * The same rule applies to direct exports of borrowed bibliography cell outputs.
 * Bibliography aggregate plain/html already accepts an independent allocator. */
bool ce_csl_document_citation_plain(const CeCslDocument *document, size_t index,
    const CeCslOutputAllocator *allocator, CeCslOutputString *out, char *error, size_t capacity);
bool ce_csl_document_citation_html(const CeCslDocument *document, size_t index,
    const CeCslOutputAllocator *allocator, CeCslOutputString *out, char *error, size_t capacity);
/* Rows include eligible entries whose style output is legitimately empty. */
size_t ce_csl_document_bibliography_count(const CeCslDocument *document);
size_t ce_csl_document_visible_bibliography_count(const CeCslDocument *document);
const CeCslDocumentBibliography *ce_csl_document_bibliography(const CeCslDocument *document, size_t index);
const CeCslRegistry *ce_csl_document_registry(const CeCslDocument *document);
const CeCslDisambiguation *ce_csl_document_disambiguation(const CeCslDocument *document);
size_t ce_csl_document_pass_count(const CeCslDocument *document);

#endif
