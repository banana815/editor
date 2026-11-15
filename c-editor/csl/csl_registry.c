#include "csl_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct CeCslRegistry {
    CeCslRegistryRow *rows;
    size_t *item_rows;
    size_t count;
    size_t item_count;
    CeCslOutputAllocator memory;
};

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

void ce_csl_registry_free(CeCslRegistry *registry) {
    if (!registry) return;
    CeCslOutputAllocator memory = registry->memory;
    if (registry->rows) memory.deallocate(memory.context, registry->rows);
    if (registry->item_rows) memory.deallocate(memory.context, registry->item_rows);
    memory.deallocate(memory.context, registry);
}

size_t ce_csl_registry_count(const CeCslRegistry *registry) {
    return registry ? registry->count : 0;
}

const CeCslRegistryRow *ce_csl_registry_row(const CeCslRegistry *registry, size_t row) {
    return registry && row < registry->count ? &registry->rows[row] : NULL;
}

size_t ce_csl_registry_item_row(const CeCslRegistry *registry, size_t item_index) {
    return registry && item_index < registry->item_count ? registry->item_rows[item_index] : SIZE_MAX;
}

uint64_t ce_csl_registry_citation_number(const CeCslRegistry *registry, size_t item_index) {
    size_t row = ce_csl_registry_item_row(registry, item_index);
    return registry && row < registry->count ? registry->rows[row].citation_number : 0;
}

static void include_item(CeCslRegistry *registry, size_t *initial, size_t item_index) {
    if (registry->item_rows[item_index] != SIZE_MAX) return;
    registry->item_rows[item_index] = registry->count;
    initial[registry->count++] = item_index;
}

bool ce_csl_registry_build(const CeCslInput *input,
    const CeCslRegistryOptions *options, const CeCslOutputAllocator *allocator,
    CeCslRegistry **out, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    CeCslRegistryOptions opts = {0};
    if (options) opts = *options;
    if (!input || !out || (allocator && (!allocator->allocate || !allocator->deallocate)) ||
        (!opts.include_items && opts.include_count) || (!opts.row_order && opts.row_order_count))
        return fail(error, capacity, "Invalid citation registry input, output, options, or allocator");
    size_t items = ce_csl_input_item_count(input);
    size_t clusters = ce_csl_input_cluster_count(input);
    if (items > CE_CSL_INPUT_MAX_ITEMS || clusters > CE_CSL_INPUT_MAX_CLUSTERS ||
        opts.include_count > CE_CSL_INPUT_MAX_ITEMS || opts.row_order_count > CE_CSL_INPUT_MAX_ITEMS)
        return fail(error, capacity, "Citation registry count limit exceeded");
    for (size_t i = 0; i < opts.include_count; ++i)
        if (opts.include_items[i] >= items)
            return fail(error, capacity, "Citation registry include index is out of range");
    size_t cites = 0;
    for (size_t i = 0; i < clusters; ++i) {
        const CeCslInputCluster *cluster = ce_csl_input_cluster(input, i);
        if (!cluster || !cluster->items || !cluster->item_count ||
            cluster->item_count > CE_CSL_INPUT_MAX_CITES - cites)
            return fail(error, capacity, "Invalid citation registry cluster or cite count");
        cites += cluster->item_count;
        for (size_t j = 0; j < cluster->item_count; ++j)
            if (cluster->items[j].item_index >= items)
                return fail(error, capacity, "Invalid citation registry item reference");
    }

    CeCslOutputAllocator memory = {NULL, default_allocate, default_deallocate};
    if (allocator) memory = *allocator;
    CeCslRegistry *next = memory.allocate(memory.context, sizeof(*next));
    if (!next) return fail(error, capacity, "Out of memory creating citation registry");
    memset(next, 0, sizeof(*next));
    next->memory = memory;
    next->item_count = items;
    size_t *initial = NULL;
    if (items) {
        next->item_rows = memory.allocate(memory.context, items * sizeof(*next->item_rows));
        if (!next->item_rows) goto oom;
        for (size_t i = 0; i < items; ++i) next->item_rows[i] = SIZE_MAX;
        initial = memory.allocate(memory.context, items * sizeof(*initial));
        if (!initial) goto oom;
    }
    for (size_t i = 0; i < clusters; ++i) {
        const CeCslInputCluster *cluster = ce_csl_input_cluster(input, i);
        for (size_t j = 0; j < cluster->item_count; ++j)
            include_item(next, initial, cluster->items[j].item_index);
    }
    for (size_t i = 0; i < opts.include_count; ++i)
        include_item(next, initial, opts.include_items[i]);
    if (opts.row_order && opts.row_order_count != next->count) {
        fail(error, capacity, "Citation registry permutation has the wrong length");
        goto cleanup;
    }
    if (next->count) {
        next->rows = memory.allocate(memory.context, next->count * sizeof(*next->rows));
        if (!next->rows) goto oom;
        for (size_t i = 0; i < next->count; ++i) next->item_rows[initial[i]] = SIZE_MAX;
        for (size_t row = 0; row < next->count; ++row) {
            size_t source = opts.row_order ? opts.row_order[row] : row;
            if (source >= next->count || next->item_rows[initial[source]] != SIZE_MAX) {
                fail(error, capacity, "Citation registry permutation repeats or exceeds eligible rows");
                goto cleanup;
            }
            size_t item = initial[source];
            next->rows[row].item_index = item;
            next->rows[row].citation_number = (uint64_t)row + 1;
            next->item_rows[item] = row;
        }
    }
    if (initial) memory.deallocate(memory.context, initial);
    ce_csl_registry_free(*out);
    *out = next;
    return true;
oom:
    fail(error, capacity, "Out of memory building citation registry");
cleanup:
    if (initial) memory.deallocate(memory.context, initial);
    ce_csl_registry_free(next);
    return false;
}
