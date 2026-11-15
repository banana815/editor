#ifndef CE_CSL_POSITION_H
#define CE_CSL_POSITION_H

#include "csl_condition.h"
#include "csl_input.h"
#include "csl_output.h"

typedef struct CeCslPositionResult {
    unsigned position; /* CeCslPosition bits, including implied predicates. */
    uint32_t first_reference_note_number; /* Zero means absent. */
} CeCslPositionResult;

typedef struct CeCslPositionOptions {
    bool note_style; /* Only controls generated first-reference-note-number. */
    bool near_note_distance_set;
    uint32_t near_note_distance; /* Default 5; an explicitly set zero is valid. */
    /* Decreasing positive note numbers are unsupported by default. Opt in only
     * when the application treats a decrease as the start of a new note-number
     * section: that resets note history, but leaves in-text history intact. */
    bool allow_note_number_reset;
    /* Optional flattened original cite indices in rendering order. Each
     * cluster's segment must be a permutation of that cluster's own indices.
     * NULL/count=0 uses input order. Never reorders clusters or mutates input. */
    const size_t *cite_order;
    size_t cite_order_count;
} CeCslPositionOptions;

typedef struct CeCslPositions {
    CeCslPositionResult *cites; /* Original flattened input order. */
    size_t cite_count;
    size_t *cluster_offsets; /* cluster_count + 1 entries. */
    size_t cluster_count;
    void (*_deallocate)(void *context, void *memory);
    void *_context;
} CeCslPositions;

/* Derives citation positions from immutable input. Initialize out to {0}; a
 * success replaces it atomically, a failure leaves it untouched. NULL options
 * means in-text style, distance 5, input order, and no note-number resets.
 * NULL allocator uses malloc/free; otherwise both callbacks are required and
 * its context must outlive out. No cJSON allocator hooks are changed.
 *
 * noteIndex=0 and positive noteIndex form independent first/subsequent/ibid
 * histories. Within a cluster, only the immediately preceding rendered cite
 * can establish ibid. Between in-text clusters, the preceding cluster must be
 * a singleton. Within the same positive note, the preceding cluster's last
 * rendered cite can establish ibid. Across positive notes, notes must be
 * consecutive and the entire previous note must contain exactly one cite.
 *
 * Locator and label are compared separately and byte-exactly: empty/missing
 * locator is absent; empty/missing label means page. Removing a locator makes
 * an otherwise ibid cite only subsequent. Affixes and author flags do not
 * change identity. Caller-provided position, near-note, first-reference note,
 * or item first-citation fields do not supply derived facts.
 *
 * near-note requires positive note numbers; first-reference-note-number also
 * requires note_style and a reference in an earlier note in the same section.
 * All positions are citation facts: bibliography evaluation suppresses them in
 * csl_condition. Parallel legal cites and author-only cluster modes are not
 * implemented here. O(items + clusters + cites + compared locator bytes), with
 * counts bounded by CeCslInput limits; no input pointers escape into out. */
bool ce_csl_positions_derive(const CeCslInput *input,
    const CeCslPositionOptions *options, const CeCslOutputAllocator *allocator,
    CeCslPositions *out, char *error, size_t capacity);
void ce_csl_positions_free(CeCslPositions *positions);
const CeCslPositionResult *ce_csl_positions_at(const CeCslPositions *positions,
    size_t cluster_index, size_t original_cite_index);

#endif
