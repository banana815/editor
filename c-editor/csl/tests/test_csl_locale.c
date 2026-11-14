#include "csl_locale.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS CE_CSL_NAMESPACE
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define TERM(n, v) "<term name='" n "'>" v "</term>"
#define EN_ORD TERM("ordinal", "th") TERM("ordinal-01", "st") TERM("ordinal-02", "nd") TERM("ordinal-03", "rd") TERM("ordinal-11", "th") TERM("ordinal-12", "th") TERM("ordinal-13", "th")

static CeCslXml *parse(const char *text) {
    char error[256];
    CeCslXml *xml = ce_csl_xml_parse(text, strlen(text), error, sizeof(error));
    if (!xml) fprintf(stderr, "XML fixture failed: %s\n", error);
    assert(xml);
    return xml;
}
static CeCslXml *style(const char *content) {
    const char *prefix = "<style xmlns='" NS "' version='1.0'>";
    size_t n = strlen(prefix) + strlen(content) + 9;
    char *xml = malloc(n);
    CeCslXml *result;
    assert(xml);
    snprintf(xml, n, "%s%s</style>", prefix, content);
    result = parse(xml); free(xml);
    return result;
}
static CeCslXml *external(const char *tag, const char *terms, const char *options, const char *dates) {
    size_t n = strlen(tag) + strlen(terms) + (options ? strlen(options) : 0) + (dates ? strlen(dates) : 0) + 512;
    char *xml = malloc(n);
    CeCslXml *result;
    assert(xml);
    snprintf(xml, n, "<locale xmlns='%s' version='1.0' xml:lang='%s'><style-options %s/>%s<terms>%s</terms></locale>",
        NS, tag, options ? options : "", dates ? dates : "<date form='text'><date-part name='year'/></date>", terms);
    result = parse(xml); free(xml);
    return result;
}
static CeCslLocale *create(CeCslXml *stylesheet, CeCslXml **files, size_t count, const char *language) {
    const CeCslXmlNode *roots[16];
    CeCslLocaleCatalog catalog = {roots, count, NULL, 0};
    CeCslLocale *locale;
    char error[256];
    size_t i;
    assert(count <= COUNT(roots));
    for (i = 0; i < count; ++i) roots[i] = files[i]->root;
    locale = ce_csl_locale_new(stylesheet ? stylesheet->root : NULL, &catalog, language, NULL, error, sizeof(error));
    if (!locale) fprintf(stderr, "Locale fixture failed: %s\n", error);
    assert(locale && !*error);
    return locale;
}
static void term_is(const CeCslLocale *locale, const char *name, CeCslLocaleForm form,
                    bool plural, CeCslLocaleGender gender, const char *value) {
    CeCslLocaleTerm term = ce_csl_locale_term(locale, name, form, plural, gender);
    if (!term.found || strcmp(term.text, value))
        fprintf(stderr, "Term %s form %d: found=%d actual='%s' expected='%s'\n", name, (int)form, term.found, term.text, value);
    assert(term.found && term.node && term.length == strlen(value) && !strcmp(term.text, value));
}
static void ordinal_is(const CeCslLocale *locale, uint64_t number, bool long_form,
                       CeCslLocaleGender gender, bool found, bool is_long, const char *text) {
    CeCslLocaleOrdinal ordinal = ce_csl_locale_ordinal(locale, number, long_form, gender);
    if (ordinal.term.found != found || ordinal.is_long != is_long || strcmp(ordinal.term.text, text))
        fprintf(stderr, "Ordinal mismatch: found=%d long=%d text=%s expected=%s\n", ordinal.term.found, ordinal.is_long, ordinal.term.text, text);
    assert(ordinal.term.found == found && ordinal.is_long == is_long && !strcmp(ordinal.term.text, text));
}

