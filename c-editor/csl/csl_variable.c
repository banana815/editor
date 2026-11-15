#include "csl_variable.h"
#include "csl_date.h"
#include "csl_names.h"
#include "csl_number_field.h"
#include "csl_unicode.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Fixed CSL 1.0.2 vocabulary, matching the style parser. No abbreviation
 * service, citation registry, host locale or processor source is used here. */
typedef struct Definition { const char *name; CeCslVariableKind kind; } Definition;
#define T(n) {n,CE_CSL_VARIABLE_TEXT}
#define N(n) {n,CE_CSL_VARIABLE_NUMBER}
#define D(n) {n,CE_CSL_VARIABLE_DATE}
#define A(n) {n,CE_CSL_VARIABLE_NAMES}
static const Definition definitions[] = {
    A("author"), A("chair"), A("collection-editor"), A("compiler"), A("composer"),
    A("container-author"), A("contributor"), A("curator"), A("director"), A("editor"),
    A("editor-translator"), A("editorial-director"), A("executive-producer"), A("guest"),
    A("host"), A("illustrator"), A("interviewer"), A("narrator"), A("organizer"),
    A("original-author"), A("performer"), A("producer"), A("recipient"), A("reviewed-author"),
    A("script-writer"), A("series-creator"), A("translator"),
    D("accessed"), D("available-date"), D("event-date"), D("issued"), D("original-date"), D("submitted"),
    N("chapter-number"), N("citation-number"), N("collection-number"), N("edition"),
    N("first-reference-note-number"), N("issue"), N("locator"), N("number"), N("number-of-pages"),
    N("number-of-volumes"), N("page"), N("page-first"), N("part-number"), N("printing-number"),
    N("section"), N("supplement-number"), N("version"), N("volume"),
    T("collection-title"), T("container-title"), T("original-title"), T("part-title"),
    T("reviewed-title"), T("title"), T("volume-title"),
    T("abstract"), T("annote"), T("archive"), T("archive_collection"), T("archive_location"),
    T("archive-place"), T("authority"), T("call-number"), T("citation-key"), T("citation-label"),
    T("dimensions"), T("division"), T("DOI"), T("event-title"), T("event-place"), T("genre"),
    T("ISBN"), T("ISSN"), T("jurisdiction"), T("keyword"), T("language"), T("license"),
    T("medium"), T("note"), T("original-publisher"), T("original-publisher-place"),
    T("PMCID"), T("PMID"), T("publisher"), T("publisher-place"), T("references"),
    T("reviewed-genre"), T("scale"), T("source"), T("status"), T("URL"), T("year-suffix")
};
#undef T
#undef N
#undef D
#undef A
typedef struct Resolve {
    const CeCslVariableContext *context;
    char *error; size_t capacity;
} Resolve;
static bool eq(const char *a, const char *b) { return a && !strcmp(a,b); }
static CeCslVariableStatus fail(Resolve *r, CeCslVariableStatus status, const char *message) {
    if (r->error && r->capacity) snprintf(r->error,r->capacity,"CSL variable: %s",message);
    return status;
}
static CeCslVariableStatus delegated(Resolve *r, const char *fallback) {
    if (r->error && r->capacity && !*r->error) return fail(r,CE_CSL_VARIABLE_INVALID,fallback);
    return CE_CSL_VARIABLE_INVALID;
}
static const Definition *definition(const char *name) {
    for (size_t i = 0; i < sizeof(definitions)/sizeof(*definitions); ++i)
        if (!strcmp(name,definitions[i].name)) return &definitions[i];
    return NULL;
}
static CeCslVariableStatus text_check(Resolve *r, const char *text, size_t size) {
    if (!text && size) return fail(r,CE_CSL_VARIABLE_INVALID,"text storage is missing");
    if (size > CE_CSL_VARIABLE_MAX_TEXT_BYTES) return fail(r,CE_CSL_VARIABLE_LIMIT,"text exceeds the per-entry limit");
    if (ce_csl_unicode_validate(text,size,NULL) != CE_CSL_UNICODE_OK)
        return fail(r,CE_CSL_VARIABLE_INVALID,"invalid UTF-8 text");
    for (size_t i = 0; i < size; ++i) if ((unsigned char)text[i] < 32 && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
        return fail(r,CE_CSL_VARIABLE_INVALID,"text contains NUL or an unsupported control");
    return CE_CSL_VARIABLE_OK;
}
static CeCslVariableStatus string_size(Resolve *r, const char *text, size_t *size) {
    size_t n = 0;
    if (!text) return fail(r,CE_CSL_VARIABLE_INVALID,"invalid string storage");
    while (n <= CE_CSL_VARIABLE_MAX_TEXT_BYTES && text[n]) ++n;
    CeCslVariableStatus status = text_check(r,text,n);
    if (status == CE_CSL_VARIABLE_OK) *size = n;
    return status;
}
static const cJSON *property(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object,name);
}
static const cJSON *item_property(Resolve *r, const char *name) {
    return property(r->context->item ? r->context->item->data : NULL,name);
}
static CeCslVariableStatus suppression(Resolve *r, const char *name, bool *suppressed) {
    *suppressed = false;
    if (r->context->suppressed && !r->context->suppressed(r->context->context,name,suppressed,r->error,r->capacity))
        return delegated(r,"suppression callback failed");
    return CE_CSL_VARIABLE_OK;
}
static CeCslVariableStatus scalar_node(Resolve *r, const cJSON *node, CeCslVariableValue *value) {
    value->node = node;
    if (!node) { value->state = CE_CSL_VARIABLE_MISSING; return CE_CSL_VARIABLE_OK; }
    if (cJSON_IsNull(node)) { value->state = CE_CSL_VARIABLE_EMPTY; return CE_CSL_VARIABLE_OK; }
    if (cJSON_IsString(node)) {
        CeCslVariableStatus status = string_size(r,node->valuestring,&value->text_size);
        if (status != CE_CSL_VARIABLE_OK) return status;
        value->borrowed_text = node->valuestring;
        value->state = value->text_size ? CE_CSL_VARIABLE_VALUE : CE_CSL_VARIABLE_EMPTY;
        return CE_CSL_VARIABLE_OK;
    }
    if (value->kind == CE_CSL_VARIABLE_NUMBER && cJSON_IsNumber(node)) {
        const double max_safe = 9007199254740991.0;
        double n = node->valuedouble;
        if (!isfinite(n)) return fail(r,CE_CSL_VARIABLE_INVALID,"nonfinite JSON number");
        if (n < -max_safe || n > max_safe || floor(n) != n)
            return fail(r,CE_CSL_VARIABLE_UNSUPPORTED,"JSON numeric scalars require safe integers; retain other decimal spellings as strings");
        int size = snprintf(value->numeric_text,sizeof(value->numeric_text),"%lld",(long long)n);
        if (size < 0 || (size_t)size >= sizeof(value->numeric_text)) return fail(r,CE_CSL_VARIABLE_LIMIT,"integer spelling overflow");
        value->text_size = (size_t)size; value->text_is_inline = true; value->state = CE_CSL_VARIABLE_VALUE;
        return CE_CSL_VARIABLE_OK;
    }
    return fail(r,CE_CSL_VARIABLE_INVALID,"wrong scalar metadata type");
}
static CeCslVariableStatus typed_node(Resolve *r, const cJSON *node, CeCslVariableValue *value) {
    value->node = node;
    if (!node) { value->state = CE_CSL_VARIABLE_MISSING; return CE_CSL_VARIABLE_OK; }
    if (cJSON_IsNull(node)) { value->state = CE_CSL_VARIABLE_EMPTY; return CE_CSL_VARIABLE_OK; }
    if (value->kind == CE_CSL_VARIABLE_NAMES) {
        size_t count;
        if (!ce_csl_names_inspect(node,&count,r->error,r->capacity)) return delegated(r,"names validation failed");
        value->state = count ? CE_CSL_VARIABLE_VALUE : CE_CSL_VARIABLE_EMPTY;
        return CE_CSL_VARIABLE_OK;
    }
    if (value->kind == CE_CSL_VARIABLE_DATE) {
        CeCslDateInfo info;
        if (!ce_csl_date_inspect(node,&info,r->error,r->capacity)) return delegated(r,"date validation failed");
        value->state = info.present ? CE_CSL_VARIABLE_VALUE : CE_CSL_VARIABLE_EMPTY;
        value->uncertain_date = info.present && info.circa;
        return CE_CSL_VARIABLE_OK;
    }
    return scalar_node(r,node,value);
}
static CeCslVariableStatus generated_number(Resolve *r, const CeCslGeneratedNumber *number, CeCslVariableValue *value) {
    if (!number || !number->resolved) return fail(r,CE_CSL_VARIABLE_UNRESOLVED,"processor-generated number has not been resolved");
    value->derived = true;
    if (!number->present) { value->state = CE_CSL_VARIABLE_MISSING; return CE_CSL_VARIABLE_OK; }
    if (!number->value) return fail(r,CE_CSL_VARIABLE_INVALID,"generated citation/note numbers must be positive when present");
    int n = snprintf(value->numeric_text,sizeof(value->numeric_text),"%llu",(unsigned long long)number->value);
    if (n < 0 || (size_t)n >= sizeof(value->numeric_text)) return fail(r,CE_CSL_VARIABLE_LIMIT,"generated integer spelling overflow");
    value->text_size = (size_t)n; value->text_is_inline = true; value->state = CE_CSL_VARIABLE_VALUE;
    return CE_CSL_VARIABLE_OK;
}
static CeCslVariableStatus generated_text(Resolve *r, const CeCslGeneratedText *text, CeCslVariableValue *value) {
    if (!text || !text->resolved) {
        const cJSON *source = item_property(r,value->canonical_name);
        if (source) return scalar_node(r,source,value);
        return fail(r,CE_CSL_VARIABLE_UNRESOLVED,"processor-generated text has not been resolved");
    }
    CeCslVariableStatus status = text_check(r,text->text,text->size);
    if (status != CE_CSL_VARIABLE_OK) return status;
    value->derived = true; value->borrowed_text = text->text; value->text_size = text->size;
    value->state = !text->text ? CE_CSL_VARIABLE_MISSING : text->size ? CE_CSL_VARIABLE_VALUE : CE_CSL_VARIABLE_EMPTY;
    return CE_CSL_VARIABLE_OK;
}
static CeCslVariableStatus resolve(Resolve *, const Definition *, CeCslVariableForm, CeCslVariableValue *);
static CeCslVariableStatus combined_names(Resolve *r, CeCslVariableValue *value) {
    const cJSON *explicit_value = item_property(r,"editor-translator");
    if (explicit_value) return typed_node(r,explicit_value,value);
    bool editor_suppressed, translator_suppressed;
    CeCslVariableStatus status = suppression(r,"editor",&editor_suppressed);
    if (status != CE_CSL_VARIABLE_OK) return status;
    status = suppression(r,"translator",&translator_suppressed);
    if (status != CE_CSL_VARIABLE_OK) return status;
    if (editor_suppressed || translator_suppressed) {
        value->state = CE_CSL_VARIABLE_SUPPRESSED;
        value->dependencies[0] = "editor"; value->dependencies[1] = "translator"; value->dependency_count = 2;
        value->derived = true; return CE_CSL_VARIABLE_OK;
    }
    CeCslVariableValue editor = {.kind=CE_CSL_VARIABLE_NAMES}, translator = {.kind=CE_CSL_VARIABLE_NAMES};
    status = typed_node(r,item_property(r,"editor"),&editor);
    if (status != CE_CSL_VARIABLE_OK) return status;
    status = typed_node(r,item_property(r,"translator"),&translator);
    if (status != CE_CSL_VARIABLE_OK) return status;
    if (editor.state != CE_CSL_VARIABLE_VALUE || translator.state != CE_CSL_VARIABLE_VALUE) return CE_CSL_VARIABLE_OK;
    bool same;
    if (!ce_csl_names_equal(editor.node,translator.node,&same,r->error,r->capacity)) return delegated(r,"name identity comparison failed");
    if (!same) return CE_CSL_VARIABLE_OK;
    value->state = CE_CSL_VARIABLE_VALUE; value->node = editor.node; value->derived = true;
    value->dependencies[0] = "editor"; value->dependencies[1] = "translator"; value->dependency_count = 2;
    return CE_CSL_VARIABLE_OK;
}
static CeCslVariableStatus first_page(Resolve *r, CeCslVariableValue *value) {
    const cJSON *explicit_value = item_property(r,"page-first");
    if (explicit_value) return scalar_node(r,explicit_value,value);
    CeCslVariableValue page = {0};
    CeCslVariableStatus status = resolve(r,definition("page"),CE_CSL_VARIABLE_LONG,&page);
    if (status != CE_CSL_VARIABLE_OK) return status;
    if (page.state == CE_CSL_VARIABLE_SUPPRESSED) {
        value->state = CE_CSL_VARIABLE_SUPPRESSED; value->derived = true;
        value->dependencies[0] = "page"; value->dependency_count = 1; return CE_CSL_VARIABLE_OK;
    }
    if (page.state != CE_CSL_VARIABLE_VALUE) return CE_CSL_VARIABLE_OK;
    size_t size;
    const char *text = ce_csl_variable_text(&page,&size);
    CeCslNumberFieldFirst first;
    if (!ce_csl_number_field_first(text,size,&first,r->error,r->capacity)) return delegated(r,"page-field parsing failed");
    if (!first.numeric) return fail(r,CE_CSL_VARIABLE_UNSUPPORTED,"page-first derivation requires a supported numeric page field; supply page-first explicitly");
    value->node = page.node; value->text_size = first.length; value->derived = true;
    if (page.text_is_inline) {
        if (first.length >= sizeof(value->numeric_text)) return fail(r,CE_CSL_VARIABLE_LIMIT,"first page scalar exceeds inline capacity");
        memcpy(value->numeric_text,text+first.offset,first.length); value->numeric_text[first.length] = 0; value->text_is_inline = true;
    } else value->borrowed_text = text + first.offset;
    value->state = CE_CSL_VARIABLE_VALUE; value->dependencies[0] = "page"; value->dependency_count = 1;
    return CE_CSL_VARIABLE_OK;
}
static CeCslVariableStatus short_text(Resolve *r, const cJSON *long_node, CeCslVariableValue *value, bool *found) {
    const char *short_name = eq(value->canonical_name,"title") ? "title-short" :
        eq(value->canonical_name,"container-title") ? "container-title-short" : NULL;
    *found = false;
    if (short_name) {
        const cJSON *node = item_property(r,short_name);
        if (node) { *found = true; return scalar_node(r,node,value); }
    }
    if (r->context->short_value) {
        CeCslVariableShortValue short_value = {0};
        CeCslVariableStatus status = r->context->short_value(r->context->context,value->canonical_name,long_node,&short_value,r->error,r->capacity);
        if (status != CE_CSL_VARIABLE_OK) {
            if (status < CE_CSL_VARIABLE_INVALID || status > CE_CSL_VARIABLE_LIMIT) return fail(r,CE_CSL_VARIABLE_INVALID,"invalid short-value callback status");
            if (r->error && r->capacity && !*r->error) return fail(r,status,"short-value callback failed");
            return status;
        }
        if (short_value.found) {
            status = text_check(r,short_value.text,short_value.size);
            if (status != CE_CSL_VARIABLE_OK) return status;
            value->state = short_value.size ? CE_CSL_VARIABLE_VALUE : CE_CSL_VARIABLE_EMPTY;
            value->borrowed_text = short_value.text; value->text_size = short_value.size; *found = true;
        }
    }
    return CE_CSL_VARIABLE_OK;
}
static CeCslVariableStatus generated_short(Resolve *r, CeCslVariableStatus status,
    CeCslVariableForm form, CeCslVariableValue *value) {
    if (status != CE_CSL_VARIABLE_OK || form != CE_CSL_VARIABLE_SHORT || value->state != CE_CSL_VARIABLE_VALUE) return status;
    CeCslVariableValue abbreviated = {0};
    abbreviated.kind = value->kind; abbreviated.canonical_name = value->canonical_name;
    bool found;
    status = short_text(r,value->node,&abbreviated,&found);
    if (status != CE_CSL_VARIABLE_OK) return status;
    if (found) {
        abbreviated.derived = value->derived;
        *value = abbreviated;
    }
    return CE_CSL_VARIABLE_OK;
}
static CeCslVariableStatus resolve(Resolve *r, const Definition *def, CeCslVariableForm form, CeCslVariableValue *value) {
    value->canonical_name = def->name; value->kind = def->kind;
    value->state = CE_CSL_VARIABLE_MISSING;
    if (form == CE_CSL_VARIABLE_SHORT && (def->kind == CE_CSL_VARIABLE_NAMES || def->kind == CE_CSL_VARIABLE_DATE))
        return fail(r,CE_CSL_VARIABLE_INVALID,"name/date rendering forms are not scalar short forms");
    bool suppressed;
    CeCslVariableStatus status = suppression(r,def->name,&suppressed);
    if (status != CE_CSL_VARIABLE_OK) return status;
    if (suppressed) { value->state = CE_CSL_VARIABLE_SUPPRESSED; return CE_CSL_VARIABLE_OK; }
    const CeCslGeneratedVariables *g = r->context->generated;
    if (eq(def->name,"citation-number")) return generated_short(r,generated_number(r,g ? &g->citation_number : NULL,value),form,value);
    if (eq(def->name,"first-reference-note-number")) return generated_short(r,generated_number(r,g ? &g->first_reference_note_number : NULL,value),form,value);
    if (eq(def->name,"year-suffix")) return generated_short(r,generated_text(r,g ? &g->year_suffix : NULL,value),form,value);
    if (eq(def->name,"citation-label")) return generated_short(r,generated_text(r,g ? &g->citation_label : NULL,value),form,value);
    if (eq(def->name,"editor-translator")) return combined_names(r,value);
    const cJSON *node;
    if (eq(def->name,"locator")) node = property(r->context->cite ? r->context->cite->data : NULL,"locator");
    else {
        node = item_property(r,def->name);
        if (!node && eq(def->name,"event-title")) node = item_property(r,"event");
    }
    if (form == CE_CSL_VARIABLE_SHORT) {
        bool found;
        status = short_text(r,node,value,&found);
        if (status != CE_CSL_VARIABLE_OK || found) return status;
    }
    if (eq(def->name,"page-first")) return first_page(r,value);
    return typed_node(r,node,value);
}

