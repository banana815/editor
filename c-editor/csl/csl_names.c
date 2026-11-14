#include "csl_names.h"
#include "csl_format.h"
#include "csl_unicode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CSL 1.0.2 names/name-part/et-al/label rules and pinned Unicode Script data.
 * No reference processor source or host-locale services are used. */
struct CeCslNamesData {
    CeCslOutputAllocator allocator;
    size_t references, count, nonempty;
    CeCslNameOrigin origin;
    CeCslNameIdentity *people;
    char *strings;
};
struct CeCslNamesResult {
    CeCslOutputAllocator allocator;
    CeCslOutput *output;
    CeCslNamesSummary summary;
    CeCslNamesData **data;
    size_t binding_count, person_count, person_capacity;
    CeCslNameRendered *people;
    unsigned char *overrides;
};
static bool fail(char *error, size_t capacity, const char *message) {
    if (error && capacity)
        snprintf(error, capacity, "CSL names: %s", message);
    return false;
}
static bool eq(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static const char *empty(const char *s) { return s ? s : ""; }
static void *alloc_default(void *context, size_t n) {
    (void)context;
    return malloc(n);
}
static void free_default(void *context, void *p) {
    (void)context;
    free(p);
}
static bool allocator_get(const CeCslOutputAllocator *in, CeCslOutputAllocator *out, char *error,
                          size_t cap) {
    if (in && (!in->allocate || !in->deallocate))
        return fail(error, cap, "invalid allocator");
    *out = in ? *in : (CeCslOutputAllocator){NULL, alloc_default, free_default};
    return true;
}
static bool string_check(const char *s, size_t *budget, char *error, size_t cap) {
    size_t n = 0;
    if (!s)
        return fail(error, cap, "invalid string storage");
    while (n <= CE_CSL_NAMES_MAX_BYTES && s[n])
        ++n;
    if (n > CE_CSL_NAMES_MAX_BYTES || n > CE_CSL_NAMES_MAX_BYTES - *budget)
        return fail(error, cap, "name data size limit exceeded");
    if (ce_csl_unicode_validate(s, n, NULL) != CE_CSL_UNICODE_OK)
        return fail(error, cap, "invalid UTF-8");
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)s[i] < 32 && s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
            return fail(error, cap, "invalid control character");
    *budget += n;
    return true;
}
static bool boolean(const cJSON *v, bool *out) {
    if (!v) {
        *out = false;
        return true;
    }
    if (cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v);
        return true;
    }
    if (cJSON_IsNumber(v) && (v->valuedouble == 0 || v->valuedouble == 1)) {
        *out = v->valuedouble != 0;
        return true;
    }
    if (cJSON_IsString(v)) {
        if (eq(v->valuestring, "true") || eq(v->valuestring, "1")) {
            *out = true;
            return true;
        }
        if (eq(v->valuestring, "false") || eq(v->valuestring, "0")) {
            *out = false;
            return true;
        }
    }
    return false;
}
static bool person_parse(const cJSON *v, CeCslNameIdentity *p, size_t *bytes, char *error, size_t cap) {
    static const char *const keys[] = {"family",     "given",   "dropping-particle", "non-dropping-particle",
                                       "suffix",     "literal", "comma-suffix",      "static-ordering",
                                       "parse-names"};
    unsigned seen = 0;
    *p = (CeCslNameIdentity){.family = "",
                             .given = "",
                             .dropping_particle = "",
                             .non_dropping_particle = "",
                             .suffix = "",
                             .literal = ""};
    if (!cJSON_IsObject(v))
        return fail(error, cap, "name must be an object");
    for (const cJSON *n = v->child; n; n = n->next) {
        size_t k;
        for (k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k)
            if (eq(n->string, keys[k]))
                break;
        if (k == sizeof(keys) / sizeof(keys[0]))
            return fail(error, cap, "unsupported name field");
        if (seen & (1u << k))
            return fail(error, cap, "duplicate name field");
        seen |= 1u << k;
        if (k < 6) {
            if (!cJSON_IsString(n) || !string_check(n->valuestring, bytes, error, cap))
                return fail(error, cap, "invalid name text");
            const char **dest[] = {&p->family, &p->given,  &p->dropping_particle, &p->non_dropping_particle,
                                   &p->suffix, &p->literal};
            *dest[k] = n->valuestring;
        } else {
            bool *dest[] = {&p->comma_suffix, &p->static_ordering, &p->parse_names};
            if (!boolean(n, dest[k - 6]))
                return fail(error, cap, "invalid name flag");
            if (k == 8)
                p->parse_names_present = true;
        }
    }
    bool personal =
        *p->family || *p->given || *p->dropping_particle || *p->non_dropping_particle || *p->suffix;
    if (*p->literal && personal)
        return fail(error, cap, "literal and personal name fields conflict");
    if (personal && !*p->family)
        return fail(error, cap, "personal name requires family");
    p->kind = *p->literal ? CE_CSL_NAME_LITERAL : personal ? CE_CSL_NAME_PERSONAL : CE_CSL_NAME_EMPTY;
    return true;
}
static bool array_inspect(const cJSON *a, size_t *count, size_t *nonempty, size_t *bytes, char *error,
                          size_t cap) {
    *count = *nonempty = *bytes = 0;
    if (!a)
        return true;
    if (!cJSON_IsArray(a))
        return fail(error, cap, "names must be an array");
    for (const cJSON *n = a->child; n; n = n->next) {
        CeCslNameIdentity p;
        if (++*count > CE_CSL_NAMES_MAX_PEOPLE)
            return fail(error, cap, "name count limit exceeded");
        if (!person_parse(n, &p, bytes, error, cap))
            return false;
        if (p.kind != CE_CSL_NAME_EMPTY)
            ++*nonempty;
    }
    return true;
}
bool ce_csl_names_inspect(const cJSON *a, size_t *nonempty, char *error, size_t cap) {
    if (error && cap)
        *error = 0;
    size_t count, n, bytes;
    if (!nonempty)
        return fail(error, cap, "missing count output");
    if (!array_inspect(a, &count, &n, &bytes, error, cap))
        return false;
    *nonempty = n;
    return true;
}
bool ce_csl_name_identity_equal(const CeCslNameIdentity *a, const CeCslNameIdentity *b) {
    return a && b && a->kind == b->kind && eq(a->family, b->family) && eq(a->given, b->given) &&
           eq(a->dropping_particle, b->dropping_particle) &&
           eq(a->non_dropping_particle, b->non_dropping_particle) && eq(a->suffix, b->suffix) &&
           eq(a->literal, b->literal) && a->comma_suffix == b->comma_suffix &&
           a->static_ordering == b->static_ordering && a->parse_names == b->parse_names &&
           a->parse_names_present == b->parse_names_present;
}
bool ce_csl_names_equal(const cJSON *a, const cJSON *b, bool *equal, char *error, size_t cap) {
    if (error && cap)
        *error = 0;
    size_t na, nb, count, bytes;
    if (!equal)
        return fail(error, cap, "missing equality output");
    if (!array_inspect(a, &na, &count, &bytes, error, cap) ||
        !array_inspect(b, &nb, &count, &bytes, error, cap))
        return false;
    bool same = na == nb;
    const cJSON *av = a ? a->child : NULL, *bv = b ? b->child : NULL;
    while (same && av && bv) {
        CeCslNameIdentity ap, bp;
        bytes = 0;
        if (!person_parse(av, &ap, &bytes, error, cap))
            return false;
        bytes = 0;
        if (!person_parse(bv, &bp, &bytes, error, cap))
            return false;
        same = ce_csl_name_identity_equal(&ap, &bp);
        av = av->next;
        bv = bv->next;
    }
    *equal = same;
    return true;
}
static char *copy_string(char **next, const char *s) {
    size_t n = strlen(s) + 1;
    char *result = *next;
    memcpy(result, s, n);
    *next += n;
    return result;
}
bool ce_csl_names_data_load(const cJSON *a, const CeCslNameOrigin *source,
                            const CeCslOutputAllocator *allocator, CeCslNamesData **out, char *error,
                            size_t cap) {
    if (error && cap)
        *error = 0;
    size_t count, nonempty, bytes;
    CeCslOutputAllocator mem;
    if (!out || !allocator_get(allocator, &mem, error, cap))
        return fail(error, cap, "invalid data arguments");
    if (!array_inspect(a, &count, &nonempty, &bytes, error, cap))
        return false;
    const char *item = source ? empty(source->item_id) : "",
               *variable = source ? empty(source->variable) : "";
    if (!string_check(item, &bytes, error, cap) || !string_check(variable, &bytes, error, cap))
        return false;
    CeCslNamesData *d = mem.allocate(mem.context, sizeof(*d));
    if (!d)
        return fail(error, cap, "out of memory");
    *d = (CeCslNamesData){.allocator = mem, .references = 1, .count = count, .nonempty = nonempty};
    if (count)
        d->people = mem.allocate(mem.context, count * sizeof(*d->people));
    d->strings = mem.allocate(mem.context, bytes + 6 * count + 2);
    if ((count && !d->people) || !d->strings) {
        ce_csl_names_data_free(d);
        return fail(error, cap, "out of memory");
    }
    char *next = d->strings;
    d->origin.item_id = copy_string(&next, item);
    d->origin.variable = copy_string(&next, variable);
    size_t i = 0;
    for (const cJSON *n = a ? a->child : NULL; n; n = n->next) {
        CeCslNameIdentity *p = &d->people[i++];
        size_t ignored = 0;
        if (!person_parse(n, p, &ignored, error, cap)) {
            ce_csl_names_data_free(d);
            return false;
        }
        p->family = copy_string(&next, p->family);
        p->given = copy_string(&next, p->given);
        p->dropping_particle = copy_string(&next, p->dropping_particle);
        p->non_dropping_particle = copy_string(&next, p->non_dropping_particle);
        p->suffix = copy_string(&next, p->suffix);
        p->literal = copy_string(&next, p->literal);
    }
    ce_csl_names_data_free(*out);
    *out = d;
    return true;
}
void ce_csl_names_data_free(CeCslNamesData *d) {
    if (!d || --d->references)
        return;
    if (d->people)
        d->allocator.deallocate(d->allocator.context, d->people);
    if (d->strings)
        d->allocator.deallocate(d->allocator.context, d->strings);
    d->allocator.deallocate(d->allocator.context, d);
}
size_t ce_csl_names_data_count(const CeCslNamesData *d) { return d ? d->count : 0; }
size_t ce_csl_names_data_nonempty_count(const CeCslNamesData *d) { return d ? d->nonempty : 0; }
const CeCslNameIdentity *ce_csl_names_data_person(const CeCslNamesData *d, size_t i) {
    return d && i < d->count ? &d->people[i] : NULL;
}
const CeCslNameOrigin *ce_csl_names_data_origin(const CeCslNamesData *d) { return d ? &d->origin : NULL; }