static void test_fallback(void) {
    CeCslXml *files[] = {
        external("de-AT", TERM("p0","AT") TERM("p1","AT") TERM("p2","AT") TERM("p3","AT")
            TERM("editor","AT_LONG") TERM("empty","BASE") "<term name='blank' form='short'>BASE_SHORT</term>",
            "punctuation-in-quote='true'", "<date form='text'><date-part name='day'/></date>"),
        external("de-DE", TERM("p0","DE") TERM("p1","DE") TERM("p2","DE") TERM("p3","DE") TERM("p4","DE")
            "<term name='editor' form='short'>DE_SHORT</term>" "<term name='blank' form='short'>DE_BLANK</term>", NULL, NULL),
        external("en-US", TERM("p0","EN") TERM("p1","EN") TERM("p2","EN") TERM("p3","EN") TERM("p4","EN") TERM("p5","EN")
            "<term name='editor' form='short'>EN_SHORT</term><term name='writer' form='verb-short'>EN_VSHORT</term>"
            "<term name='section' form='symbol'><single>§</single><multiple>§§</multiple></term>",
            "limit-day-ordinals-to-day-1='true'", "<date form='numeric'><date-part name='month'/></date>")
    };
    CeCslXml *s = style(
        "<locale xml:lang='DE-at'><terms>" TERM("p0","EXACT") TERM("editor","INLINE_LONG")
        TERM("empty","") "<term name='blank' form='short'/>" TERM("space","  中 &amp; 文  ")
        "<term name='writer' form='verb'>INLINE_VERB</term>"
        "<term name='other' form='verb'>OTHER_VERB</term>"
        "<term name='page'><single>page</single><multiple/></term>"
        "</terms><style-options punctuation-in-quote='false'/></locale>"
        "<locale xml:lang='de'><terms>" TERM("p0","LANG") TERM("p1","LANG") "</terms></locale>"
        "<locale><terms>" TERM("p0","ANY") TERM("p1","ANY") TERM("p2","ANY") "</terms></locale>");
    CeCslLocale *locale = create(s, files, COUNT(files), "de-AT");
    const char *expected[] = {"EXACT","LANG","ANY","AT","DE","EN"};
    size_t i;
    bool value;
    assert(!strcmp(ce_csl_locale_language(locale), "de-at"));
    for (i = 0; i < COUNT(expected); ++i) {
        char name[3] = {'p', (char)('0' + i), 0};
        term_is(locale, name, CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER, expected[i]);
    }
    term_is(locale, "editor", CE_CSL_LOCALE_SHORT, false, CE_CSL_LOCALE_NEUTER, "DE_SHORT");
    term_is(locale, "editor", CE_CSL_LOCALE_SYMBOL, false, CE_CSL_LOCALE_NEUTER, "DE_SHORT");
    term_is(locale, "editor", CE_CSL_LOCALE_VERB, false, CE_CSL_LOCALE_NEUTER, "INLINE_LONG");
    term_is(locale, "writer", CE_CSL_LOCALE_VERB_SHORT, false, CE_CSL_LOCALE_NEUTER, "EN_VSHORT");
    term_is(locale, "other", CE_CSL_LOCALE_VERB_SHORT, false, CE_CSL_LOCALE_NEUTER, "OTHER_VERB");
    term_is(locale, "empty", CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER, "");
    term_is(locale, "blank", CE_CSL_LOCALE_SYMBOL, false, CE_CSL_LOCALE_NEUTER, "");
    term_is(locale, "space", CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER, "  中 & 文  ");
    term_is(locale, "page", CE_CSL_LOCALE_LONG, true, CE_CSL_LOCALE_NEUTER, "");
    term_is(locale, "page", CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER, "page");
    term_is(locale, "section", CE_CSL_LOCALE_SYMBOL, true, CE_CSL_LOCALE_NEUTER, "§§");
    assert(!ce_csl_locale_term(locale, "missing", CE_CSL_LOCALE_SYMBOL, false, CE_CSL_LOCALE_NEUTER).found);
    assert(ce_csl_locale_option(locale, CE_CSL_LOCALE_PUNCTUATION_IN_QUOTE, &value) && !value);
    assert(ce_csl_locale_option(locale, CE_CSL_LOCALE_DAY_ORDINALS_ONLY_FIRST, &value) && value);
    assert(ce_csl_locale_date(locale, CE_CSL_LOCALE_DATE_TEXT) == ce_csl_xml_child(files[0]->root, "date"));
    assert(ce_csl_locale_date(locale, CE_CSL_LOCALE_DATE_NUMERIC) == ce_csl_xml_child(files[2]->root, "date"));
    ce_csl_locale_free(locale);
    locale = create(s, files, COUNT(files), "de");
    assert(!strcmp(ce_csl_locale_language(locale), "de-de"));
    term_is(locale, "p0", CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER, "LANG");
    ce_csl_locale_free(locale);
    ce_csl_xml_free(s);
    for (i = 0; i < COUNT(files); ++i) ce_csl_xml_free(files[i]);
}

