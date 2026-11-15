#include "csl_document.h"
#include "csl_bibliography.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union MemoryBlock {
    long double alignment;
    void *pointer;
    unsigned char padding[32];
    struct { size_t size; } value;
} MemoryBlock;
typedef struct NameContext {
    const CeCslDisambiguationItem *state;
    CeCslDisambiguationProfile profile;
} NameContext;
struct CeCslDocument {
    CeCslOutputAllocator user, allocator;
    CeCslDocumentOptions options;
    const CeCslStyle *style;
    const CeCslLocale *locale;
    const CeCslInput *input;
    CeCslRegistry *registry;
    CeCslDisambiguation *disambiguation;
    CeCslDocumentCitation *citations;
    CeCslDocumentBibliography *bibliography;
    size_t citation_count, bibliography_count, visible_bibliography_count;
    size_t *offsets, *base_order, *group_order;
    size_t cite_count, live, output_bytes, evaluations, sort_cells, passes;
    bool limit, oom;
    char *error;
    size_t error_capacity;
};
static void *normal_allocate(void *context, size_t size) { (void)context; return malloc(size); }
static void normal_free(void *context, void *memory) { (void)context; free(memory); }
static bool fail(CeCslDocument *d, const char *message) {
    if (d->error && d->error_capacity) snprintf(d->error, d->error_capacity, "CSL document: %s", message);
    return false;
}
static void *budget_allocate(void *context, size_t bytes) {
    CeCslDocument *d = context;
    if (bytes > d->options.max_workspace_bytes ||
        sizeof(MemoryBlock) > d->options.max_workspace_bytes - bytes ||
        d->live > d->options.max_workspace_bytes - bytes - sizeof(MemoryBlock)) {
        d->limit = true; return NULL;
    }
    MemoryBlock *block = d->user.allocate(d->user.context, sizeof(*block) + bytes);
    if (!block) { d->oom = true; return NULL; }
    block->value.size = sizeof(*block) + bytes;
    d->live += block->value.size;
    return block + 1;
}
static void budget_free(void *context, void *memory) {
    if (!memory) return;
    CeCslDocument *d = context;
    MemoryBlock *block = (MemoryBlock *)memory - 1;
    d->live -= block->value.size;
    d->user.deallocate(d->user.context, block);
}
static void *array(CeCslDocument *d, size_t count, size_t size) {
    if (!count) return NULL;
    if (count > SIZE_MAX / size) { d->limit = true; fail(d, "array size overflow"); return NULL; }
    void *memory = budget_allocate(d, count * size);
    if (!memory) { fail(d, "out of memory"); return NULL; }
    memset(memory, 0, count * size); return memory;
}
static bool active(CeCslDocument *d) {
    return !d->options.cancelled || !d->options.cancelled(d->options.cancel_context) || fail(d, "cancelled");
}
static bool evaluation_work(CeCslDocument *d) {
    if (!active(d)) return false;
    if (d->evaluations == d->options.max_evaluations) return fail(d, "evaluation count limit exceeded");
    ++d->evaluations; return true;
}
static bool output_work(CeCslDocument *d, size_t bytes) {
    if (bytes > d->options.max_output_bytes - d->output_bytes) return fail(d, "document output limit exceeded");
    d->output_bytes += bytes; return true;
}
static bool attribute_true(const CeCslXmlNode *node, const char *name) {
    const char *value = ce_csl_xml_attribute(node, name);
    return value && (!strcmp(value, "true") || !strcmp(value, "1"));
}
static bool token(const char *list, const char *word) {
    if (!list) return false;
    size_t length = strlen(word);
    while (*list) {
        while (*list == ' ' || *list == '\t' || *list == '\r' || *list == '\n') ++list;
        const char *begin = list;
        while (*list && *list != ' ' && *list != '\t' && *list != '\r' && *list != '\n') ++list;
        if ((size_t)(list - begin) == length && !memcmp(begin, word, length)) return true;
    }
    return false;
}
typedef struct SortScan {
    const CeCslXmlNode **seen;
    size_t capacity, work;
} SortScan;
/* Visit each reachable macro definition once per section. Styles may reuse a
 * large valid macro graph in many keys without exponentially expanding it just
 * to discover a forbidden dependency. Citation/bibliography policies differ. */
