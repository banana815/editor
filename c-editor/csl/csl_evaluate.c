#include "csl_evaluate.h"
#include "csl_format.h"
#include "csl_page_field.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union Allocation {
    struct { size_t bytes; } header;
    /* MSVC's C headers do not consistently provide max_align_t. Preserve
     * malloc alignment for the scalar/pointer types used by all child modules. */
    long double alignment;
    void *pointer;
    unsigned char padding[32];
} Allocation;
typedef struct Use {
    CeCslEvalVariableUse public;
    char *owned_text;
} Use;
struct CeCslEvalNode {
    CeCslEvalNodeInfo info;
    struct CeCslEvalNode *allocated_next, *last_child;
    const CeCslXmlNode **calls;
    Use *uses;
    size_t use_count;
    CeCslInline *preview, *content;
    size_t quote_depth;
    CeCslNamesResult *names;
    CeCslDateResult date;
    CeCslDateValueView date_value;
    unsigned date_parts;
    CeCslNumberFieldInfo number;
};
struct CeCslEvaluation {
    CeCslOutputAllocator owner, allocator;
    size_t workspace, expanded_count;
    bool limit, oom;
    CeCslEvalRequest request;
    CeCslVariableContext variables;
    CeCslEvalNode *root, *allocated;
    CeCslEvalSummary summary;
    const char *language;
    char *language_owned;
    const char *rendered[CE_CSL_EVALUATE_MAX_VARIABLES];
    const char *suppressed[CE_CSL_EVALUATE_MAX_VARIABLES];
    size_t rendered_count, suppressed_count;
    const CeCslXmlNode *calls[CE_CSL_EVALUATE_MAX_DEPTH];
    size_t call_count, quote_depth;
    char *error;
    size_t capacity;
};
typedef struct Mark {
    CeCslEvalNode *allocated;
    size_t rendered, suppressed;
    CeCslEvalSummary summary;
} Mark;
static bool fail(CeCslEvaluation *e, const char *message) {
    if (e->error && e->capacity)
        snprintf(e->error, e->capacity, "CSL evaluator: %s", message);
    return false;
}
static void *default_allocate(void *context, size_t bytes) { (void)context; return malloc(bytes); }
static void default_free(void *context, void *memory) { (void)context; free(memory); }
static void *allocate(void *context, size_t bytes) {
    CeCslEvaluation *e = context;
    if (bytes > CE_CSL_EVALUATE_MAX_WORKSPACE - sizeof(Allocation) ||
        bytes + sizeof(Allocation) > CE_CSL_EVALUATE_MAX_WORKSPACE - e->workspace) {
        e->limit = true;
        fail(e, "workspace limit exceeded");
        return NULL;
    }
    Allocation *block = e->owner.allocate(e->owner.context, sizeof(*block) + bytes);
    if (!block) { e->oom = true; fail(e, "out of memory"); return NULL; }
    block->header.bytes = sizeof(*block) + bytes;
    e->workspace += block->header.bytes;
    return block + 1;
}
static void release(void *context, void *memory) {
    if (!memory) return;
    CeCslEvaluation *e = context;
    Allocation *block = (Allocation *)memory - 1;
    e->workspace -= block->header.bytes;
    e->owner.deallocate(e->owner.context, block);
}
static bool eq(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static const char *attr(const CeCslXmlNode *n, const char *name) { return ce_csl_xml_attribute(n, name); }
static const char *empty(const char *s) { return s ? s : ""; }
static size_t inline_size(const CeCslInline *s) { size_t n = 0; ce_csl_inline_text(s, &n); return n; }
static bool payload(CeCslEvaluation *e, size_t bytes) {
    if (bytes > CE_CSL_EVALUATE_MAX_BYTES - e->summary.leaf_bytes)
        return fail(e, "aggregate entry leaf payload limit exceeded");
    e->summary.leaf_bytes += bytes;
    return true;
}
static bool set_has(const char *const *set, size_t count, const char *name) {
    for (size_t i = 0; i < count; ++i) if (eq(set[i], name)) return true;
    return false;
}
static bool set_add(CeCslEvaluation *e, const char **set, size_t *count, const char *name) {
    if (set_has(set, *count, name)) return true;
    if (*count == CE_CSL_EVALUATE_MAX_VARIABLES) return fail(e, "canonical variable set limit exceeded");
    set[(*count)++] = name;
    return true;
}
static bool suppressed(void *context, const char *name, bool *value, char *error, size_t cap) {
    CeCslEvaluation *e = context;
    bool result = set_has(e->suppressed, e->suppressed_count, name);
    if (!result && e->request.variables.suppressed &&
        !e->request.variables.suppressed(e->request.variables.context, name, &result, error, cap)) return false;
    *value = result;
    return true;
}
static CeCslVariableStatus abbreviation(void *context, const char *name, const cJSON *long_value,
    CeCslVariableShortValue *value, char *error, size_t cap) {
    CeCslEvaluation *e = context;
    return e->request.variables.short_value(e->request.variables.context, name, long_value, value, error, cap);
}
static void node_free(CeCslEvaluation *e, CeCslEvalNode *n) {
    ce_csl_inline_free(n->preview);
    ce_csl_inline_free(n->content);
    ce_csl_names_result_free(n->names);
    for (size_t i = 0; i < n->use_count; ++i) release(e, n->uses[i].owned_text);
    release(e, n->uses);
    release(e, n->calls);
    release(e, n);
}
static Mark mark(CeCslEvaluation *e) {
    return (Mark){e->allocated, e->rendered_count, e->suppressed_count, e->summary};
}
static void rollback(CeCslEvaluation *e, Mark m, bool nodes) {
    e->rendered_count = m.rendered;
    e->suppressed_count = m.suppressed;
    if (nodes) {
        while (e->allocated != m.allocated) {
            CeCslEvalNode *n = e->allocated;
            e->allocated = n->allocated_next;
            node_free(e, n);
        }
        e->summary = m.summary;
    }
}
void ce_csl_evaluation_free(CeCslEvaluation *e) {
    if (!e) return;
    while (e->allocated) {
        CeCslEvalNode *n = e->allocated;
        e->allocated = n->allocated_next;
        node_free(e, n);
    }
    release(e, e->language_owned);
    CeCslOutputAllocator owner = e->owner;
    owner.deallocate(owner.context, e);
}
static CeCslEvalNode *node_new(CeCslEvaluation *e, const CeCslXmlNode *source) {
    if (e->expanded_count == CE_CSL_EVALUATE_MAX_NODES) {
        fail(e, "expanded node limit exceeded"); return NULL;
    }
    ++e->expanded_count; /* Failed/empty speculative branches still consume work. */
    CeCslEvalNode *n = allocate(e, sizeof(*n));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->allocated_next = e->allocated;
    e->allocated = n;
    n->info.index = e->summary.node_count++;
    n->info.occurrence.source = source;
    if (e->call_count) {
        n->calls = allocate(e, e->call_count * sizeof(*n->calls));
        if (!n->calls) return NULL;
        memcpy(n->calls, e->calls, e->call_count * sizeof(*n->calls));
    }
    n->info.occurrence.macro_calls = n->calls;
    n->info.occurrence.macro_depth = e->call_count;
    return n;
}
static void child_add(CeCslEvalNode *parent, CeCslEvalNode *child) {
    child->info.parent = parent;
    if (parent->last_child) parent->last_child->info.next = child;
    else parent->info.first_child = child;
    parent->last_child = child;
}
static bool inline_plain(CeCslEvaluation *e, const char *text, size_t size, unsigned format,
    bool nocase, CeCslInline **out) {
    CeCslOutput *body = ce_csl_output_new(&e->allocator, e->error, e->capacity);
    if (!body) return false;
    bool ok = ce_csl_output_append(body, text, size, format, NULL, e->error, e->capacity) &&
        ce_csl_inline_from_output(body, nocase, &e->allocator, out, e->error, e->capacity);
    ce_csl_output_free(body);
    return ok;
}
static bool append_plain(CeCslEvaluation *e, CeCslInline *out, const char *text, unsigned format) {
    if (!text || !*text) return true;
    CeCslInline *part = NULL;
    bool ok = inline_plain(e, text, strlen(text), format, false, &part) &&
        ce_csl_inline_concat(out, part, e->error, e->capacity);
    ce_csl_inline_free(part);
    return ok;
}
static bool strip_periods(CeCslEvaluation *e, CeCslInline **text) {
    return ce_csl_inline_strip_periods(*text, &e->allocator, text, NULL, e->error, e->capacity);
}
static bool bool_attr(CeCslEvaluation *e, const CeCslXmlNode *node, const char *name, bool *out) {
    const char *value = attr(node, name);
    if (!value || eq(value, "false")) { *out = false; return true; }
    if (eq(value, "true")) { *out = true; return true; }
    return fail(e, "invalid boolean rendering attribute");
}
static bool decorations(CeCslEvaluation *e, CeCslEvalNode *n, unsigned inherited) {
    const CeCslXmlNode *source = n->info.occurrence.source;
    CeCslEvalDecoration *d = &n->info.decoration;
    d->inherited_format = inherited;
    if (!ce_csl_format_resolve(source, inherited, &d->effective_format, e->error, e->capacity)) return false;
    d->prefix = attr(source, "prefix"); d->suffix = attr(source, "suffix");
    d->delimiter = attr(source, "delimiter");
    if (!bool_attr(e, source, "quotes", &d->quotes) ||
        !bool_attr(e, source, "strip-periods", &d->strip_periods)) return false;
    const char *mode = attr(source, "text-case");
    const char *cases[] = {"", "lowercase", "uppercase", "capitalize-first", "capitalize-all", "sentence", "title"};
    if (mode) {
        bool found = false;
        for (size_t i = 1; i < sizeof(cases) / sizeof(*cases); ++i)
            if (eq(mode, cases[i])) { d->text_case = (CeCslTextCase)i; found = true; break; }
        if (!found) return fail(e, "unsupported text-case");
    }
    const char *display = attr(source, "display");
    const char *displays[] = {"", "block", "left-margin", "right-inline", "indent"};
    if (display) {
        bool found = false;
        for (size_t i = 1; i < sizeof(displays) / sizeof(*displays); ++i)
            if (eq(display, displays[i])) { d->display = (CeCslEvalDisplay)i; found = true; break; }
        if (!found) return fail(e, "unsupported display");
        if (!ce_csl_xml_is(e->request.section, "bibliography")) return fail(e, "display is only supported in bibliography");
    }
    return true;
}
/* Probe uses the same inline case operation as finalization will use, retaining
 * original semantic decoration metadata. No case transform is done per run. */
static bool decorate(CeCslEvaluation *e, CeCslEvalNode *n) {
    CeCslEvalDecoration *d = &n->info.decoration;
    if (d->strip_periods && !strip_periods(e, &n->preview)) return false;
    if (d->text_case != CE_CSL_CASE_NONE &&
        !ce_csl_inline_case(n->preview, d->text_case, e->language,
            ce_csl_style_default_locale(e->request.style), &e->allocator,
            &n->preview, NULL, e->error, e->capacity)) return false;
    if (!inline_size(n->preview)) { n->info.nonempty = false; return true; }
    CeCslInline *wrapped = NULL;
    if (!inline_plain(e, "", 0, 0, false, &wrapped)) return false;
    unsigned affix_format = n->info.kind == CE_CSL_EVAL_LAYOUT ? d->effective_format : d->inherited_format;
    bool ok = d->cluster_envelope || append_plain(e, wrapped, d->prefix, affix_format);
    CeCslLocaleTerm open = {0}, close = {0};
    if (d->quotes) {
        open = ce_csl_locale_term(e->request.locale, n->quote_depth % 2 ? "open-inner-quote" : "open-quote",
            CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER);
        close = ce_csl_locale_term(e->request.locale, n->quote_depth % 2 ? "close-inner-quote" : "close-quote",
            CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER);
        n->info.missing_term |= !open.found || !close.found;
        ok = ok && append_plain(e, wrapped, open.text, d->effective_format);
    }
    ok = ok && ce_csl_inline_concat(wrapped, n->preview, e->error, e->capacity);
    if (d->quotes) ok = ok && append_plain(e, wrapped, close.text, d->effective_format);
    ok = ok && (d->cluster_envelope || append_plain(e, wrapped, d->suffix, affix_format));
    if (!ok) { ce_csl_inline_free(wrapped); return false; }
    ce_csl_inline_free(n->preview);
    n->preview = wrapped;
    n->info.nonempty = inline_size(wrapped) != 0;
    return true;
}
static bool uses_allocate(CeCslEvaluation *e, CeCslEvalNode *n, size_t count) {
    if (count > CE_CSL_NAMES_MAX_BINDINGS) return fail(e, "too many variable bindings");
    if (!count) return true;
    n->uses = allocate(e, count * sizeof(*n->uses));
    if (!n->uses) return false;
    memset(n->uses, 0, count * sizeof(*n->uses));
    n->use_count = count;
    return true;
}
static bool bind(CeCslEvaluation *e, CeCslEvalNode *n, size_t index, const char *name, CeCslVariableForm form) {
    Use *use = &n->uses[index];
    CeCslVariableValue *value = &use->public.value;
    if (ce_csl_variable_resolve(&e->variables, name, form, value, e->error, e->capacity) != CE_CSL_VARIABLE_OK) return false;
    n->info.variable_called = true;
    n->info.metadata_present |= value->state == CE_CSL_VARIABLE_VALUE;
    size_t size = 0;
    const char *text = ce_csl_variable_text(value, &size);
    if (text && !size && !value->text_is_inline) value->borrowed_text = "";
    if (text && size && !value->text_is_inline) {
        if (!payload(e, size)) return false;
        use->owned_text = allocate(e, size + 1);
        if (!use->owned_text) return false;
        memcpy(use->owned_text, text, size); use->owned_text[size] = 0;
        value->borrowed_text = use->owned_text;
    }
    return true;
}
static bool rendered_use(CeCslEvaluation *e, CeCslEvalNode *n, size_t index) {
    Use *u = &n->uses[index];
    if (u->public.value.state != CE_CSL_VARIABLE_VALUE) return true;
    u->public.rendered = true;
    n->info.rendered_variable_present = true;
    if (!set_add(e, e->rendered, &e->rendered_count, u->public.value.canonical_name)) return false;
    for (size_t i = 0; i < u->public.value.dependency_count; ++i)
        if (!set_add(e, e->rendered, &e->rendered_count, u->public.value.dependencies[i])) return false;
    return true;
}
static bool locale_form(CeCslEvaluation *e, const char *value, CeCslLocaleForm *form) {
    const char *forms[] = {"long", "short", "verb", "verb-short", "symbol"};
    if (!value) { *form = CE_CSL_LOCALE_LONG; return true; }
    for (size_t i = 0; i < sizeof(forms) / sizeof(*forms); ++i)
        if (eq(value, forms[i])) { *form = (CeCslLocaleForm)i; return true; }
    return fail(e, "unsupported term form");
}
static bool filtered_node(CeCslEvaluation *e, const CeCslXmlNode *source,
    CeCslXmlNode *view, CeCslXmlAttr attributes[128], bool date) {
    if (source->attribute_count > 128) return fail(e, "too many rendering attributes");
    *view = *source;
    view->attributes = attributes; view->attribute_count = 0;
    for (size_t i = 0; i < source->attribute_count; ++i) {
        const char *key = source->attributes[i].name;
        if (eq(key, "display") || eq(key, "prefix") || eq(key, "suffix") || (date && eq(key, "text-case"))) continue;
        attributes[view->attribute_count++] = source->attributes[i];
    }
    return true;
}
static bool numeric_form(CeCslEvaluation *e, const char *value, CeCslNumberForm *form) {
    const char *forms[] = {"numeric", "ordinal", "long-ordinal", "roman"};
    if (!value) { *form = CE_CSL_NUMBER_NUMERIC; return true; }
    for (size_t i = 0; i < sizeof(forms) / sizeof(*forms); ++i)
        if (eq(value, forms[i])) { *form = (CeCslNumberForm)i; return true; }
    return fail(e, "unsupported number form");
}
static bool page_inline(CeCslEvaluation *e, CeCslEvalNode *n,
    const char *text, size_t size, const char *variable) {
    CeCslPageFieldOptions options = {CE_CSL_PAGE_FIELD_PAGE, false, CE_CSL_PAGE_EXPANDED};
    const char *style_format = attr(ce_csl_style_root(e->request.style), "page-range-format");
    if (style_format) {
        const char *formats[] = {"expanded", "minimal", "minimal-two", "chicago", "chicago-15", "chicago-16"};
        bool found = false;
        for (size_t i = 0; i < sizeof(formats)/sizeof(*formats); ++i)
            if (eq(style_format, formats[i])) { options.format = (CeCslPageRangeFormat)i; found = true; break; }
        if (!found) return fail(e, "unsupported page-range-format");
        options.has_format = true;
    }
    if (eq(variable, "locator")) {
        const cJSON *label = e->request.variables.cite ?
            cJSON_GetObjectItemCaseSensitive(e->request.variables.cite->data, "label") : NULL;
        if (label && !cJSON_IsString(label)) return fail(e, "invalid locator label");
        options.policy = !label || eq(label->valuestring, "page") ?
            CE_CSL_PAGE_FIELD_PAGE_LOCATOR : CE_CSL_PAGE_FIELD_OTHER_LOCATOR;
    }
    /* Sorting consumes retained typed numeric sources, not display abbreviations. */
    if (e->request.sorting) { options.policy = CE_CSL_PAGE_FIELD_PAGE; options.has_format = false; }
    CeCslOutputString rendered = {0}; CeCslPageFieldInfo info = {0};
    bool ok = ce_csl_page_field_render(text, size, &options, e->request.locale, &e->allocator,
        &rendered, &info, e->error, e->capacity);
    if (ok) ok = inline_plain(e, rendered.data, rendered.length, n->info.decoration.effective_format, false, &n->preview);
    if (ok) n->info.missing_term |= info.missing_term;
    ce_csl_output_string_free(&rendered);
    return ok;
}
static bool scalar_inline(CeCslEvaluation *e, CeCslEvalNode *n, const char *text, size_t size,
    const char *variable, bool markup) {
    unsigned format = n->info.decoration.effective_format;
    if (eq(variable, "page") || eq(variable, "locator")) return page_inline(e, n, text, size, variable);
    if (markup) return ce_csl_inline_parse(text, size, format, &e->allocator, &n->preview,
        &n->info.inline_diagnostic, e->error, e->capacity);
    return inline_plain(e, text, size, format, false, &n->preview);
}
static bool text_leaf(CeCslEvaluation *e, CeCslEvalNode *n) {
    const CeCslXmlNode *source = n->info.occurrence.source;
    const char *variable = attr(source, "variable"), *term = attr(source, "term"), *value = attr(source, "value");
    if ((variable != NULL) + (term != NULL) + (value != NULL) != 1) return fail(e, "text requires exactly one source");
    if (variable) {
        CeCslVariableForm form = CE_CSL_VARIABLE_LONG;
        if (eq(attr(source, "form"), "short")) form = CE_CSL_VARIABLE_SHORT;
        else if (attr(source, "form") && !eq(attr(source, "form"), "long")) return fail(e, "unsupported variable text form");
        if (!uses_allocate(e, n, 1) || !bind(e, n, 0, variable, form)) return false;
        const CeCslVariableValue *v = &n->uses[0].public.value;
        if (v->kind == CE_CSL_VARIABLE_NAMES || v->kind == CE_CSL_VARIABLE_DATE)
            return fail(e, "text cannot directly render structured names/date variables");
        size_t size;
        const char *text = ce_csl_variable_text(v, &size);
        return scalar_inline(e, n, text, size, v->canonical_name, v->kind == CE_CSL_VARIABLE_TEXT);
    }
    if (term) {
        CeCslLocaleForm form;
        bool plural;
        if (!locale_form(e, attr(source, "form"), &form) || !bool_attr(e, source, "plural", &plural)) return false;
        CeCslLocaleTerm t = ce_csl_locale_term(e->request.locale, term, form, plural, CE_CSL_LOCALE_NEUTER);
        n->info.missing_term = !t.found;
        return payload(e, t.length) && scalar_inline(e, n, t.text, t.length, NULL, false);
    }
    return payload(e, strlen(value)) && scalar_inline(e, n, value, strlen(value), NULL, false);
}
static bool number_leaf(CeCslEvaluation *e, CeCslEvalNode *n) {
    const CeCslXmlNode *source = n->info.occurrence.source;
    const char *variable = attr(source, "variable");
    CeCslNumberForm form = CE_CSL_NUMBER_NUMERIC;
    if (!variable || !numeric_form(e, attr(source, "form"), &form) ||
        !uses_allocate(e, n, 1) || !bind(e, n, 0, variable, CE_CSL_VARIABLE_LONG)) return false;
    CeCslVariableValue *v = &n->uses[0].public.value;
    if (v->kind != CE_CSL_VARIABLE_NUMBER && !eq(v->canonical_name, "page"))
        return fail(e, "number requires a numeric variable");
    size_t size;
    const char *text = ce_csl_variable_text(v, &size);
    if (!ce_csl_number_field_scan(text, size, &n->number, e->error, e->capacity)) return false;
    CeCslOutputString rendered = {0};
    bool missing = false;
    /* CSL Sorting Macros returns numeric fields as integers, regardless of the
     * display form on cs:number. Retain original typed value/source metadata. */
    CeCslNumberForm rendered_form = e->request.sorting ? CE_CSL_NUMBER_NUMERIC : form;
    bool ok = ce_csl_number_field_render(text, size, rendered_form, e->request.locale,
        ce_csl_locale_noun_gender(e->request.locale, v->canonical_name), &e->allocator,
        &rendered, &missing, e->error, e->capacity);
    if (ok) ok = scalar_inline(e, n, rendered.data, rendered.length, v->canonical_name, false);
    n->info.missing_term |= missing;
    ce_csl_output_string_free(&rendered);
    return ok;
}
static bool label_leaf(CeCslEvaluation *e, CeCslEvalNode *n) {
    const CeCslXmlNode *source = n->info.occurrence.source;
    const char *variable = attr(source, "variable");
    if (!variable || !uses_allocate(e, n, 1) || !bind(e, n, 0, variable, CE_CSL_VARIABLE_LONG)) return false;
    /* Labels read a variable only to choose descriptive text. They neither
     * supply a rendered-variable group fact nor consume it in substitution. */
    n->info.variable_called = false;
    CeCslVariableValue *v = &n->uses[0].public.value;
    if (v->kind != CE_CSL_VARIABLE_NUMBER && !eq(v->canonical_name, "page"))
        return fail(e, "label requires a numeric variable");
    CeCslLocaleForm form;
    if (!locale_form(e, attr(source, "form"), &form) || form == CE_CSL_LOCALE_VERB || form == CE_CSL_LOCALE_VERB_SHORT)
        return fail(e, "unsupported standalone label form");
    size_t size;
    const char *text = ce_csl_variable_text(v, &size);
    if (!ce_csl_number_field_scan(text, size, &n->number, e->error, e->capacity)) return false;
    bool plural = n->number.number_count > 1;
    if (eq(v->canonical_name, "number-of-pages") || eq(v->canonical_name, "number-of-volumes"))
        plural |= n->number.single_greater_than_one;
    const char *p = attr(source, "plural");
    if (eq(p, "always")) plural = true;
    else if (eq(p, "never")) plural = false;
    else if (p && !eq(p, "contextual")) return fail(e, "unsupported label pluralization");
    const char *term = v->canonical_name;
    if (eq(term, "locator")) {
        const cJSON *label = e->request.variables.cite ?
            cJSON_GetObjectItemCaseSensitive(e->request.variables.cite->data, "label") : NULL;
        if (label && !cJSON_IsNull(label) && !cJSON_IsString(label)) return fail(e, "invalid locator label metadata");
        term = label && cJSON_IsString(label) && *label->valuestring ? label->valuestring : "page";
        if (eq(term, "sub verbo")) term = "sub-verbo";
    }
    CeCslLocaleTerm t = ce_csl_locale_term(e->request.locale, term, form, plural, CE_CSL_LOCALE_NEUTER);
    bool present = v->state == CE_CSL_VARIABLE_VALUE;
    n->info.missing_term = present && !t.found;
    return scalar_inline(e, n, present ? t.text : "", present ? t.length : 0, NULL, false);
}
static bool date_leaf(CeCslEvaluation *e, CeCslEvalNode *n) {
    const CeCslXmlNode *source = n->info.occurrence.source;
    const char *variable = attr(source, "variable");
    if (!variable || !uses_allocate(e, n, 1) || !bind(e, n, 0, variable, CE_CSL_VARIABLE_LONG)) return false;
    CeCslVariableValue *v = &n->uses[0].public.value;
    if (v->kind != CE_CSL_VARIABLE_DATE) return fail(e, "date requires a date variable");
    const cJSON *value = v->state == CE_CSL_VARIABLE_VALUE ? v->node : NULL;
    CeCslXmlNode view;
    CeCslXmlAttr attributes[128];
    const cJSON *typed = v->state != CE_CSL_VARIABLE_SUPPRESSED && v->node && !cJSON_IsNull(v->node) ? v->node : NULL;
    if (!filtered_node(e, source, &view, attributes, true) ||
        !ce_csl_date_value_view(typed, &n->date_value, e->error, e->capacity) ||
        !ce_csl_date_selected_parts(&view, e->request.locale, &n->date_parts, e->error, e->capacity)) return false;
    CeCslOutput *body = ce_csl_output_new(&e->allocator, e->error, e->capacity);
    if (!body) return false;
    CeCslDateOptions options = {n->info.decoration.inherited_format, e->language,
        ce_csl_style_default_locale(e->request.style), &e->allocator};
    bool ok = ce_csl_date_render(value, &view, e->request.locale, &options, body, &n->date, e->error, e->capacity) &&
        ce_csl_inline_from_output(body, false, &e->allocator, &n->preview, e->error, e->capacity);
    n->info.missing_term = n->date.missing_term;
    ce_csl_output_free(body);
    return ok;
}
static bool space(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }
static size_t word_count(const char *text) {
    size_t count = 0;
    for (const char *p = empty(text); *p;) {
        while (space(*p)) ++p;
        if (!*p) break;
        ++count;
        while (*p && !space(*p)) ++p;
    }
    return count;
}
static bool names_no_markup(CeCslEvaluation *e, const CeCslNamesData *data) {
    for (size_t i = 0; i < ce_csl_names_data_count(data); ++i) {
        const CeCslNameIdentity *person = ce_csl_names_data_person(data, i);
        const char *texts[] = {person->family, person->given, person->dropping_particle,
            person->non_dropping_particle, person->suffix, person->literal};
        for (size_t j = 0; j < sizeof(texts) / sizeof(*texts); ++j) {
            const char *text = empty(texts[j]);
            size_t bytes = strlen(text);
            if (!payload(e, bytes)) return false;
            if (!memchr(text, '<', bytes)) continue;
            CeCslInline *parsed = NULL;
            CeCslInlineDiagnostic diagnostic = {0};
            bool ok = ce_csl_inline_parse(text, bytes, 0, &e->allocator, &parsed, &diagnostic, e->error, e->capacity);
            bool markup = ok && (inline_size(parsed) != bytes || diagnostic.malformed_markup);
            ce_csl_inline_free(parsed);
            if (!ok) return false;
            if (markup) return fail(e, "unsupported inline markup in structured names before initialization");
        }
    }
    return true;
}
static bool walk(CeCslEvaluation *, const CeCslXmlNode *, unsigned, const char *,
    const CeCslNameOptions *, size_t, CeCslEvalNode **);
static void aggregate(CeCslEvalNode *, const CeCslEvalNode *);
static bool consume_tree(CeCslEvaluation *e, const CeCslEvalNode *n) {
    if (!n->info.nonempty) return true;
    for (size_t i = 0; i < n->use_count; ++i) {
        const CeCslEvalVariableUse *u = &n->uses[i].public;
        if (!u->rendered) continue;
        if (!set_add(e, e->suppressed, &e->suppressed_count, u->value.canonical_name)) return false;
        for (size_t j = 0; j < u->value.dependency_count; ++j)
            if (!set_add(e, e->suppressed, &e->suppressed_count, u->value.dependencies[j])) return false;
    }
    for (const CeCslEvalNode *child = n->info.first_child; child; child = child->info.next)
        if (!consume_tree(e, child)) return false;
    return true;
}
static bool names_leaf(CeCslEvaluation *e, CeCslEvalNode *n, const CeCslNameOptions *parent,
    const char *delimiter, size_t depth) {
    const CeCslXmlNode *source = n->info.occurrence.source;
    const char *variables = attr(source, "variable");
    size_t count = word_count(variables);
    if (!count) return fail(e, "names requires variables");
    if (!uses_allocate(e, n, count)) return false;
    CeCslNamesBinding bindings[CE_CSL_NAMES_MAX_BINDINGS] = {{0}};
    bool ok = false;
    const char *p = variables;
    for (size_t i = 0; i < count; ++i) {
        while (space(*p)) ++p;
        const char *start = p;
        while (*p && !space(*p)) ++p;
        if ((size_t)(p - start) > CE_CSL_VARIABLE_MAX_NAME_BYTES) { fail(e, "oversized names variable"); goto done; }
        char name[CE_CSL_VARIABLE_MAX_NAME_BYTES + 1];
        memcpy(name, start, (size_t)(p - start)); name[p - start] = 0;
        if (!bind(e, n, i, name, CE_CSL_VARIABLE_LONG)) goto done;
        CeCslVariableValue *v = &n->uses[i].public.value;
        if (v->kind != CE_CSL_VARIABLE_NAMES) { fail(e, "names requires name variables"); goto done; }
        bindings[i].variable = v->canonical_name;
        CeCslNameOrigin origin = {e->request.variables.item->id, v->canonical_name};
        if (!ce_csl_names_data_load(v->state == CE_CSL_VARIABLE_VALUE ? v->node : NULL,
                &origin, &e->allocator, &bindings[i].data, e->error, e->capacity) ||
            !names_no_markup(e, bindings[i].data)) goto done;
    }
    CeCslXmlNode view;
    CeCslXmlAttr attributes[128];
    CeCslNameOptions options;
    if (!filtered_node(e, source, &view, attributes, false) ||
        !ce_csl_names_resolve(ce_csl_style_root(e->request.style), e->request.section,
            &view, parent, &options, e->error, e->capacity)) goto done;
    CeCslEvalNameAdjustment adjustment = {0};
    if (e->request.adjust_names && !e->request.adjust_names(e->request.name_context,
            &n->info.occurrence, bindings, count, &options, &adjustment, e->error, e->capacity)) goto done;
    CeCslNameRenderContext context = {
        .base_format = n->info.decoration.inherited_format,
        .item_language = e->language, .style_default_locale = ce_csl_style_default_locale(e->request.style),
        .allocator = &e->allocator, .subsequent = (e->request.position & CE_CSL_POSITION_SUBSEQUENT) != 0,
        .sorting = e->request.sorting, .minimum_names = adjustment.minimum_names,
        .overrides = adjustment.overrides, .override_count = adjustment.override_count,
        .has_sort_min = e->request.has_sort_min, .has_sort_first = e->request.has_sort_first,
        .has_sort_last = e->request.has_sort_last, .sort_min = e->request.sort_min,
        .sort_first = e->request.sort_first, .sort_last = e->request.sort_last
    };
    if (!ce_csl_names_render(bindings, count, &options, e->request.locale, &context,
            &n->names, e->error, e->capacity)) goto done;
    const CeCslNamesSummary *summary = ce_csl_names_result_summary(n->names);
    n->info.missing_term = summary->missing_term;
    if (summary->needs_substitute) {
        for (const CeCslXmlNode *candidate = options.substitute->children; candidate; candidate = candidate->next) {
            Mark snapshot = mark(e);
            CeCslEvalNode *child = NULL;
            if (!walk(e, candidate, n->info.decoration.effective_format, delimiter, &options, depth + 1, &child)) goto done;
            if (!child->info.nonempty) { rollback(e, snapshot, true); continue; }
            if (!consume_tree(e, child) ||
                !ce_csl_inline_clone(child->preview, &e->allocator, &n->preview, e->error, e->capacity)) goto done;
            child_add(n, child);
            n->info.used_substitute = true;
            aggregate(n, child);
            ok = true;
            goto done;
        }
    }
    ok = ce_csl_inline_from_output(ce_csl_names_result_output(n->names), false,
        &e->allocator, &n->preview, e->error, e->capacity);
done:
    for (size_t i = 0; i < count; ++i) ce_csl_names_data_free(bindings[i].data);
    return ok;
}
static void aggregate(CeCslEvalNode *n, const CeCslEvalNode *child) {
    n->info.metadata_present |= child->info.metadata_present;
    n->info.variable_called |= child->info.variable_called;
    n->info.rendered_variable_present |= child->info.rendered_variable_present;
    if (child->info.nonempty &&
        (child->info.kind == CE_CSL_EVAL_GROUP || child->info.kind == CE_CSL_EVAL_MACRO)) {
        n->info.variable_called = true;
        n->info.rendered_variable_present = true;
    }
    n->info.missing_term |= child->info.missing_term;
}
static bool sequence(CeCslEvaluation *e, CeCslEvalNode *n, const CeCslXmlNode *first,
    const char *delimiter, const CeCslNameOptions *parent, size_t depth) {
    if (!inline_plain(e, "", 0, 0, false, &n->preview)) return false;
    bool prior = false;
    for (const CeCslXmlNode *source = first; source; source = source->next) {
        CeCslEvalNode *child = NULL;
        if (!walk(e, source, n->info.decoration.effective_format, delimiter, parent, depth + 1, &child)) return false;
        child_add(n, child);
        aggregate(n, child);
        if (!child->info.nonempty) continue;
        if (prior && !append_plain(e, n->preview, delimiter, n->info.decoration.effective_format)) return false;
        if (!ce_csl_inline_concat(n->preview, child->preview, e->error, e->capacity)) return false;
        prior = true;
    }
    return true;
}
static bool choose(CeCslEvaluation *e, CeCslEvalNode *n, const char *delimiter,
    const CeCslNameOptions *parent, size_t depth) {
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(e->request.variables.item->data, "type");
    const cJSON *label = e->request.variables.cite ?
        cJSON_GetObjectItemCaseSensitive(e->request.variables.cite->data, "label") : NULL;
    if ((type && !cJSON_IsNull(type) && !cJSON_IsString(type)) ||
        (label && !cJSON_IsNull(label) && !cJSON_IsString(label)))
        return fail(e, "invalid item type/locator label metadata");
    CeCslVariableValue locator;
    if (ce_csl_variable_resolve(&e->variables, "locator", CE_CSL_VARIABLE_LONG,
            &locator, e->error, e->capacity) != CE_CSL_VARIABLE_OK) return false;
    CeCslConditionContext context = {
        .context = &e->variables, .variable = ce_csl_variable_condition,
        .item_type = cJSON_IsString(type) ? type->valuestring : NULL,
        .locator_type = cJSON_IsString(label) ? label->valuestring : NULL,
        .locator_present = locator.state == CE_CSL_VARIABLE_VALUE,
        .position = e->request.position, .bibliography = ce_csl_xml_is(e->request.section, "bibliography"),
        .disambiguate = e->request.disambiguate
    };
    if (!ce_csl_choose_branch(n->info.occurrence.source, &context, &n->info.target, e->error, e->capacity)) return false;
    n->info.decoration.delimiter = delimiter;
    if (!n->info.target) n->info.suppression = CE_CSL_EVAL_NO_CHOSEN_BRANCH;
    return sequence(e, n, n->info.target ? n->info.target->children : NULL, delimiter, parent, depth);
}
static bool walk(CeCslEvaluation *e, const CeCslXmlNode *source, unsigned inherited,
    const char *delimiter, const CeCslNameOptions *parent, size_t depth, CeCslEvalNode **out) {
    if (depth > CE_CSL_EVALUATE_MAX_DEPTH) return fail(e, "expanded depth limit exceeded");
    CeCslEvalNode *n = node_new(e, source);
    if (!n || !decorations(e, n, inherited)) return false;
    n->quote_depth = e->quote_depth;
    bool quoted = n->info.decoration.quotes;
    if (quoted) ++e->quote_depth;
    bool leaf = true, ok = false;
    Mark state = mark(e);
    if (ce_csl_xml_is(source, "layout")) {
        leaf = false; n->info.kind = CE_CSL_EVAL_LAYOUT;
        n->info.decoration.delimiter_boundary = true;
        n->info.decoration.cluster_envelope = ce_csl_xml_is(e->request.section, "citation");
        ok = sequence(e, n, source->children, "", parent, depth);
    } else if (ce_csl_xml_is(source, "group")) {
        leaf = false; n->info.kind = CE_CSL_EVAL_GROUP;
        n->info.decoration.delimiter_boundary = true;
        ok = sequence(e, n, source->children, empty(attr(source, "delimiter")), parent, depth);
        if (ok && n->info.variable_called && !n->info.rendered_variable_present) {
            n->info.suppression = CE_CSL_EVAL_ALL_GROUP_VARIABLES_EMPTY;
            ok = inline_plain(e, "", 0, 0, false, &n->preview);
            rollback(e, state, false);
        }
    } else if (ce_csl_xml_is(source, "choose")) {
        leaf = false; n->info.kind = CE_CSL_EVAL_CHOOSE;
        ok = choose(e, n, delimiter, parent, depth);
    } else if (ce_csl_xml_is(source, "macro") || (ce_csl_xml_is(source, "text") && attr(source, "macro"))) {
        leaf = false; n->info.kind = CE_CSL_EVAL_MACRO;
        bool call = ce_csl_xml_is(source, "text");
        n->info.target = call ? ce_csl_style_macro(e->request.style, attr(source, "macro")) : source;
        if (!n->info.target) ok = fail(e, "unknown macro");
        else if (e->call_count == CE_CSL_EVALUATE_MAX_DEPTH) ok = fail(e, "macro call-chain limit exceeded");
        else {
            if (call) e->calls[e->call_count++] = source;
            ok = sequence(e, n, n->info.target->children, "", parent, depth);
            if (call) --e->call_count;
        }
    } else if (ce_csl_xml_is(source, "text")) {
        n->info.kind = CE_CSL_EVAL_TEXT; ok = text_leaf(e, n);
    } else if (ce_csl_xml_is(source, "names")) {
        n->info.kind = CE_CSL_EVAL_NAMES;
        n->info.decoration.delimiter_boundary = true;
        ok = names_leaf(e, n, parent, "", depth);
        if (n->info.used_substitute) leaf = false;
    } else if (ce_csl_xml_is(source, "date")) {
        n->info.kind = CE_CSL_EVAL_DATE;
        n->info.decoration.delimiter_boundary = true;
        ok = date_leaf(e, n);
    } else if (ce_csl_xml_is(source, "number")) {
        n->info.kind = CE_CSL_EVAL_NUMBER; ok = number_leaf(e, n);
    } else if (ce_csl_xml_is(source, "label")) {
        n->info.kind = CE_CSL_EVAL_LABEL; ok = label_leaf(e, n);
    } else ok = fail(e, "unsupported rendering element");
    if (quoted) --e->quote_depth;
    if (!ok) return false;
    if (leaf && (!payload(e, inline_size(n->preview)) ||
        !ce_csl_inline_clone(n->preview, &e->allocator, &n->content, e->error, e->capacity))) return false;
    if (!decorate(e, n)) return false;
    if (leaf && n->info.nonempty && n->info.kind != CE_CSL_EVAL_LABEL) {
        for (size_t i = 0; i < n->use_count; ++i) if (!rendered_use(e, n, i)) return false;
    }
    if (!n->info.nonempty) {
        n->info.rendered_variable_present = false;
        for (size_t i = 0; i < n->use_count; ++i) n->uses[i].public.rendered = false;
        rollback(e, state, false);
        if (n->info.variable_called && n->info.suppression == CE_CSL_EVAL_NOT_SUPPRESSED)
            n->info.suppression = CE_CSL_EVAL_EMPTY_VARIABLE;
    }
    e->summary.missing_term |= n->info.missing_term;
    e->summary.literal_markup |= n->info.inline_diagnostic.literal_markup;
    e->summary.malformed_markup |= n->info.inline_diagnostic.malformed_markup;
    *out = n;
    return true;
}
bool ce_csl_evaluate(const CeCslEvalRequest *request, const CeCslOutputAllocator *allocator,
    CeCslEvaluation **out, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    CeCslEvaluation dummy = {.error = error, .capacity = capacity};
    if (!request || !out || !request->style || !request->variables.item ||
        !request->variables.item->data || !request->variables.item->id ||
        (allocator && (!allocator->allocate || !allocator->deallocate))) return fail(&dummy, "invalid request/allocator");
    if (request->section != ce_csl_style_citation(request->style) &&
        request->section != ce_csl_style_bibliography(request->style)) return fail(&dummy, "section does not belong to style");
    if (!request->section) return fail(&dummy, "missing style section");
    if (ce_csl_xml_is(request->section, "bibliography") && request->variables.cite)
        return fail(&dummy, "bibliography evaluation requires no current cite");
    const CeCslXmlNode *entry = request->entry ? request->entry : ce_csl_xml_child(request->section, "layout");
    if (!entry) return fail(&dummy, "missing evaluation entry");
    const CeCslXmlNode *ancestor = entry;
    size_t hops = 0;
    while (ancestor && ancestor->parent && ++hops <= CE_CSL_XML_MAX_DEPTH) ancestor = ancestor->parent;
    if (ancestor != ce_csl_style_root(request->style)) return fail(&dummy, "entry does not belong to style");
    CeCslOutputAllocator owner = allocator ? *allocator : (CeCslOutputAllocator){NULL, default_allocate, default_free};
    CeCslEvaluation *e = owner.allocate(owner.context, sizeof(*e));
    if (!e) return fail(&dummy, "out of memory");
    memset(e, 0, sizeof(*e));
    e->owner = owner; e->allocator = (CeCslOutputAllocator){e, allocate, release};
    e->workspace = sizeof(*e); e->request = *request;
    e->error = error; e->capacity = capacity;
    e->variables = request->variables;
    e->variables.context = e; e->variables.suppressed = suppressed;
    e->variables.short_value = request->variables.short_value ? abbreviation : NULL;
    const unsigned positions = CE_CSL_POSITION_FIRST | CE_CSL_POSITION_SUBSEQUENT |
        CE_CSL_POSITION_IBID | CE_CSL_POSITION_IBID_WITH_LOCATOR | CE_CSL_POSITION_NEAR_NOTE;
    if (e->request.position & ~positions) { fail(e, "unknown position flag"); goto failed; }
    if (e->request.position & CE_CSL_POSITION_IBID_WITH_LOCATOR) e->request.position |= CE_CSL_POSITION_IBID;
    if (e->request.position & (CE_CSL_POSITION_IBID | CE_CSL_POSITION_NEAR_NOTE))
        e->request.position |= CE_CSL_POSITION_SUBSEQUENT;
    if ((e->request.position & CE_CSL_POSITION_FIRST) && (e->request.position & CE_CSL_POSITION_SUBSEQUENT)) {
        fail(e, "conflicting first/subsequent position"); goto failed;
    }
    e->language = request->item_language;
    if (!e->language) {
        const cJSON *language = cJSON_GetObjectItemCaseSensitive(request->variables.item->data, "language");
        if (language && !cJSON_IsNull(language) && !cJSON_IsString(language)) {
            fail(e, "invalid item language metadata"); goto failed;
        }
        e->language = cJSON_IsString(language) ? language->valuestring : NULL;
    }
    if (e->language) {
        size_t size = strlen(e->language);
        if (size > CE_CSL_EVALUATE_MAX_BYTES) { fail(e, "oversized language metadata"); goto failed; }
        e->language_owned = allocate(e, size + 1);
        if (!e->language_owned) goto failed;
        memcpy(e->language_owned, e->language, size + 1);
        e->language = e->language_owned;
    }
    if (!walk(e, entry, request->base_format, "", NULL, 1, &e->root)) goto failed;
    e->summary.nonempty = e->root->info.nonempty;
    /* Construction callbacks, generated slots and the current cite may be
     * caller stack objects. Retain only the immutable environment used by
     * post-evaluation getters; typed node sources have their own contract. */
    e->request = (CeCslEvalRequest){.style = request->style,
        .locale = request->locale, .section = request->section};
    e->variables = (CeCslVariableContext){0};
    e->error = NULL; e->capacity = 0;
    ce_csl_evaluation_free(*out);
    *out = e;
    return true;
failed:
    if (e->limit) fail(e, "workspace limit exceeded");
    else if (e->oom) fail(e, "out of memory");
    else if (error && capacity && !*error) fail(e, "operation failed without a diagnostic");
    ce_csl_evaluation_free(e);
    return false;
}
const CeCslEvalNode *ce_csl_evaluation_root(const CeCslEvaluation *e) { return e ? e->root : NULL; }
const CeCslEvalSummary *ce_csl_evaluation_summary(const CeCslEvaluation *e) { return e ? &e->summary : NULL; }
const CeCslLocale *ce_csl_evaluation_locale(const CeCslEvaluation *e) { return e ? e->request.locale : NULL; }
const char *ce_csl_evaluation_item_language(const CeCslEvaluation *e) { return e ? e->language : NULL; }
const char *ce_csl_evaluation_default_locale(const CeCslEvaluation *e) {
    return e ? ce_csl_style_default_locale(e->request.style) : NULL;
}
const CeCslEvalNodeInfo *ce_csl_eval_node_info(const CeCslEvalNode *n) { return n ? &n->info : NULL; }
size_t ce_csl_eval_node_variable_count(const CeCslEvalNode *n) { return n ? n->use_count : 0; }
const CeCslEvalVariableUse *ce_csl_eval_node_variable(const CeCslEvalNode *n, size_t index) {
    return n && index < n->use_count ? &n->uses[index].public : NULL;
}
const CeCslNamesResult *ce_csl_eval_node_names(const CeCslEvalNode *n) { return n ? n->names : NULL; }
const CeCslDateResult *ce_csl_eval_node_date(const CeCslEvalNode *n) {
    return n && n->info.kind == CE_CSL_EVAL_DATE ? &n->date : NULL;
}
const CeCslDateValueView *ce_csl_eval_node_date_value(const CeCslEvalNode *n) {
    return n && n->info.kind == CE_CSL_EVAL_DATE ? &n->date_value : NULL;
}
unsigned ce_csl_eval_node_date_parts(const CeCslEvalNode *n) { return n ? n->date_parts : 0; }
const CeCslNumberFieldInfo *ce_csl_eval_node_number(const CeCslEvalNode *n) {
    return n && (n->info.kind == CE_CSL_EVAL_NUMBER || n->info.kind == CE_CSL_EVAL_LABEL) ? &n->number : NULL;
}
const CeCslInline *ce_csl_eval_node_content(const CeCslEvalNode *n) { return n ? n->content : NULL; }
const CeCslInline *ce_csl_eval_node_preview(const CeCslEvalNode *n) { return n ? n->preview : NULL; }