static void test_ordinals(void) {
    CeCslXml *base = external("en-US", EN_ORD TERM("long-ordinal-01", "first") TERM("long-ordinal-02", "second"), NULL, NULL);
    CeCslXml *s = style("<locale><terms><term name='ordinal-01'>X</term></terms></locale>");
    CeCslLocale *locale = create(s, &base, 1, "en-US");
    /* Any ordinal definition replaces the entire suffix set, including when
     * there is no generic 'ordinal'. Older citeproc-js leaks base suffixes here. */
    ordinal_is(locale, 1, false, CE_CSL_LOCALE_NEUTER, true, false, "X");
    ordinal_is(locale, 13, false, CE_CSL_LOCALE_NEUTER, false, false, "");
    assert(!ce_csl_locale_term(locale, "ordinal-02", CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER).found);
    ordinal_is(locale, 1, true, CE_CSL_LOCALE_NEUTER, true, true, "first");
    ordinal_is(locale, 2, true, CE_CSL_LOCALE_NEUTER, true, true, "second");
    ce_csl_locale_free(locale); ce_csl_xml_free(s);
    s = style("<locale><terms><term name='ordinal'/><term name='long-ordinal-01'/></terms></locale>");
    locale = create(s, &base, 1, "en-US");
    ordinal_is(locale, 1, false, CE_CSL_LOCALE_NEUTER, true, false, "");
    ordinal_is(locale, 1, true, CE_CSL_LOCALE_NEUTER, true, true, "");
    ce_csl_locale_free(locale); ce_csl_xml_free(s);
    s = style("<locale><terms>" TERM("ordinal-01","OLD1") TERM("ordinal-02","OLD2")
        TERM("ordinal-03","OLD3") TERM("ordinal-04","OLD4") "</terms></locale>");
    locale = create(s, &base, 1, "en-US");
    ordinal_is(locale, 0, false, CE_CSL_LOCALE_NEUTER, true, false, "OLD4");
    ordinal_is(locale, 11, false, CE_CSL_LOCALE_NEUTER, true, false, "OLD4");
    ordinal_is(locale, 112, false, CE_CSL_LOCALE_NEUTER, true, false, "OLD4");
    ordinal_is(locale, 101, false, CE_CSL_LOCALE_NEUTER, true, false, "OLD1");
    ordinal_is(locale, 22, false, CE_CSL_LOCALE_NEUTER, true, false, "OLD2");
    ordinal_is(locale, 23, false, CE_CSL_LOCALE_NEUTER, true, false, "OLD3");
    ordinal_is(locale, UINT64_MAX, false, CE_CSL_LOCALE_NEUTER, true, false, "OLD4");
    ce_csl_locale_free(locale); ce_csl_xml_free(s);
    s = style("<locale><terms>" TERM("ordinal","D") TERM("ordinal-00","L0") TERM("ordinal-01","L1") TERM("ordinal-03","L3")
        "<term name='ordinal-02' match='last-two-digits'>T02</term>"
        "<term name='ordinal-11' match='whole-number'>W11</term>"
        "<term name='ordinal-13'>T13</term>"
        "<term name='long-ordinal-03' match='last-digit'>THIRD</term>"
        "</terms></locale>");
    locale = create(s, &base, 1, "en-US");
    ordinal_is(locale, 11, false, CE_CSL_LOCALE_NEUTER, true, false, "W11");
    ordinal_is(locale, 111, false, CE_CSL_LOCALE_NEUTER, true, false, "L1");
    ordinal_is(locale, 102, false, CE_CSL_LOCALE_NEUTER, true, false, "T02");
    ordinal_is(locale, 12, false, CE_CSL_LOCALE_NEUTER, true, false, "D");
    ordinal_is(locale, 113, false, CE_CSL_LOCALE_NEUTER, true, false, "T13");
    ordinal_is(locale, UINT64_C(9007199254740993), false, CE_CSL_LOCALE_NEUTER, true, false, "L3");
    ordinal_is(locale, 0, true, CE_CSL_LOCALE_NEUTER, true, false, "L0");
    ordinal_is(locale, 3, true, CE_CSL_LOCALE_NEUTER, true, true, "THIRD");
    ordinal_is(locale, 103, true, CE_CSL_LOCALE_NEUTER, true, false, "L3");
    ce_csl_locale_free(locale); ce_csl_xml_free(s);
    /* A less specific in-style layer replaces base, but exact dialect wins
     * as a whole set over both generic and language-only definitions. */
    s = style("<locale><terms>" TERM("ordinal","ANY") "</terms></locale>"
        "<locale xml:lang='en'><terms>" TERM("ordinal","LANG") "</terms></locale>"
        "<locale xml:lang='en-US'><terms>" TERM("ordinal-02","EXACT2") "</terms></locale>");
    locale = create(s, &base, 1, "en-US");
    ordinal_is(locale, 2, false, CE_CSL_LOCALE_NEUTER, true, false, "EXACT2");
    ordinal_is(locale, 3, false, CE_CSL_LOCALE_NEUTER, false, false, "");
    ce_csl_locale_free(locale); ce_csl_xml_free(s); ce_csl_xml_free(base);
}