CeCslVariableStatus ce_csl_variable_resolve(const CeCslVariableContext *context,
    const char *variable, CeCslVariableForm form, CeCslVariableValue *out, char *error, size_t capacity) {
    Resolve r = {context,error,capacity};
    CeCslVariableValue next = {0};
    size_t length = 0;
    if (error && capacity) *error = 0;
    if (!context || !variable || !out || (form != CE_CSL_VARIABLE_LONG && form != CE_CSL_VARIABLE_SHORT))
        return fail(&r,CE_CSL_VARIABLE_INVALID,"invalid resolver arguments");
    if ((context->item && !cJSON_IsObject(context->item->data)) || (context->cite && !cJSON_IsObject(context->cite->data)))
        return fail(&r,CE_CSL_VARIABLE_INVALID,"item/cite data must be objects from the strict input layer");
    while (length <= CE_CSL_VARIABLE_MAX_NAME_BYTES && variable[length]) ++length;
    if (length > CE_CSL_VARIABLE_MAX_NAME_BYTES) return fail(&r,CE_CSL_VARIABLE_LIMIT,"variable name is too long");
    if (eq(variable,"event")) variable = "event-title";
    if (eq(variable,"title-short")) { variable = "title"; form = CE_CSL_VARIABLE_SHORT; }
    else if (eq(variable,"container-title-short")) { variable = "container-title"; form = CE_CSL_VARIABLE_SHORT; }
    const Definition *def = definition(variable);
    if (!def) return fail(&r,CE_CSL_VARIABLE_UNKNOWN,"unknown CSL variable");
    CeCslVariableStatus status = resolve(&r,def,form,&next);
    if (status == CE_CSL_VARIABLE_OK) *out = next;
    return status;
}
const char *ce_csl_variable_text(const CeCslVariableValue *value, size_t *size) {
    if (size) *size = 0;
    if (!value || (value->kind != CE_CSL_VARIABLE_TEXT && value->kind != CE_CSL_VARIABLE_NUMBER)) return NULL;
    if (value->state != CE_CSL_VARIABLE_VALUE) return "";
    if (size) *size = value->text_size;
    return value->text_is_inline ? value->numeric_text : value->borrowed_text;
}
bool ce_csl_variable_condition(void *context, const char *variable, CeCslVariableFacts *facts, char *error, size_t capacity) {
    CeCslVariableValue value = {0};
    CeCslVariableFacts next = {0};
    if (!facts) { Resolve r = {context,error,capacity}; fail(&r,CE_CSL_VARIABLE_INVALID,"facts output is required"); return false; }
    if (ce_csl_variable_resolve(context,variable,CE_CSL_VARIABLE_LONG,&value,error,capacity) != CE_CSL_VARIABLE_OK) return false;
    next.present = value.state == CE_CSL_VARIABLE_VALUE;
    next.uncertain_date = next.present && value.uncertain_date;
    if (next.present && (value.kind == CE_CSL_VARIABLE_TEXT || value.kind == CE_CSL_VARIABLE_NUMBER)) {
        size_t size; const char *text = ce_csl_variable_text(&value,&size);
        CeCslNumberFieldInfo info;
        if (!ce_csl_number_field_scan(text,size,&info,error,capacity)) return false;
        next.numeric = info.numeric;
    }
    *facts = next; return true;
}
const char *ce_csl_variable_status_string(CeCslVariableStatus status) {
    static const char *const names[] = {"ok","invalid","unknown variable","unresolved generated value","unsupported","limit"};
    return status >= CE_CSL_VARIABLE_OK && status <= CE_CSL_VARIABLE_LIMIT ? names[status] : "unknown status";
}
