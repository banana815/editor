#include "csl_position.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ItemHistory {
    uint32_t epoch;
    uint32_t first_note;
    uint32_t last_note;
} ItemHistory;

static bool fail(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return false;
}

static void *default_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void default_deallocate(void *context, void *memory) {
    (void)context;
    free(memory);
}

void ce_csl_positions_free(CeCslPositions *positions) {
    if (!positions) return;
    if (positions->_deallocate) {
        if (positions->cites) positions->_deallocate(positions->_context, positions->cites);
        if (positions->cluster_offsets)
            positions->_deallocate(positions->_context, positions->cluster_offsets);
    }
    memset(positions, 0, sizeof(*positions));
}

const CeCslPositionResult *ce_csl_positions_at(const CeCslPositions *positions,
    size_t cluster_index, size_t original_cite_index) {
    if (!positions || cluster_index >= positions->cluster_count ||
        !positions->cluster_offsets) return NULL;
    size_t start = positions->cluster_offsets[cluster_index];
    size_t end = positions->cluster_offsets[cluster_index + 1];
    if (start > end || end > positions->cite_count || original_cite_index >= end - start ||
        !positions->cites) return NULL;
    return &positions->cites[start + original_cite_index];
}

static const char *string_field(const CeCslInputCite *cite, const char *key) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(cite->data, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

static unsigned ibid_position(const CeCslInputCite *previous, const CeCslInputCite *current) {
    if (!previous || previous->item_index != current->item_index) return 0;
    const char *old_locator = string_field(previous, "locator");
    const char *new_locator = string_field(current, "locator");
    if (*old_locator && !*new_locator) return 0;
    if (!*new_locator) return CE_CSL_POSITION_IBID;
    const char *old_label = string_field(previous, "label");
    const char *new_label = string_field(current, "label");
    if (!*old_label) old_label = "page";
    if (!*new_label) new_label = "page";
    if (*old_locator && !strcmp(old_locator, new_locator) && !strcmp(old_label, new_label))
        return CE_CSL_POSITION_IBID;
    return CE_CSL_POSITION_IBID | CE_CSL_POSITION_IBID_WITH_LOCATOR;
}

bool ce_csl_positions_derive(const CeCslInput *input,
    const CeCslPositionOptions *options, const CeCslOutputAllocator *allocator,
    CeCslPositions *out, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    CeCslPositionOptions opts = {0};
    if (options) opts = *options;
    if (!input || !out || (allocator && (!allocator->allocate || !allocator->deallocate)) ||
        (!opts.cite_order && opts.cite_order_count))
        return fail(error, capacity, "Invalid citation position input, output, permutation, or allocator");
    size_t items = ce_csl_input_item_count(input);
    size_t clusters = ce_csl_input_cluster_count(input);
    if (items > CE_CSL_INPUT_MAX_ITEMS || clusters > CE_CSL_INPUT_MAX_CLUSTERS)
        return fail(error, capacity, "Citation position input exceeds count limits");
    size_t total = 0;
    uint32_t previous_note = 0;
    for (size_t i = 0; i < clusters; ++i) {
        const CeCslInputCluster *cluster = ce_csl_input_cluster(input, i);
        if (!cluster || !cluster->items || !cluster->item_count ||
            cluster->item_count > CE_CSL_INPUT_MAX_CITES - total)
            return fail(error, capacity, "Invalid citation position cluster or cite count");
        total += cluster->item_count;
        if (cluster->note_index) {
            if (cluster->note_index < previous_note && !opts.allow_note_number_reset)
                return fail(error, capacity, "Unsupported decreasing noteIndex; explicit section-reset policy required");
            previous_note = cluster->note_index;
        }
        for (size_t j = 0; j < cluster->item_count; ++j)
            if (cluster->items[j].item_index >= items)
                return fail(error, capacity, "Invalid citation position item reference");
    }
    if (opts.cite_order && opts.cite_order_count != total)
        return fail(error, capacity, "Citation position permutation has the wrong length");

    CeCslOutputAllocator memory = {NULL, default_allocate, default_deallocate};
    if (allocator) memory = *allocator;
    CeCslPositions next = {0};
    next._deallocate = memory.deallocate;
    next._context = memory.context;
    next.cite_count = total;
    next.cluster_count = clusters;
    ItemHistory *history = NULL;
    next.cluster_offsets = memory.allocate(memory.context, (clusters + 1) * sizeof(size_t));
    if (!next.cluster_offsets) goto oom;
    if (total) {
        next.cites = memory.allocate(memory.context, total * sizeof(*next.cites));
        if (!next.cites) goto oom;
        memset(next.cites, 0, total * sizeof(*next.cites));
    }
    next.cluster_offsets[0] = 0;
    for (size_t i = 0; i < clusters; ++i) {
        size_t start = next.cluster_offsets[i];
        size_t end = start + ce_csl_input_cluster(input, i)->item_count;
        next.cluster_offsets[i + 1] = end;
        if (opts.cite_order) {
            for (size_t j = start; j < end; ++j) {
                size_t row = opts.cite_order[j];
                if (row < start || row >= end || next.cites[row].position) {
                    fail(error, capacity, "Citation position permutation repeats or crosses cluster boundaries");
                    goto cleanup;
                }
                next.cites[row].position = CE_CSL_POSITION_FIRST;
            }
        }
    }
    if (total) {
        memset(next.cites, 0, total * sizeof(*next.cites));
        history = memory.allocate(memory.context, 2 * items * sizeof(*history));
        if (!history) goto oom;
        memset(history, 0, 2 * items * sizeof(*history));
    }

    uint32_t note_epoch = 1, note = 0;
    uint32_t distance = opts.near_note_distance_set ? opts.near_note_distance : 5;
    size_t note_cites = 0, text_cluster_cites = 0;
    const CeCslInputCite *last_note_cite = NULL, *last_text_cite = NULL;
    for (size_t i = 0; i < clusters; ++i) {
        const CeCslInputCluster *cluster = ce_csl_input_cluster(input, i);
        uint32_t current_note = cluster->note_index;
        const CeCslInputCite *previous = NULL;
        if (current_note) {
            if (current_note < note) {
                ++note_epoch; /* At most MAX_CLUSTERS epochs, never wraps. */
                note = 0;
                note_cites = 0;
                last_note_cite = NULL;
            }
            if (current_note == note ||
                (note && current_note > note && current_note - note == 1 && note_cites == 1))
                previous = last_note_cite;
            if (current_note != note) note_cites = 0;
            note = current_note;
        } else if (text_cluster_cites == 1) previous = last_text_cite;

        size_t start = next.cluster_offsets[i];
        for (size_t j = 0; j < cluster->item_count; ++j) {
            size_t row = opts.cite_order ? opts.cite_order[start + j] : start + j;
            const CeCslInputCite *cite = &cluster->items[row - start];
            CeCslPositionResult *result = &next.cites[row];
            ItemHistory *item = &history[(current_note ? items : 0) + cite->item_index];
            uint32_t epoch = current_note ? note_epoch : 1;
            if (item->epoch != epoch) {
                result->position = CE_CSL_POSITION_FIRST;
                item->epoch = epoch;
                item->first_note = current_note;
            } else {
                result->position = CE_CSL_POSITION_SUBSEQUENT | ibid_position(previous, cite);
                if (current_note && current_note >= item->last_note &&
                    current_note - item->last_note <= distance)
                    result->position |= CE_CSL_POSITION_NEAR_NOTE;
                if (opts.note_style && current_note && item->first_note < current_note)
                    result->first_reference_note_number = item->first_note;
            }
            item->last_note = current_note;
            previous = cite;
        }
        if (current_note) {
            note_cites = note_cites || cluster->item_count > 1 ? 2 : 1;
            last_note_cite = previous;
        } else {
            text_cluster_cites = cluster->item_count;
            last_text_cite = previous;
        }
    }
    if (history) memory.deallocate(memory.context, history);
    ce_csl_positions_free(out);
    *out = next;
    return true;
oom:
    fail(error, capacity, "Out of memory deriving citation positions");
cleanup:
    if (history) memory.deallocate(memory.context, history);
    ce_csl_positions_free(&next);
    return false;
}
