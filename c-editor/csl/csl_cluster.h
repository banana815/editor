#ifndef CE_CSL_CLUSTER_H
#define CE_CSL_CLUSTER_H

#include "csl_finalize.h"

#define CE_CSL_CLUSTER_MAX_KEY_BYTES ((size_t)64 * 1024)
#define CE_CSL_CLUSTER_MAX_KEYS_BYTES ((size_t)16 * 1024 * 1024)
#define CE_CSL_CLUSTER_MAX_COMPARED_BYTES ((size_t)128 * 1024 * 1024)
#define CE_CSL_CLUSTER_MAX_WORK ((size_t)16 * 1024 * 1024)
#define CE_CSL_CLUSTER_MAX_WORKSPACE ((size_t)256 * 1024 * 1024)

typedef enum CeCslClusterStatus {
    CE_CSL_CLUSTER_OK, CE_CSL_CLUSTER_INVALID, CE_CSL_CLUSTER_UNSUPPORTED,
    CE_CSL_CLUSTER_LIMIT, CE_CSL_CLUSTER_OOM, CE_CSL_CLUSTER_CANCELLED,
    CE_CSL_CLUSTER_RENDER_ERROR
} CeCslClusterStatus;
typedef struct CeCslClusterEntry {
    size_t source_cite_index; /* Caller mapping, not an item or citation number; SIZE_MAX reserved. */
    const CeCslInputCite *cite;
    const CeCslEvaluation *evaluation;
    /* Normal, unprojected standalone finalization of precisely evaluation. */
    const CeCslFinalized *finalized;
    CeCslGeneratedNumber citation_number;
} CeCslClusterEntry;
typedef struct CeCslClusterOptions {
    /* Zero selects the hard limit above. Positive limits may only lower it. */
    size_t max_key_bytes, max_keys_bytes, max_compared_bytes;
    size_t max_work_units, max_workspace_bytes;
    void *cancel_context;
    bool (*cancelled)(void *context);
} CeCslClusterOptions;
typedef enum CeCslClusterGroupKind {
    CE_CSL_CLUSTER_SINGLE, CE_CSL_CLUSTER_NAMES, CE_CSL_CLUSTER_NUMERIC_RANGE
} CeCslClusterGroupKind;
typedef struct CeCslClusterGroup {
    size_t first_order, member_count;
    size_t first_piece, piece_count;
    CeCslClusterGroupKind kind;
    bool collapsed, visible;
} CeCslClusterGroup;
typedef struct CeCslClusterPiece {
    size_t entry_index, source_cite_index, group_index;
    bool omitted_names, visible;
} CeCslClusterPiece;
typedef struct CeCslCluster CeCslCluster;

/* Builds ONE citation from already sorted and disambiguated entries. Does not
 * assign citation numbers, derive positions, sort bibliography rows or resolve
 * document-wide ambiguity. All entries must use this style's citation layout
 * and the same locale object. Baseline finalizations must not omit names.
 *
 * Grouping compares the first emitted names field's exact plain bytes, with
 * substitution and parent case/strip, before cross-boundary punctuation.
 * No casefolding, normalization, font comparison or name-identity guessing.
 * Groups move stably to their first entry's location. YEAR collapse projects
 * away subsequent first-names nodes; original substitutions/branches remain
 * intact. suppress-author uses the same projection. author-only is explicitly
 * unsupported. year-suffix modes fall back to YEAR only when adding suffixes
 * is disabled; otherwise those modes are explicitly unsupported.
 *
 * Numeric collapse retains complete endpoints of >=3 strictly increasing
 * consecutive exact uint64 numbers. Duplicates are retained. Other visible
 * variable/literal content, differing semantic number paths, a later prefix,
 * or any suffix interrupt a range. Unrendered locator data is not a barrier.
 * Prefix/suffix are literal cite bytes, never markup or automatically spaced.
 * The finalizer joins semantic pieces, retaining generated delimiters/quotes
 * through its final punctuation pass and applying the layout envelope once.
 *
 * Initialize *out=NULL/live; all failure paths preserve it and all inputs.
 * Owned output/planning arrays use per-object allocation; no global hooks.
 * Evaluations and their borrowed source objects must outlive the result's
 * semantic span views. Entry arrays/baseline finalizations need survive only
 * this call. Existing outputs are excluded from the operation heap budget.
 * Each finalize call is bounded but is not interrupted inside the finalizer;
 * cancellation/work checks occur between such calls and during grouping.
 */
CeCslClusterStatus ce_csl_cluster_build(const CeCslStyle *style,
    const CeCslLocale *locale, const CeCslClusterEntry *entries, size_t count,
    const CeCslClusterOptions *options, const CeCslOutputAllocator *allocator,
    CeCslCluster **out, char *error, size_t capacity);
void ce_csl_cluster_free(CeCslCluster *result);
const CeCslFinalized *ce_csl_cluster_finalized(const CeCslCluster *result);
const CeCslInline *ce_csl_cluster_inline(const CeCslCluster *result);
const CeCslOutput *ce_csl_cluster_output(const CeCslCluster *result);
size_t ce_csl_cluster_count(const CeCslCluster *result);
/* Original entry-array index at this grouped-order position, SIZE_MAX outside
 * bounds. Includes entries hidden inside a range or made empty by projection. */
size_t ce_csl_cluster_order(const CeCslCluster *result, size_t position);
size_t ce_csl_cluster_group_count(const CeCslCluster *result);
const CeCslClusterGroup *ce_csl_cluster_group(const CeCslCluster *result, size_t index);
size_t ce_csl_cluster_piece_count(const CeCslCluster *result);
const CeCslClusterPiece *ce_csl_cluster_piece(const CeCslCluster *result, size_t index);

#endif
