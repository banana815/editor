#ifndef CE_CSL_REGISTRY_H
#define CE_CSL_REGISTRY_H

#include "csl_input.h"
#include "csl_output.h"

typedef struct CeCslRegistry CeCslRegistry;

typedef struct CeCslRegistryOptions {
    /* Explicit input item indices to include even when uncited. Append in this
     * order, deduplicating both repeated indices and already-cited items.
     * NULL/count=0 includes no additional items; count <= INPUT_MAX_ITEMS. */
    const size_t *include_items;
    size_t include_count;
    /* Optional full permutation of the initial eligible ROWS, not input item
     * indices. Its length must equal the deduplicated eligible count. Build
     * once without it to obtain that initial row set for external sorting.
     * NULL/count=0 keeps the initial order. */
    const size_t *row_order;
    size_t row_order_count;
} CeCslRegistryOptions;

typedef struct CeCslRegistryRow {
    size_t item_index; /* Original immutable input item index. */
    uint64_t citation_number; /* 1-based final row number, never an input value. */
} CeCslRegistryRow;

/* Default eligibility is ONLY cited items, in first-use order while traversing
 * input clusters/cites. Explicit includes are appended, then row_order is
 * applied. Sorting cites for display never feeds back into first-use numbering;
 * this avoids a cycle when citations sort on citation-number. No implicit
 * inclusion of the full input library, filtering, sorting or disambiguation.
 * noteIndex and user-provided citation-number values do not affect numbering.
 *
 * Initialize *out to NULL. Success atomically replaces it; failure preserves
 * the old object and its borrowed row views. The result owns its indices and
 * reverse map and may outlive input. NULL allocator uses malloc/free; otherwise
 * both callbacks are required and their context must outlive the result. No
 * global allocator changes, input mutation, or borrowed input pointers.
 *
 * Out-of-range includes, invalid permutations and input count-limit violations
 * are errors. Runtime and workspace are O(items + clusters + cites + includes),
 * with at most INPUT_MAX_ITEMS rows and a reverse map for all input items. */
bool ce_csl_registry_build(const CeCslInput *input,
    const CeCslRegistryOptions *options, const CeCslOutputAllocator *allocator,
    CeCslRegistry **out, char *error, size_t capacity);
void ce_csl_registry_free(CeCslRegistry *registry);
size_t ce_csl_registry_count(const CeCslRegistry *registry);
/* Borrowed row, invalidated only by successful replacement or free. */
const CeCslRegistryRow *ce_csl_registry_row(const CeCslRegistry *registry, size_t row);
/* Ineligible or out-of-range item indices (and NULL registry) return the
 * missing sentinels 0 for citation number and SIZE_MAX for row. */
uint64_t ce_csl_registry_citation_number(const CeCslRegistry *registry, size_t item_index);
size_t ce_csl_registry_item_row(const CeCslRegistry *registry, size_t item_index);

#endif
