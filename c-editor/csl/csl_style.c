#include "csl_style.h"
#include "utf8proc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Vocabulary and element contexts follow the official CSL 1.0.2 specification
 * and schema: https://docs.citationstyles.org/en/stable/specification.html
 * https://github.com/citation-style-language/schema/tree/master/schemas/styles
 * This deliberately does not claim to replace RELAX NG/XSD validation. */
#define NONE ((size_t)-1)
#define RENDER "names date label text number choose group"
#define NAME_OPTIONS "and delimiter-precedes-et-al delimiter-precedes-last et-al-min et-al-use-first et-al-use-last et-al-subsequent-min et-al-subsequent-use-first initialize initialize-with name-as-sort-order sort-separator"
#define INHERITED NAME_OPTIONS " name-form name-delimiter names-delimiter"
#define DATES "accessed available-date event-date issued original-date submitted"
#define NAMES "author chair collection-editor compiler composer container-author contributor curator director editor editor-translator editorial-director executive-producer guest host illustrator interviewer narrator organizer original-author performer producer recipient reviewed-author script-writer series-creator translator"
#define NUMBERS "chapter-number citation-number collection-number edition first-reference-note-number issue locator number number-of-pages number-of-volumes page page-first part-number printing-number section supplement-number version volume"
#define TITLES "collection-title container-title original-title part-title reviewed-title title volume-title title-short container-title-short"
#define STRINGS "abstract annote archive archive_collection archive_location archive-place authority call-number citation-key citation-label dimensions division DOI event event-title event-place genre ISBN ISSN jurisdiction keyword language license medium note original-publisher original-publisher-place PMCID PMID publisher publisher-place references reviewed-genre scale source status URL year-suffix"
#define STANDARD NUMBERS " " TITLES " " STRINGS
#define VARIABLES DATES " " NAMES " " STANDARD
#define TYPES "article article-journal article-magazine article-newspaper bill book broadcast chapter classic collection dataset document entry entry-dictionary entry-encyclopedia event figure graphic hearing interview legal_case legislation manuscript map motion_picture musical_score pamphlet paper-conference patent performance periodical personal_communication post post-weblog regulation report review review-book software song speech standard thesis treaty webpage"
#define LOCATORS "act appendix article-locator book canon chapter column elocation equation figure folio line note opus paragraph rule scene sub-verbo table timestamp title-locator verse issue page part section supplement version volume"