static void test_gender(void) {
    CeCslXml *base = external("en-US", EN_ORD "<term name='long-ordinal-01' gender-form='feminine'>BASE_FEM</term>"
        "<term name='edition' gender='feminine'>edition</term>", NULL, NULL);
    CeCslXml *s = style("<locale><terms>" TERM("ordinal","D")
        "<term name='ordinal-03' gender-form='feminine'>F3</term>"
        "<term name='ordinal-13'>N13</term>"
        "<term name='ordinal-01' gender-form='masculine' match='whole-number'>M1</term>"
        "<term name='long-ordinal-01'>INLINE_NEUTER</term>"
        "<term name='edition' form='short'>ed.</term>"
        "<term name='month-01' gender='masculine'>January</term>"
        "</terms></locale>");
    CeCslLocale *locale = create(s, &base, 1, "en-US");
    assert(ce_csl_locale_noun_gender(locale, "edition") == CE_CSL_LOCALE_FEMININE);
    assert(ce_csl_locale_noun_gender(locale, "month-01") == CE_CSL_LOCALE_MASCULINE);
    assert(ce_csl_locale_noun_gender(locale, "missing") == CE_CSL_LOCALE_NEUTER);
    ordinal_is(locale, 13, false, CE_CSL_LOCALE_FEMININE, true, false, "F3");
    ordinal_is(locale, 13, false, CE_CSL_LOCALE_MASCULINE, true, false, "N13");
    ordinal_is(locale, 1, false, CE_CSL_LOCALE_MASCULINE, true, false, "M1");
    ordinal_is(locale, 21, false, CE_CSL_LOCALE_MASCULINE, true, false, "D");
    ordinal_is(locale, 1, true, CE_CSL_LOCALE_FEMININE, true, true, "BASE_FEM");
    ordinal_is(locale, 1, true, CE_CSL_LOCALE_MASCULINE, true, true, "INLINE_NEUTER");
    ce_csl_locale_free(locale); ce_csl_xml_free(s); ce_csl_xml_free(base);
}