static bool sort_dependencies(CeCslDocument *d, const CeCslXmlNode *node,
    bool bibliography, size_t depth, SortScan *scan) {
    if (!node) return true;
    if (depth > CE_CSL_EVALUATE_MAX_DEPTH || ++scan->work > CE_CSL_EVALUATE_MAX_NODES)
        return fail(d, "sort dependency traversal limit exceeded");
    const char *variables = ce_csl_xml_attribute(node, "variable");
    if (ce_csl_xml_attribute(node, "position") || ce_csl_xml_attribute(node, "disambiguate") ||
        token(variables, "year-suffix") || token(variables, "first-reference-note-number") ||
        (bibliography && token(variables, "citation-number")))
        return fail(d, "unsupported sort dependency on document positions, disambiguation or reassigned numbers");
    const char *macro = ce_csl_xml_attribute(node, "macro");
    if (macro) {
        const CeCslXmlNode *definition = ce_csl_style_macro(d->style, macro);
        if (!definition) return fail(d, "missing sort macro definition");
        size_t slot = ((uintptr_t)definition >> 4) & (scan->capacity - 1);
        while (scan->seen[slot] && scan->seen[slot] != definition) slot = (slot + 1) & (scan->capacity - 1);
        if (!scan->seen[slot]) {
            scan->seen[slot] = definition;
            if (!sort_dependencies(d, definition, bibliography, depth + 1, scan)) return false;
        }
    }
    for (const CeCslXmlNode *child = node->children; child; child = child->next)
        if (!sort_dependencies(d, child, bibliography, depth + 1, scan)) return false;
    return true;
}
static bool preflight(CeCslDocument *d) {
    const CeCslXmlNode *bib = ce_csl_style_bibliography(d->style);
    if (ce_csl_xml_attribute(bib, "subsequent-author-substitute"))
        return fail(d, "unsupported bibliography subsequent-author-substitute");
    SortScan scan = {.capacity = 1};
    while (scan.capacity < ce_csl_style_macro_count(d->style) * 2) scan.capacity *= 2;
    scan.seen = array(d, scan.capacity, sizeof(*scan.seen));
    if (!scan.seen) return false;
    bool ok = sort_dependencies(d, ce_csl_xml_child(bib, "sort"), true, 0, &scan);
    memset(scan.seen, 0, scan.capacity * sizeof(*scan.seen)); scan.work = 0;
    if (ok) ok = sort_dependencies(d, ce_csl_xml_child(ce_csl_style_citation(d->style), "sort"), false, 0, &scan);
    budget_free(d, scan.seen);
    if (!ok) return false;
    for (size_t i = 0; i < d->citation_count; ++i) {
        const CeCslInputCluster *cluster = ce_csl_input_cluster(d->input, i);
        const cJSON *properties = cJSON_GetObjectItemCaseSensitive(cluster->data, "properties");
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(properties, "mode");
        const cJSON *unsorted = cJSON_GetObjectItemCaseSensitive(properties, "unsorted");
        if ((mode && !cJSON_IsNull(mode)) || (unsorted && !cJSON_IsFalse(unsorted) && !cJSON_IsNull(unsorted)))
            return fail(d, "unsupported citation properties mode or unsorted extension");
    }
    return active(d);
}
static bool adjust_names(void *context, const CeCslEvalOccurrence *occurrence,
    const CeCslNamesBinding *bindings, size_t binding_count, const CeCslNameOptions *options,
    CeCslEvalNameAdjustment *adjustment, char *error, size_t capacity) {
    (void)bindings; (void)binding_count; (void)options; (void)error; (void)capacity;
    const NameContext *name = context;
    return ce_csl_disambiguation_adjust_names(name->state, name->profile, occurrence, adjustment);
}
static CeCslEvalRequest request(CeCslDocument *d, size_t item_index,
    const CeCslInputCite *cite, const CeCslPositionResult *position, bool bibliography,
    bool sorted, CeCslGeneratedVariables *generated, NameContext *names) {
    const CeCslDisambiguationItem *state = ce_csl_disambiguation_item(d->disambiguation, item_index);
    uint64_t number = ce_csl_registry_citation_number(d->registry, item_index);
    *generated = (CeCslGeneratedVariables){
        .citation_number = {true, number != 0, number},
        .first_reference_note_number = {true, position && position->first_reference_note_number,
            position ? position->first_reference_note_number : 0},
        .year_suffix = {true, state ? state->year_suffix : "", state ? strlen(state->year_suffix) : 0}
    };
    names->state = state;
    names->profile = bibliography ? CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY :
        (!position || (position->position & CE_CSL_POSITION_FIRST) ? CE_CSL_DISAMBIGUATION_FIRST : CE_CSL_DISAMBIGUATION_SUBSEQUENT);
    return (CeCslEvalRequest){
        .style = d->style, .locale = d->locale,
        .section = bibliography ? ce_csl_style_bibliography(d->style) : ce_csl_style_citation(d->style),
        .variables = {.item = ce_csl_input_item(d->input, item_index), .cite = cite,
            .generated = generated, .context = d->options.short_context, .short_value = d->options.short_value},
        .position = position ? position->position : CE_CSL_POSITION_FIRST,
        .disambiguate = !sorted && state && state->disambiguate,
        .name_context = names, .adjust_names = !sorted && state ? adjust_names : NULL
    };
}
static bool sort_rows(CeCslDocument *d, const CeCslInputCluster *cluster, CeCslSortOrder *out) {
    bool bibliography = cluster == NULL;
    const CeCslXmlNode *section = bibliography ? ce_csl_style_bibliography(d->style) : ce_csl_style_citation(d->style);
    const CeCslXmlNode *sort = ce_csl_xml_child(section, "sort");
    size_t rows = bibliography ? ce_csl_registry_count(d->registry) : cluster->item_count;
    size_t columns = 0;
    for (const CeCslXmlNode *key = sort ? sort->children : NULL; key; key = key->next) ++columns;
    if (columns > CE_CSL_SORT_MAX_COLUMNS || (rows && columns > (d->options.max_sort_cells - d->sort_cells) / rows))
        return fail(d, "document sort-cell limit exceeded");
    size_t cells = rows * columns; d->sort_cells += cells;
    CeCslSortEvaluated *keys = array(d, cells, sizeof(*keys));
    if (cells && !keys) return false;
    bool ok = true;
    for (size_t row = 0; row < rows && ok; ++row) {
        const CeCslInputCite *cite = bibliography ? NULL : &cluster->items[row];
        size_t item = bibliography ? ce_csl_registry_row(d->registry, row)->item_index : cite->item_index;
        CeCslGeneratedVariables generated; NameContext names;
        CeCslEvalRequest context = request(d, item, cite, NULL, bibliography, true, &generated, &names);
        size_t column = 0;
        for (const CeCslXmlNode *key = sort ? sort->children : NULL; key && ok; key = key->next, ++column)
            ok = evaluation_work(d) && ce_csl_sort_evaluate(&context, key, d->options.collation_language,
                &d->allocator, &keys[row * columns + column], d->error, d->error_capacity);
    }
    if (ok) ok = ce_csl_sort_evaluated_order(keys, rows, columns, &d->allocator, out, d->error, d->error_capacity);
    for (size_t i = 0; i < cells; ++i) ce_csl_sort_evaluated_free(&keys[i]);
    budget_free(d, keys); return ok;
}
static bool bibliography_registry(CeCslDocument *d) {
    CeCslRegistryOptions options = {.include_items = d->options.include_items, .include_count = d->options.include_count};
    if (!ce_csl_registry_build(d->input, &options, &d->allocator, &d->registry, d->error, d->error_capacity)) return false;
    CeCslSortOrder order = {0};
    bool ok = sort_rows(d, NULL, &order);
    if (ok) {
        options.row_order = order.indices; options.row_order_count = order.count;
        ok = ce_csl_registry_build(d->input, &options, &d->allocator, &d->registry, d->error, d->error_capacity);
    }
    ce_csl_sort_order_free(&order); return ok;
}
typedef struct ProbeContext {
    CeCslDocument *document;
    CeCslDisambiguationProvider inner;
} ProbeContext;
static CeCslDisambiguationStatus probe_render(void *context, const CeCslDisambiguationRequest *request,
    const CeCslOutputAllocator *allocator, CeCslDisambiguationProbe **out, char *error, size_t capacity) {
    ProbeContext *probe = context;
    if (!evaluation_work(probe->document)) return CE_CSL_DISAMBIGUATION_LIMIT;
    return probe->inner.render(probe->inner.context, request, allocator, out, error, capacity);
}
static void probe_release(void *context, CeCslDisambiguationProbe *value) {
    ProbeContext *probe = context;
    probe->inner.release(probe->inner.context, value);
}
static bool disambiguate(CeCslDocument *d) {
    const CeCslXmlNode *citation = ce_csl_style_citation(d->style);
    CeCslDisambiguationOptions options = {
        .add_given_names = attribute_true(citation, "disambiguate-add-givenname"),
        .add_names = attribute_true(citation, "disambiguate-add-names"),
        .add_year_suffix = attribute_true(citation, "disambiguate-add-year-suffix"),
        .use_disambiguate_branch = true,
        .profiles = CE_CSL_DISAMBIGUATION_FIRST | CE_CSL_DISAMBIGUATION_SUBSEQUENT,
        .cancel_context = d->options.cancel_context, .cancelled = d->options.cancelled
    };
    if (ce_csl_style_bibliography(d->style)) options.profiles |= CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY;
    const char *rule = ce_csl_xml_attribute(citation, "givenname-disambiguation-rule");
    if (rule && !strcmp(rule, "primary-name-with-initials")) options.given_rule = CE_CSL_DISAMBIGUATE_PRIMARY_NAME_INITIALS;
    else if (rule && !strcmp(rule, "primary-name")) options.given_rule = CE_CSL_DISAMBIGUATE_PRIMARY_NAME;
    else if (rule && !strcmp(rule, "all-names")) options.given_rule = CE_CSL_DISAMBIGUATE_ALL_NAMES;
    else if (rule && !strcmp(rule, "all-names-with-initials")) options.given_rule = CE_CSL_DISAMBIGUATE_ALL_NAMES_INITIALS;
    CeCslDisambiguationRenderConfig config = {
        .style = d->style, .locale = d->locale, .input = d->input, .registry = d->registry,
        .short_context = d->options.short_context, .short_value = d->options.short_value,
        .cancel_context = d->options.cancel_context, .cancelled = d->options.cancelled
    };
    ProbeContext probes = {d, ce_csl_disambiguation_render_provider(&config)};
    CeCslDisambiguationProvider provider = {&probes, probe_render, probe_release};
    return ce_csl_disambiguation_build(d->input, d->registry, &options, &provider, &d->allocator,
        &d->disambiguation, d->error, d->error_capacity) == CE_CSL_DISAMBIGUATION_OK;
}
static bool citation_order(CeCslDocument *d) {
    d->offsets = array(d, d->citation_count + 1, sizeof(*d->offsets));
    if (!d->offsets) return false;
    for (size_t i = 0; i < d->citation_count; ++i) {
        size_t count = ce_csl_input_cluster(d->input, i)->item_count;
        if (count > CE_CSL_INPUT_MAX_CITES - d->cite_count) return fail(d, "citation count limit exceeded");
        d->cite_count += count; d->offsets[i + 1] = d->cite_count;
    }
    d->base_order = array(d, d->cite_count, sizeof(*d->base_order));
    d->group_order = array(d, d->cite_count, sizeof(*d->group_order));
    d->citations = array(d, d->citation_count, sizeof(*d->citations));
    if ((d->cite_count && (!d->base_order || !d->group_order)) || (d->citation_count && !d->citations)) return false;
    for (size_t i = 0; i < d->citation_count; ++i) {
        if (!active(d)) return false;
        CeCslSortOrder order = {0};
        if (!sort_rows(d, ce_csl_input_cluster(d->input, i), &order)) return false;
        for (size_t j = 0; j < order.count; ++j)
            d->base_order[d->offsets[i] + j] = d->offsets[i] + order.indices[j];
        ce_csl_sort_order_free(&order);
    }
    if (d->cite_count) memcpy(d->group_order, d->base_order, d->cite_count * sizeof(*d->base_order));
    return true;
}
static bool positions(CeCslDocument *d, CeCslPositions *out) {
    const CeCslXmlNode *root = ce_csl_style_root(d->style);
    const char *class_name = ce_csl_xml_attribute(root, "class");
    CeCslPositionOptions options = {
        .note_style = class_name && !strcmp(class_name, "note"),
        .allow_note_number_reset = d->options.allow_note_number_reset,
        .cite_order = d->group_order, .cite_order_count = d->cite_count
    };
    const char *distance = ce_csl_xml_attribute(ce_csl_style_citation(d->style), "near-note-distance");
    if (distance) {
        uint32_t value = 0;
        if (*distance == '+') ++distance;
        for (const char *p = distance; *p; ++p) {
            if (*p < '0' || *p > '9' || value > (UINT32_MAX - (unsigned)(*p - '0')) / 10)
                return fail(d, "invalid near-note-distance");
            value = value * 10 + (unsigned)(*p - '0');
        }
        options.near_note_distance_set = true; options.near_note_distance = value;
    }
    return active(d) && ce_csl_positions_derive(d->input, &options, &d->allocator, out, d->error, d->error_capacity);
}
static bool citation_render(CeCslDocument *d, size_t index, const CeCslPositions *derived, bool *changed) {
    const CeCslInputCluster *source = ce_csl_input_cluster(d->input, index);
    size_t count = source->item_count, offset = d->offsets[index];
    CeCslClusterEntry *entries = array(d, count, sizeof(*entries));
    CeCslEvaluation **evaluations = array(d, count, sizeof(*evaluations));
    CeCslFinalized **finalized = array(d, count, sizeof(*finalized));
    size_t *order = array(d, count, sizeof(*order));
    CeCslCluster *cluster = NULL; CeCslOutput *output = NULL;
    bool ok = entries && evaluations && finalized && order;
    bool missing = false, literal = false, malformed = false;
    for (size_t i = 0; i < count && ok; ++i) {
        size_t original = d->base_order[offset + i] - offset;
        const CeCslInputCite *cite = &source->items[original];
        CeCslGeneratedVariables generated; NameContext names;
        CeCslEvalRequest context = request(d, cite->item_index, cite,
            ce_csl_positions_at(derived, index, original), false, false, &generated, &names);
        ok = evaluation_work(d) && ce_csl_evaluate(&context, &d->allocator, &evaluations[i], d->error, d->error_capacity) &&
            ce_csl_finalize(evaluations[i], &d->allocator, &finalized[i], d->error, d->error_capacity);
        if (ok) {
            const CeCslEvalSummary *summary = ce_csl_evaluation_summary(evaluations[i]);
            literal |= summary->literal_markup; malformed |= summary->malformed_markup;
            entries[i] = (CeCslClusterEntry){.source_cite_index = original, .cite = cite,
                .evaluation = evaluations[i], .finalized = finalized[i], .citation_number = generated.citation_number};
        }
    }
    CeCslClusterOptions options = {.cancel_context = d->options.cancel_context, .cancelled = d->options.cancelled};
    if (ok) ok = active(d) && ce_csl_cluster_build(d->style, d->locale, entries, count, &options, &d->allocator,
        &cluster, d->error, d->error_capacity) == CE_CSL_CLUSTER_OK;
    if (ok) {
        for (size_t i = 0; i < count; ++i) {
            size_t entry_index = ce_csl_cluster_order(cluster, i);
            if (entry_index >= count) { ok = fail(d, "invalid internal cluster permutation"); break; }
            order[i] = entries[entry_index].source_cite_index;
            *changed |= d->group_order[offset + i] != offset + order[i];
            d->group_order[offset + i] = offset + order[i];
        }
    }
    if (ok) {
        missing = ce_csl_finalized_missing_term(ce_csl_cluster_finalized(cluster));
        const CeCslOutput *body = ce_csl_cluster_output(cluster);
        output = ce_csl_output_new(&d->allocator, d->error, d->error_capacity);
        ok = output_work(d, ce_csl_output_text_bytes(body)) && output &&
            ce_csl_output_concat(output, body, d->error, d->error_capacity);
    }
    if (ok) {
        CeCslDocumentCitation *old = &d->citations[index];
        ce_csl_output_free((CeCslOutput *)old->output); budget_free(d, (void *)old->cite_order);
        *old = (CeCslDocumentCitation){.cluster_index = index, .output = output, .cite_order = order,
            .cite_count = count, .missing_term = missing, .literal_markup = literal, .malformed_markup = malformed};
        output = NULL; order = NULL;
    }
    ce_csl_output_free(output); ce_csl_cluster_free(cluster);
    for (size_t i = 0; i < count; ++i) {
        if (finalized) ce_csl_finalized_free(finalized[i]);
        if (evaluations) ce_csl_evaluation_free(evaluations[i]);
    }
    budget_free(d, order); budget_free(d, finalized); budget_free(d, evaluations); budget_free(d, entries);
    return ok;
}
static bool citations_render(CeCslDocument *d) {
    CeCslPositions derived = {0};
    bool ok = true, stable = false;
    for (size_t pass = 0; pass < d->options.max_passes && ok; ++pass) {
        ok = positions(d, &derived);
        bool changed = false; d->output_bytes = 0;
        for (size_t i = 0; i < d->citation_count && ok; ++i) ok = citation_render(d, i, &derived, &changed);
        ++d->passes;
        if (ok && !changed) { stable = true; break; }
    }
    ce_csl_positions_free(&derived);
    return ok && (stable || fail(d, "unsupported non-converging position/grouping order"));
}
static bool bibliography_render(CeCslDocument *d) {
    if (!ce_csl_style_bibliography(d->style)) return true;
    d->bibliography_count = ce_csl_registry_count(d->registry);
    d->bibliography = array(d, d->bibliography_count, sizeof(*d->bibliography));
    if (d->bibliography_count && !d->bibliography) return false;
    for (size_t i = 0; i < d->bibliography_count; ++i) {
        const CeCslRegistryRow *row = ce_csl_registry_row(d->registry, i);
        CeCslGeneratedVariables generated; NameContext names;
        CeCslEvalRequest context = request(d, row->item_index, NULL, NULL, true, false, &generated, &names);
        CeCslEvaluation *evaluation = NULL; CeCslFinalized *finalized = NULL;
        CeCslBibliographyEntry *entry = NULL;
        bool ok = evaluation_work(d) && ce_csl_evaluate(&context, &d->allocator, &evaluation, d->error, d->error_capacity) &&
            ce_csl_finalize(evaluation, &d->allocator, &finalized, d->error, d->error_capacity) &&
            ce_csl_bibliography_entry_build(d->style, finalized, row->item_index, &d->allocator, &entry, d->error, d->error_capacity);
        if (ok) ok = output_work(d, ce_csl_bibliography_entry_text_bytes(entry));
        if (ok) {
            const CeCslEvalSummary *summary = ce_csl_evaluation_summary(evaluation);
            bool visible = ce_csl_bibliography_entry_text_bytes(entry) != 0;
            d->bibliography[i] = (CeCslDocumentBibliography){.item_index = row->item_index,
                .citation_number = row->citation_number, .entry = entry, .visible = visible,
                .missing_term = ce_csl_finalized_missing_term(finalized),
                .literal_markup = summary->literal_markup, .malformed_markup = summary->malformed_markup};
            d->visible_bibliography_count += visible; entry = NULL;
        }
        ce_csl_bibliography_entry_free(entry); ce_csl_finalized_free(finalized); ce_csl_evaluation_free(evaluation);
        if (!ok) return false;
    }
    return true;
}
bool ce_csl_document_build(const CeCslStyle *style, const CeCslLocale *locale,
    const CeCslInput *input, const CeCslDocumentOptions *options,
    const CeCslOutputAllocator *allocator, CeCslDocument **out, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    if (!style || !locale || !input || !out || (allocator && (!allocator->allocate || !allocator->deallocate))) {
        if (error && capacity) snprintf(error, capacity, "CSL document: invalid arguments");
        return false;
    }
    CeCslDocumentOptions limits = options ? *options : (CeCslDocumentOptions){0};
#define LIMIT(field, maximum) do { \
    if (!limits.field) limits.field = (maximum); \
    if (limits.field > (maximum)) { \
        if (error && capacity) snprintf(error, capacity, "CSL document: invalid limit"); \
        return false; \
    } \
} while (0)
    LIMIT(max_workspace_bytes, CE_CSL_DOCUMENT_MAX_WORKSPACE);
    LIMIT(max_output_bytes, CE_CSL_DOCUMENT_MAX_OUTPUT_BYTES);
    LIMIT(max_evaluations, CE_CSL_DOCUMENT_MAX_EVALUATIONS);
    LIMIT(max_sort_cells, CE_CSL_DOCUMENT_MAX_SORT_CELLS);
    LIMIT(max_passes, CE_CSL_DOCUMENT_MAX_PASSES);