static const char *attr(const CeCslXmlNode *n, const char *key) { return ce_csl_xml_attribute(n, key); }
static bool words_have(const char *words, const char *word) {
    size_t n = strlen(word);
    for (const char *p = words; *p;) {
        const char *end = strchr(p, ' ');
        size_t k = end ? (size_t)(end - p) : strlen(p);
        if (k == n && !memcmp(p, word, n))
            return true;
        p += k;
        if (*p)
            ++p;
    }
    return false;
}
static bool style_bool(const char *v, bool *out) {
    if (eq(v, "true") || eq(v, "1")) {
        *out = true;
        return true;
    }
    if (eq(v, "false") || eq(v, "0")) {
        *out = false;
        return true;
    }
    return false;
}
static bool number(const char *v, size_t *out) {
    size_t n = 0;
    if (!v || !*v)
        return false;
    for (; *v; ++v) {
        if (*v < '0' || *v > '9' || n > (CE_CSL_NAMES_MAX_PEOPLE - (unsigned)(*v - '0')) / 10)
            return false;
        n = n * 10 + (unsigned)(*v - '0');
    }
    *out = n;
    return true;
}
static int enumeration(const char *v, const char *const *values, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (eq(v, values[i]))
            return (int)i;
    return -1;
}
static bool format_attr(const char *s) {
    return words_have("font-style font-variant font-weight text-decoration vertical-align", s);
}
static bool whitespace(const char *s) {
    for (; s && *s; ++s)
        if (*s != ' ' && *s != '\r' && *s != '\n' && *s != '\t')
            return false;
    return true;
}
static bool case_mode(const char *s, CeCslTextCase *mode) {
    static const char *const values[] = {
        "", "lowercase", "uppercase", "capitalize-first", "capitalize-all", "sentence", "title"};
    int n = enumeration(empty(s), values, 7);
    if (n < 0)
        return false;
    *mode = (CeCslTextCase)n;
    return true;
}
static const char *const option_names =
    "and delimiter-precedes-et-al delimiter-precedes-last et-al-min et-al-use-first et-al-use-last "
    "et-al-subsequent-min et-al-subsequent-use-first initialize initialize-with name-as-sort-order "
    "sort-separator";