static void test_languages_dates_options(void) {
    CeCslXml *base = external("en-US", TERM("and","and"), "punctuation-in-quote='1' limit-day-ordinals-to-day-1='0'", NULL);
    CeCslXml *s = style("<locale><style-options punctuation-in-quote=' 0 '/>"
        "<date form='numeric' delimiter='/'><date-part name='day' suffix='日'/><date-part name='year'/></date></locale>");
    CeCslLocale *locale = create(s, &base, 1, "sr");
    bool value = true;
    assert(!strcmp(ce_csl_locale_language(locale), "sr-latn-rs"));
    assert(ce_csl_locale_option(locale, CE_CSL_LOCALE_PUNCTUATION_IN_QUOTE, &value) && !value);
    assert(ce_csl_locale_option(locale, CE_CSL_LOCALE_DAY_ORDINALS_ONLY_FIRST, &value) && !value);
    assert(ce_csl_locale_date(locale, CE_CSL_LOCALE_DATE_NUMERIC) == ce_csl_xml_child(s->root->children, "date"));
    ce_csl_locale_free(locale); ce_csl_xml_free(s);
    s = parse("<style xmlns='" NS "' version='1.0' default-locale='zh-TW'/>");
    locale = create(s, &base, 1, NULL);
    assert(!strcmp(ce_csl_locale_language(locale), "zh-tw"));
    term_is(locale, "and", CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER, "and");
    ce_csl_locale_free(locale); ce_csl_xml_free(s);
    {
        const char *bad[] = {"", "en_US", "-en", "en-", "en--US", "123", "en-@US", "abcdefghi", "en-123456789", "zz-ZZ"};
        size_t i;
        char error[128];
        for (i = 0; i < COUNT(bad); ++i) {
            assert(!ce_csl_locale_new(NULL, NULL, bad[i], NULL, error, sizeof(error)) && *error);
        }
        {
            CeCslLocalePrimary map[] = {{"zz", "zz-AA"}};
            CeCslLocaleCatalog catalog = {NULL, 0, map, 1};
            locale = ce_csl_locale_new(NULL, &catalog, "ZZ", NULL, error, sizeof(error));
            assert(locale && !strcmp(ce_csl_locale_language(locale), "zz-aa"));
            assert(!ce_csl_locale_option(locale, CE_CSL_LOCALE_PUNCTUATION_IN_QUOTE, &value) && !value);
            assert(!ce_csl_locale_date(locale, CE_CSL_LOCALE_DATE_TEXT));
            ce_csl_locale_free(locale);
            map[0].primary_dialect = "xx-AA";
            assert(!ce_csl_locale_new(NULL, &catalog, "zz", NULL, error, sizeof(error)));
            map[0] = (CeCslLocalePrimary){"en", "en-GB"};
            assert(!ce_csl_locale_new(NULL, &catalog, "en", NULL, error, sizeof(error)));
        }
    }
    {
        CeCslXml *arabic = external("ar", TERM("and","و"), NULL, NULL);
        locale = create(NULL, &arabic, 1, "ar");
        assert(!strcmp(ce_csl_locale_language(locale), "ar"));
        term_is(locale, "and", CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER, "و");
        ce_csl_locale_free(locale); ce_csl_xml_free(arabic);
    }
    ce_csl_xml_free(base);
}