typedef struct Macro {
    const char *name;
    const CeCslXmlNode *node;
    size_t first_edge;
    unsigned char color;
} Macro;
typedef struct Edge { size_t target, next; } Edge;
typedef struct Frame { size_t macro, edge; } Frame;
struct CeCslStyle {
    CeCslStyleAllocator allocator;
    CeCslXml *xml;
    Macro *macros;
    size_t macro_count, locale_count;
    const CeCslXmlNode **locales;
    const CeCslXmlNode *citation, *bibliography;
    const char *id, *title, *default_locale;
};
typedef struct Check {
    CeCslStyle *style;
    char *error;
    size_t capacity;
} Check;
typedef enum Kind {
    STYLE, INFO, PERSON, META_TEXT, META_LANG, META_LINK, META_RIGHTS, META_CATEGORY,
    LOCALE, OPTIONS, TERMS, TERM, TERM_TEXT, MACRO, CITATION, BIBLIOGRAPHY, LAYOUT,
    TEXT, NUMBER, NAMES_NODE, NAME, NAME_PART, ET_AL, SUBSTITUTE, LABEL, DATE,
    LOCALE_DATE, DATE_PART, GROUP, CHOOSE, IF_NODE, ELSE_IF, ELSE_NODE, SORT, KEY
} Kind;
enum { AFFIX = 1u, FONT = 2u, DISPLAY = 4u, CASE = 8u, DELIMITER = 16u, CHARACTER_DATA = 32u };
typedef struct Rule {
    const char *name, *attributes, *children;
    Kind kind;
    unsigned groups;
} Rule;
static const Rule rules[] = {
    {"style", "class version default-locale demote-non-dropping-particle initialize-with-hyphen page-range-format " INHERITED, "info locale macro citation bibliography", STYLE, 0},
    {"info", "", "author contributor id issn eissn issnl link published rights summary title title-short updated category", INFO, 0},
    {"author", "", "name email uri", PERSON, 0}, {"contributor", "", "name email uri", PERSON, 0},
    {"link", "href rel xml:lang", "", META_LINK, CHARACTER_DATA},
    {"rights", "license xml:lang", "", META_RIGHTS, CHARACTER_DATA},
    {"category", "citation-format field", "", META_CATEGORY, 0},
    {"locale", "xml:lang", "style-options date terms", LOCALE, 0},
    {"style-options", "limit-day-ordinals-to-day-1 punctuation-in-quote", "", OPTIONS, 0},
    {"terms", "", "term", TERMS, 0},
    {"term", "name form gender gender-form match", "single multiple", TERM, CHARACTER_DATA},
    {"single", "", "", TERM_TEXT, CHARACTER_DATA}, {"multiple", "", "", TERM_TEXT, CHARACTER_DATA},
    {"macro", "name", RENDER, MACRO, 0},
    {"citation", "cite-group-delimiter collapse year-suffix-delimiter after-collapse-delimiter disambiguate-add-names disambiguate-add-givenname disambiguate-add-year-suffix givenname-disambiguation-rule near-note-distance " INHERITED, "sort layout", CITATION, 0},
    {"bibliography", "hanging-indent entry-spacing line-spacing second-field-align subsequent-author-substitute subsequent-author-substitute-rule " INHERITED, "sort layout", BIBLIOGRAPHY, 0},
    {"layout", "", RENDER, LAYOUT, AFFIX | FONT | DELIMITER},
    {"text", "macro term variable value form plural quotes strip-periods", "", TEXT, AFFIX | FONT | DISPLAY | CASE},
    {"number", "variable form", "", NUMBER, AFFIX | FONT | DISPLAY | CASE},
    {"names", "variable", "name et-al label substitute", NAMES_NODE, AFFIX | FONT | DISPLAY | DELIMITER},
    {"name", "form " NAME_OPTIONS, "name-part", NAME, AFFIX | FONT | DELIMITER},
    {"name-part", "name", "", NAME_PART, AFFIX | FONT | CASE},
    {"et-al", "term", "", ET_AL, FONT},
    {"substitute", "", RENDER, SUBSTITUTE, 0},
    {"label", "variable form plural strip-periods", "", LABEL, AFFIX | FONT | CASE},
    {"date", "variable form date-parts", "date-part", DATE, AFFIX | FONT | DISPLAY | CASE | DELIMITER},
    {"date-part", "name form range-delimiter strip-periods", "", DATE_PART, AFFIX | FONT | CASE},
    {"group", "", RENDER, GROUP, AFFIX | FONT | DISPLAY | DELIMITER},
    {"choose", "", "if else-if else", CHOOSE, 0},
    {"if", "disambiguate is-numeric is-uncertain-date locator position type variable match", RENDER, IF_NODE, 0},
    {"else-if", "disambiguate is-numeric is-uncertain-date locator position type variable match", RENDER, ELSE_IF, 0},
    {"else", "", RENDER, ELSE_NODE, 0},
    {"sort", "", "key", SORT, 0},
    {"key", "variable macro names-min names-use-first names-use-last sort", "", KEY, 0}
};
static const Rule metadata_text = {"metadata", "", "", META_TEXT, CHARACTER_DATA};
static const Rule metadata_language = {"metadata", "xml:lang", "", META_LANG, CHARACTER_DATA};
static const Rule locale_date = {"date", "form", "date-part", LOCALE_DATE, FONT | CASE | DELIMITER};