static bool apply_options(const CeCslXmlNode *node, bool local, CeCslNameOptions *o, char *error,
                          size_t cap) {
    const char *v;
    int e;
    static const char *const forms[] = {"long", "short", "count"}, *const conjunctions[] = {"text", "symbol"};
    static const char *const delimiters[] = {"contextual", "always", "never", "after-inverted-name"},
                             *const sorts[] = {"first", "all"};
    if ((v = attr(node, local ? "form" : "name-form"))) {
        e = enumeration(v, forms, 3);
        if (e < 0)
            goto invalid;
        o->form = (CeCslNameForm)e;
    }
    if ((v = attr(node, local ? "delimiter" : "name-delimiter")))
        o->name_delimiter = v;
    if (!local && (v = attr(node, "names-delimiter")))
        o->names_delimiter = v;
    if ((v = attr(node, "and"))) {
        e = enumeration(v, conjunctions, 2);
        if (e < 0)
            goto invalid;
        o->and = (CeCslNameAnd)(e + 1);
    }
    if ((v = attr(node, "delimiter-precedes-last"))) {
        e = enumeration(v, delimiters, 4);
        if (e < 0)
            goto invalid;
        o->precedes_last = (CeCslNameDelimiter)e;
    }
    if ((v = attr(node, "delimiter-precedes-et-al"))) {
        e = enumeration(v, delimiters, 4);
        if (e < 0)
            goto invalid;
        o->precedes_et_al = (CeCslNameDelimiter)e;
    }
    if ((v = attr(node, "name-as-sort-order"))) {
        e = enumeration(v, sorts, 2);
        if (e < 0)
            goto invalid;
        o->sort = (CeCslNameSort)(e + 1);
    }
    if ((v = attr(node, "sort-separator")))
        o->sort_separator = v;
    if ((v = attr(node, "initialize-with")))
        o->initialize_with = v;
    if ((v = attr(node, "initialize")) && !style_bool(v, &o->initialize))
        goto invalid;
    if ((v = attr(node, "et-al-use-last")) && !style_bool(v, &o->et_al_use_last))
        goto invalid;
    const char *const names[] = {"et-al-min", "et-al-use-first", "et-al-subsequent-min",
                                 "et-al-subsequent-use-first"};
    size_t *dest[] = {&o->et_al_min, &o->et_al_use_first, &o->subsequent_min, &o->subsequent_use_first};
    bool *has[] = {&o->has_et_al_min, &o->has_et_al_use_first, &o->has_subsequent_min,
                   &o->has_subsequent_use_first};
    for (size_t i = 0; i < 4; ++i)
        if ((v = attr(node, names[i]))) {
            if (!number(v, dest[i]))
                goto invalid;
            *has[i] = true;
        }
    return true;
invalid:
    return fail(error, cap, "invalid name option");
}
static bool validate_local(const CeCslXmlNode *n, const char *kind, char *error, size_t cap) {
    if (!ce_csl_xml_is(n, kind) || !whitespace(n->text))
        return fail(error, cap, "invalid names element/mixed text");
    for (size_t i = 0; i < n->attribute_count; ++i) {
        const char *key = n->attributes[i].name;
        bool allowed = format_attr(key);
        if (eq(kind, "names"))
            allowed = allowed || words_have("variable delimiter prefix suffix", key);
        else if (eq(kind, "name"))
            allowed =
                allowed || words_have(option_names, key) || words_have("form delimiter prefix suffix", key);
        else if (eq(kind, "name-part"))
            allowed = allowed || words_have("name prefix suffix text-case", key);
        else if (eq(kind, "et-al"))
            allowed = allowed || eq(key, "term");
        else if (eq(kind, "label"))
            allowed = allowed || words_have("form plural prefix suffix text-case strip-periods", key);
        if (!allowed)
            return fail(error, cap,
                        eq(key, "display") ? "display requires the evaluator"
                                           : "unsupported names attribute");
        size_t bytes = 0;
        if (!string_check(n->attributes[i].value, &bytes, error, cap))
            return false;
    }
    unsigned format;
    if (!ce_csl_format_resolve(n, 0, &format, error, cap))
        return false;
    if (!eq(kind, "name") && !eq(kind, "names") && n->children)
        return fail(error, cap, "unexpected names child");
    if (eq(kind, "name-part") || eq(kind, "label")) {
        CeCslTextCase mode;
        if (!case_mode(attr(n, "text-case"), &mode))
            return fail(error, cap, "invalid text-case");
    }
    if (eq(kind, "name-part") && !eq(attr(n, "name"), "given") && !eq(attr(n, "name"), "family"))
        return fail(error, cap, "invalid name-part");
    if (eq(kind, "et-al") && attr(n, "term") && !eq(attr(n, "term"), "et-al") &&
        !eq(attr(n, "term"), "and others"))
        return fail(error, cap, "invalid et-al term");
    if (eq(kind, "label")) {
        const char *v = attr(n, "form");
        if (v && !words_have("long short verb verb-short symbol", v))
            return fail(error, cap, "invalid label form");
        v = attr(n, "plural");
        if (v && !words_have("contextual always never", v))
            return fail(error, cap, "invalid label plural");
        bool flag;
        v = attr(n, "strip-periods");
        if (v && !style_bool(v, &flag))
            return fail(error, cap, "invalid strip-periods");
    }
    return true;
}
bool ce_csl_names_resolve(const CeCslXmlNode *style, const CeCslXmlNode *section, const CeCslXmlNode *names,
                          const CeCslNameOptions *parent, CeCslNameOptions *out, char *error, size_t cap) {
    if (error && cap)
        *error = 0;
    if (!out || (style && !ce_csl_xml_is(style, "style")) ||
        (section && !ce_csl_xml_is(section, "citation") && !ce_csl_xml_is(section, "bibliography")))
        return fail(error, cap, "invalid option scope");
    if (!validate_local(names, "names", error, cap))
        return false;
    if (!attr(names, "variable") || !*attr(names, "variable"))
        return fail(error, cap, "missing names variable");
    CeCslNameOptions o = {.form = CE_CSL_NAME_LONG,
                          .initialize = true,
                          .initialize_with_hyphen = true,
                          .demote = CE_CSL_NAME_DEMOTE_DISPLAY,
                          .name_delimiter = ", ",
                          .names_delimiter = "",
                          .sort_separator = ", "};
    if (!apply_options(style, false, &o, error, cap) || !apply_options(section, false, &o, error, cap))
        return false;
    const char *v = attr(style, "initialize-with-hyphen");
    if (v && !style_bool(v, &o.initialize_with_hyphen))
        return fail(error, cap, "invalid initialize-with-hyphen");
    v = attr(style, "demote-non-dropping-particle");
    if (v) {
        static const char *const ds[] = {"never", "sort-only", "display-and-sort"};
        int e = enumeration(v, ds, 3);
        if (e < 0)
            return fail(error, cap, "invalid particle demotion");
        o.demote = (CeCslNameDemote)e;
    }
    if (parent && !names->children) {
        o = *parent;
        o.label = NULL;
        o.substitute = NULL;
    }
    o.names = names;
    if ((v = attr(names, "delimiter")))
        o.names_delimiter = v;
    bool seen_name = false, seen_et = false, seen_label = false, seen_sub = false;
    for (const CeCslXmlNode *n = names->children; n; n = n->next) {
        if (seen_sub)
            return fail(error, cap, "substitute must be last");
        if (ce_csl_xml_is(n, "name")) {
            if (seen_name || seen_et || !validate_local(n, "name", error, cap))
                return fail(error, cap, "invalid/duplicate name child");
            seen_name = true;
            o.name = n;
            if (!apply_options(n, true, &o, error, cap))
                return false;
            for (const CeCslXmlNode *p = n->children; p; p = p->next) {
                if (!validate_local(p, "name-part", error, cap))
                    return false;
                const CeCslXmlNode **dest = eq(attr(p, "name"), "given") ? &o.given_part : &o.family_part;
                if (*dest)
                    return fail(error, cap, "duplicate name-part");
                *dest = p;
            }
        } else if (ce_csl_xml_is(n, "et-al")) {
            if (seen_et || !validate_local(n, "et-al", error, cap))
                return fail(error, cap, "invalid/duplicate et-al");
            seen_et = true;
            o.et_al = n;
        } else if (ce_csl_xml_is(n, "label")) {
            if (seen_label || !validate_local(n, "label", error, cap))
                return fail(error, cap, "invalid/duplicate label");
            seen_label = true;
            o.label = n;
        } else if (ce_csl_xml_is(n, "substitute")) {
            if (n->attribute_count || !whitespace(n->text) || !n->children)
                return fail(error, cap, "invalid substitute");
            seen_sub = true;
            o.substitute = n;
        } else
            return fail(error, cap, "unsupported names child");
    }
    *out = o;
    return true;
}

