#include "csl_locale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Primary-dialect facts transcribed from Citation Style Language locales.json:
 * https://github.com/citation-style-language/locales/blob/
 * 14940e56f7c783e4173cafbd92e0f5856f2caa14/locales.json
 * SHA-256: 82faf814d3d86cf15132dd6062ee143090609b4f01b9fe8e422d8024ca4eabb2
 * Source attribution: Citation Style Language project, https://citationstyles.org/
 * Source license: CC BY-SA 3.0, https://creativecommons.org/licenses/by-sa/3.0/
 * Only the primary-dialects object is represented; tags are lowercased for
 * case-insensitive comparison. No inference from the operating system. */
static const CeCslLocalePrimary primary_dialects[] = {
    {"af", "af-za"},
    {"ar", "ar"},
    {"bal", "bal-pk"},
    {"bg", "bg-bg"},
    {"brh", "brh-pk"},
    {"ca", "ca-ad"},
    {"cs", "cs-cz"},
    {"cy", "cy-gb"},
    {"da", "da-dk"},
    {"de", "de-de"},
    {"el", "el-gr"},
    {"en", "en-us"},
    {"es", "es-es"},
    {"et", "et-ee"},
    {"eu", "eu"},
    {"fa", "fa-ir"},
    {"fi", "fi-fi"},
    {"fr", "fr-fr"},
    {"gl", "gl-es"},
    {"he", "he-il"},
    {"hi", "hi-in"},
    {"hr", "hr-hr"},
    {"hu", "hu-hu"},
    {"hy", "hy-am"},
    {"id", "id-id"},
    {"is", "is-is"},
    {"it", "it-it"},
    {"ja", "ja-jp"},
    {"km", "km-kh"},
    {"ko", "ko-kr"},
    {"la", "la"},
    {"lij", "lij-it"},
    {"lt", "lt-lt"},
    {"lv", "lv-lv"},
    {"mn", "mn-mn"},
    {"ms", "ms-my"},
    {"nb", "nb-no"},
    {"nl", "nl-nl"},
    {"nn", "nn-no"},
    {"pa", "pa-pk"},
    {"pl", "pl-pl"},
    {"pt", "pt-pt"},
    {"ro", "ro-ro"},
    {"ru", "ru-ru"},
    {"sk", "sk-sk"},
    {"sl", "sl-si"},
    {"sr", "sr-latn-rs"},
    {"sv", "sv-se"},
    {"th", "th-th"},
    {"tl", "tl-ph"},
    {"tr", "tr-tr"},
    {"uk", "uk-ua"},
    {"vi", "vi-vn"},
    {"zh", "zh-cn"},
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
typedef enum OrdinalKind { ORDINARY, ORDINAL_DEFAULT, ORDINAL_SUFFIX, ORDINAL_LONG } OrdinalKind;
typedef enum OrdinalMatch { LAST_DIGIT = 1, LAST_TWO = 2, WHOLE_NUMBER = 3 } OrdinalMatch;
typedef struct Term {
    const CeCslXmlNode *node;
    const char *name;
    CeCslLocaleForm form;
    CeCslLocaleGender gender, noun_gender;
    OrdinalKind kind;
    OrdinalMatch match;
    unsigned number;
} Term;
typedef struct Layer {
    const CeCslXmlNode *root;
    char tag[CE_CSL_LOCALE_TAG_CAPACITY];
    Term *terms;
    size_t term_count;
    const CeCslXmlNode *dates[2];
    bool option_found[2], option_value[2], has_ordinals;
} Layer;
struct CeCslLocale {
    CeCslLocaleAllocator allocator;
    Layer *layers;
    size_t layer_count;
    const Layer *selected[6], *ordinals;
    size_t selected_count;
    bool legacy_ordinals;
    char language[CE_CSL_LOCALE_TAG_CAPACITY];
};

static void *allocate_default(void *context, size_t size) { (void)context; return malloc(size); }
static void deallocate_default(void *context, void *pointer) { (void)context; free(pointer); }
static bool fail(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return false;
}
static bool same(const char *a, const char *b) { return a && b && !strcmp(a, b); }
static bool ascii_letter(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool xml_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool whitespace(const char *text) {
    if (text) while (*text) if (!xml_space((unsigned char)*text++)) return false;
    return true;
}
static bool tag_normalize(const char *tag, char output[CE_CSL_LOCALE_TAG_CAPACITY]) {
    size_t i = 0, part = 0;
    bool first = true;
    if (!tag || !*tag) return false;
    while (tag[i]) {
        unsigned char c = (unsigned char)tag[i];
        if (i + 1 >= CE_CSL_LOCALE_TAG_CAPACITY) return false;
        if (c == '-') {
            if (!part) return false;
            first = false; part = 0;
        } else {
            if (!ascii_letter(c) && (first || c < '0' || c > '9')) return false;
            if (++part > 8) return false;
        }
        output[i++] = c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : (char)c;
    }
    if (!part) return false;
    output[i] = '\0';
    return true;
}
static void language_part(const char *tag, char output[CE_CSL_LOCALE_TAG_CAPACITY]) {
    size_t i = 0;
    while (tag[i] && tag[i] != '-') { output[i] = tag[i]; ++i; }
    output[i] = '\0';
}
static const char *builtin_primary(const char *language) {
    size_t i;
    for (i = 0; i < COUNT(primary_dialects); ++i)
        if (same(primary_dialects[i].language, language)) return primary_dialects[i].primary_dialect;
    return NULL;
}
static bool validate_primary(const CeCslLocaleCatalog *catalog, char *error, size_t capacity) {
    size_t i, j;
    for (i = 0; i < catalog->primary_count; ++i) {
        char language[CE_CSL_LOCALE_TAG_CAPACITY], primary[CE_CSL_LOCALE_TAG_CAPACITY];
        char base[CE_CSL_LOCALE_TAG_CAPACITY];
        const char *builtin;
        if (!tag_normalize(catalog->primary[i].language, language) || strchr(language, '-') ||
            !tag_normalize(catalog->primary[i].primary_dialect, primary))
            return fail(error, capacity, "Invalid primary-dialect mapping");
        language_part(primary, base);
        if (!same(language, base)) return fail(error, capacity, "Primary dialect belongs to a different language");
        builtin = builtin_primary(language);
        if (builtin && !same(builtin, primary)) return fail(error, capacity, "Primary mapping conflicts with pinned CSL metadata");
        for (j = 0; j < i; ++j) {
            char previous[CE_CSL_LOCALE_TAG_CAPACITY];
            (void)tag_normalize(catalog->primary[j].language, previous);
            if (same(previous, language)) return fail(error, capacity, "Duplicate primary-dialect mapping");
        }
    }
    return true;
}
static bool find_primary(const CeCslLocaleCatalog *catalog, const char *language,
                         char output[CE_CSL_LOCALE_TAG_CAPACITY]) {
    const char *builtin = builtin_primary(language);
    size_t i;
    if (builtin) return tag_normalize(builtin, output);
    for (i = 0; i < catalog->primary_count; ++i) {
        char key[CE_CSL_LOCALE_TAG_CAPACITY];
        (void)tag_normalize(catalog->primary[i].language, key);
        if (same(key, language)) return tag_normalize(catalog->primary[i].primary_dialect, output);
    }
    return false;
}

static bool boolean_value(const char *text, bool *value) {
    size_t length;
    if (!text) return false;
    while (xml_space((unsigned char)*text)) ++text;
    length = strlen(text);
    while (length && xml_space((unsigned char)text[length - 1])) --length;
    if ((length == 4 && !memcmp(text, "true", 4)) || (length == 1 && text[0] == '1')) { *value = true; return true; }
    if ((length == 5 && !memcmp(text, "false", 5)) || (length == 1 && text[0] == '0')) { *value = false; return true; }
    return false;
}
static int form_value(const char *form) {
    static const char *const names[] = {"long", "short", "verb", "verb-short", "symbol"};
    size_t i;
    if (!form) return CE_CSL_LOCALE_LONG;
    for (i = 0; i < COUNT(names); ++i) if (same(form, names[i])) return (int)i;
    return -1;
}
static int gender_value(const char *gender) {
    if (!gender) return CE_CSL_LOCALE_NEUTER;
    if (same(gender, "masculine")) return CE_CSL_LOCALE_MASCULINE;
    if (same(gender, "feminine")) return CE_CSL_LOCALE_FEMININE;
    return -1;
}
static OrdinalKind ordinal_kind(const char *name, unsigned *number) {
    size_t prefix;
    if (same(name, "ordinal")) return ORDINAL_DEFAULT;
    if (!strncmp(name, "ordinal-", 8)) prefix = 8;
    else if (!strncmp(name, "long-ordinal-", 13)) prefix = 13;
    else return ORDINARY;
    if (strlen(name) != prefix + 2 || name[prefix] < '0' || name[prefix] > '9' ||
        name[prefix + 1] < '0' || name[prefix + 1] > '9') return ORDINARY;
    *number = (unsigned)((name[prefix] - '0') * 10 + name[prefix + 1] - '0');
    return prefix == 8 ? ORDINAL_SUFFIX : ORDINAL_LONG;
}
static bool attribute_allowed(const char *name, const char *const *allowed, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i) if (same(name, allowed[i])) return true;
    return false;
}
static bool attributes(const CeCslXmlNode *node, const char *const *allowed,
                       size_t count, char *error, size_t capacity) {
    size_t i;
    for (i = 0; i < node->attribute_count; ++i)
        if (!attribute_allowed(node->attributes[i].name, allowed, count))
            return fail(error, capacity, "Unsupported attribute in locale resource");
    return true;
}
static bool validate_term(Term *term, const CeCslXmlNode *node, char *error, size_t capacity) {
    static const char *const allowed[] = {"name", "form", "gender", "gender-form", "match"};
    const CeCslXmlNode *single, *multiple;
    const char *match;
    int form, gender, noun_gender;
    if (!ce_csl_xml_is(node, "term") || !attributes(node, allowed, COUNT(allowed), error, capacity))
        return fail(error, capacity, "Invalid term element or attribute");
    term->node = node; term->name = ce_csl_xml_attribute(node, "name");
    if (!term->name || whitespace(term->name) || strlen(term->name) > 128)
        return fail(error, capacity, "Term requires a bounded nonempty name");
    form = form_value(ce_csl_xml_attribute(node, "form"));
    gender = gender_value(ce_csl_xml_attribute(node, "gender-form"));
    noun_gender = gender_value(ce_csl_xml_attribute(node, "gender"));
    if (form < 0 || gender < 0 || noun_gender < 0) return fail(error, capacity, "Invalid term form or gender");
    term->form = (CeCslLocaleForm)form; term->gender = (CeCslLocaleGender)gender;
    term->noun_gender = (CeCslLocaleGender)noun_gender;
    term->kind = ordinal_kind(term->name, &term->number);
    if (term->kind == ORDINARY && (!strncmp(term->name, "ordinal-", 8) ||
        !strncmp(term->name, "long-ordinal-", 13)))
        return fail(error, capacity, "Malformed ordinal term name");
    match = ce_csl_xml_attribute(node, "match");
    if (term->kind == ORDINARY && (gender || match)) return fail(error, capacity, "gender-form/match require an ordinal term");
    if (noun_gender && (form != CE_CSL_LOCALE_LONG || term->kind != ORDINARY))
        return fail(error, capacity, "Noun gender belongs on a long non-ordinal term");
    if (term->kind != ORDINARY && form != CE_CSL_LOCALE_LONG)
        return fail(error, capacity, "Ordinal terms require long form");
    if (term->kind == ORDINAL_LONG && (term->number < 1 || term->number > 10))
        return fail(error, capacity, "Long ordinal term must be in 01..10");
    term->match = term->number < 10 ? LAST_DIGIT : LAST_TWO;
    if (match) {
        if (same(match, "whole-number")) term->match = WHOLE_NUMBER;
        else if (same(match, "last-two-digits")) term->match = LAST_TWO;
        else if (same(match, "last-digit")) term->match = LAST_DIGIT;
        else return fail(error, capacity, "Invalid ordinal match");
    }
    if (term->kind == ORDINAL_LONG) term->match = WHOLE_NUMBER;
    if (term->kind == ORDINAL_SUFFIX && term->number >= 10 && term->match == LAST_DIGIT)
        return fail(error, capacity, "ordinal-10..99 cannot use last-digit matching");
    if (!node->children) return true; /* Includes explicitly empty strings. */
    single = node->children; multiple = single->next;
    if (!whitespace(node->text) || !ce_csl_xml_is(single, "single") || !multiple ||
        !ce_csl_xml_is(multiple, "multiple") || multiple->next || single->children ||
        multiple->children || single->attribute_count || multiple->attribute_count)
        return fail(error, capacity, "Term must be text or exactly single then multiple");
    return true;
}
static bool one_of(const char *value, const char *const *choices, size_t count) {
    return attribute_allowed(value, choices, count);
}
static bool format_attribute(const char *name, const char *value) {
    static const char *const style[] = {"normal", "italic", "oblique"};
    static const char *const variant[] = {"normal", "small-caps"};
    static const char *const weight[] = {"normal", "bold", "light"};
    static const char *const decoration[] = {"none", "underline"};
    static const char *const vertical[] = {"baseline", "sup", "sub"};
    static const char *const textcase[] = {"lowercase", "uppercase", "capitalize-first", "capitalize-all", "sentence", "title"};
    if (same(name, "font-style")) return one_of(value, style, COUNT(style));
    if (same(name, "font-variant")) return one_of(value, variant, COUNT(variant));
    if (same(name, "font-weight")) return one_of(value, weight, COUNT(weight));
    if (same(name, "text-decoration")) return one_of(value, decoration, COUNT(decoration));
    if (same(name, "vertical-align")) return one_of(value, vertical, COUNT(vertical));
    if (same(name, "text-case")) return one_of(value, textcase, COUNT(textcase));
    return false;
}
static bool validate_date(const CeCslXmlNode *date, char *error, size_t capacity) {
    const CeCslXmlNode *part;
    unsigned seen = 0;
    size_t i;
    if (!date->children || !whitespace(date->text)) return fail(error, capacity, "Localized date requires date-part elements");
    for (i = 0; i < date->attribute_count; ++i) {
        const CeCslXmlAttr *a = &date->attributes[i];
        if (same(a->name, "form") || same(a->name, "delimiter")) continue;
        if (!format_attribute(a->name, a->value)) return fail(error, capacity, "Invalid localized date attribute (affixes belong on date-parts)");
    }
    for (part = date->children; part; part = part->next) {
        const char *name = ce_csl_xml_attribute(part, "name");
        const char *form = ce_csl_xml_attribute(part, "form");
        unsigned bit = same(name, "year") ? 1u : same(name, "month") ? 2u : same(name, "day") ? 4u : 0u;
        if (!ce_csl_xml_is(part, "date-part") || !bit || (seen & bit) || part->children || !whitespace(part->text))
            return fail(error, capacity, "Invalid or repeated localized date-part");
        seen |= bit;
        if (form && !(bit == 1 && (same(form, "long") || same(form, "short"))) &&
            !(bit == 2 && (same(form, "long") || same(form, "short") || same(form, "numeric") || same(form, "numeric-leading-zeros"))) &&
            !(bit == 4 && (same(form, "numeric") || same(form, "numeric-leading-zeros") || same(form, "ordinal"))))
            return fail(error, capacity, "Invalid date-part form");
        for (i = 0; i < part->attribute_count; ++i) {
            const CeCslXmlAttr *a = &part->attributes[i];
            bool value;
            if (same(a->name, "name") || same(a->name, "form") || same(a->name, "prefix") ||
                same(a->name, "suffix") || same(a->name, "range-delimiter")) continue;
            if (same(a->name, "strip-periods") && bit == 2 && boolean_value(a->value, &value)) continue;
            if (!format_attribute(a->name, a->value)) return fail(error, capacity, "Invalid date-part attribute");
        }
    }
    return true;
}
static int compare_terms(const void *left, const void *right) {
    const Term *a = left, *b = right;
    int result = strcmp(a->name, b->name);
    if (result) return result;
    if (a->form != b->form) return (int)a->form - (int)b->form;
    return (int)a->gender - (int)b->gender;
}

static bool parse_layer(CeCslLocale *locale, Layer *layer, const CeCslXmlNode *root,
                        bool external, size_t *total_terms, char *error, size_t capacity) {
    const CeCslXmlNode *node, *terms = NULL;
    const char *tag, *version;
    bool has_options = false, has_info = false;
    size_t count = 0, i;
    if (!root || !ce_csl_xml_is(root, "locale") || !whitespace(root->text))
        return fail(error, capacity, "Expected a CSL namespace locale root");
    tag = ce_csl_xml_attribute(root, "xml:lang"); version = ce_csl_xml_attribute(root, "version");
    if (external && (!tag || !same(version, "1.0"))) return fail(error, capacity, "Standalone locale requires xml:lang and version=1.0");
    if (!external && version) return fail(error, capacity, "In-style locale cannot declare version");
    for (i = 0; i < root->attribute_count; ++i) {
        const char *name = root->attributes[i].name;
        if (!same(name, "http://www.w3.org/XML/1998/namespace\x1f" "lang") && !same(name, "version"))
            return fail(error, capacity, "Unsupported locale root attribute");
    }
    if (tag && !tag_normalize(tag, layer->tag)) return fail(error, capacity, "Malformed locale language tag");
    layer->root = root;
    for (node = root->children; node; node = node->next) {
        if (ce_csl_xml_is(node, "info")) {
            if (!external || has_info || node != root->children) return fail(error, capacity, "Locale info must occur once, first, in an external resource");
            has_info = true;
        } else if (ce_csl_xml_is(node, "terms")) {
            if (terms || node->attribute_count || !whitespace(node->text) || !node->children)
                return fail(error, capacity, "Invalid or duplicate terms container");
            terms = node;
        } else if (ce_csl_xml_is(node, "date")) {
            const char *form = ce_csl_xml_attribute(node, "form");
            int index = same(form, "text") ? 0 : same(form, "numeric") ? 1 : -1;
            if (index < 0 || layer->dates[index] || !validate_date(node, error, capacity))
                return fail(error, capacity, "Invalid or duplicate localized date");
            layer->dates[index] = node;
        } else if (ce_csl_xml_is(node, "style-options")) {
            if (has_options || node->children || !whitespace(node->text)) return fail(error, capacity, "Invalid or duplicate style-options");
            has_options = true;
            for (i = 0; i < node->attribute_count; ++i) {
                const CeCslXmlAttr *a = &node->attributes[i];
                int index = same(a->name, "punctuation-in-quote") ? 0 : same(a->name, "limit-day-ordinals-to-day-1") ? 1 : -1;
                if (index < 0 || !boolean_value(a->value, &layer->option_value[index]))
                    return fail(error, capacity, "Invalid localized style option");
                layer->option_found[index] = true;
            }
        } else return fail(error, capacity, "Unexpected element in locale resource");
    }
    if (external && (!has_options || (!layer->dates[0] && !layer->dates[1]) || !terms))
        return fail(error, capacity, "Standalone locale requires style-options, date and terms");
    if (!terms) return true;
    for (node = terms->children; node; node = node->next) {
        if (++count > CE_CSL_LOCALE_MAX_TERMS - *total_terms)
            return fail(error, capacity, "Locale term limit exceeded");
    }
    layer->terms = locale->allocator.allocate(locale->allocator.context, count * sizeof(*layer->terms));
    if (!layer->terms) return fail(error, capacity, "Out of memory indexing locale terms");
    memset(layer->terms, 0, count * sizeof(*layer->terms));
    layer->term_count = count; *total_terms += count;
    i = 0;
    for (node = terms->children; node; node = node->next, ++i) {
        if (!validate_term(&layer->terms[i], node, error, capacity)) return false;
        if (layer->terms[i].kind == ORDINAL_DEFAULT || layer->terms[i].kind == ORDINAL_SUFFIX)
            layer->has_ordinals = true;
    }
    qsort(layer->terms, count, sizeof(*layer->terms), compare_terms);
    for (i = 1; i < count; ++i)
        if (!compare_terms(&layer->terms[i - 1], &layer->terms[i]))
            return fail(error, capacity, "Duplicate term name/form/gender-form key");
    return true;
}

static const Term *find_term(const Layer *layer, const char *name,
                            CeCslLocaleForm form, CeCslLocaleGender gender) {
    size_t lo = 0, hi;
    Term key = {0};
    if (!layer) return NULL;
    hi = layer->term_count; key.name = name; key.form = form; key.gender = gender;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int result = compare_terms(&layer->terms[mid], &key);
        if (result < 0) lo = mid + 1;
        else if (result > 0) hi = mid;
        else return &layer->terms[mid];
    }
    return NULL;
}
static bool has_term_name(const Layer *layer, const char *name) {
    int gender;
    for (gender = CE_CSL_LOCALE_NEUTER; gender <= CE_CSL_LOCALE_FEMININE; ++gender)
        if (find_term(layer, name, CE_CSL_LOCALE_LONG, (CeCslLocaleGender)gender)) return true;
    return false;
}
static void select_layer(CeCslLocale *locale, size_t begin, size_t end, const char *tag) {
    size_t i, j;
    for (i = begin; i < end; ++i) {
        if (!same(locale->layers[i].tag, tag)) continue;
        for (j = 0; j < locale->selected_count; ++j) if (locale->selected[j] == &locale->layers[i]) return;
        locale->selected[locale->selected_count++] = &locale->layers[i];
        return;
    }
}