#undef LIMIT
    if (limits.collation_language < CE_CSL_COLLATION_DEFAULT || limits.collation_language > CE_CSL_COLLATION_AZ ||
        (limits.include_count && !limits.include_items) || limits.include_count > CE_CSL_INPUT_MAX_ITEMS ||
        limits.max_workspace_bytes < sizeof(CeCslDocument)) {
        if (error && capacity) snprintf(error, capacity, "CSL document: invalid options");
        return false;
    }
    CeCslOutputAllocator user = allocator ? *allocator : (CeCslOutputAllocator){NULL, normal_allocate, normal_free};
    CeCslDocument *d = user.allocate(user.context, sizeof(*d));
    if (!d) { if (error && capacity) snprintf(error, capacity, "CSL document: out of memory"); return false; }
    memset(d, 0, sizeof(*d)); d->user = user; d->options = limits; d->live = sizeof(*d);
    d->allocator = (CeCslOutputAllocator){d, budget_allocate, budget_free};
    d->style = style; d->locale = locale; d->input = input; d->error = error; d->error_capacity = capacity;
    d->citation_count = ce_csl_input_cluster_count(input);
    bool ok = preflight(d) && bibliography_registry(d) && active(d) && disambiguate(d) &&
        citation_order(d) && citations_render(d) && bibliography_render(d) && active(d);
    if (!ok) {
        if (d->limit) fail(d, "workspace limit exceeded");
        else if (d->oom) fail(d, "out of memory");
        ce_csl_document_free(d); return false;
    }
    d->options.include_items = NULL; d->options.include_count = 0;
    d->options.short_context = NULL; d->options.short_value = NULL;
    d->options.cancel_context = NULL; d->options.cancelled = NULL;
    d->error = NULL; d->error_capacity = 0;
    CeCslDocument *old = *out; *out = d; ce_csl_document_free(old); return true;
}
void ce_csl_document_free(CeCslDocument *d) {
    if (!d) return;
    if (d->citations) for (size_t i = 0; i < d->citation_count; ++i) {
        ce_csl_output_free((CeCslOutput *)d->citations[i].output);
        budget_free(d, (void *)d->citations[i].cite_order);
    }
    if (d->bibliography) for (size_t i = 0; i < d->bibliography_count; ++i)
        ce_csl_bibliography_entry_free((CeCslBibliographyEntry *)d->bibliography[i].entry);
    budget_free(d, d->citations); budget_free(d, d->bibliography);
    budget_free(d, d->offsets); budget_free(d, d->base_order); budget_free(d, d->group_order);
    ce_csl_disambiguation_free(d->disambiguation); ce_csl_registry_free(d->registry);
    d->user.deallocate(d->user.context, d);
}
size_t ce_csl_document_citation_count(const CeCslDocument *d) { return d ? d->citation_count : 0; }
const CeCslDocumentCitation *ce_csl_document_citation(const CeCslDocument *d, size_t index) {
    return d && index < d->citation_count ? &d->citations[index] : NULL;
}
static bool export_citation(const CeCslDocument *d, size_t index, bool html,
    const CeCslOutputAllocator *allocator, CeCslOutputString *out, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    const CeCslDocumentCitation *citation = ce_csl_document_citation(d, index);
    if (!citation || !out) {
        if (error && capacity) snprintf(error, capacity, "CSL document: invalid citation export arguments");
        return false;
    }
    CeCslOutput *owned = ce_csl_output_new(allocator, error, capacity);
    bool ok = owned && ce_csl_output_concat(owned, citation->output, error, capacity);
    if (ok) ok = html ? ce_csl_output_html(owned, out, error, capacity) : ce_csl_output_plain(owned, out, error, capacity);
    ce_csl_output_free(owned); return ok;
}
bool ce_csl_document_citation_plain(const CeCslDocument *d, size_t index,
    const CeCslOutputAllocator *allocator, CeCslOutputString *out, char *error, size_t capacity) {
    return export_citation(d, index, false, allocator, out, error, capacity);
}
bool ce_csl_document_citation_html(const CeCslDocument *d, size_t index,
    const CeCslOutputAllocator *allocator, CeCslOutputString *out, char *error, size_t capacity) {
    return export_citation(d, index, true, allocator, out, error, capacity);
}
size_t ce_csl_document_bibliography_count(const CeCslDocument *d) { return d ? d->bibliography_count : 0; }
size_t ce_csl_document_visible_bibliography_count(const CeCslDocument *d) { return d ? d->visible_bibliography_count : 0; }
const CeCslDocumentBibliography *ce_csl_document_bibliography(const CeCslDocument *d, size_t index) {
    return d && index < d->bibliography_count ? &d->bibliography[index] : NULL;
}
const CeCslRegistry *ce_csl_document_registry(const CeCslDocument *d) { return d ? d->registry : NULL; }
const CeCslDisambiguation *ce_csl_document_disambiguation(const CeCslDocument *d) { return d ? d->disambiguation : NULL; }
size_t ce_csl_document_pass_count(const CeCslDocument *d) { return d ? d->passes : 0; }