typedef struct Render {
    CeCslNameOptions options;
    CeCslNameRenderContext context;
    const CeCslLocale *locale;
    CeCslNamesResult *result;
    unsigned names_format, name_format, given_format, family_format, etal_format;
    size_t binding_offsets[CE_CSL_NAMES_MAX_BINDINGS];
    char *error;
    size_t cap;
} Render;
static CeCslOutput *new_output(Render *r) {
    return ce_csl_output_new(&r->result->allocator, r->error, r->cap);
}
static bool append_n(Render *r, CeCslOutput *out, const char *s, size_t n, unsigned f) {
    return ce_csl_output_append(out, s, n, f, NULL, r->error, r->cap);
}
static bool append(Render *r, CeCslOutput *out, const char *s, unsigned f) {
    return append_n(r, out, empty(s), strlen(empty(s)), f);
}
static bool concat(Render *r, CeCslOutput *out, const CeCslOutput *source) {
    return ce_csl_output_concat(out, source, r->error, r->cap);
}
static bool affix(Render *r, CeCslOutput *out, const CeCslXmlNode *n, const CeCslOutput *body, unsigned f) {
    return ce_csl_format_affixes(n, body, f, &r->result->allocator, out, r->error, r->cap);
}
static char last_byte(const CeCslOutput *out) {
    size_t n = ce_csl_output_run_count(out);
    while (n) {
        const CeCslOutputRun *run = ce_csl_output_run(out, --n);
        if (run->byte_length)
            return run->text[run->byte_length - 1];
    }
    return 0;
}
static bool ends_particle_joiner(const char *s, size_t size) {
    return size &&
           (s[size - 1] == '-' || s[size - 1] == '\'' || (size >= 3 && !memcmp(s + size - 3, "’", 3)));
}
static bool join_text(Render *r, CeCslOutput *out, const char *s, unsigned format) {
    if (!s || !*s)
        return true;
    char last = last_byte(out);
    const CeCslOutputRun *run =
        ce_csl_output_run(out, ce_csl_output_run_count(out) ? ce_csl_output_run_count(out) - 1 : 0);
    bool joiner = run && ends_particle_joiner(run->text, run->byte_length);
    if (last && last != ' ' && last != '\t' && last != '\n' && !joiner &&
        !append(r, out, " ", r->name_format))
        return false;
    return append(r, out, s, format);
}
static bool cjk_script(const char *s) {
    return eq(s, "Han") || eq(s, "Hiragana") || eq(s, "Katakana") || eq(s, "Hangul") || eq(s, "Bopomofo");
}
static int script_class(const char *s) {
    size_t offset = 0, n = strlen(s);
    uint32_t cp;
    bool cjk = false, other = false;
    while (offset < n) {
        (void)ce_csl_unicode_next(s, n, &offset, &cp);
        if (!ce_csl_unicode_is_letter(cp))
            continue;
        const char *script = ce_csl_unicode_script(cp);
        if (cjk_script(script))
            cjk = true;
        else
            other = true;
    }
    return cjk ? (other ? 2 : 1) : 0;
}
static bool casing(Render *r, const char *s, const CeCslXmlNode *part, CeCslOutputString *out) {
    CeCslTextCase mode;
    if (!case_mode(attr(part, "text-case"), &mode))
        return fail(r->error, r->cap, "invalid text-case");
    return ce_csl_case_transform(s, strlen(s), mode, r->context.item_language,
                                 r->context.style_default_locale, &r->result->allocator, out, r->error,
                                 r->cap);
}
static bool make_given(Render *r, const char *s, bool initialize, CeCslOutputString *out) {
    const char *with = r->options.initialize_with;
    if (!with || script_class(s) == 1)
        return ce_csl_case_transform(s, strlen(s), CE_CSL_CASE_NONE, NULL, NULL, &r->result->allocator, out,
                                     r->error, r->cap);
    CeCslOutput *body = new_output(r);
    if (!body)
        return false;
    size_t n = strlen(s), offset = 0;
    bool ok = false, pending_hyphen = false, previous_initial = false;
    while (offset < n) {
        size_t start = offset;
        uint32_t cp;
        (void)ce_csl_unicode_next(s, n, &offset, &cp);
        if (ce_csl_unicode_is_space(cp) || cp == '-' || cp == '.') {
            if (cp == '-')
                pending_hyphen = true;
            continue;
        }
        size_t end = offset;
        while (end < n) {
            size_t next = end;
            uint32_t c;
            (void)ce_csl_unicode_next(s, n, &next, &c);
            if (ce_csl_unicode_is_space(c) || c == '-' || c == '.')
                break;
            end = next;
        }
        size_t first_end = offset;
        while (first_end < end) {
            size_t next = first_end;
            uint32_t c;
            (void)ce_csl_unicode_next(s, n, &next, &c);
            if (!ce_csl_unicode_is_mark(c))
                break;
            first_end = next;
        }
        bool initial = ce_csl_unicode_is_letter(cp) && (initialize || first_end == end);
        if (ce_csl_output_text_bytes(body)) {
            if (pending_hyphen && r->options.initialize_with_hyphen) {
                /* initialize-with trailing spaces surround ordinary initials,
                 * but not either side of a retained compound-name hyphen. */
                if (!append(r, body, "-", 0))
                    goto done;
            } else if (previous_initial) {
                size_t ending = strlen(with);
                while (ending && with[ending - 1] == ' ')
                    --ending;
                if (!append(r, body, with + ending, 0))
                    goto done;
                if (!initial && !with[ending] && !append(r, body, " ", 0))
                    goto done;
            } else if (!append(r, body, " ", 0))
                goto done;
        }
        if (initial) {
            CeCslOutputString mapped = {0};
            if (!ce_csl_case_transform(s + start, first_end - start, CE_CSL_CASE_CAPITALIZE_FIRST,
                                       r->context.item_language, r->context.style_default_locale,
                                       &r->result->allocator, &mapped, r->error, r->cap))
                goto done;
            bool added = append_n(r, body, mapped.data, mapped.length, 0);
            ce_csl_output_string_free(&mapped);
            if (!added)
                goto done;
            size_t ending = strlen(with);
            while (ending && with[ending - 1] == ' ')
                --ending;
            if (!append_n(r, body, with, ending, 0))
                goto done;
        } else if (!append_n(r, body, s + start, end - start, 0))
            goto done;
        previous_initial = initial;
        pending_hyphen = false;
        offset = end;
    }
    ok = ce_csl_output_plain(body, out, r->error, r->cap);
done:
    ce_csl_output_free(body);
    return ok;
}
static CeCslGivenExpansion given_override(Render *r, size_t binding, size_t index) {
    unsigned value = r->result->overrides[r->binding_offsets[binding] + index];
    return value ? (CeCslGivenExpansion)(value - 1) : CE_CSL_GIVEN_STYLE;
}
static bool part_emit(Render *r, CeCslOutput *out, const CeCslXmlNode *part, const CeCslOutput *body) {
    return affix(r, out, part, body, r->name_format);
}
static bool person_output(Render *r, const CeCslNameIdentity *p, size_t ordinal,
                          CeCslGivenExpansion expansion, CeCslOutput *out, bool *inverted) {
    if (p->parse_names)
        return fail(r->error, r->cap, "parse-names=true requires an explicit name parsing layer");
    *inverted = false;
    if (p->kind == CE_CSL_NAME_EMPTY)
        return true;
    if (p->kind == CE_CSL_NAME_LITERAL)
        return append(r, out, p->literal, r->name_format);
    bool short_form = r->options.form == CE_CSL_NAME_SHORT && expansion == CE_CSL_GIVEN_STYLE;
    bool initialize = r->options.initialize && expansion != CE_CSL_GIVEN_FULL;
    if (expansion == CE_CSL_GIVEN_INITIALS && (!r->options.initialize_with || !r->options.initialize))
        return fail(r->error, r->cap, "initials expansion is unavailable for these name options");
    bool family_first = script_class(p->family) == 1 || p->static_ordering;
    bool invert = !family_first && (r->context.sorting || r->options.sort == CE_CSL_NAME_SORT_ALL ||
                                    (r->options.sort == CE_CSL_NAME_SORT_FIRST && ordinal == 0));
    bool demote = !short_form && invert &&
                  (r->options.demote == CE_CSL_NAME_DEMOTE_DISPLAY ||
                   (r->context.sorting && r->options.demote == CE_CSL_NAME_DEMOTE_SORT));
    if (family_first && (*p->dropping_particle || *p->non_dropping_particle || *p->suffix))
        return fail(r->error, r->cap,
                    "particles/suffix with family-first scripts require explicit ordering support");
    CeCslOutputString given = {0}, family = {0}, drop = {0}, non_drop = {0}, raw_given = {0};
    CeCslOutput *family_body = new_output(r), *given_body = new_output(r);
    bool ok = false;
    if (!family_body || !given_body)
        goto done;
    if (!make_given(r, p->given, initialize, &raw_given) ||
        !casing(r, raw_given.data, r->options.given_part, &given) ||
        !casing(r, p->family, r->options.family_part, &family) ||
        !casing(r, p->dropping_particle, r->options.given_part, &drop) ||
        !casing(r, p->non_dropping_particle, r->options.family_part, &non_drop))
        goto done;
    if (!demote && !join_text(r, family_body, non_drop.data, r->family_format))
        goto done;
    if (!join_text(r, family_body, family.data, r->family_format))
        goto done;
    if (short_form) {
        ok = part_emit(r, out, r->options.family_part, family_body);
        goto done;
    }
    if (!append(r, given_body, given.data, r->given_format))
        goto done;
    if (invert) {
        if (!join_text(r, given_body, drop.data, r->given_format))
            goto done;
        if (demote && !join_text(r, given_body, non_drop.data, r->family_format))
            goto done;
        if (!part_emit(r, out, r->options.family_part, family_body))
            goto done;
        if (ce_csl_output_text_bytes(given_body) &&
            (!append(r, out, r->options.sort_separator, r->name_format) ||
             !part_emit(r, out, r->options.given_part, given_body)))
            goto done;
        if (*p->suffix &&
            (!append(r, out, ", ", r->name_format) || !append(r, out, p->suffix, r->name_format)))
            goto done;
        *inverted = ce_csl_output_text_bytes(given_body) > 0;
    } else if (family_first) {
        if (!part_emit(r, out, r->options.family_part, family_body))
            goto done;
        if (ce_csl_output_text_bytes(given_body)) {
            if ((p->static_ordering || script_class(p->given) != 1) && !append(r, out, " ", r->name_format))
                goto done;
            if (!part_emit(r, out, r->options.given_part, given_body))
                goto done;
        }
    } else {
        if (!part_emit(r, out, r->options.given_part, given_body))
            goto done;
        if (!join_text(r, out, drop.data, r->given_format))
            goto done;
        if (ce_csl_output_text_bytes(out) && !ends_particle_joiner(drop.data, drop.length) &&
            !append(r, out, " ", r->name_format))
            goto done;
        if (*p->suffix) {
            if (p->comma_suffix) {
                if (!append(r, family_body, ", ", r->name_format) ||
                    !append(r, family_body, p->suffix, r->name_format))
                    goto done;
            } else if (!join_text(r, family_body, p->suffix, r->name_format))
                goto done;
        }
        if (!part_emit(r, out, r->options.family_part, family_body))
            goto done;
    }
    ok = true;
done:
    ce_csl_output_free(family_body);
    ce_csl_output_free(given_body);
    ce_csl_output_string_free(&given);
    ce_csl_output_string_free(&family);
    ce_csl_output_string_free(&drop);
    ce_csl_output_string_free(&non_drop);
    ce_csl_output_string_free(&raw_given);
    return ok;
}