CeCslLocale *ce_csl_locale_new(const CeCslXmlNode *style, const CeCslLocaleCatalog *catalog,
    const char *chosen_language, const CeCslLocaleAllocator *allocator, char *error, size_t capacity) {
    CeCslLocaleCatalog empty = {0};
    CeCslLocaleAllocator memory = {NULL, allocate_default, deallocate_default};
    CeCslLocale *locale;
    const CeCslXmlNode *node;
    size_t in_style = 0, i, j, total_terms = 0;
    char language[CE_CSL_LOCALE_TAG_CAPACITY], primary[CE_CSL_LOCALE_TAG_CAPACITY];
    char chosen[CE_CSL_LOCALE_TAG_CAPACITY];
    if (error && capacity) *error = '\0';
    if (!catalog) catalog = &empty;
    if ((catalog->root_count && !catalog->roots) || (catalog->primary_count && !catalog->primary) ||
        catalog->root_count > CE_CSL_LOCALE_MAX_SOURCES || catalog->primary_count > CE_CSL_LOCALE_MAX_SOURCES) {
        fail(error, capacity, "Invalid or oversized locale catalog"); return NULL;
    }
    if (allocator) {
        if (!allocator->allocate || !allocator->deallocate) { fail(error, capacity, "Both allocator callbacks are required"); return NULL; }
        memory = *allocator;
    }
    if (style && (!ce_csl_xml_is(style, "style") || !same(ce_csl_xml_attribute(style, "version"), "1.0"))) {
        fail(error, capacity, "Expected a CSL namespace style version=1.0"); return NULL;
    }
    if (!chosen_language && style) chosen_language = ce_csl_xml_attribute(style, "default-locale");
    if (!chosen_language) chosen_language = "en-US";
    if (!tag_normalize(chosen_language, chosen) || !validate_primary(catalog, error, capacity)) {
        if (error && capacity && !*error) fail(error, capacity, "Malformed chosen locale tag");
        return NULL;
    }
    language_part(chosen, language);
    if (!find_primary(catalog, language, primary)) {
        fail(error, capacity, "Unknown primary dialect; provide an explicit catalog mapping"); return NULL;
    }
    if (!strchr(chosen, '-')) memcpy(chosen, primary, strlen(primary) + 1);
    if (style) for (node = style->children; node; node = node->next) {
        if (ce_csl_xml_is(node, "locale")) ++in_style;
        else if (node->name) {
            const char *local = strrchr(node->name, '\x1f');
            local = local ? local + 1 : node->name;
            if (same(local, "locale")) {
                fail(error, capacity, "In-style locale has the wrong namespace"); return NULL;
            }
        }
    }
    if (in_style > CE_CSL_LOCALE_MAX_SOURCES - catalog->root_count) {
        fail(error, capacity, "Locale source limit exceeded"); return NULL;
    }
    locale = memory.allocate(memory.context, sizeof(*locale));
    if (!locale) { fail(error, capacity, "Out of memory creating locale resolver"); return NULL; }
    memset(locale, 0, sizeof(*locale)); locale->allocator = memory;
    memcpy(locale->language, chosen, strlen(chosen) + 1);
    locale->layer_count = in_style + catalog->root_count;
    if (locale->layer_count) {
        locale->layers = memory.allocate(memory.context, locale->layer_count * sizeof(*locale->layers));
        if (!locale->layers) { locale->layer_count = 0; fail(error, capacity, "Out of memory creating locale layers"); goto error; }
        memset(locale->layers, 0, locale->layer_count * sizeof(*locale->layers));
    }
    i = 0;
    if (style) for (node = style->children; node; node = node->next) {
        if (ce_csl_xml_is(node, "locale")) {
            if (!parse_layer(locale, &locale->layers[i++], node, false, &total_terms, error, capacity)) goto error;
        }
    }
    for (j = 0; j < catalog->root_count; ++j) {
        Layer *layer = &locale->layers[i++];
        if (!parse_layer(locale, layer, catalog->roots[j], true, &total_terms, error, capacity)) goto error;
        if (!strchr(layer->tag, '-')) {
            char dialect[CE_CSL_LOCALE_TAG_CAPACITY];
            if (!find_primary(catalog, layer->tag, dialect) || !same(dialect, layer->tag)) {
                fail(error, capacity, "Standalone locale must identify a dialect (or a documented bare primary)"); goto error;
            }
        }
    }
    for (i = 0; i < locale->layer_count; ++i) for (j = 0; j < i; ++j) {
        if ((i < in_style) == (j < in_style) && same(locale->layers[i].tag, locale->layers[j].tag)) {
            fail(error, capacity, "Duplicate normalized locale layer"); goto error;
        }
    }
    select_layer(locale, 0, in_style, chosen);
    select_layer(locale, 0, in_style, language);
    select_layer(locale, 0, in_style, "");
    select_layer(locale, in_style, locale->layer_count, chosen);
    select_layer(locale, in_style, locale->layer_count, primary);
    select_layer(locale, in_style, locale->layer_count, "en-us");
    for (i = 0; i < locale->selected_count; ++i) if (locale->selected[i]->has_ordinals) {
        locale->ordinals = locale->selected[i]; break;
    }
    locale->legacy_ordinals = locale->ordinals && !has_term_name(locale->ordinals, "ordinal") &&
        has_term_name(locale->ordinals, "ordinal-01") && has_term_name(locale->ordinals, "ordinal-02") &&
        has_term_name(locale->ordinals, "ordinal-03") && has_term_name(locale->ordinals, "ordinal-04");
    return locale;
error:
    ce_csl_locale_free(locale);
    return NULL;
}