static const char *local_name(const CeCslXmlNode *node) {
    static const char prefix[] = CE_CSL_NAMESPACE "\x1f";
    return node && !strncmp(node->name, prefix, sizeof prefix - 1) ? node->name + sizeof prefix - 1 : NULL;
}
static const char *attr(const CeCslXmlNode *node, const char *name) { return ce_csl_xml_attribute(node, name); }
static bool space(unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }
static bool whitespace(const char *text) {
    if (text) for (; *text; ++text) if (!space((unsigned char)*text)) return false;
    return true;
}
static bool contains_n(const char *list, const char *value, size_t length, char separator) {
    for (const char *at = list; *at;) {
        const char *end = strchr(at, separator);
        size_t size = end ? (size_t)(end - at) : strlen(at);
        if (size == length && !memcmp(at, value, size)) return true;
        if (!end) break;
        at = end + 1;
    }
    return false;
}
static bool word(const char *list, const char *value) { return value && contains_n(list, value, strlen(value), ' '); }
static bool one_of(const char *list, const char *value) { return value && contains_n(list, value, strlen(value), '|'); }
static bool word_list(const char *allowed, const char *value) {
    bool any = false;
    for (const char *at = value; *at;) {
        while (space((unsigned char)*at)) ++at;
        if (!*at) break;
        const char *end = at;
        while (*end && !space((unsigned char)*end)) ++end;
        if (!contains_n(allowed, at, (size_t)(end - at), ' ')) return false;
        any = true; at = end;
    }
    return any;
}
static bool problem(Check *check, const CeCslXmlNode *node, const char *reason, const char *detail) {
    if (check->error && check->capacity) {
        const char *name = local_name(node);
        snprintf(check->error, check->capacity, "CSL style line %zu <%.*s>: %s%.*s",
                 node ? node->line : 0, 48, name ? name : "unknown namespace", reason, 100, detail ? detail : "");
    }
    return false;
}
static bool language(const char *value) {
    size_t segment = 0;
    bool first = true;
    for (const unsigned char *at = (const unsigned char *)value;; ++at) {
        if (!*at || *at == '-') {
            if (!segment || segment > 8) return false;
            if (!*at) return true;
            segment = 0; first = false;
        } else {
            bool letter = (*at >= 'a' && *at <= 'z') || (*at >= 'A' && *at <= 'Z');
            if (!letter && (first || *at < '0' || *at > '9')) return false;
            ++segment;
        }
    }
}
static bool name_token(const char *value) {
    if (!value || !*value) return false;
    size_t remaining = strlen(value);
    const utf8proc_uint8_t *at = (const utf8proc_uint8_t *)value;
    while (remaining) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t n = utf8proc_iterate(at, (utf8proc_ssize_t)remaining, &cp);
        if (n <= 0) return false;
        bool valid = (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
            (cp >= '0' && cp <= '9') || cp == ':' || cp == '_' || cp == '-' || cp == '.' || cp == 0xb7 ||
            (cp >= 0xc0 && cp <= 0xd6) || (cp >= 0xd8 && cp <= 0xf6) || (cp >= 0xf8 && cp <= 0x37d) ||
            (cp >= 0x37f && cp <= 0x1fff) || (cp >= 0x200c && cp <= 0x200d) ||
            (cp >= 0x203f && cp <= 0x2040) || (cp >= 0x2070 && cp <= 0x218f) ||
            (cp >= 0x2c00 && cp <= 0x2fef) || (cp >= 0x3001 && cp <= 0xd7ff) ||
            (cp >= 0xf900 && cp <= 0xfdcf) || (cp >= 0xfdf0 && cp <= 0xfffd) ||
            (cp >= 0x10000 && cp <= 0xeffff);
        if (!valid) return false;
        remaining -= (size_t)n; at += n;
    }
    return true;
}
static bool integer(const char *value, bool positive) {
    if (*value == '+') ++value;
    if (!*value) return false;
    uint32_t result = 0;
    for (; *value; ++value) {
        if (*value < '0' || *value > '9' || result > (INT32_MAX - (unsigned)(*value - '0')) / 10u) return false;
        result = result * 10u + (unsigned)(*value - '0');
    }
    return !positive || result > 0;
}
static bool term_name(const char *value) {
    if (word(NAMES " " TYPES " " LOCATORS " chapter-number citation-number collection-number edition first-reference-note-number number number-of-pages number-of-volumes page-first printing", value)) return true;
    if (one_of("editortranslator|accessed|ad|advance-online-publication|album|and|and others|anonymous|at|audio-recording|available at|bc|bce|by|ce|circa|cited|et-al|film|forthcoming|from|henceforth|ibid|in|in press|internet|interview|letter|loc-cit|no date|no-place|no-publisher|on|online|op-cit|original-work-published|personal-communication|podcast|podcast-episode|preprint|presented at|radio-broadcast|radio-series|radio-series-episode|reference|retrieved|review-of|scale|special-issue|special-section|television-broadcast|television-series|television-series-episode|video|working-paper|open-quote|close-quote|open-inner-quote|close-inner-quote|page-range-delimiter|colon|comma|semicolon|printing", value)) return true;
    const char *digits = NULL;
    unsigned maximum = 0;
    if (!strncmp(value, "month-", 6)) { digits = value + 6; maximum = 12; }
    else if (!strncmp(value, "season-", 7)) { digits = value + 7; maximum = 4; }
    else if (!strncmp(value, "long-ordinal-", 13)) { digits = value + 13; maximum = 10; }
    else if (!strcmp(value, "ordinal")) return true;
    else if (!strncmp(value, "ordinal-", 8)) { digits = value + 8; maximum = 99; }
    if (!digits || strlen(digits) != 2 || digits[0] < '0' || digits[0] > '9' || digits[1] < '0' || digits[1] > '9') return false;
    unsigned number = (unsigned)((digits[0] - '0') * 10 + digits[1] - '0');
    return number <= maximum && (number || maximum == 99);
}
static const Rule *rule_for(const CeCslXmlNode *node, Kind parent) {
    const char *name = local_name(node);
    if (!name) return NULL;
    if (parent == INFO && word("id issn eissn issnl published updated", name)) return &metadata_text;
    if (parent == INFO && word("title title-short summary", name)) return &metadata_language;
    if (parent == PERSON && word("name email uri", name)) return &metadata_text;
    if (parent == LOCALE && !strcmp(name, "date")) return &locale_date;
    for (size_t i = 0; i < sizeof rules / sizeof *rules; ++i)
        if (!strcmp(name, rules[i].name)) return &rules[i];
    return NULL;
}
static bool allowed_attribute(const Rule *rule, const char *name) {
    return word(rule->attributes, name) ||
        ((rule->groups & AFFIX) && word("prefix suffix", name)) ||
        ((rule->groups & FONT) && word("font-style font-variant font-weight text-decoration vertical-align", name)) ||
        ((rule->groups & DISPLAY) && !strcmp(name, "display")) ||
        ((rule->groups & CASE) && !strcmp(name, "text-case")) ||
        ((rule->groups & DELIMITER) && !strcmp(name, "delimiter"));
}
static bool attribute_value(const CeCslXmlNode *node, Kind kind, const char *name, const char *value) {
    if (!strcmp(name, "version")) return !strcmp(value, "1.0");
    if (!strcmp(name, "class")) return one_of("in-text|note", value);
    if (word("xml:lang default-locale", name)) return language(value);
    if (!strcmp(name, "font-style")) return one_of("normal|italic|oblique", value);
    if (!strcmp(name, "font-variant")) return one_of("normal|small-caps", value);
    if (!strcmp(name, "font-weight")) return one_of("normal|bold|light", value);
    if (!strcmp(name, "text-decoration")) return one_of("none|underline", value);
    if (!strcmp(name, "vertical-align")) return one_of("baseline|sup|sub", value);
    if (!strcmp(name, "display")) return one_of("block|left-margin|right-inline|indent", value);
    if (!strcmp(name, "text-case")) return one_of("lowercase|uppercase|capitalize-first|capitalize-all|title|sentence", value);
    if (!strcmp(name, "and")) return one_of("text|symbol", value);
    if (word("delimiter-precedes-et-al delimiter-precedes-last", name)) return one_of("contextual|always|never|after-inverted-name", value);
    if (!strcmp(name, "name-as-sort-order")) return one_of("first|all", value);
    if (!strcmp(name, "name-form")) return one_of("long|short|count", value);
    if (!strcmp(name, "demote-non-dropping-particle")) return one_of("never|sort-only|display-and-sort", value);
    if (!strcmp(name, "page-range-format")) return one_of("expanded|minimal|minimal-two|chicago|chicago-15|chicago-16", value);
    if (!strcmp(name, "collapse")) return one_of("citation-number|year|year-suffix|year-suffix-ranged", value);
    if (!strcmp(name, "givenname-disambiguation-rule")) return one_of("all-names|all-names-with-initials|primary-name|primary-name-with-initials|by-cite", value);
    if (!strcmp(name, "second-field-align")) return one_of("flush|margin", value);
    if (!strcmp(name, "subsequent-author-substitute-rule")) return one_of("complete-all|complete-each|partial-each|partial-first", value);
    if (!strcmp(name, "sort")) return one_of("ascending|descending", value);
    if (!strcmp(name, "date-parts")) return one_of("year-month-day|year-month|year", value);
    if (!strcmp(name, "gender") || !strcmp(name, "gender-form")) return one_of("masculine|feminine", value);
    if (!strcmp(name, "match")) return one_of(kind == TERM ? "last-digit|last-two-digits|whole-number" : "all|any|none", value);
    if (!strcmp(name, "plural")) return one_of(kind == TEXT ? "true|false|1|0" : "always|never|contextual", value);
    if (!strcmp(name, "disambiguate")) return !strcmp(value, "true");
    if (word("initialize initialize-with-hyphen et-al-use-last names-use-last disambiguate-add-names disambiguate-add-givenname disambiguate-add-year-suffix hanging-indent quotes strip-periods limit-day-ordinals-to-day-1 punctuation-in-quote", name)) return one_of("true|false|1|0", value);
    if (word("et-al-min et-al-use-first et-al-subsequent-min et-al-subsequent-use-first near-note-distance entry-spacing names-min names-use-first line-spacing", name)) return integer(value, !strcmp(name, "line-spacing"));
    if (!strcmp(name, "macro")) return name_token(value);
    if (!strcmp(name, "name")) {
        if (kind == MACRO) return name_token(value);
        if (kind == NAME_PART) return one_of("family|given", value);
        if (kind == DATE_PART) return one_of("day|month|year", value);
        if (kind == TERM) return term_name(value);
    }
    if (!strcmp(name, "term")) return kind == ET_AL ? one_of("et-al|and others", value) : term_name(value);
    if (!strcmp(name, "form")) {
        switch (kind) {
            case NAME: return one_of("long|short|count", value);
            case NUMBER: return one_of("numeric|ordinal|long-ordinal|roman", value);
            case DATE: case LOCALE_DATE: return one_of("text|numeric", value);
            case DATE_PART:
                if (!attr(node, "name")) return false;
                if (!strcmp(attr(node, "name"), "day")) return one_of("numeric|numeric-leading-zeros|ordinal", value);
                if (!strcmp(attr(node, "name"), "month")) return one_of("long|short|numeric|numeric-leading-zeros", value);
                return one_of("long|short", value);
            case TEXT: return one_of(attr(node, "term") ? "long|short|verb|verb-short|symbol" : "long|short", value);
            case LABEL: return one_of(ce_csl_xml_is(node->parent, "names") ? "long|short|verb|verb-short|symbol" : "long|short|symbol", value);
            case TERM: return one_of("long|short|verb|verb-short|symbol", value);
            default: return false;
        }
    }
    if (!strcmp(name, "variable")) {
        switch (kind) {
            case TEXT: return word(STANDARD, value);
            case NUMBER: case LABEL: return word(NUMBERS, value);
            case DATE: return word(DATES, value);
            case NAMES_NODE: return word_list(NAMES, value);
            case KEY: return word(VARIABLES, value);
            case IF_NODE: case ELSE_IF: return word_list(VARIABLES, value);
            default: return false;
        }
    }
    if (!strcmp(name, "type")) return word_list(TYPES, value);
    if (!strcmp(name, "position")) return word_list("first subsequent ibid ibid-with-locator near-note", value);
    if (!strcmp(name, "locator")) return word_list(LOCATORS, value);
    if (!strcmp(name, "is-numeric")) return word_list(VARIABLES, value);
    if (!strcmp(name, "is-uncertain-date")) return word_list(DATES, value);
    if (!strcmp(name, "rel")) return one_of("self|template|documentation", value);
    if (!strcmp(name, "citation-format")) return one_of("author|author-date|label|note|numeric", value);
    if (!strcmp(name, "field")) return word("anthropology astronomy biology botany chemistry communications engineering generic-base geography geology history humanities law linguistics literature math medicine philosophy physics political_science psychology science social_science sociology theology zoology", value);
    return true; /* Free-form affixes, delimiters, metadata and verbatim value. */
}
static size_t child_count(const CeCslXmlNode *node, const char *name) {
    size_t count = 0;
    for (const CeCslXmlNode *child = node->children; child; child = child->next)
        if (!name || ce_csl_xml_is(child, name)) ++count;
    return count;
}
static bool required(Check *check, const CeCslXmlNode *node, const char *name) {
    return attr(node, name) || problem(check, node, "Missing required attribute: ", name);
}
static bool cardinality(Check *check, const CeCslXmlNode *node, const char *name, size_t low, size_t high) {
    size_t count = child_count(node, name);
    return (count >= low && count <= high) || problem(check, node, "Invalid child count: ", name ? name : "rendering element");
}
static bool validate_structure(Check *check, const CeCslXmlNode *node, const Rule *rule) {
    switch (rule->kind) {
        case STYLE:
            if (!required(check, node, "version") || !required(check, node, "class") ||
                !cardinality(check, node, "info", 1, 1) || !cardinality(check, node, "citation", 1, 1) ||
                !cardinality(check, node, "bibliography", 0, 1)) return false;
            if (!ce_csl_xml_is(node->children, "info")) return problem(check, node, "info must be the first child", NULL);
            break;
        case INFO:
            if (!cardinality(check, node, "id", 1, 1) || !cardinality(check, node, "title", 1, 1) ||
                !cardinality(check, node, "updated", 1, 1)) return false;
            for (const CeCslXmlNode *child = node->children; child; child = child->next)
                if (word("eissn issnl published rights summary title-short", local_name(child)) &&
                    !cardinality(check, node, local_name(child), 0, 1)) return false;
            break;
        case PERSON:
            if (!cardinality(check, node, "name", 1, 1) || !cardinality(check, node, "email", 0, 1) ||
                !cardinality(check, node, "uri", 0, 1)) return false;
            break;
        case META_LINK: if (!required(check, node, "href") || !required(check, node, "rel")) return false; break;
        case META_CATEGORY:
            if (!!attr(node, "field") + !!attr(node, "citation-format") != 1) return problem(check, node, "category needs exactly one field or citation-format", NULL);
            break;
        case MACRO:
            if (!required(check, node, "name")) return false;
            return cardinality(check, node, NULL, 1, CE_CSL_XML_MAX_NODES);
        case CITATION: case BIBLIOGRAPHY:
            if (!cardinality(check, node, "sort", 0, 1) || !cardinality(check, node, "layout", 1, 1)) return false;
            if (child_count(node, "sort") && !ce_csl_xml_is(node->children, "sort")) return problem(check, node, "sort must precede layout", NULL);
            break;
        case LAYOUT:
            if (ce_csl_xml_is(node->parent, "bibliography") && attr(node, "delimiter")) return problem(check, node, "bibliography layout does not accept delimiter", NULL);
            return cardinality(check, node, NULL, 1, CE_CSL_XML_MAX_NODES);
        case TEXT: {
            unsigned selectors = !!attr(node, "variable") + !!attr(node, "macro") + !!attr(node, "term") + !!attr(node, "value");
            if (selectors != 1) return problem(check, node, "text needs exactly one variable, macro, term or value", NULL);
            if (attr(node, "plural") && !attr(node, "term")) return problem(check, node, "plural is only valid for a term", NULL);
            if (attr(node, "form") && !attr(node, "term") && !attr(node, "variable")) return problem(check, node, "form requires a term or variable", NULL);
            break;
        }
        case KEY:
            if (!!attr(node, "variable") + !!attr(node, "macro") != 1) return problem(check, node, "key needs exactly one variable or macro", NULL);
            break;
        case NUMBER: if (!required(check, node, "variable")) return false; break;
        case NAMES_NODE: {
            if (!required(check, node, "variable")) return false;
            if (!cardinality(check, node, "name", 0, 1) || !cardinality(check, node, "et-al", 0, 1) ||
                !cardinality(check, node, "label", 0, 1) || !cardinality(check, node, "substitute", 0, 1)) return false;
            bool et_al = false;
            for (const CeCslXmlNode *child = node->children; child; child = child->next) {
                if (ce_csl_xml_is(child, "substitute") && child->next) return problem(check, node, "substitute must be last", NULL);
                if (ce_csl_xml_is(child, "et-al")) et_al = true;
                if (ce_csl_xml_is(child, "name") && et_al) return problem(check, node, "name must precede et-al", NULL);
            }
            break;
        }
        case NAME:
            for (const CeCslXmlNode *child = node->children; child; child = child->next)
                for (const CeCslXmlNode *prior = node->children; prior != child; prior = prior->next)
                    if (attr(child, "name") && attr(prior, "name") && !strcmp(attr(child, "name"), attr(prior, "name"))) return problem(check, child, "Duplicate name-part", NULL);
            break;
        case NAME_PART: case TERM: if (!required(check, node, "name")) return false; break;
        case LABEL:
            if (ce_csl_xml_is(node->parent, "names")) {
                if (attr(node, "variable")) return problem(check, node, "names label inherits its variable", NULL);
            } else if (!required(check, node, "variable")) return false;
            break;
        case DATE: case LOCALE_DATE:
            if (rule->kind == DATE && !required(check, node, "variable")) return false;
            if (rule->kind == LOCALE_DATE && !required(check, node, "form")) return false;
            if (!attr(node, "form") && !cardinality(check, node, NULL, 1, 3)) return false;
            if (rule->kind == LOCALE_DATE && !cardinality(check, node, NULL, 1, 3)) return false;
            if (rule->kind == DATE && attr(node, "form") && attr(node, "delimiter")) return problem(check, node, "Localized date cannot override delimiter", NULL);
            if (!attr(node, "form") && attr(node, "date-parts")) return problem(check, node, "date-parts requires localized form", NULL);
            for (const CeCslXmlNode *child = node->children; child; child = child->next)
                for (const CeCslXmlNode *prior = node->children; prior != child; prior = prior->next)
                    if (attr(child, "name") && attr(prior, "name") && !strcmp(attr(child, "name"), attr(prior, "name"))) return problem(check, child, "Duplicate date-part", NULL);
            break;
        case DATE_PART:
            if (!required(check, node, "name")) return false;
            if (attr(node, "strip-periods") && strcmp(attr(node, "name"), "month")) return problem(check, node, "strip-periods requires the month part", NULL);
            if (node->parent && attr(node->parent, "variable") && attr(node->parent, "form") &&
                (attr(node, "prefix") || attr(node, "suffix"))) return problem(check, node, "Localized date-part cannot override affixes", NULL);
            break;
        case CHOOSE: {
            if (!ce_csl_xml_is(node->children, "if")) return problem(check, node, "choose must start with if", NULL);
            bool saw_else = false;
            for (const CeCslXmlNode *child = node->children->next; child; child = child->next) {
                if (saw_else || ce_csl_xml_is(child, "if")) return problem(check, child, "Invalid choose branch order", NULL);
                saw_else = ce_csl_xml_is(child, "else");
            }
            break;
        }
        case IF_NODE: case ELSE_IF:
            if (!attr(node, "disambiguate") && !attr(node, "is-numeric") && !attr(node, "is-uncertain-date") &&
                !attr(node, "locator") && !attr(node, "position") && !attr(node, "type") && !attr(node, "variable")) return problem(check, node, "Conditional branch has no condition", NULL);
            break;
        case GROUP: case ELSE_NODE: case SUBSTITUTE: case SORT: case TERMS:
            return cardinality(check, node, NULL, 1, CE_CSL_XML_MAX_NODES);
        case LOCALE:
            if (!cardinality(check, node, "style-options", 0, 1) || !cardinality(check, node, "terms", 0, 1) ||
                !cardinality(check, node, "date", 0, 2)) return false;
            for (const CeCslXmlNode *child = node->children; child; child = child->next)
                if (ce_csl_xml_is(child, "date")) for (const CeCslXmlNode *prior = node->children; prior != child; prior = prior->next)
                    if (ce_csl_xml_is(prior, "date") && attr(child, "form") && attr(prior, "form") && !strcmp(attr(child, "form"), attr(prior, "form"))) return problem(check, child, "Duplicate localized date form", NULL);
            break;
        default: break;
    }
    if (rule->kind == TERM && node->children) {
        if (!whitespace(node->text) || child_count(node, NULL) != 2 ||
            !ce_csl_xml_is(node->children, "single") || !ce_csl_xml_is(node->children->next, "multiple"))
            return problem(check, node, "term requires text or ordered single/multiple children", NULL);
    }
    return true;
}
static bool validate_node(Check *check, const CeCslXmlNode *node, const Rule *rule) {
    if (!(rule->groups & CHARACTER_DATA) && !whitespace(node->text)) return problem(check, node, "Unexpected literal character data", NULL);
    for (size_t i = 0; i < node->attribute_count; ++i) {
        const char *name = node->attributes[i].name;
        if (!strcmp(name, "http://www.w3.org/XML/1998/namespace\x1flang")) name = "xml:lang";
        if (!allowed_attribute(rule, name)) return problem(check, node, "Unknown or misplaced attribute: ", name);
        if (!attribute_value(node, rule->kind, name, node->attributes[i].value)) return problem(check, node, "Invalid attribute value: ", name);
    }
    /* Validate descendants before cardinality checks that inspect their attrs. */
    for (const CeCslXmlNode *child = node->children; child; child = child->next) {
        const char *name = local_name(child);
        if (!name) return problem(check, child, "Unknown element namespace", NULL);
        if (!word(rule->children, name)) return problem(check, child, "Unknown or misplaced element: ", name);
        const Rule *child_rule = rule_for(child, rule->kind);
        if (!child_rule) return problem(check, child, "Unsupported element: ", name);
        if (!validate_node(check, child, child_rule)) return false;
    }
    return validate_structure(check, node, rule);
}