static bool data_equal(const CeCslNamesData *a, const CeCslNamesData *b) {
    if (ce_csl_names_data_count(a) != ce_csl_names_data_count(b))
        return false;
    for (size_t i = 0; i < ce_csl_names_data_count(a); ++i)
        if (!ce_csl_name_identity_equal(&a->people[i], &b->people[i]))
            return false;
    return true;
}
static bool setup(Render *r, const CeCslNamesBinding *bindings, size_t count, const CeCslNameOptions *options,
                  const CeCslLocale *locale, const CeCslNameRenderContext *context, char *error, size_t cap) {
    *r = (Render){.locale = locale, .error = error, .cap = cap};
    if (!options || count > CE_CSL_NAMES_MAX_BINDINGS || (!bindings && count))
        return fail(error, cap, "invalid render arguments");
    r->options = *options;
    if (context)
        r->context = *context;
    if (options->form > CE_CSL_NAME_COUNT || options->and > CE_CSL_NAME_AND_SYMBOL ||
        options->sort > CE_CSL_NAME_SORT_ALL || options->demote > CE_CSL_NAME_DEMOTE_DISPLAY ||
        options->precedes_last > CE_CSL_NAME_AFTER_INVERTED ||
        options->precedes_et_al > CE_CSL_NAME_AFTER_INVERTED || !options->name_delimiter ||
        !options->names_delimiter || !options->sort_separator ||
        r->context.override_count > CE_CSL_NAMES_MAX_PEOPLE ||
        (!r->context.overrides && r->context.override_count))
        return fail(error, cap, "invalid resolved options");
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!bindings[i].variable || !*bindings[i].variable)
            return fail(error, cap, "missing variable binding");
        size_t n = ce_csl_names_data_count(bindings[i].data);
        r->binding_offsets[i] = total;
        if (n > CE_CSL_NAMES_MAX_PEOPLE - total)
            return fail(error, cap, "combined name count limit exceeded");
        total += n;
        for (size_t j = 0; j < i; ++j)
            if (eq(bindings[i].variable, bindings[j].variable))
                return fail(error, cap, "duplicate variable binding");
        if (bindings[i].data && bindings[i].data->references == SIZE_MAX)
            return fail(error, cap, "reference count limit exceeded");
        for (size_t j = 0; j < n; ++j)
            if (bindings[i].data->people[j].parse_names)
                return fail(error, cap, "parse-names=true requires an explicit name parsing layer");
    }
    for (size_t i = 0; i < r->context.override_count; ++i) {
        const CeCslNameOverride *o = &r->context.overrides[i];
        if (o->binding >= count || o->person_index >= ce_csl_names_data_count(bindings[o->binding].data) ||
            o->given > CE_CSL_GIVEN_FULL)
            return fail(error, cap, "invalid name expansion override");
    }
    if (!ce_csl_format_resolve(options->names, r->context.base_format, &r->names_format, error, cap) ||
        !ce_csl_format_resolve(options->name, r->names_format, &r->name_format, error, cap) ||
        !ce_csl_format_resolve(options->given_part, r->name_format, &r->given_format, error, cap) ||
        !ce_csl_format_resolve(options->family_part, r->name_format, &r->family_format, error, cap) ||
        !ce_csl_format_resolve(options->et_al, r->names_format, &r->etal_format, error, cap))
        return false;
    CeCslOutputAllocator a;
    if (!allocator_get(r->context.allocator, &a, error, cap))
        return false;
    CeCslNamesResult *result = a.allocate(a.context, sizeof(*result));
    if (!result)
        return fail(error, cap, "out of memory");
    *result = (CeCslNamesResult){.allocator = a, .person_capacity = total};
    r->result = result;
    result->output = new_output(r);
    if (count)
        result->data = a.allocate(a.context, count * sizeof(*result->data));
    if (total)
        result->people = a.allocate(a.context, total * sizeof(*result->people));
    if (total)
        result->overrides = a.allocate(a.context, total);
    if (!result->output || (count && !result->data) || (total && (!result->people || !result->overrides)))
        return fail(error, cap, "out of memory");
    if (total)
        memset(result->overrides, 0, total);
    for (size_t i = 0; i < r->context.override_count; ++i) {
        const CeCslNameOverride *o = &r->context.overrides[i];
        size_t index = r->binding_offsets[o->binding] + o->person_index;
        if (result->overrides[index])
            return fail(error, cap, "duplicate name expansion override");
        result->overrides[index] = (unsigned char)(o->given + 1);
    }
    for (size_t i = 0; i < count; ++i) {
        result->data[i] = bindings[i].data;
        if (result->data[i])
            ++result->data[i]->references;
        ++result->binding_count;
    }
    return true;
}
void ce_csl_names_result_free(CeCslNamesResult *r) {
    if (!r)
        return;
    for (size_t i = 0; i < r->binding_count; ++i)
        ce_csl_names_data_free(r->data[i]);
    if (r->data)
        r->allocator.deallocate(r->allocator.context, r->data);
    if (r->people)
        r->allocator.deallocate(r->allocator.context, r->people);
    if (r->overrides)
        r->allocator.deallocate(r->allocator.context, r->overrides);
    ce_csl_output_free(r->output);
    r->allocator.deallocate(r->allocator.context, r);
}
const CeCslOutput *ce_csl_names_result_output(const CeCslNamesResult *r) { return r ? r->output : NULL; }
const CeCslNamesSummary *ce_csl_names_result_summary(const CeCslNamesResult *r) {
    return r ? &r->summary : NULL;
}
size_t ce_csl_names_result_person_count(const CeCslNamesResult *r) { return r ? r->person_count : 0; }
const CeCslNameRendered *ce_csl_names_result_person(const CeCslNamesResult *r, size_t i) {
    return r && i < r->person_count ? &r->people[i] : NULL;
}
const CeCslNamesData *ce_csl_names_result_data(const CeCslNamesResult *r, size_t i) {
    return r && i < r->binding_count ? r->data[i] : NULL;
}
static CeCslLocaleTerm term(Render *r, const char *name, CeCslLocaleForm form, bool plural) {
    CeCslLocaleTerm t = ce_csl_locale_term(r->locale, name, form, plural, CE_CSL_LOCALE_NEUTER);
    if (!t.found)
        r->result->summary.missing_term = true;
    return t;
}
static bool label_before(const CeCslNameOptions *o) {
    for (const CeCslXmlNode *n = o->names->children; n; n = n->next) {
        if (n == o->name)
            return false;
        if (n == o->label)
            return o->name != NULL;
    }
    return false;
}
static bool label_output(Render *r, CeCslOutput *out, const char *variable, size_t original_count) {
    const CeCslXmlNode *label = r->options.label;
    if (!label || r->context.sorting)
        return true;
    static const char *const forms[] = {"long", "short", "verb", "verb-short", "symbol"};
    const char *v = attr(label, "form");
    int form = v ? enumeration(v, forms, 5) : 0;
    if (form < 0)
        return fail(r->error, r->cap, "invalid label form");
    bool plural = original_count > 1;
    if (eq(attr(label, "plural"), "always"))
        plural = true;
    if (eq(attr(label, "plural"), "never"))
        plural = false;
    const char *role = eq(variable, "editor-translator") ? "editortranslator" : variable;
    CeCslLocaleTerm t = term(r, role, (CeCslLocaleForm)form, plural);
    CeCslOutputString changed = {0};
    CeCslTextCase mode;
    if (!case_mode(attr(label, "text-case"), &mode))
        return false;
    if (!ce_csl_case_transform(t.text, t.length, mode, r->context.item_language,
                               r->context.style_default_locale, &r->result->allocator, &changed, r->error,
                               r->cap))
        return false;
    bool strip = false;
    v = attr(label, "strip-periods");
    if (v && !style_bool(v, &strip)) {
        ce_csl_output_string_free(&changed);
        return false;
    }
    if (strip) {
        size_t write = 0;
        for (size_t i = 0; i < changed.length; ++i)
            if (changed.data[i] != '.')
                changed.data[write++] = changed.data[i];
        changed.length = write;
        changed.data[write] = 0;
    }
    unsigned f;
    bool ok = false;
    CeCslOutput *body = new_output(r);
    if (body && ce_csl_format_resolve(label, r->names_format, &f, r->error, r->cap) &&
        append_n(r, body, changed.data, changed.length, f))
        ok = affix(r, out, label, body, r->names_format);
    ce_csl_output_string_free(&changed);
    ce_csl_output_free(body);
    return ok;
}
static bool need_delimiter(CeCslNameDelimiter mode, size_t preceding, bool inverted) {
    return mode == CE_CSL_NAME_ALWAYS || (mode == CE_CSL_NAME_CONTEXTUAL && preceding >= 2) ||
           (mode == CE_CSL_NAME_AFTER_INVERTED && inverted);
}
static bool before_last(Render *r, CeCslOutput *out, size_t preceding, bool inverted) {
    if (r->options.and == CE_CSL_NAME_AND_NONE)
        return append(r, out, r->options.name_delimiter, r->name_format);
    CeCslLocaleTerm conjunction = {.text = "&", .length = 1, .found = true};
    if (r->options.and == CE_CSL_NAME_AND_TEXT)
        conjunction = term(r, "and", CE_CSL_LOCALE_LONG, false);
    if (!conjunction.length)
        return append(r, out, r->options.name_delimiter, r->name_format);
    if (need_delimiter(r->options.precedes_last, preceding, inverted)) {
        if (!append(r, out, r->options.name_delimiter, r->name_format))
            return false;
    } else if (!append(r, out, " ", r->name_format))
        return false;
    return append_n(r, out, conjunction.text, conjunction.length, r->name_format) &&
           append(r, out, " ", r->name_format);
}
typedef struct Selection {
    size_t first, count, total;
    bool truncated, last;
} Selection;
static Selection selection(Render *r, const CeCslNamesData *data) {
    CeCslNameOptions *o = &r->options;
    size_t total = ce_csl_names_data_nonempty_count(data), min = o->et_al_min, first = o->et_al_use_first;
    bool has_min = o->has_et_al_min, has_first = o->has_et_al_use_first, last = o->et_al_use_last;
    if (r->context.subsequent) {
        if (o->has_subsequent_min) {
            min = o->subsequent_min;
            has_min = true;
        }
        if (o->has_subsequent_use_first) {
            first = o->subsequent_use_first;
            has_first = true;
        }
    }
    if (r->context.sorting) {
        if (r->context.has_sort_min) {
            min = r->context.sort_min;
            has_min = true;
        }
        if (r->context.has_sort_first) {
            first = r->context.sort_first;
            has_first = true;
        }
        if (r->context.has_sort_last)
            last = r->context.sort_last;
    }
    bool truncate = has_min && has_first && total >= min && total > first;
    if (!truncate)
        first = total;
    if (first < r->context.minimum_names)
        first = r->context.minimum_names;
    if (first >= total)
        return (Selection){total, total, total, false, false};
    last = last && total - first >= 2;
    return (Selection){first, first + (last ? 1 : 0), total, true, last};
}
static bool group_output(Render *r, const CeCslNamesBinding *b, size_t binding, Selection s) {
    CeCslOutput *destination = r->result->output, *out = new_output(r);
    size_t person_start = r->result->person_count;
    bool ok = false;
    if (!out)
        return false;
    if (r->options.label && label_before(&r->options) && !label_output(r, destination, b->variable, s.total))
        goto done;
    size_t ordinal = 0, rendered = 0, last_index = SIZE_MAX;
    bool previous_inverted = false;
    for (size_t i = 0; i < ce_csl_names_data_count(b->data); ++i)
        if (b->data->people[i].kind != CE_CSL_NAME_EMPTY)
            last_index = i;
    for (size_t i = 0; i < ce_csl_names_data_count(b->data); ++i) {
        const CeCslNameIdentity *person = &b->data->people[i];
        if (person->kind == CE_CSL_NAME_EMPTY)
            continue;
        bool selected = ordinal < s.first || (s.last && i == last_index), is_last = s.last && i == last_index;
        size_t current_ordinal = ordinal++;
        if (!selected)
            continue;
        if (is_last) {
            if ((rendered && !append(r, out, r->options.name_delimiter, r->name_format)) ||
                !append(r, out, "… ", r->name_format))
                goto done;
        } else if (rendered) {
            if (!s.truncated && rendered + 1 == s.count) {
                if (!before_last(r, out, rendered, previous_inverted))
                    goto done;
            } else if (!append(r, out, r->options.name_delimiter, r->name_format))
                goto done;
        }
        CeCslOutput *body = new_output(r);
        if (!body)
            goto done;
        bool inverted = false;
        CeCslGivenExpansion expansion = given_override(r, binding, i);
        if (!person_output(r, person, current_ordinal, expansion, body, &inverted)) {
            ce_csl_output_free(body);
            goto done;
        }
        size_t start = ce_csl_output_text_bytes(out), length = ce_csl_output_text_bytes(body);
        bool added = concat(r, out, body);
        ce_csl_output_free(body);
        if (!added)
            goto done;
        r->result->people[r->result->person_count++] = (CeCslNameRendered){
            .binding = binding,
            .person_index = i,
            .identity = person,
            .given = expansion,
            .inverted = inverted,
            .given_visible = person->kind == CE_CSL_NAME_PERSONAL && *person->given &&
                             (r->options.form != CE_CSL_NAME_SHORT || expansion != CE_CSL_GIVEN_STYLE),
            .output_offset = start,
            .output_length = length};
        CeCslNameRendered *record = &r->result->people[r->result->person_count - 1];
        record->given_initialized = record->given_visible && r->options.initialize &&
                                    r->options.initialize_with && expansion != CE_CSL_GIVEN_FULL &&
                                    script_class(person->given) != 1;
        previous_inverted = inverted;
        ++rendered;
    }
    if (s.truncated && !s.last && !r->context.sorting) {
        CeCslLocaleTerm t = term(r, attr(r->options.et_al, "term") ? attr(r->options.et_al, "term") : "et-al",
                                 CE_CSL_LOCALE_LONG, false);
        if (t.length) {
            if (rendered && !append(r, out,
                                    need_delimiter(r->options.precedes_et_al, rendered, previous_inverted)
                                        ? r->options.name_delimiter
                                        : " ",
                                    r->name_format))
                goto done;
            if (!append_n(r, out, t.text, t.length, r->etal_format))
                goto done;
        }
    }
    size_t offset = ce_csl_output_text_bytes(destination);
    if (ce_csl_output_text_bytes(out))
        offset += strlen(empty(attr(r->options.name, "prefix")));
    if (!affix(r, destination, r->options.name, out, r->names_format))
        goto done;
    for (size_t i = person_start; i < r->result->person_count; ++i)
        r->result->people[i].output_offset += offset;
    if (r->options.label && !label_before(&r->options) && !label_output(r, destination, b->variable, s.total))
        goto done;
    ok = true;
done:
    ce_csl_output_free(out);
    return ok;
}
bool ce_csl_name_render(const CeCslNamesData *data, size_t index, const CeCslNameOptions *options,
                        const CeCslLocale *locale, const CeCslNameRenderContext *context,
                        CeCslNamesResult **out, char *error, size_t cap) {
    if (error && cap)
        *error = 0;
    if (!out || !data || index >= data->count || !options || options->form == CE_CSL_NAME_COUNT)
        return fail(error, cap, "invalid scalar render arguments");
    CeCslNamesBinding binding = {data->origin.variable && *data->origin.variable ? data->origin.variable
                                                                                 : "author",
                                 (CeCslNamesData *)data};
    Render r;
    if (!setup(&r, &binding, 1, options, locale, context, error, cap)) {
        ce_csl_names_result_free(r.result);
        return false;
    }
    bool inverted = false;
    CeCslGivenExpansion expansion = given_override(&r, 0, index);
    size_t ordinal = 0;
    for (size_t i = 0; i < index; ++i)
        if (data->people[i].kind != CE_CSL_NAME_EMPTY)
            ++ordinal;
    if (!person_output(&r, &data->people[index], ordinal, expansion, r.result->output, &inverted)) {
        ce_csl_names_result_free(r.result);
        return false;
    }
    size_t length = ce_csl_output_text_bytes(r.result->output);
    r.result->summary = (CeCslNamesSummary){.total_names = data->people[index].kind != CE_CSL_NAME_EMPTY,
                                            .rendered_names = length ? 1 : 0,
                                            .rendered = length != 0};
    if (length) {
        r.result->people[0] = (CeCslNameRendered){
            .person_index = index,
            .identity = &data->people[index],
            .given = expansion,
            .inverted = inverted,
            .given_visible = data->people[index].kind == CE_CSL_NAME_PERSONAL && *data->people[index].given &&
                             (options->form != CE_CSL_NAME_SHORT || expansion != CE_CSL_GIVEN_STYLE),
            .output_length = length};
        r.result->person_count = 1;
    }
    if (length)
        r.result->people[0].given_initialized = r.result->people[0].given_visible && options->initialize &&
                                                options->initialize_with && expansion != CE_CSL_GIVEN_FULL &&
                                                script_class(data->people[index].given) != 1;
    ce_csl_names_result_free(*out);
    *out = r.result;
    return true;
}
bool ce_csl_names_render(const CeCslNamesBinding *bindings, size_t count, const CeCslNameOptions *options,
                         const CeCslLocale *locale, const CeCslNameRenderContext *context,
                         CeCslNamesResult **out, char *error, size_t cap) {
    if (error && cap)
        *error = 0;
    if (!out)
        return fail(error, cap, "missing render output");
    Render r;
    if (!setup(&r, bindings, count, options, locale, context, error, cap)) {
        ce_csl_names_result_free(r.result);
        return false;
    }
    bool collapse = count == 2 &&
                    ((eq(bindings[0].variable, "editor") && eq(bindings[1].variable, "translator")) ||
                     (eq(bindings[0].variable, "translator") && eq(bindings[1].variable, "editor"))) &&
                    data_equal(bindings[0].data, bindings[1].data);
    r.result->summary.collapsed_editor_translator = collapse;
    bool prior = false;
    size_t displayed = 0;
    for (size_t i = 0; i < count; ++i) {
        if (collapse && i == 1)
            continue;
        CeCslNamesBinding b = bindings[i];
        if (collapse)
            b.variable = "editor-translator";
        Selection s = selection(&r, b.data);
        if (!s.total)
            continue;
        r.result->summary.total_names += s.total;
        r.result->summary.rendered_names += s.count;
        r.result->summary.truncated |= s.truncated;
        r.result->summary.used_last |= s.last;
        displayed += s.count;
        if (options->form == CE_CSL_NAME_COUNT)
            continue;
        CeCslOutput *previous = r.result->output, *group = new_output(&r);
        if (!group)
            goto failed;
        size_t person_start = r.result->person_count;
        r.result->output = group;
        bool ok = group_output(&r, &b, i, s);
        r.result->output = previous;
        if (!ok) {
            ce_csl_output_free(group);
            goto failed;
        }
        if (ce_csl_output_text_bytes(group)) {
            if (prior && !append(&r, previous, options->names_delimiter, r.names_format)) {
                ce_csl_output_free(group);
                goto failed;
            }
            size_t offset = ce_csl_output_text_bytes(previous);
            if (!concat(&r, previous, group)) {
                ce_csl_output_free(group);
                goto failed;
            }
            for (size_t j = person_start; j < r.result->person_count; ++j)
                r.result->people[j].output_offset += offset;
            prior = true;
        }
        ce_csl_output_free(group);
    }
    if (options->form == CE_CSL_NAME_COUNT && r.result->summary.total_names) {
        char number_text[32];
        snprintf(number_text, sizeof(number_text), "%zu", displayed);
        CeCslOutput *body = new_output(&r);
        if (!body)
            goto failed;
        bool ok = append(&r, body, number_text, r.name_format) &&
                  affix(&r, r.result->output, options->name, body, r.names_format);
        ce_csl_output_free(body);
        if (!ok)
            goto failed;
    }
    r.result->summary.needs_substitute = r.result->summary.total_names == 0 && options->substitute != NULL;
    if (ce_csl_output_text_bytes(r.result->output)) {
        CeCslOutput *wrapped = new_output(&r);
        if (!wrapped)
            goto failed;
        if (!affix(&r, wrapped, options->names, r.result->output, r.context.base_format)) {
            ce_csl_output_free(wrapped);
            goto failed;
        }
        size_t prefix = strlen(empty(attr(options->names, "prefix")));
        for (size_t i = 0; i < r.result->person_count; ++i)
            r.result->people[i].output_offset += prefix;
        ce_csl_output_free(r.result->output);
        r.result->output = wrapped;
        r.result->summary.rendered = true;
    }
    ce_csl_names_result_free(*out);
    *out = r.result;
    return true;
failed:
    ce_csl_names_result_free(r.result);
    return false;
}
