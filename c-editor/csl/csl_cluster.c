#include "csl_cluster.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union Allocation {
    long double alignment;
    void *pointer;
    unsigned char padding[32];
    size_t size;
} Allocation;
typedef struct Memory {
    CeCslOutputAllocator owner;
    size_t live, limit;
    bool limit_hit, oom;
} Memory;
struct CeCslCluster {
    Memory memory;
    CeCslFinalized *finalized;
    size_t *order;
    CeCslClusterGroup *groups;
    CeCslClusterPiece *pieces;
    size_t count, group_count, piece_count;
};
typedef struct Row {
    const char *key, *prefix, *suffix;
    size_t key_size;
    const CeCslEvalNode *names, *number;
    bool suppress_author, numeric;
} Row;
typedef enum Collapse { NONE, YEAR, NUMBER } Collapse;
typedef struct Build {
    CeCslCluster *result;
    CeCslOutputAllocator allocator;
    CeCslClusterOptions options;
    const CeCslClusterEntry *entries;
    Row *rows;
    CeCslFinalizeJoinEntry *join;
    size_t *sorted, *scratch, *heads, *next;
    size_t count, ordered, compared, work, keys_bytes;
    const char *layout_delimiter, *group_delimiter, *after_delimiter;
    Collapse collapse;
    bool grouping;
    CeCslClusterStatus status;
    char *error; size_t capacity;
} Build;
static void *normal_allocate(void *context, size_t size) { (void)context; return malloc(size); }
static void normal_free(void *context, void *memory) { (void)context; free(memory); }
static void *allocate(void *context, size_t size) {
    Memory *m = context;
    if (size > SIZE_MAX - sizeof(Allocation) || sizeof(Allocation) + size > m->limit - m->live) {
        m->limit_hit = true; return NULL;
    }
    Allocation *block = m->owner.allocate(m->owner.context, sizeof(*block) + size);
    if (!block) { m->oom = true; return NULL; }
    block->size = sizeof(*block) + size; m->live += block->size; return block + 1;
}
static void deallocate(void *context, void *memory) {
    if (!memory) return;
    Memory *m = context; Allocation *block = (Allocation *)memory - 1;
    m->live -= block->size; m->owner.deallocate(m->owner.context, block);
}
void ce_csl_cluster_free(CeCslCluster *r) {
    if (!r) return;
    ce_csl_finalized_free(r->finalized);
    deallocate(&r->memory, r->order); deallocate(&r->memory, r->groups); deallocate(&r->memory, r->pieces);
    r->memory.owner.deallocate(r->memory.owner.context, r);
}
const CeCslFinalized *ce_csl_cluster_finalized(const CeCslCluster *r) { return r ? r->finalized : NULL; }
const CeCslInline *ce_csl_cluster_inline(const CeCslCluster *r) { return r ? ce_csl_finalized_inline(r->finalized) : NULL; }
const CeCslOutput *ce_csl_cluster_output(const CeCslCluster *r) { return r ? ce_csl_finalized_output(r->finalized) : NULL; }
size_t ce_csl_cluster_count(const CeCslCluster *r) { return r ? r->count : 0; }
size_t ce_csl_cluster_order(const CeCslCluster *r, size_t index) { return r && index < r->count ? r->order[index] : SIZE_MAX; }
size_t ce_csl_cluster_group_count(const CeCslCluster *r) { return r ? r->group_count : 0; }
const CeCslClusterGroup *ce_csl_cluster_group(const CeCslCluster *r, size_t i) { return r && i < r->group_count ? &r->groups[i] : NULL; }
size_t ce_csl_cluster_piece_count(const CeCslCluster *r) { return r ? r->piece_count : 0; }
const CeCslClusterPiece *ce_csl_cluster_piece(const CeCslCluster *r, size_t i) { return r && i < r->piece_count ? &r->pieces[i] : NULL; }
static bool stop(Build *b, CeCslClusterStatus status, const char *message) {
    if (b->status == CE_CSL_CLUSTER_OK) {
        b->status = status;
        if (b->error && b->capacity) snprintf(b->error, b->capacity, "CSL cluster: %s", message);
    }
    return false;
}
static bool work(Build *b, size_t amount) {
    if (b->status != CE_CSL_CLUSTER_OK) return false;
    if (b->options.cancelled && b->options.cancelled(b->options.cancel_context))
        return stop(b, CE_CSL_CLUSTER_CANCELLED, "cancelled");
    if (amount > b->options.max_work_units - b->work)
        return stop(b, CE_CSL_CLUSTER_LIMIT, "work budget exceeded");
    b->work += amount; return true;
}
static bool memory_error(Build *b) {
    return stop(b, b->result->memory.limit_hit ? CE_CSL_CLUSTER_LIMIT : CE_CSL_CLUSTER_OOM,
        b->result->memory.limit_hit ? "workspace budget exceeded" : "out of memory");
}
static void *array(Build *b, size_t count, size_t size) {
    if (count > SIZE_MAX / size) { stop(b, CE_CSL_CLUSTER_LIMIT, "allocation size overflow"); return NULL; }
    void *p = allocate(&b->result->memory, count * size);
    if (!p) { memory_error(b); return NULL; }
    memset(p, 0, count * size); return p;
}
static bool eq(const char *a, const char *c) { return a && c && !strcmp(a, c); }
static const char *empty(const char *s) { return s ? s : ""; }
static bool child_error(Build *b) {
    if (b->result->memory.limit_hit || b->result->memory.oom) return memory_error(b);
    b->status = CE_CSL_CLUSTER_RENDER_ERROR; return false; /* Retain child diagnostic. */
}
static bool configuration(Build *b, const CeCslStyle *style) {
    const CeCslXmlNode *citation = ce_csl_style_citation(style), *layout = ce_csl_xml_child(citation, "layout");
    if (!citation || !layout) return stop(b, CE_CSL_CLUSTER_INVALID, "missing citation layout");
    const char *collapse = ce_csl_xml_attribute(citation, "collapse");
    if (!collapse) b->collapse = NONE;
    else if (eq(collapse, "citation-number")) b->collapse = NUMBER;
    else if (eq(collapse, "year")) b->collapse = YEAR;
    else if (eq(collapse, "year-suffix") || eq(collapse, "year-suffix-ranged")) {
        const char *suffix = ce_csl_xml_attribute(citation, "disambiguate-add-year-suffix");
        if (eq(suffix, "true") || eq(suffix, "1"))
            return stop(b, CE_CSL_CLUSTER_UNSUPPORTED, "year-suffix collapsing is not implemented");
        b->collapse = YEAR;
    } else return stop(b, CE_CSL_CLUSTER_INVALID, "unknown collapse mode");
    b->group_delimiter = ce_csl_xml_attribute(citation, "cite-group-delimiter");
    b->grouping = b->group_delimiter != NULL || b->collapse == YEAR;
    if (!b->group_delimiter) b->group_delimiter = ", ";
    b->layout_delimiter = empty(ce_csl_xml_attribute(layout, "delimiter"));
    b->after_delimiter = ce_csl_xml_attribute(citation, "after-collapse-delimiter");
    if (!b->after_delimiter) b->after_delimiter = b->layout_delimiter;
    /* Numeric ranges are based on the already sorted number sequence, never
     * on name regrouping even if a numeric style also sets a group delimiter. */
    if (b->collapse == NUMBER) b->grouping = false;
    return true;
}
static bool cite_string(Build *b, const cJSON *data, const char *name, const char **out) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(data, name);
    if (!value || cJSON_IsNull(value)) { *out = ""; return true; }
    if (!cJSON_IsString(value) || !value->valuestring) return stop(b, CE_CSL_CLUSTER_INVALID, "invalid cite affix");
    size_t size = 0;
    while (size <= CE_CSL_OUTPUT_MAX_BYTES && value->valuestring[size]) ++size;
    if (size > CE_CSL_OUTPUT_MAX_BYTES) return stop(b, CE_CSL_CLUSTER_LIMIT, "cite affix exceeds output limit");
    if (!work(b, size)) return false;
    *out = value->valuestring; return true;
}
static bool cite_flag(Build *b, const cJSON *data, const char *name, bool *out) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(data, name);
    if (value && !cJSON_IsBool(value)) return stop(b, CE_CSL_CLUSTER_INVALID, "invalid cite flag");
    *out = cJSON_IsTrue(value); return true;
}
static bool integer(const CeCslVariableValue *v, uint64_t *out) {
    size_t n = 0; const char *s = ce_csl_variable_text(v, &n);
    uint64_t value = 0;
    if (!s || !n || n > 20) return false;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9' || value > (UINT64_MAX - (unsigned)(s[i] - '0')) / 10) return false;
        value = value * 10 + (unsigned)(s[i] - '0');
    }
    *out = value; return true;
}
static bool numeric_tree(Build *b, size_t index, const CeCslEvalNode *node, size_t depth,
    size_t *number_count, bool *other) {
    if (!work(b, 1)) return false;
    if (depth > CE_CSL_EVALUATE_MAX_DEPTH) return stop(b, CE_CSL_CLUSTER_LIMIT, "numeric tree depth exceeded");
    const CeCslEvalNodeInfo *info = ce_csl_eval_node_info(node);
    if (!info) return stop(b, CE_CSL_CLUSTER_INVALID, "invalid evaluation node");
    Row *row = &b->rows[index];
    if (!info->nonempty || (row->suppress_author && node == row->names)) return true;
    if (info->first_child) {
        for (const CeCslEvalNode *n = info->first_child; n; n = ce_csl_eval_node_info(n)->next)
            if (!numeric_tree(b, index, n, depth + 1, number_count, other)) return false;
        return true;
    }
    size_t bytes = 0; (void)ce_csl_inline_text(ce_csl_eval_node_content(node), &bytes);
    if (!bytes) return true;
    size_t uses = ce_csl_eval_node_variable_count(node);
    const CeCslEvalVariableUse *u = ce_csl_eval_node_variable(node, 0);
    if ((info->kind != CE_CSL_EVAL_TEXT && info->kind != CE_CSL_EVAL_NUMBER) ||
        uses != 1 || !u || !u->rendered || !eq(u->value.canonical_name, "citation-number")) {
        *other = true; return true;
    }
    uint64_t number = 0; CeCslGeneratedNumber supplied = b->entries[index].citation_number;
    if (!supplied.resolved || !supplied.present || !supplied.value ||
        !integer(&u->value, &number) || number != supplied.value)
        return stop(b, CE_CSL_CLUSTER_INVALID, "citation number does not match its evaluated generated value");
    ++*number_count; row->number = node; return true;
}
static bool prepare(Build *b, const CeCslStyle *style, const CeCslLocale *locale) {
    const CeCslXmlNode *layout = ce_csl_xml_child(ce_csl_style_citation(style), "layout");
    for (size_t i = 0; i < b->count; ++i) {
        if (!work(b, 1)) return false;
        const CeCslClusterEntry *entry = &b->entries[i]; Row *row = &b->rows[i];
        const CeCslEvalNode *root = ce_csl_evaluation_root(entry->evaluation);
        const CeCslEvalNodeInfo *info = ce_csl_eval_node_info(root);
        if (!entry->cite || !cJSON_IsObject(entry->cite->data) || entry->source_cite_index == SIZE_MAX ||
            !info || info->kind != CE_CSL_EVAL_LAYOUT || info->occurrence.source != layout ||
            ce_csl_evaluation_locale(entry->evaluation) != locale || !entry->finalized ||
            ce_csl_finalized_evaluation(entry->finalized) != entry->evaluation)
            return stop(b, CE_CSL_CLUSTER_INVALID, "entry does not match its citation evaluation/finalization");
        bool author_only = false;
        if (!cite_flag(b, entry->cite->data, "author-only", &author_only) ||
            !cite_flag(b, entry->cite->data, "suppress-author", &row->suppress_author) ||
            !cite_string(b, entry->cite->data, "prefix", &row->prefix) ||
            !cite_string(b, entry->cite->data, "suffix", &row->suffix)) return false;
        if (author_only) return stop(b, CE_CSL_CLUSTER_UNSUPPORTED, "author-only projection is not implemented");
        row->names = ce_csl_finalized_first_names(entry->finalized);
        const CeCslInline *names = ce_csl_finalized_first_names_inline(entry->finalized);
        if (b->grouping && names) {
            row->key = ce_csl_inline_text(names, &row->key_size);
            if (row->key_size > b->options.max_key_bytes ||
                row->key_size > b->options.max_keys_bytes - b->keys_bytes)
                return stop(b, CE_CSL_CLUSTER_LIMIT, "names grouping key budget exceeded");
            b->keys_bytes += row->key_size;
        }
        if (b->collapse == NUMBER) {
            size_t number_count = 0; bool other = false;
            if (!numeric_tree(b, i, root, 0, &number_count, &other)) return false;
            row->numeric = number_count == 1 && !other && !*row->suffix;
        }
        b->sorted[i] = i; b->heads[i] = i; b->next[i] = SIZE_MAX;
    }
    return true;
}
static int key_compare(Build *b, size_t a, size_t c) {
    if (!work(b, 1)) return 0;
    const Row *x = &b->rows[a], *y = &b->rows[c];
    /* Empty name fields never form a group. */
    if (!x->key_size || !y->key_size) {
        if (!!x->key_size != !!y->key_size) return x->key_size ? -1 : 1;
        return a < c ? -1 : a > c ? 1 : 0;
    }
    size_t n = x->key_size < y->key_size ? x->key_size : y->key_size;
    if (n > b->options.max_compared_bytes - b->compared) {
        stop(b, CE_CSL_CLUSTER_LIMIT, "names comparison byte budget exceeded"); return 0;
    }
    b->compared += n;
    int d = memcmp(x->key, y->key, n);
    return d ? d : (x->key_size > y->key_size) - (x->key_size < y->key_size);
}
static bool group_names(Build *b) {
    for (size_t width = 1; width < b->count; width *= 2) {
        for (size_t start = 0; start < b->count; start += 2 * width) {
            size_t middle = start + width < b->count ? start + width : b->count;
            size_t end = start + 2 * width < b->count ? start + 2 * width : b->count;
            size_t left = start, right = middle, out = start;
            while (left < middle && right < end) {
                int d = key_compare(b, b->sorted[left], b->sorted[right]);
                if (b->status != CE_CSL_CLUSTER_OK) return false;
                b->scratch[out++] = d <= 0 ? b->sorted[left++] : b->sorted[right++];
            }
            while (left < middle) { if (!work(b, 1)) return false; b->scratch[out++] = b->sorted[left++]; }
            while (right < end) { if (!work(b, 1)) return false; b->scratch[out++] = b->sorted[right++]; }
        }
        size_t *swap = b->sorted; b->sorted = b->scratch; b->scratch = swap;
    }
    size_t head = b->sorted[0], previous = head;
    for (size_t i = 1; i < b->count; ++i) {
        size_t current = b->sorted[i];
        int d = key_compare(b, previous, current);
        if (b->status != CE_CSL_CLUSTER_OK) return false;
        if (!d && b->rows[current].key_size) { b->heads[current] = head; b->next[previous] = current; }
        else head = current;
        previous = current;
    }
    return true;
}
static bool number_path_equal(Build *b, const CeCslEvalNode *a, const CeCslEvalNode *c) {
    while (a && c) {
        if (!work(b, 1)) return false;
        const CeCslEvalNodeInfo *x = ce_csl_eval_node_info(a), *y = ce_csl_eval_node_info(c);
        if (x->kind != y->kind || x->occurrence.source != y->occurrence.source || x->target != y->target) return false;
        a = x->parent; c = y->parent;
    }
    return !a && !c;
}
static bool new_piece(Build *b, size_t entry_index, size_t group_index, bool omit) {
    if (!work(b, 1)) return false;
    CeCslCluster *r = b->result; size_t index = r->piece_count++;
    const CeCslClusterEntry *entry = &b->entries[entry_index]; Row *row = &b->rows[entry_index];
    const CeCslEvalNode *omitted = omit ? row->names : NULL;
    r->pieces[index] = (CeCslClusterPiece){entry_index, entry->source_cite_index, group_index, omitted != NULL, false};
    b->join[index] = (CeCslFinalizeJoinEntry){entry->evaluation, omitted,
        row->prefix, row->suffix, "", entry->source_cite_index};
    const CeCslFinalized *output = entry->finalized; CeCslFinalized *projected = NULL;
    if (omitted) {
        if (!work(b, ce_csl_evaluation_summary(entry->evaluation)->node_count)) return false;
        CeCslFinalizeOptions options = {.omit_names = omitted};
        if (!ce_csl_finalize_with_options(entry->evaluation, &options, &b->allocator, &projected,
            b->error, b->capacity)) return child_error(b);
        output = projected;
    }
    size_t size = 0; (void)ce_csl_inline_text(ce_csl_finalized_inline(output), &size);
    r->pieces[index].visible = size != 0; ce_csl_finalized_free(projected);
    return true;
}
static bool plan_names(Build *b) {
    CeCslCluster *r = b->result;
    for (size_t i = 0; i < b->count; ++i) {
        if (!work(b, 1)) return false;
        if (b->heads[i] != i) continue;
        size_t group = r->group_count++;
        CeCslClusterGroup *g = &r->groups[group];
        g->first_order = b->ordered; g->first_piece = r->piece_count;
        for (size_t row = i; row != SIZE_MAX; row = b->next[row]) {
            r->order[b->ordered++] = row;
            bool omit = b->rows[row].suppress_author || (b->collapse == YEAR && g->member_count > 0);
            if (!new_piece(b, row, group, omit)) return false;
            ++g->member_count;
        }
        g->piece_count = r->piece_count - g->first_piece;
        g->kind = g->member_count > 1 ? CE_CSL_CLUSTER_NAMES : CE_CSL_CLUSTER_SINGLE;
        g->collapsed = g->member_count > 1 && b->collapse == YEAR;
    }
    return true;
}
static bool plan_numbers(Build *b) {
    CeCslCluster *r = b->result;
    for (size_t start = 0; start < b->count;) {
        if (!work(b, 1)) return false;
        size_t end = start + 1;
        if (b->rows[start].numeric) while (end < b->count && b->rows[end].numeric && !*b->rows[end].prefix) {
            uint64_t prior = b->entries[end - 1].citation_number.value;
            if (prior == UINT64_MAX || b->entries[end].citation_number.value != prior + 1 ||
                !number_path_equal(b, b->rows[start].number, b->rows[end].number)) break;
            ++end;
        }
        if (b->status != CE_CSL_CLUSTER_OK) return false;
        bool range = end - start >= 3;
        if (!range) end = start + 1;
        size_t group = r->group_count++;
        CeCslClusterGroup *g = &r->groups[group];
        *g = (CeCslClusterGroup){b->ordered, end - start, r->piece_count, range ? 2 : 1,
            range ? CE_CSL_CLUSTER_NUMERIC_RANGE : CE_CSL_CLUSTER_SINGLE, range, false};
        for (size_t row = start; row < end; ++row) {
            if (!work(b, 1)) return false;
            r->order[b->ordered++] = row;
        }
        if (!new_piece(b, start, group, b->rows[start].suppress_author) ||
            (range && !new_piece(b, end - 1, group, b->rows[end - 1].suppress_author))) return false;
        start = end;
    }
    return true;
}
static bool delimiters(Build *b) {
    CeCslCluster *r = b->result; bool previous_collapsed = false;
    for (size_t i = 0; i < r->group_count; ++i) {
        if (!work(b, 1)) return false;
        CeCslClusterGroup *g = &r->groups[i];
        for (size_t j = g->first_piece; j < g->first_piece + g->piece_count; ++j) {
            if (!work(b, 1)) return false;
            if (!r->pieces[j].visible) continue;
            b->join[j].before_delimiter = !g->visible ?
                (previous_collapsed ? b->after_delimiter : b->layout_delimiter) :
                g->kind == CE_CSL_CLUSTER_NUMERIC_RANGE ? "\xe2\x80\x93" : b->group_delimiter;
            g->visible = true;
        }
        if (g->visible) previous_collapsed = g->collapsed;
    }
    return true;
}
static void build_clear(Build *b) {
    if (!b->result) return;
    Memory *m = &b->result->memory;
    deallocate(m, b->rows); deallocate(m, b->join); deallocate(m, b->sorted);
    deallocate(m, b->scratch); deallocate(m, b->heads); deallocate(m, b->next);
}
CeCslClusterStatus ce_csl_cluster_build(const CeCslStyle *style, const CeCslLocale *locale,
    const CeCslClusterEntry *entries, size_t count, const CeCslClusterOptions *options,
    const CeCslOutputAllocator *allocator, CeCslCluster **out, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    Build b = {.entries = entries, .count = count, .error = error, .capacity = capacity};
    if (!style || !locale || !entries || !count || count > CE_CSL_INPUT_MAX_CITES || !out ||
        (allocator && (!allocator->allocate || !allocator->deallocate))) {
        stop(&b, CE_CSL_CLUSTER_INVALID, "invalid cluster arguments"); return b.status;
    }
    if (options) b.options = *options;
#define LIMIT(field, maximum) do { \
    if (!b.options.field) b.options.field = maximum; \
    if (b.options.field > maximum) { stop(&b, CE_CSL_CLUSTER_INVALID, "budget exceeds hard limit"); return b.status; } \
} while (0)
    LIMIT(max_key_bytes, CE_CSL_CLUSTER_MAX_KEY_BYTES);
    LIMIT(max_keys_bytes, CE_CSL_CLUSTER_MAX_KEYS_BYTES);
    LIMIT(max_compared_bytes, CE_CSL_CLUSTER_MAX_COMPARED_BYTES);
    LIMIT(max_work_units, CE_CSL_CLUSTER_MAX_WORK);
    LIMIT(max_workspace_bytes, CE_CSL_CLUSTER_MAX_WORKSPACE);