void ce_csl_locale_free(CeCslLocale *locale) {
    CeCslLocaleAllocator memory;
    size_t i;
    if (!locale) return;
    memory = locale->allocator;
    for (i = 0; i < locale->layer_count; ++i)
        if (locale->layers[i].terms) memory.deallocate(memory.context, locale->layers[i].terms);
    if (locale->layers) memory.deallocate(memory.context, locale->layers);
    memory.deallocate(memory.context, locale);
}
const char *ce_csl_locale_language(const CeCslLocale *locale) { return locale ? locale->language : ""; }

static CeCslLocaleTerm term_result(const Term *term, bool plural) {
    CeCslLocaleTerm result = {false, "", 0, NULL, CE_CSL_LOCALE_LONG, CE_CSL_LOCALE_NEUTER};
    const CeCslXmlNode *text;
    if (!term) return result;
    text = term->node;
    if (text->children) text = plural ? text->children->next : text->children;
    result.found = true; result.text = text->text ? text->text : "";
    result.length = text->text_length; result.node = term->node;
    result.form = term->form; result.gender_form = term->gender;
    return result;
}
static CeCslLocaleForm fallback_form(CeCslLocaleForm form) {
    if (form == CE_CSL_LOCALE_VERB_SHORT) return CE_CSL_LOCALE_VERB;
    if (form == CE_CSL_LOCALE_SYMBOL) return CE_CSL_LOCALE_SHORT;
    return CE_CSL_LOCALE_LONG;
}
static const Term *lookup(const CeCslLocale *locale, const char *name,
                         CeCslLocaleForm form, CeCslLocaleGender gender) {
    size_t i;
    unsigned number = 0;
    OrdinalKind kind = ordinal_kind(name, &number);
    if (kind == ORDINAL_DEFAULT || kind == ORDINAL_SUFFIX)
        return find_term(locale->ordinals, name, form, gender);
    for (i = 0; i < locale->selected_count; ++i) {
        const Term *term = find_term(locale->selected[i], name, form, gender);
        if (term) return term;
    }
    return NULL;
}
CeCslLocaleTerm ce_csl_locale_term(const CeCslLocale *locale, const char *name,
    CeCslLocaleForm form, bool plural, CeCslLocaleGender gender) {
    if (!locale || !name || form < CE_CSL_LOCALE_LONG || form > CE_CSL_LOCALE_SYMBOL ||
        gender < CE_CSL_LOCALE_NEUTER || gender > CE_CSL_LOCALE_FEMININE) return term_result(NULL, false);
    for (;;) {
        const Term *term = lookup(locale, name, form, gender);
        if (!term && gender != CE_CSL_LOCALE_NEUTER) term = lookup(locale, name, form, CE_CSL_LOCALE_NEUTER);
        if (term) return term_result(term, plural);
        if (form == CE_CSL_LOCALE_LONG) return term_result(NULL, false);
        form = fallback_form(form);
    }
}
CeCslLocaleGender ce_csl_locale_noun_gender(const CeCslLocale *locale, const char *name) {
    const Term *term;
    if (!locale || !name) return CE_CSL_LOCALE_NEUTER;
    term = lookup(locale, name, CE_CSL_LOCALE_LONG, CE_CSL_LOCALE_NEUTER);
    return term ? term->noun_gender : CE_CSL_LOCALE_NEUTER;
}

