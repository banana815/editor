#include "csl_sort_evaluate.h"
#include "csl_finalize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union Allocation {
    struct { size_t size; } info;
    long double number;
    void *pointer;
    unsigned char alignment[32];
} Allocation;
typedef struct Work {
    CeCslOutputAllocator owner, allocator;
    size_t live;
    bool limited;
    CeCslCollationLanguage language;
    char *error;
    size_t capacity;
} Work;
static bool fail(char *error, size_t cap, const char *message) {
    if (error && cap) snprintf(error, cap, "%s", message);
    return false;
}
static void *normal_allocate(void *context, size_t size) { (void)context; return malloc(size); }
static void normal_free(void *context, void *memory) { (void)context; free(memory); }
static bool owner_get(const CeCslOutputAllocator *a, CeCslOutputAllocator *out) {
    if (a && (!a->allocate || !a->deallocate)) return false;
    *out = a ? *a : (CeCslOutputAllocator){NULL, normal_allocate, normal_free};
    return true;
}
static void *work_allocate(void *context, size_t size) {
    Work *w = context;
    if (size > CE_CSL_SORT_EVALUATE_MAX_WORKSPACE - sizeof(Allocation) ||
        w->live > CE_CSL_SORT_EVALUATE_MAX_WORKSPACE - size - sizeof(Allocation)) {
        w->limited = true; return NULL;
    }
    Allocation *a = w->owner.allocate(w->owner.context, sizeof(*a) + size);
    if (!a) return NULL;
    a->info.size = size + sizeof(*a); w->live += a->info.size;
    return a + 1;
}
static void work_free(void *context, void *memory) {
    if (!memory) return;
    Work *w = context; Allocation *a = (Allocation *)memory - 1;
    w->live -= a->info.size; w->owner.deallocate(w->owner.context, a);
}
static bool unsupported(Work *w, const char *message) {
    if (w->error && w->capacity) snprintf(w->error, w->capacity, "Unsupported sort key: %s", message);
    return false;
}
void ce_csl_sort_evaluated_free(CeCslSortEvaluated *value) {
    if (!value) return;
    ce_csl_sort_key_free(&value->key); memset(value, 0, sizeof(*value));
}
CeCslSortKeyView ce_csl_sort_evaluated_view(const CeCslSortEvaluated *value) {
    if (!value) return (CeCslSortKeyView){NULL, 0, true};
    return (CeCslSortKeyView){value->key.data, value->key.length, value->missing};
}
static bool eq(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static bool key_count(const char *text, size_t *out) {
    size_t n = 0;
    if (!text || !*text) return false;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' ||
            n > (CE_CSL_NAMES_MAX_PEOPLE - (unsigned)(*text - '0')) / 10) return false;
        n = n * 10 + (unsigned)(*text - '0');
    }
    *out = n; return true;
}
static bool setup_request(Work *w, const CeCslEvalRequest *context,
    const CeCslXmlNode *key, CeCslEvalRequest *request, bool *descending) {
    if (!context || !context->style || !context->locale || !context->variables.item ||
        !context->variables.item->data || !ce_csl_xml_is(key, "key") ||
        (context->section != ce_csl_style_citation(context->style) &&
         context->section != ce_csl_style_bibliography(context->style)) ||
        !context->section || (context->variables.cite &&
         context->section == ce_csl_style_bibliography(context->style)))
        return fail(w->error, w->capacity, "Invalid sort context or key");
    const CeCslXmlNode *sort = ce_csl_xml_child(context->section, "sort");
    bool found = false; size_t count = 0;
    for (const CeCslXmlNode *n = sort ? sort->children : NULL; n; n = n->next) {
        if (++count > CE_CSL_SORT_MAX_COLUMNS)
            return fail(w->error, w->capacity, "Sort exceeds 64 keys");
        if (n == key) found = true;
    }
    if (!found) return fail(w->error, w->capacity, "Sort key does not belong to this section");
    *request = *context; request->sorting = true; request->entry = NULL;
    request->has_sort_min = request->has_sort_first = request->has_sort_last = false;
    const char *direction = ce_csl_xml_attribute(key, "sort");
    if (direction && !eq(direction, "ascending") && !eq(direction, "descending"))
        return fail(w->error, w->capacity, "Invalid sort direction");
    *descending = eq(direction, "descending");
    for (size_t i = 0; i < key->attribute_count; ++i) {
        const char *name = key->attributes[i].name, *v = key->attributes[i].value;
        if (eq(name, "macro") || eq(name, "variable") || eq(name, "sort")) continue;
        if (eq(name, "names-min")) {
            request->has_sort_min = true;
            if (key_count(v, &request->sort_min)) continue;
        } else if (eq(name, "names-use-first")) {
            request->has_sort_first = true;
            if (key_count(v, &request->sort_first)) continue;
        } else if (eq(name, "names-use-last")) {
            request->has_sort_last = true;
            if (eq(v, "true") || eq(v, "1")) { request->sort_last = true; continue; }
            if (eq(v, "false") || eq(v, "0")) { request->sort_last = false; continue; }
        }
        return fail(w->error, w->capacity, "Invalid sort key attribute");
    }
    return true;
}
static bool collation(Work *w, const char *text, size_t size, CeCslSortKey *key) {
    return ce_csl_collation_key(text, size, w->language, &w->allocator, key, w->error, w->capacity);
}
static bool text_key(Work *w, const char *text, size_t size, CeCslSortEvaluated *out) {
    out->domain = CE_CSL_SORT_LEXICAL;
    if (!size) { out->missing = true; return true; }
    CeCslSortKey component = {0};
    if (!collation(w, text, size, &component)) return false;
    CeCslSortKeyView parts[4] = {{component.data, component.length, false}, {0}, {0}, {0}};
    bool ok = ce_csl_sort_key_sequence(parts, 4, &w->allocator, &out->key, w->error, w->capacity);
    ce_csl_sort_key_free(&component); return ok;
}
static bool scalar_key(Work *w, const CeCslVariableValue *value, bool numeric, CeCslSortEvaluated *out) {
    size_t size = 0; const char *text = ce_csl_variable_text(value, &size);
    if (!text) return fail(w->error, w->capacity, "Sort scalar is not a scalar value");
    if (!size) { out->missing = true; return true; }
    if (numeric) {
        size_t i = (text[0] == '+' || text[0] == '-') ? 1 : 0, first = i;
        while (i < size && text[i] >= '0' && text[i] <= '9') ++i;
        if (i > first && i == size) {
            out->domain = CE_CSL_SORT_INTEGER;
            return ce_csl_sort_key_integer(text, size, &w->allocator, &out->key, w->error, w->capacity);
        }
        for (i = 0; i < size; ++i)
            if (text[i] >= '0' && text[i] <= '9')
                return unsupported(w, "numeric prefixes, ranges and noninteger values");
    }
    CeCslInline *plain = NULL;
    if (!ce_csl_inline_parse(text, size, 0, &w->allocator, &plain, NULL, w->error, w->capacity)) return false;
    text = ce_csl_inline_text(plain, &size);
    bool ok = text_key(w, text, size, out); ce_csl_inline_free(plain); return ok;
}
static bool date_key(Work *w, const CeCslDateValueView *value, unsigned mask, CeCslSortEvaluated *out) {
    if (!value || !value->present || !mask) { out->missing = true; return true; }
    if (value->literal_size) return unsupported(w, "literal dates have no chronological key");
    if (!value->endpoint_count || value->endpoint_count > 2)
        return fail(w->error, w->capacity, "Invalid sort date endpoints");
    CeCslSortDatePoint points[2] = {{0}};
    bool selected = false;
    for (size_t i = 0; i < value->endpoint_count; ++i) {
        const CeCslDateEndpointView *p = &value->endpoints[i];
        if (!p->year) return unsupported(w, "open date range endpoints");
        points[i].year = mask & CE_CSL_DATE_PART_YEAR ? p->year : 1;
        points[i].month = mask & CE_CSL_DATE_PART_MONTH ? p->month : 0;
        points[i].day = mask & CE_CSL_DATE_PART_DAY ? p->day : 0;
        selected |= (mask & CE_CSL_DATE_PART_YEAR) || points[i].month || points[i].day;
    }
    if (!selected) { out->missing = true; return true; }
    if (mask & CE_CSL_DATE_PART_YEAR) {
        /* A day-only selection with a year has no calendar month, so the
         * complete-date helper would invent an invalid date. Keep it explicit. */
        if ((mask & CE_CSL_DATE_PART_DAY) && !(mask & CE_CSL_DATE_PART_MONTH))
            return unsupported(w, "year/day date mask without month");
        out->domain = CE_CSL_SORT_DATE;
        return ce_csl_sort_key_date(&points[0], value->endpoint_count == 2 ? &points[1] : NULL,
            &w->allocator, &out->key, w->error, w->capacity);
    }
    out->domain = mask == CE_CSL_DATE_PART_MONTH ? CE_CSL_SORT_MONTH :
                  mask == CE_CSL_DATE_PART_DAY ? CE_CSL_SORT_DAY : CE_CSL_SORT_MONTH_DAY;
    /* Fixed-width small unsigned parts, then range marker/end. This domain
     * is deliberately incompatible with complete chronological date keys. */
    unsigned char bytes[5]; size_t used = 0;
    if (mask & CE_CSL_DATE_PART_MONTH) bytes[used++] = (unsigned char)points[0].month;
    if (mask & CE_CSL_DATE_PART_DAY) bytes[used++] = (unsigned char)points[0].day;
    bytes[used++] = value->endpoint_count == 2 ? 1 : 0;
    if (value->endpoint_count == 2) {
        if (mask & CE_CSL_DATE_PART_MONTH) bytes[used++] = (unsigned char)points[1].month;
        if (mask & CE_CSL_DATE_PART_DAY) bytes[used++] = (unsigned char)points[1].day;
    }
    return ce_csl_sort_key_copy(bytes, used, &w->allocator, &out->key, w->error, w->capacity);
}
static unsigned lower_ascii(unsigned c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static void literal_article(const char **text, size_t *size) {
    const char *s = *text; size_t n = *size, skip = 0;
    if (n >= 2 && lower_ascii((unsigned char)s[0]) == 'a') {
        if (s[1] == ' ' || s[1] == '\t' || s[1] == '\n' || s[1] == '\r') skip = 1;
        else if (n >= 3 && lower_ascii((unsigned char)s[1]) == 'n' &&
                 (s[2] == ' ' || s[2] == '\t' || s[2] == '\n' || s[2] == '\r')) skip = 2;
    } else if (n >= 4 && lower_ascii((unsigned char)s[0]) == 't' &&
               lower_ascii((unsigned char)s[1]) == 'h' && lower_ascii((unsigned char)s[2]) == 'e' &&
               (s[3] == ' ' || s[3] == '\t' || s[3] == '\n' || s[3] == '\r')) skip = 3;
    if (skip) {
        while (skip < n && (s[skip] == ' ' || s[skip] == '\t' || s[skip] == '\n' || s[skip] == '\r')) ++skip;
        *text += skip; *size -= skip;
    }
}
static bool names_key(Work *w, const CeCslNamesResult *names, CeCslSortEvaluated *out) {
    const CeCslNamesSummary *summary = ce_csl_names_result_summary(names);
    if (!summary || !summary->rendered_names) { out->missing = true; return true; }
    size_t count = ce_csl_names_result_person_count(names);
    if (!count) { /* COUNT form has summary count but no rendered people. */
        char number[32]; int n = snprintf(number, sizeof(number), "%zu", summary->rendered_names);
        out->domain = CE_CSL_SORT_INTEGER;
        return ce_csl_sort_key_integer(number, (size_t)n, &w->allocator, &out->key, w->error, w->capacity);
    }
    if (count > CE_CSL_SORT_MAX_KEY_PARTS / 4)
        return fail(w->error, w->capacity, "Sort name components exceed their limit");
    size_t n = count * 4;
    CeCslSortKey *owned = work_allocate(w, n * sizeof(*owned));
    CeCslSortKeyView *parts = work_allocate(w, n * sizeof(*parts));
    if (!owned || !parts) {
        work_free(w, owned); work_free(w, parts);
        return fail(w->error, w->capacity, "Out of memory allocating sort name components");
    }
    memset(owned, 0, n * sizeof(*owned)); bool ok = true; size_t bytes = 0;
    for (size_t i = 0; ok && i < count; ++i) {
        const CeCslNameSortView *v = ce_csl_names_result_sort_person(names, i);
        if (!v) { ok = fail(w->error, w->capacity, "Missing typed name sort view"); break; }
        const char *strings[4] = {v->primary_family, v->secondary_particles, v->given, v->suffix};
        size_t sizes[4] = {v->primary_family_length, v->secondary_particles_length, v->given_length, v->suffix_length};
        if (v->kind == CE_CSL_NAME_LITERAL) {
            strings[0] = v->literal; sizes[0] = v->literal_length;
            literal_article(&strings[0], &sizes[0]);
        }
        for (size_t j = 0; ok && j < 4; ++j) {
            size_t index = i * 4 + j;
            if (sizes[j] > CE_CSL_COLLATION_MAX_TEXT_BYTES - bytes) {
                ok = fail(w->error, w->capacity, "Sort name text exceeds aggregate 64 KiB limit"); break;
            }
            bytes += sizes[j];
            if (sizes[j]) ok = collation(w, strings[j], sizes[j], &owned[index]);
            parts[index] = (CeCslSortKeyView){owned[index].data, owned[index].length, false};
        }
    }
    if (ok && bytes) ok = ce_csl_sort_key_sequence(parts, n, &w->allocator, &out->key, w->error, w->capacity);
    if (ok && !bytes) out->missing = true;
    for (size_t i = 0; i < n; ++i) ce_csl_sort_key_free(&owned[i]);
    work_free(w, parts); work_free(w, owned); out->domain = CE_CSL_SORT_LEXICAL; return ok;
}
static bool direct_names(Work *w, const CeCslEvalRequest *context,
    const CeCslVariableValue *value, CeCslSortEvaluated *out) {
    CeCslNamesData *data = NULL; CeCslNamesResult *rendered = NULL;
    CeCslNameOrigin origin = {context->variables.item->id, value->canonical_name};
    CeCslXmlAttr variable = {.name = "variable", .value = (char *)value->canonical_name};
    CeCslXmlNode dummy = {.name = CE_CSL_NAMESPACE "\x1f" "names", .attributes = &variable, .attribute_count = 1};
    CeCslNameOptions options;
    if (!ce_csl_names_resolve(ce_csl_style_root(context->style), context->section, &dummy,
        NULL, &options, w->error, w->capacity)) return false;
    options.form = CE_CSL_NAME_LONG; options.sort = CE_CSL_NAME_SORT_ALL;
    options.has_et_al_min = options.has_et_al_use_first = false;
    options.has_subsequent_min = options.has_subsequent_use_first = false;
    options.et_al_use_last = false;
    /* The direct-variable key has no local initializing name element. */
    options.initialize = false; options.initialize_with = NULL;
    if (!ce_csl_names_data_load(value->node, &origin, &w->allocator, &data, w->error, w->capacity)) return false;
    CeCslNamesBinding binding = {value->canonical_name, data};
    CeCslNameRenderContext render = {.sorting = true, .allocator = &w->allocator,
        .item_language = context->item_language, .style_default_locale = ce_csl_style_default_locale(context->style)};
    bool ok = ce_csl_names_render(&binding, 1, &options, context->locale, &render,
        &rendered, w->error, w->capacity);
    if (ok) ok = names_key(w, rendered, out);
    ce_csl_names_result_free(rendered); ce_csl_names_data_free(data); return ok;
}
static bool direct_key(Work *w, const CeCslEvalRequest *context,
    const char *variable, CeCslSortEvaluated *out) {
    CeCslVariableValue value;
    if (ce_csl_variable_resolve(&context->variables, variable, CE_CSL_VARIABLE_LONG,
        &value, w->error, w->capacity) != CE_CSL_VARIABLE_OK) return false;
    if (value.state != CE_CSL_VARIABLE_VALUE) { out->missing = true; return true; }
    if (value.kind == CE_CSL_VARIABLE_NAMES) return direct_names(w, context, &value, out);
    if (value.kind == CE_CSL_VARIABLE_DATE) {
        CeCslDateValueView view;
        return ce_csl_date_value_view(value.node, &view, w->error, w->capacity) &&
            date_key(w, &view, CE_CSL_DATE_PART_YEAR | CE_CSL_DATE_PART_MONTH | CE_CSL_DATE_PART_DAY, out);
    }
    return scalar_key(w, &value, value.kind == CE_CSL_VARIABLE_NUMBER, out);
}
typedef struct Leaves { const CeCslEvalNode *typed; size_t typed_count, text_count, visited; } Leaves;
static bool scan_leaves(Work *w, const CeCslEvalNode *node, Leaves *leaves, size_t depth) {
    if (!node) return true;
    if (depth > CE_CSL_EVALUATE_MAX_DEPTH || ++leaves->visited > CE_CSL_EVALUATE_MAX_NODES)
        return fail(w->error, w->capacity, "Sort evaluation tree exceeds its traversal limit");
    const CeCslEvalNodeInfo *info = ce_csl_eval_node_info(node);
    if (!info->nonempty || info->suppression != CE_CSL_EVAL_NOT_SUPPRESSED) return true;
    if (info->first_child) {
        for (const CeCslEvalNode *child = info->first_child; child; child = ce_csl_eval_node_info(child)->next)
            if (!scan_leaves(w, child, leaves, depth + 1)) return false;
    } else if (info->kind == CE_CSL_EVAL_NAMES || info->kind == CE_CSL_EVAL_DATE || info->kind == CE_CSL_EVAL_NUMBER) {
        leaves->typed = node; ++leaves->typed_count;
    } else {
        size_t size = 0;
        (void)ce_csl_inline_text(ce_csl_eval_node_content(node), &size);
        if (size) ++leaves->text_count;
    }
    return true;
}
static bool macro_key(Work *w, CeCslEvalRequest *context, const char *macro, CeCslSortEvaluated *out) {
    context->entry = ce_csl_style_macro(context->style, macro);
    if (!context->entry) return fail(w->error, w->capacity, "Unknown sort macro");
    CeCslEvaluation *evaluation = NULL; CeCslFinalized *final = NULL;
    bool ok = ce_csl_evaluate(context, &w->allocator, &evaluation, w->error, w->capacity);
    Leaves leaves = {0};
    if (ok) ok = scan_leaves(w, ce_csl_evaluation_root(evaluation), &leaves, 0);
    if (!ok) goto done;
    if (leaves.typed_count > 1 || (leaves.typed_count && leaves.text_count)) {
        ok = unsupported(w, "mixed typed/text or multiple typed macro leaves"); goto done;
    }
    if (leaves.typed) {
        const CeCslEvalNodeInfo *info = ce_csl_eval_node_info(leaves.typed);
        for (const CeCslEvalNode *n = leaves.typed; n; n = ce_csl_eval_node_info(n)->parent) {
            const CeCslEvalDecoration *d = &ce_csl_eval_node_info(n)->decoration;
            if ((d->prefix && *d->prefix) || (d->suffix && *d->suffix) || d->quotes ||
                d->strip_periods || d->text_case != CE_CSL_CASE_NONE) {
                ok = unsupported(w, "outer text decoration of a typed macro key"); goto done;
            }
        }
        if (info->kind == CE_CSL_EVAL_NAMES) ok = names_key(w, ce_csl_eval_node_names(leaves.typed), out);
        else if (info->kind == CE_CSL_EVAL_DATE) ok = date_key(w, ce_csl_eval_node_date_value(leaves.typed),
            ce_csl_eval_node_date_parts(leaves.typed), out);
        else {
            const CeCslEvalVariableUse *value = ce_csl_eval_node_variable(leaves.typed, 0);
            ok = value ? scalar_key(w, &value->value, true, out) :
                fail(w->error, w->capacity, "Missing typed macro number value");
        }
    } else {
        ok = ce_csl_finalize(evaluation, &w->allocator, &final, w->error, w->capacity);
        if (ok) {
            size_t size = 0; const char *text = ce_csl_inline_text(ce_csl_finalized_inline(final), &size);
            ok = text_key(w, text, size, out);
        }
    }
done:
    ce_csl_finalized_free(final); ce_csl_evaluation_free(evaluation); return ok;
}
bool ce_csl_sort_evaluate(const CeCslEvalRequest *context, const CeCslXmlNode *key,
    CeCslCollationLanguage language, const CeCslOutputAllocator *allocator,
    CeCslSortEvaluated *out, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    Work w = {.language = language, .error = error, .capacity = capacity};
    if (!out || language < CE_CSL_COLLATION_DEFAULT || language > CE_CSL_COLLATION_AZ ||
        !owner_get(allocator, &w.owner)) return fail(error, capacity, "Invalid sort output, allocator or language");
    w.allocator = (CeCslOutputAllocator){&w, work_allocate, work_free};
    CeCslEvalRequest request; CeCslSortEvaluated next = {.language = language};
    if (!setup_request(&w, context, key, &request, &next.descending)) return false;
    const char *variable = ce_csl_xml_attribute(key, "variable"), *macro = ce_csl_xml_attribute(key, "macro");
    if (!!variable == !!macro || (variable && !*variable) || (macro && !*macro))
        return fail(error, capacity, "Sort key requires exactly one variable or macro");
    bool ok = variable ? direct_key(&w, &request, variable, &next) : macro_key(&w, &request, macro, &next);
    if (ok) {
        CeCslSortKey owned = {0};
        ok = ce_csl_sort_key_copy(next.key.data, next.key.length, &w.owner, &owned, error, capacity);
        ce_csl_sort_key_free(&next.key); next.key = owned;
        if (ok) { ce_csl_sort_evaluated_free(out); *out = next; return true; }
    }
    ce_csl_sort_evaluated_free(&next);
    if (w.limited) fail(error, capacity, "Sort evaluation exceeds its 256 MiB workspace limit");
    return false;
}
bool ce_csl_sort_evaluated_order(const CeCslSortEvaluated *keys, size_t rows, size_t columns,
    const CeCslOutputAllocator *allocator, CeCslSortOrder *out, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    CeCslOutputAllocator a;
    if (!out || !owner_get(allocator, &a) || rows > CE_CSL_SORT_MAX_ROWS || columns > CE_CSL_SORT_MAX_COLUMNS ||
        (rows && columns && !keys)) return fail(error, capacity, "Invalid evaluated sort matrix");
    bool directions[CE_CSL_SORT_MAX_COLUMNS];
    for (size_t c = 0; c < columns; ++c) {
        const CeCslSortEvaluated *first = NULL;
        directions[c] = rows ? keys[c].descending : false;
        for (size_t r = 0; r < rows; ++r) {
            const CeCslSortEvaluated *v = &keys[r * columns + c];
            if (v->domain < CE_CSL_SORT_LEXICAL || v->domain > CE_CSL_SORT_DAY ||
                v->language < CE_CSL_COLLATION_DEFAULT || v->language > CE_CSL_COLLATION_AZ ||
                v->descending != directions[c]) return fail(error, capacity, "Inconsistent sort column metadata");
            if (v->missing) continue;
            if (!first) first = v;
            else if (v->domain != first->domain || (v->domain == CE_CSL_SORT_LEXICAL && v->language != first->language))
                return fail(error, capacity, "Unsupported sort column: incompatible key domains or collation languages");
        }
    }
    size_t count = rows * columns;
    CeCslSortKeyView *views = count ? a.allocate(a.context, count * sizeof(*views)) : NULL;
    if (count && !views) return fail(error, capacity, "Out of memory preparing evaluated sort matrix");
    for (size_t i = 0; i < count; ++i) views[i] = ce_csl_sort_evaluated_view(&keys[i]);
    bool ok = ce_csl_sort_order(views, rows, columns, directions, &a, out, error, capacity);
    if (views) a.deallocate(a.context, views);
    return ok;
}