#undef LIMIT
    if (!configuration(&b, style) || !work(&b, 1)) return b.status;
    CeCslOutputAllocator owner = allocator ? *allocator : (CeCslOutputAllocator){NULL, normal_allocate, normal_free};
    if (sizeof(*b.result) > b.options.max_workspace_bytes) {
        stop(&b, CE_CSL_CLUSTER_LIMIT, "workspace is too small"); return b.status;
    }
    b.result = owner.allocate(owner.context, sizeof(*b.result));
    if (!b.result) { stop(&b, CE_CSL_CLUSTER_OOM, "out of memory"); return b.status; }
    memset(b.result, 0, sizeof(*b.result));
    b.result->memory = (Memory){owner, sizeof(*b.result), b.options.max_workspace_bytes, false, false};
    b.allocator = (CeCslOutputAllocator){&b.result->memory, allocate, deallocate};
    b.result->count = count;
#define ARRAY(field) do { b.field = array(&b, count, sizeof(*b.field)); if (!b.field) goto cleanup; } while (0)
    ARRAY(result->order); ARRAY(result->groups); ARRAY(result->pieces);
    ARRAY(rows); ARRAY(join); ARRAY(sorted); ARRAY(scratch); ARRAY(heads); ARRAY(next);
#undef ARRAY
    if (!prepare(&b, style, locale) || (b.grouping && !group_names(&b))) goto cleanup;
    if (!(b.collapse == NUMBER ? plan_numbers(&b) : plan_names(&b)) || !delimiters(&b)) goto cleanup;
    for (size_t i = 0; i < b.result->piece_count; ++i)
        if (!work(&b, ce_csl_evaluation_summary(b.join[i].evaluation)->node_count)) goto cleanup;
    if (!ce_csl_finalize_join(b.join, b.result->piece_count, &b.allocator, &b.result->finalized, error, capacity)) {
        child_error(&b); goto cleanup;
    }
    if (!work(&b, 1)) goto cleanup;
cleanup:
    build_clear(&b);
    if (b.status == CE_CSL_CLUSTER_OK) { ce_csl_cluster_free(*out); *out = b.result; }
    else ce_csl_cluster_free(b.result);
    return b.status;
}