static const Term *ordinal_for_gender(const CeCslLocale *locale, uint64_t number, CeCslLocaleGender gender) {
    const Term *best = NULL;
    size_t i;
    if (locale->legacy_ordinals) {
        unsigned tail = (unsigned)(number % 100), digit = (unsigned)(number % 10);
        char name[] = "ordinal-04";
        if (!(tail >= 11 && tail <= 13) && digit >= 1 && digit <= 3) name[9] = (char)('0' + digit);
        return find_term(locale->ordinals, name, CE_CSL_LOCALE_LONG, gender);
    }
    for (i = 0; i < locale->ordinals->term_count; ++i) {
        const Term *term = &locale->ordinals->terms[i];
        bool matches;
        if (term->kind != ORDINAL_SUFFIX || term->gender != gender) continue;
        matches = term->match == WHOLE_NUMBER ? number == term->number :
            term->match == LAST_TWO ? number % 100 == term->number : number % 10 == term->number;
        if (matches && (!best || term->match > best->match)) best = term;
    }
    return best;
}
CeCslLocaleOrdinal ce_csl_locale_ordinal(const CeCslLocale *locale, uint64_t number,
    bool long_form, CeCslLocaleGender gender) {
    CeCslLocaleOrdinal result = {0};
    const Term *term;
    result.term = term_result(NULL, false);
    if (!locale || gender < CE_CSL_LOCALE_NEUTER || gender > CE_CSL_LOCALE_FEMININE) return result;
    if (long_form && number >= 1 && number <= 10) {
        char name[24];
        snprintf(name, sizeof(name), "long-ordinal-%02u", (unsigned)number);
        result.term = ce_csl_locale_term(locale, name, CE_CSL_LOCALE_LONG, false, gender);
        if (result.term.found) { result.is_long = true; return result; }
    }
    if (!locale->ordinals) return result;
    term = ordinal_for_gender(locale, number, gender);
    if (!term && gender != CE_CSL_LOCALE_NEUTER) term = ordinal_for_gender(locale, number, CE_CSL_LOCALE_NEUTER);
    if (!term) term = find_term(locale->ordinals, "ordinal", CE_CSL_LOCALE_LONG, gender);
    if (!term && gender != CE_CSL_LOCALE_NEUTER) term = find_term(locale->ordinals, "ordinal", CE_CSL_LOCALE_LONG, CE_CSL_LOCALE_NEUTER);
    result.term = term_result(term, false);
    return result;
}
const CeCslXmlNode *ce_csl_locale_date(const CeCslLocale *locale, CeCslLocaleDate form) {
    size_t i;
    if (!locale || form < CE_CSL_LOCALE_DATE_TEXT || form > CE_CSL_LOCALE_DATE_NUMERIC) return NULL;
    for (i = 0; i < locale->selected_count; ++i)
        if (locale->selected[i]->dates[form]) return locale->selected[i]->dates[form];
    return NULL;
}
bool ce_csl_locale_option(const CeCslLocale *locale, CeCslLocaleOption option, bool *value) {
    size_t i;
    if (value) *value = false;
    if (!locale || !value || option < CE_CSL_LOCALE_PUNCTUATION_IN_QUOTE || option > CE_CSL_LOCALE_DAY_ORDINALS_ONLY_FIRST) return false;
    for (i = 0; i < locale->selected_count; ++i) if (locale->selected[i]->option_found[option]) {
        *value = locale->selected[i]->option_value[option]; return true;
    }
    return false;
}