static void expect_invalid_style(const char *fragment) {
    CeCslXml *s = style(fragment);
    char error[256];
    CeCslLocale *locale = ce_csl_locale_new(s->root, NULL, "en-US", NULL, error, sizeof(error));
    if (locale) fprintf(stderr, "Unexpected accepted fixture: %s\n", fragment);
    assert(!locale && *error);
    ce_csl_xml_free(s);
}
static void test_validation(void) {
    static const char *const bad[] = {
        "<locale/><locale/>", "<locale xml:lang='en-US'/><locale xml:lang='EN-us'/>",
        "<locale xmlns='urn:wrong'/>", "<locale xmlns=''/>", "<locale xml:lang='en_US'/>",
        "<locale version='1.0'/>", "<locale><info/></locale>",
        "<locale><terms/></locale>", "<locale><terms><term name='and'/></terms><terms><term name='and'/></terms></locale>",
        "<locale><terms><term name='and'/><term name='and' form='long'/></terms></locale>",
        "<locale><terms><term name='ordinal-01' match='whole-number'/><term name='ordinal-01'/></terms></locale>",
        "<locale><terms><term name='and' form='tiny'/></terms></locale>",
        "<locale><terms><term name='and' gender-form='feminine'/></terms></locale>",
        "<locale><terms><term name='ordinal' gender='feminine'/></terms></locale>",
        "<locale><terms><term name='edition' form='short' gender='feminine'/></terms></locale>",
        "<locale><terms><term name='ordinal' gender-form='other'/></terms></locale>",
        "<locale><terms><term name='ordinal-13' match='last-digit'/></terms></locale>",
        "<locale><terms><term name='ordinal-100'/></terms></locale>",
        "<locale><terms><term name='long-ordinal-11'/></terms></locale>",
        "<locale><terms><term name='and'><single>x</single></term></terms></locale>",
        "<locale><terms><term name='and'>mixed<single>x</single><multiple>y</multiple></term></terms></locale>",
        "<locale><terms><term name='and'><multiple>x</multiple><single>y</single></term></terms></locale>",
        "<locale><terms><term name='and'><b>x</b></term></terms></locale>",
        "<locale><terms><term xmlns='urn:bad' name='and'/></terms></locale>",
        "<locale><terms><term name='and' unsupported='true'/></terms></locale>",
        "<locale><style-options punctuation-in-quote='yes'/></locale>",
        "<locale><style-options unknown='true'/></locale>", "<locale><style-options/><style-options/></locale>",
        "<locale><date form='text'/></locale>",
        "<locale><date form='text' prefix='('><date-part name='year'/></date></locale>",
        "<locale><date form='text'><date-part name='year'/><date-part name='year'/></date></locale>",
        "<locale><date form='text'><date-part name='month' form='ordinal'/></date></locale>",
        "<locale><date form='text'><date-part name='year'/></date><date form='text'><date-part name='day'/></date></locale>"
    };
    size_t i;
    for (i = 0; i < COUNT(bad); ++i) expect_invalid_style(bad[i]);
    {
        const char *invalid_roots[] = {
            "<locale xmlns='" NS "' xml:lang='en-US'/>",
            "<locale xmlns='" NS "' version='1.0'/>",
            "<locale xmlns='" NS "' version='1.0' xml:lang='en-US'><terms><term name='and'/></terms></locale>",
            "<locale xmlns='urn:wrong' version='1.0' xml:lang='en-US'/>",
            "<locale xmlns='" NS "' version='1.1' xml:lang='en-US'/>"
        };
        for (i = 0; i < COUNT(invalid_roots); ++i) {
            CeCslXml *x = parse(invalid_roots[i]);
            const CeCslXmlNode *root = x->root;
            CeCslLocaleCatalog catalog = {&root, 1, NULL, 0};
            char error[256];
            assert(!ce_csl_locale_new(NULL, &catalog, "en-US", NULL, error, sizeof(error)) && *error);
            ce_csl_xml_free(x);
        }
    }
    {
        CeCslXml *a = external("en-US", TERM("and","a"), NULL, NULL);
        CeCslXml *b = external("EN-us", TERM("and","b"), NULL, NULL);
        const CeCslXmlNode *roots[] = {a->root, b->root};
        CeCslLocaleCatalog catalog = {roots, 2, NULL, 0};
        char error[256];
        assert(!ce_csl_locale_new(NULL, &catalog, "en-US", NULL, error, sizeof(error)) && *error);
        catalog.root_count = CE_CSL_LOCALE_MAX_SOURCES + 1;
        assert(!ce_csl_locale_new(NULL, &catalog, "en-US", NULL, error, sizeof(error)) && *error);
        ce_csl_xml_free(a); ce_csl_xml_free(b);
    }
}