static void *default_allocate(void *context, size_t size) { (void)context; return malloc(size); }
static void default_deallocate(void *context, void *memory) { (void)context; free(memory); }
static void *allocate(CeCslStyle *style, size_t size) { return style->allocator.allocate(style->allocator.context, size); }
static void release(CeCslStyle *style, void *memory) { if (memory) style->allocator.deallocate(style->allocator.context, memory); }
void ce_csl_style_free(CeCslStyle *style) {
    if (!style) return;
    ce_csl_xml_free(style->xml);
    release(style, style->macros);
    release(style, style->locales);
    CeCslStyleAllocator memory = style->allocator;
    memory.deallocate(memory.context, style);
}
static int compare_macros(const void *a, const void *b) {
    return strcmp(((const Macro *)a)->name, ((const Macro *)b)->name);
}
static size_t macro_index(const CeCslStyle *style, const char *name) {
    size_t low = 0, high = style->macro_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = strcmp(name, style->macros[middle].name);
        if (!order) return middle;
        if (order < 0) high = middle; else low = middle + 1;
    }
    return NONE;
}
static bool build_indexes(Check *check) {
    CeCslStyle *style = check->style;
    for (const CeCslXmlNode *node = style->xml->root->children; node; node = node->next) {
        if (ce_csl_xml_is(node, "macro")) ++style->macro_count;
        if (ce_csl_xml_is(node, "locale")) ++style->locale_count;
        if (ce_csl_xml_is(node, "citation")) style->citation = node;
        if (ce_csl_xml_is(node, "bibliography")) style->bibliography = node;
    }
    if (style->macro_count > CE_CSL_STYLE_MAX_MACROS || style->locale_count > CE_CSL_STYLE_MAX_INLINE_LOCALES)
        return problem(check, style->xml->root, "Macro or inline locale limit exceeded", NULL);
    if (style->macro_count) {
        style->macros = allocate(style, style->macro_count * sizeof *style->macros);
        if (!style->macros) return problem(check, NULL, "Out of memory indexing CSL macros", NULL);
    }
    if (style->locale_count) {
        style->locales = allocate(style, style->locale_count * sizeof *style->locales);
        if (!style->locales) return problem(check, NULL, "Out of memory indexing inline locales", NULL);
    }
    size_t macro = 0, locale = 0;
    for (const CeCslXmlNode *node = style->xml->root->children; node; node = node->next) {
        if (ce_csl_xml_is(node, "macro")) {
            Macro entry = {attr(node, "name"), node, NONE, 0};
            style->macros[macro++] = entry;
        }
        if (ce_csl_xml_is(node, "locale")) style->locales[locale++] = node;
    }
    if (style->macro_count) qsort(style->macros, style->macro_count, sizeof *style->macros, compare_macros);
    for (size_t i = 1; i < style->macro_count; ++i)
        if (!strcmp(style->macros[i - 1].name, style->macros[i].name)) return problem(check, style->macros[i].node, "Duplicate macro: ", style->macros[i].name);
    const CeCslXmlNode *info = ce_csl_xml_child(style->xml->root, "info");
    const CeCslXmlNode *id = ce_csl_xml_child(info, "id"), *title = ce_csl_xml_child(info, "title");
    style->id = id->text ? id->text : "";
    style->title = title->text ? title->text : "";
    style->default_locale = attr(style->xml->root, "default-locale");
    return true;
}
static bool validate_references(Check *check) {
    CeCslStyle *style = check->style;
    size_t count = 0;
    for (const CeCslXmlNode *node = style->xml->allocated; node; node = node->allocated_next)
        if (attr(node, "macro")) ++count;
    if (count > CE_CSL_STYLE_MAX_REFERENCES) return problem(check, style->xml->root, "Macro reference limit exceeded", NULL);
    Edge *edges = count ? allocate(style, count * sizeof *edges) : NULL;
    Frame *stack = style->macro_count ? allocate(style, style->macro_count * sizeof *stack) : NULL;
    if ((count && !edges) || (style->macro_count && !stack)) {
        release(style, edges); release(style, stack);
        return problem(check, NULL, "Out of memory checking macro references", NULL);
    }
    size_t used = 0;
    bool valid = false;
    for (const CeCslXmlNode *node = style->xml->allocated; node; node = node->allocated_next) {
        const char *reference = attr(node, "macro");
        if (!reference) continue;
        size_t target = macro_index(style, reference);
        if (target == NONE) { problem(check, node, "Undefined macro: ", reference); goto done; }
        const CeCslXmlNode *owner = node->parent;
        while (owner && !ce_csl_xml_is(owner, "macro")) owner = owner->parent;
        if (owner) {
            size_t source = macro_index(style, attr(owner, "name"));
            Edge edge = {target, style->macros[source].first_edge};
            edges[used] = edge;
            style->macros[source].first_edge = used++;
        }
    }
    for (size_t i = 0; i < style->macro_count; ++i) {
        if (style->macros[i].color) continue;
        size_t depth = 1;
        stack[0] = (Frame){i, style->macros[i].first_edge};
        style->macros[i].color = 1;
        while (depth) {
            Frame *frame = &stack[depth - 1];
            if (frame->edge == NONE) { style->macros[frame->macro].color = 2; --depth; continue; }
            Edge edge = edges[frame->edge];
            frame->edge = edge.next;
            Macro *target = &style->macros[edge.target];
            if (target->color == 1) { problem(check, target->node, "Cyclic macro reference: ", target->name); goto done; }
            if (!target->color) {
                target->color = 1;
                stack[depth++] = (Frame){edge.target, target->first_edge};
            }
        }
    }
    valid = true;
done:
    release(style, edges); release(style, stack);
    return valid;
}
bool ce_csl_style_load_with_allocator(CeCslStyle **slot, const char *xml, size_t size,
                                      const CeCslStyleAllocator *allocator, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    Check check = {NULL, error, capacity};
    if (!slot) return problem(&check, NULL, "Invalid style destination", NULL);
    CeCslStyleAllocator memory = {NULL, default_allocate, default_deallocate};
    if (allocator) memory = *allocator;
    if (!memory.allocate || !memory.deallocate) return problem(&check, NULL, "Both style allocator callbacks are required", NULL);
    CeCslXml *document = ce_csl_xml_parse(xml, size, error, capacity);
    if (!document) return false;
    CeCslStyle *style = memory.allocate(memory.context, sizeof *style);
    if (!style) { ce_csl_xml_free(document); return problem(&check, NULL, "Out of memory creating style", NULL); }
    memset(style, 0, sizeof *style);
    style->allocator = memory; style->xml = document; check.style = style;
    if (!ce_csl_xml_is(document->root, "style")) problem(&check, document->root, "Expected CSL namespace style root", NULL);
    else if (validate_node(&check, document->root, &rules[0]) && build_indexes(&check) && validate_references(&check)) {
        CeCslStyle *old = *slot;
        *slot = style;
        ce_csl_style_free(old);
        return true;
    }
    ce_csl_style_free(style);
    return false;
}
bool ce_csl_style_load(CeCslStyle **style, const char *xml, size_t size, char *error, size_t capacity) {
    return ce_csl_style_load_with_allocator(style, xml, size, NULL, error, capacity);
}
const CeCslXmlNode *ce_csl_style_root(const CeCslStyle *style) { return style ? style->xml->root : NULL; }
const CeCslXmlNode *ce_csl_style_citation(const CeCslStyle *style) { return style ? style->citation : NULL; }
const CeCslXmlNode *ce_csl_style_bibliography(const CeCslStyle *style) { return style ? style->bibliography : NULL; }
const CeCslXmlNode *ce_csl_style_macro(const CeCslStyle *style, const char *name) {
    size_t index = style && name ? macro_index(style, name) : NONE;
    return index == NONE ? NULL : style->macros[index].node;
}
size_t ce_csl_style_macro_count(const CeCslStyle *style) { return style ? style->macro_count : 0; }
const char *ce_csl_style_id(const CeCslStyle *style) { return style ? style->id : NULL; }
const char *ce_csl_style_title(const CeCslStyle *style) { return style ? style->title : NULL; }
const char *ce_csl_style_default_locale(const CeCslStyle *style) { return style ? style->default_locale : NULL; }
size_t ce_csl_style_inline_locale_count(const CeCslStyle *style) { return style ? style->locale_count : 0; }
const CeCslXmlNode *ce_csl_style_inline_locale(const CeCslStyle *style, size_t index) {
    return style && index < style->locale_count ? style->locales[index] : NULL;
}