typedef struct Allocation { size_t calls, fail_at, live; } Allocation;
static void *allocate(void *context, size_t bytes) {
    Allocation *a = context;
    void *p;
    if (a->calls++ == a->fail_at) return NULL;
    p = malloc(bytes); if (p) ++a->live;
    return p;
}
static void deallocate(void *context, void *pointer) {
    Allocation *a = context;
    assert(pointer && a->live); --a->live; free(pointer);
}
static void test_oom(void) {
    CeCslXml *base = external("en-US", EN_ORD, NULL, NULL);
    CeCslXml *s = style("<locale><terms>" TERM("and","inline") "</terms></locale>");
    const CeCslXmlNode *root = base->root;
    CeCslLocaleCatalog catalog = {&root, 1, NULL, 0};
    size_t i;
    for (i = 0; i < 10; ++i) {
        Allocation state = {0, i, 0};
        CeCslLocaleAllocator memory = {&state, allocate, deallocate};
        char error[256];
        CeCslLocale *locale = ce_csl_locale_new(s->root, &catalog, "en-US", &memory, error, sizeof(error));
        if (locale) {
            term_is(locale, "and", CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER, "inline");
            ce_csl_locale_free(locale); assert(!state.live); break;
        }
        assert(*error && !state.live);
        assert(ce_csl_xml_is(base->root, "locale") && ce_csl_xml_is(s->root, "style"));
    }
    assert(i < 10);
    ce_csl_xml_free(base); ce_csl_xml_free(s);
}

static CeCslXml *read_xml(const char *path) {
    FILE *f = fopen(path, "rb");
    long size;
    char *data, error[256];
    CeCslXml *xml;
    assert(f && !fseek(f, 0, SEEK_END)); size = ftell(f);
    assert(size > 0 && (unsigned long)size <= CE_CSL_XML_MAX_BYTES);
    rewind(f); data = malloc((size_t)size); assert(data);
    assert(fread(data, 1, (size_t)size, f) == (size_t)size && !fclose(f));
    xml = ce_csl_xml_parse(data, (size_t)size, error, sizeof(error)); free(data);
    assert(xml); return xml;
}
static void test_official(int argc, char **argv) {
    CeCslXml *docs[16], *styles[16];
    const CeCslXmlNode *roots[16];
    size_t count = 0, style_count = 0, i;
    CeCslLocaleCatalog catalog = {roots, 0, NULL, 0};
    char error[256];
    int arg;
    assert(argc <= 17);
    for (arg = 1; arg < argc; ++arg) {
        CeCslXml *xml = read_xml(argv[arg]); docs[count++] = xml;
        if (ce_csl_xml_is(xml->root, "locale")) roots[catalog.root_count++] = xml->root;
        else { assert(ce_csl_xml_is(xml->root, "style")); styles[style_count++] = xml; }
    }
    for (i = 0; i < style_count; ++i) {
        CeCslLocale *locale = ce_csl_locale_new(styles[i]->root, &catalog, NULL, NULL, error, sizeof(error));
        const char *default_locale = ce_csl_xml_attribute(styles[i]->root, "default-locale");
        if (!locale) fprintf(stderr, "Official resource rejected: %s\n", error);
        assert(locale && !*error);
        assert(ce_csl_locale_date(locale, CE_CSL_LOCALE_DATE_TEXT));
        if (default_locale && !strcmp(default_locale, "zh-CN"))
            term_is(locale, "page-range-delimiter", CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER, "-");
        else ordinal_is(locale, 21, false, CE_CSL_LOCALE_NEUTER, true, false, "st");
        ce_csl_locale_free(locale);
    }
    for (i = 0; i < count; ++i) ce_csl_xml_free(docs[i]);
    printf("Validated %zu official locale resources and %zu real styles\n", catalog.root_count, style_count);
}
int main(int argc, char **argv) {
    test_fallback(); test_ordinals(); test_gender(); test_languages_dates_options(); test_validation(); test_oom();
    if (argc > 1) test_official(argc, argv);
    puts("CSL locale priority/forms/empty values/ordinal sets/genders/dates/options/validation/OOM passed");
    return 0;
}
