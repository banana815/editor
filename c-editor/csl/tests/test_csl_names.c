#include "csl_names.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS CE_CSL_NAMESPACE
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
static char error[256];
static CeCslXml *locale_xml;
static CeCslLocale *locale;
static cJSON *json(const char *s) {
    cJSON *v = cJSON_Parse(s);
    assert(v);
    return v;
}
static CeCslXml *xml(const char *s) {
    CeCslXml *x = ce_csl_xml_parse(s, strlen(s), error, sizeof(error));
    if (!x)
        fprintf(stderr, "%s\n", error);
    assert(x);
    return x;
}
static CeCslXml *style(const char *root, const char *section, const char *names) {
    size_t n = strlen(root) + strlen(section) + strlen(names) + 256;
    char *s = malloc(n);
    assert(s);
    snprintf(s, n, "<style xmlns='%s' version='1.0' %s><citation %s>%s</citation></style>", NS, root, section,
             names);
    CeCslXml *x = xml(s);
    free(s);
    return x;
}
static CeCslNameOptions options(CeCslXml *x) {
    CeCslNameOptions o;
    const CeCslXmlNode *section = ce_csl_xml_child(x->root, "citation");
    bool ok = ce_csl_names_resolve(x->root, section, ce_csl_xml_child(section, "names"), NULL, &o, error,
                                   sizeof(error));
    if (!ok)
        fprintf(stderr, "Resolve: %s\n", error);
    assert(ok);
    return o;
}
static CeCslNamesData *data(const char *s, const CeCslOutputAllocator *a) {
    cJSON *v = s ? json(s) : NULL;
    CeCslNamesData *d = NULL;
    CeCslNameOrigin origin = {"item", "author"};
    bool ok = ce_csl_names_data_load(v, &origin, a, &d, error, sizeof(error));
    if (!ok)
        fprintf(stderr, "Data: %s\n", error);
    assert(ok);
    cJSON_Delete(v);
    return d;
}
static void plain_is(const CeCslNamesResult *result, const char *s) {
    CeCslOutputString text = {0};
    assert(ce_csl_output_plain(ce_csl_names_result_output(result), &text, error, sizeof(error)));
    if (strcmp(text.data, s))
        fprintf(stderr, "actual <%s> expected <%s>\n", text.data, s);
    assert(!strcmp(text.data, s));
    for (size_t i = 0; i < ce_csl_names_result_person_count(result); ++i) {
        const CeCslNameRendered *p = ce_csl_names_result_person(result, i);
        assert(p && p->identity && p->output_offset + p->output_length <= text.length);
        const CeCslNameSortView *sort = ce_csl_names_result_sort_person(result, i);
        assert(sort && sort->kind == p->identity->kind);
        assert(sort->primary_family && strlen(sort->primary_family) == sort->primary_family_length);
        assert(sort->secondary_particles &&
               strlen(sort->secondary_particles) == sort->secondary_particles_length);
        assert(sort->given && strlen(sort->given) == sort->given_length);
        assert(sort->suffix && strlen(sort->suffix) == sort->suffix_length);
        assert(sort->literal && strlen(sort->literal) == sort->literal_length);
    }
    assert(!ce_csl_names_result_sort_person(result, ce_csl_names_result_person_count(result)));
    ce_csl_output_string_free(&text);
}
static CeCslNamesResult *render(const char *input, const char *root, const char *section, const char *body,
                                const CeCslNameRenderContext *context) {
    CeCslXml *x = style(root, section, body);
    CeCslNameOptions o = options(x);
    CeCslNamesData *d = data(input, NULL);
    CeCslNamesResult *r = NULL;
    CeCslNamesBinding binding = {"author", d};
    bool ok = ce_csl_names_render(&binding, 1, &o, locale, context, &r, error, sizeof(error));
    if (!ok)
        fprintf(stderr, "Render: %s\n", error);
    assert(ok);
    ce_csl_names_data_free(d);
    ce_csl_xml_free(x);
    return r;
}
static void expect(const char *input, const char *root, const char *section, const char *body,
                   const char *expected) {
    CeCslNamesResult *r = render(input, root, section, body, NULL);
    plain_is(r, expected);
    ce_csl_names_result_free(r);
}
static void setup(void) {
    locale_xml = xml(
        "<locale xmlns='" NS "' version='1.0' xml:lang='en-US'><style-options/><date form='text'><date-part "
        "name='year'/></date><terms>"
        "<term name='and'>and</term><term name='et-al'>et al.</term><term name='and others'>and others</term>"
        "<term name='editor'><single>editor</single><multiple>editors</multiple></term>"
        "<term name='editor' form='short'><single>ed.</single><multiple>eds.</multiple></term>"
        "<term name='editor' form='verb'>edited by</term>"
        "<term name='translator'><single>translator</single><multiple>translators</multiple></term>"
        "<term name='editortranslator'><single>editor &amp; translator</single><multiple>editors &amp; "
        "translators</multiple></term>"
        "</terms></locale>");
    const CeCslXmlNode *roots[] = {locale_xml->root};
    CeCslLocaleCatalog catalog = {roots, 1, NULL, 0};
    locale = ce_csl_locale_new(NULL, &catalog, NULL, NULL, error, sizeof(error));
    assert(locale);
}
static void test_parser(void) {
    const char *same[][2] = {
        {"[{\"family\":\"张\",\"given\":\"三\"}]", "[{\"given\":\"三\",\"family\":\"张\"}]"},
        {"[{\"family\":\"Doe\"}]", "[{\"given\":\"\",\"family\":\"Doe\",\"comma-suffix\":false}]"},
        {"[{\"family\":\"Doe\",\"comma-suffix\":true}]", "[{\"family\":\"Doe\",\"comma-suffix\":\"1\"}]"},
        {"[{}]", "[{\"literal\":\"\",\"family\":\"\",\"given\":\"\"}]"}};
    for (size_t i = 0; i < COUNT(same); ++i) {
        cJSON *a = json(same[i][0]), *b = json(same[i][1]);
        bool equal = false;
        assert(ce_csl_names_equal(a, b, &equal, error, sizeof(error)) && equal);
        cJSON_Delete(a);
        cJSON_Delete(b);
    }
    const char *different[][2] = {
        {"[{\"family\":\"Doe\",\"given\":\"Jane\"}]", "[{\"family\":\"Doe\",\"given\":\"John\"}]"},
        {"[{\"family\":\"é\"}]", "[{\"family\":\"é\"}]"},
        {"[{\"family\":\"A\"},{\"family\":\"B\"}]", "[{\"family\":\"B\"},{\"family\":\"A\"}]"},
        {"[{\"family\":\"A\"}]", "[{\"literal\":\"A\"}]"},
        {"[{\"family\":\"A\"}]", "[{\"family\":\"A\",\"parse-names\":false}]"}};
    for (size_t i = 0; i < COUNT(different); ++i) {
        cJSON *a = json(different[i][0]), *b = json(different[i][1]);
        bool equal = true;
        assert(ce_csl_names_equal(a, b, &equal, error, sizeof(error)) && !equal);
        cJSON_Delete(a);
        cJSON_Delete(b);
    }
    size_t n = 777;
    cJSON *empty_names = json("[{}, {\"literal\":\"\"},{\"family\":\"\",\"given\":\"\"}]");
    assert(ce_csl_names_inspect(empty_names, &n, error, sizeof(error)) && n == 0);
    cJSON_Delete(empty_names);
    const char *bad[] = {"{}",
                         "[2]",
                         "[{\"given\":\"John\"}]",
                         "[{\"family\":4}]",
                         "[{\"family\":\"x\",\"literal\":\"y\"}]",
                         "[{\"family\":\"x\",\"unknown\":0}]",
                         "[{\"family\":\"x\",\"comma-suffix\":2}]",
                         "[{\"family\":\"x\",\"family\":\"y\"}]"};
    for (size_t i = 0; i < COUNT(bad); ++i) {
        cJSON *v = json(bad[i]);
        bool equal = true;
        n = 777;
        assert(!ce_csl_names_inspect(v, &n, error, sizeof(error)) && n == 777);
        assert(!ce_csl_names_equal(NULL, v, &equal, error, sizeof(error)) && equal);
        cJSON_Delete(v);
    }
}
static void test_people(void) {
    const char *john = "[{\"family\":\"Doe\",\"given\":\"John Edward\"}]";
    expect(john, "", "", "<names variable='author'><name/></names>", "John Edward Doe");
    expect(john, "initialize-with='. '", "", "<names variable='author'/>", "J. E. Doe");
    expect(john, "initialize-with='.'", "", "<names variable='author'/>", "J.E. Doe");
    expect(john, "initialize-with=''", "", "<names variable='author'/>", "JE Doe");
    expect(john, "initialize-with='.  '", "", "<names variable='author'/>", "J.  E. Doe");
    expect(john, "initialize-with='. '", "initialize='false'", "<names variable='author'/>",
           "John Edward Doe");
    expect(john, "name-form='short'", "", "<names variable='author'/>", "Doe");
    expect(john, "name-as-sort-order='all'", "",
           "<names variable='author'><name sort-separator=' '/></names>", "Doe John Edward");
    expect("[{\"family\":\"Doe\",\"given\":\"James T Kirk\"}]", "", "",
           "<names variable='author'><name initialize='false' initialize-with='.'/></names>",
           "James T. Kirk Doe");
    expect("[{\"family\":\"Doe\",\"given\":\"Jean-Luc\"}]", "", "",
           "<names variable='author'><name initialize-with='. '/></names>", "J.-L. Doe");
    expect("[{\"family\":\"Doe\",\"given\":\"Jean-Luc\"}]", "initialize-with-hyphen='false'", "",
           "<names variable='author'><name initialize-with='. '/></names>", "J. L. Doe");
    expect("[{\"family\":\"Doe\",\"given\":\"émile ǳuro ßeta\"}]", "", "",
           "<names variable='author'><name initialize-with='. '/></names>", "É. ǲ. Ss. Doe");
    expect("[{\"family\":\"毛\",\"given\":\"泽东\"},{\"family\":\"山田\",\"given\":\"太郎\"},{\"family\":"
           "\"김\",\"given\":\"민수\"}]",
           "name-as-sort-order='all' initialize-with='. '", "", "<names variable='author'/>",
           "毛泽东, 山田太郎, 김민수");
    expect("[{\"family\":\"毛\",\"given\":\"John\"},{\"family\":\"Mao\",\"given\":\"泽东\"}]",
           "name-as-sort-order='all' initialize-with='. '", "", "<names variable='author'/>",
           "毛 J., Mao, 泽东");
    expect("[{\"literal\":\"The Royal Society & <友>\"}]", "name-form='short' initialize-with='.'", "",
           "<names variable='author'><name><name-part name='family' text-case='uppercase'/></name></names>",
           "The Royal Society & <友>");
    expect("[{\"family\":\"علي\",\"given\":\"محمد\"}]", "name-as-sort-order='all' initialize-with='. '", "",
           "<names variable='author'/>", "علي, م.");
    expect("[{\"family\":\"Doe\",\"given\":\"John\",\"suffix\":\"Jr.\",\"comma-suffix\":true}]", "", "",
           "<names variable='author'/>", "John Doe, Jr.");
    const char *particles = "[{\"family\":\"Martinière\",\"given\":\"Gérard\",\"dropping-particle\":\"de\","
                            "\"non-dropping-particle\":\"la\",\"suffix\":\"III\"}]";
    expect(particles, "demote-non-dropping-particle='never'", "",
           "<names variable='author'><name name-as-sort-order='all'/></names>",
           "la Martinière, Gérard de, III");
    expect(particles, "", "", "<names variable='author'><name name-as-sort-order='all'/></names>",
           "Martinière, Gérard de la, III");
    expect(particles, "", "", "<names variable='author'><name form='short'/></names>", "la Martinière");
    expect(particles, "name-as-sort-order='all'", "", "<names variable='author'><name form='short'/></names>",
           "la Martinière");
    expect("[{\"family\":\"Artagnan\",\"given\":\"Charles\",\"non-dropping-particle\":\"d’\"}]", "", "",
           "<names variable='author'/>", "Charles d’Artagnan");
    expect("[{\"family\":\"Hakim\",\"given\":\"Tawfiq\",\"non-dropping-particle\":\"al-\"}]", "", "",
           "<names variable='author'/>", "Tawfiq al-Hakim");
}
static void test_lists(void) {
    const char *three = "[{\"family\":\"A\",\"given\":\"John\"},{\"family\":\"B\",\"given\":\"Jane\"},{"
                        "\"family\":\"C\",\"given\":\"Jim\"}]";
    expect(three, "", "", "<names variable='author'><name and='text'/></names>", "John A, Jane B, and Jim C");
    expect(three, "", "",
           "<names variable='author'><name and='symbol' delimiter-precedes-last='never'/></names>",
           "John A, Jane B & Jim C");
    expect(three, "", "",
           "<names variable='author'><name and='text' name-as-sort-order='first' "
           "delimiter-precedes-last='after-inverted-name'/></names>",
           "A, John, Jane B and Jim C");
    expect(three, "", "", "<names variable='author'><name et-al-min='3' et-al-use-first='1'/></names>",
           "John A et al.");
    expect(three, "", "",
           "<names variable='author'><name name-as-sort-order='first' et-al-min='3' et-al-use-first='1' "
           "delimiter-precedes-et-al='after-inverted-name'/></names>",
           "A, John, et al.");
    expect(three, "", "",
           "<names variable='author'><name et-al-min='3' et-al-use-first='2' "
           "delimiter-precedes-et-al='never'/><et-al term='and others'/></names>",
           "John A, Jane B and others");
    expect(three, "", "",
           "<names variable='author'><name et-al-min='3' et-al-use-first='1' et-al-use-last='true'/></names>",
           "John A, … Jim C");
    expect(three, "", "",
           "<names variable='author'><name et-al-min='3' et-al-use-first='2' et-al-use-last='true'/></names>",
           "John A, Jane B, et al.");
    expect(three, "", "",
           "<names variable='author'><name et-al-min='3' et-al-use-first='1' et-al-use-last='true' "
           "form='count'/></names>",
           "2");
    CeCslNameRenderContext context = {.subsequent = true};
    CeCslNamesResult *r =
        render(three, "",
               "et-al-min='4' et-al-use-first='3' et-al-subsequent-min='3' et-al-subsequent-use-first='1'",
               "<names variable='author'/>", &context);
    plain_is(r, "John A et al.");
    ce_csl_names_result_free(r);
    context.minimum_names = 2;
    r = render(three, "", "et-al-min='3' et-al-use-first='1'", "<names variable='author'/>", &context);
    plain_is(r, "John A, Jane B, et al.");
    assert(ce_csl_names_result_data(r, 0) && ce_csl_names_data_count(ce_csl_names_result_data(r, 0)) == 3);
    ce_csl_names_result_free(r);
    context.minimum_names = 3;
    r = render(three, "", "et-al-min='3' et-al-use-first='1'",
               "<names variable='author'><name and='text'/></names>", &context);
    plain_is(r, "John A, Jane B, and Jim C");
    ce_csl_names_result_free(r);
    context = (CeCslNameRenderContext){
        .sorting = true, .has_sort_min = true, .sort_min = 3, .has_sort_first = true, .sort_first = 1};
    r = render(three, "", "", "<names variable='author'><name/></names>", &context);
    plain_is(r, "A, John");
    ce_csl_names_result_free(r);
    cJSON *people = cJSON_CreateArray();
    assert(people);
    for (size_t i = 0; i < 21; ++i) {
        char family[16];
        snprintf(family, sizeof(family), "F%zu", i + 1);
        cJSON *p = cJSON_CreateObject();
        assert(p && cJSON_AddStringToObject(p, "family", family) &&
               cJSON_AddStringToObject(p, "given", "Jane"));
        cJSON_AddItemToArray(people, p);
    }
    char *input = cJSON_PrintUnformatted(people);
    assert(input);
    r = render(input, "initialize-with='. '", "et-al-min='21' et-al-use-first='19' et-al-use-last='true'",
               "<names variable='author'><name form='count'/></names>", NULL);
    plain_is(r, "20");
    assert(ce_csl_names_result_summary(r)->used_last);
    ce_csl_names_result_free(r);
    cJSON_free(input);
    cJSON_Delete(people);
}
static void test_roles_format_identity(void) {
    CeCslXml *x = style("", "",
                        "<names variable='editor translator' delimiter='; ' prefix='[' suffix=']'><name "
                        "initialize-with='. '/><label prefix=' (' suffix=')'/></names>");
    CeCslNameOptions o = options(x);
    CeCslNamesData *a = data("[{\"family\":\"Doe\",\"given\":\"Jane\"}]", NULL),
                   *b = data("[{\"given\":\"Jane\",\"family\":\"Doe\"}]", NULL);
    CeCslNamesBinding bindings[] = {{"editor", a}, {"translator", b}};
    CeCslNamesResult *r = NULL;
    assert(ce_csl_names_render(bindings, 2, &o, locale, NULL, &r, error, sizeof(error)));
    plain_is(r, "[J. Doe (editor & translator)]");
    assert(ce_csl_names_result_summary(r)->collapsed_editor_translator);
    const CeCslNameRendered *person = ce_csl_names_result_person(r, 0);
    assert(person && person->output_offset == 1 && person->output_length == 6);
    o.form = CE_CSL_NAME_COUNT;
    assert(ce_csl_names_render(bindings, 2, &o, locale, NULL, &r, error, sizeof(error)));
    plain_is(r, "[1]");
    ce_csl_names_data_free(b);
    b = data("[{\"family\":\"Doe\",\"given\":\"John\"}]", NULL);
    bindings[1].data = b;
    o.form = CE_CSL_NAME_LONG;
    assert(ce_csl_names_render(bindings, 2, &o, locale, NULL, &r, error, sizeof(error)));
    plain_is(r, "[J. Doe (editor); J. Doe (translator)]");
    assert(!ce_csl_names_result_summary(r)->collapsed_editor_translator);
    assert(!ce_csl_name_identity_equal(ce_csl_names_result_person(r, 0)->identity,
                                       ce_csl_names_result_person(r, 1)->identity));
    CeCslNameOverride overrides[] = {{0, 0, CE_CSL_GIVEN_FULL}, {1, 0, CE_CSL_GIVEN_FULL}};
    CeCslNameRenderContext context = {.overrides = overrides, .override_count = 2};
    assert(ce_csl_names_render(bindings, 2, &o, locale, &context, &r, error, sizeof(error)));
    plain_is(r, "[Jane Doe (editor); John Doe (translator)]");
    ce_csl_names_data_free(a);
    ce_csl_names_data_free(b);
    ce_csl_xml_free(x);
    /* The result's input names and copied source identity outlive input and XML. */
    assert(!strcmp(ce_csl_names_result_person(r, 0)->identity->given, "Jane"));
    assert(!strcmp(ce_csl_names_data_origin(ce_csl_names_result_data(r, 0))->item_id, "item"));
    ce_csl_names_result_free(r);
    x = style("", "", "<names variable='editor'><label form='verb' suffix=' '/><name/></names>");
    o = options(x);
    a = data("[{\"family\":\"Doe\",\"given\":\"Jane\"}]", NULL);
    CeCslNamesBinding binding = {"editor", a};
    r = NULL;
    assert(ce_csl_names_render(&binding, 1, &o, locale, NULL, &r, error, sizeof(error)));
    plain_is(r, "edited by Jane Doe");
    assert(ce_csl_names_result_person(r, 0)->output_offset == 10);
    ce_csl_names_result_free(r);
    ce_csl_names_data_free(a);
    ce_csl_xml_free(x);
    r = render("[{\"family\":\"Straße\",\"given\":\"Jane\",\"suffix\":\"Jr.\"}]", "", "",
               "<names variable='author' prefix='(' suffix=')' font-style='italic'><name><name-part "
               "name='given' prefix='[' suffix=']' font-weight='bold'/><name-part name='family' "
               "text-case='uppercase' prefix='{' suffix='}' font-style='normal'/></name></names>",
               NULL);
    plain_is(r, "([Jane] {STRASSE Jr.})");
    CeCslOutputString html = {0};
    assert(ce_csl_output_html(ce_csl_names_result_output(r), &html, error, sizeof(error)));
    assert(strstr(html.data, "STRASSE") && strstr(html.data, "<strong>"));
    ce_csl_output_string_free(&html);
    ce_csl_names_result_free(r);
    r = render("[{\"literal\":\"<img onerror=x>&\"}]", "", "", "<names variable='author'/>", NULL);
    assert(ce_csl_output_html(ce_csl_names_result_output(r), &html, error, sizeof(error)));
    assert(strstr(html.data, "&lt;img onerror=x&gt;&amp;") && !strstr(html.data, "<img"));
    ce_csl_output_string_free(&html);
    ce_csl_names_result_free(r);
}
static void test_scalar_and_empty(void) {
    CeCslXml *x = style(
        "name-as-sort-order='first' initialize-with='. '", "",
        "<names variable='author' prefix='[' suffix=']'><name prefix='(' suffix=')' form='short'/></names>");
    CeCslNameOptions o = options(x);
    CeCslNamesData *d = data("[{}, {\"family\":\"Doe\",\"given\":\"Jane\"}]", NULL);
    CeCslNamesResult *r = NULL;
    CeCslNameOverride override = {0, 1, CE_CSL_GIVEN_INITIALS};
    CeCslNameRenderContext ctx = {.overrides = &override, .override_count = 1};
    assert(ce_csl_name_render(d, 1, &o, locale, &ctx, &r, error, sizeof(error)));
    plain_is(r, "Doe, J.");
    const CeCslNameRendered *person = ce_csl_names_result_person(r, 0);
    assert(person->person_index == 1 && person->inverted && person->given_initialized &&
           person->given_visible);
    const CeCslNameSortView *sort = ce_csl_names_result_sort_person(r, 0);
    assert(sort && !strcmp(sort->primary_family, "Doe") && !strcmp(sort->given, "J."));
    assert(ce_csl_name_render(d, 0, &o, locale, NULL, &r, error, sizeof(error)));
    plain_is(r, "");
    assert(!ce_csl_names_result_summary(r)->rendered);
    assert(!ce_csl_names_result_sort_person(r, 0));
    CeCslNamesBinding b = {"author", d};
    o.form = CE_CSL_NAME_COUNT;
    assert(ce_csl_names_render(&b, 1, &o, locale, NULL, &r, error, sizeof(error)));
    plain_is(r, "[(1)]");
    assert(!ce_csl_names_result_sort_person(r, 0));
    CeCslNamesResult *saved = r;
    ctx.override_count = 2;
    CeCslNameOverride duplicate[] = {override, override};
    ctx.overrides = duplicate;
    assert(!ce_csl_names_render(&b, 1, &o, locale, &ctx, &r, error, sizeof(error)) && r == saved);
    ce_csl_names_result_free(r);
    ce_csl_names_data_free(d);
    ce_csl_xml_free(x);
    x = style("", "et-al-min='1' et-al-use-first='0'",
              "<names variable='author'><name prefix='(' suffix=')'/></names>");
    o = options(x);
    d = data("[{\"family\":\"Doe\"}]", NULL);
    b.data = d;
    r = NULL;
    assert(ce_csl_names_render(&b, 1, &o, NULL, NULL, &r, error, sizeof(error)));
    plain_is(r, "");
    assert(ce_csl_names_result_summary(r)->missing_term && !ce_csl_names_result_summary(r)->needs_substitute);
    ce_csl_names_result_free(r);
    ce_csl_names_data_free(d);
    ce_csl_xml_free(x);
}
static void sort_is(const CeCslNamesResult *result, size_t index, CeCslNameKind kind, const char *family,
                    const char *particles, const char *given, const char *suffix, const char *literal) {
    const CeCslNameSortView *view = ce_csl_names_result_sort_person(result, index);
    assert(view && view->kind == kind);
    assert(!strcmp(view->primary_family, family));
    assert(!strcmp(view->secondary_particles, particles));
    assert(!strcmp(view->given, given));
    assert(!strcmp(view->suffix, suffix));
    assert(!strcmp(view->literal, literal));
}
static void test_sort_components(void) {
    const char *input = "[{\"family\":\"Martinière\",\"given\":\"Gérard Jean-Luc\","
                        "\"dropping-particle\":\"de\",\"non-dropping-particle\":\"la\","
                        "\"suffix\":\"III\"}]";
    const char *body =
        "<names variable='author' prefix='N[' suffix=']'><name initialize-with='. ' prefix='P[' "
        "suffix=']'><name-part name='given' text-case='lowercase' prefix='G[' suffix=']'/>"
        "<name-part name='family' text-case='uppercase' prefix='F[' suffix=']'/></name>"
        "<label prefix=' role:'/></names>";
    CeCslNameRenderContext ctx = {.sorting = true};
    CeCslNamesResult *r = render(input, "demote-non-dropping-particle='never'", "", body, &ctx);
    sort_is(r, 0, CE_CSL_NAME_PERSONAL, "LA MARTINIÈRE", "de", "g. j.-l.", "III", "");
    /* render() freed both the source JSON/data and XML before these getters. */
    assert(!strcmp(ce_csl_names_result_person(r, 0)->identity->given, "Gérard Jean-Luc"));
    ce_csl_names_result_free(r);
    r = render(input, "demote-non-dropping-particle='sort-only'", "", body, &ctx);
    sort_is(r, 0, CE_CSL_NAME_PERSONAL, "MARTINIÈRE", "de LA", "g. j.-l.", "III", "");
    ce_csl_names_result_free(r);
    /* Display inversion alone does not demote sort-only particles. */
    r = render(input, "demote-non-dropping-particle='sort-only' name-as-sort-order='all'", "", body, NULL);
    sort_is(r, 0, CE_CSL_NAME_PERSONAL, "LA MARTINIÈRE", "de", "g. j.-l.", "III", "");
    ce_csl_names_result_free(r);
    r = render(input, "demote-non-dropping-particle='display-and-sort' name-as-sort-order='all'", "", body,
               NULL);
    sort_is(r, 0, CE_CSL_NAME_PERSONAL, "MARTINIÈRE", "de LA", "g. j.-l.", "III", "");
    ce_csl_names_result_free(r);
    CeCslNameOverride override = {0, 0, CE_CSL_GIVEN_FULL};
    ctx.overrides = &override;
    ctx.override_count = 1;
    r = render(input, "", "", body, &ctx);
    sort_is(r, 0, CE_CSL_NAME_PERSONAL, "MARTINIÈRE", "de LA", "gérard jean-luc", "III", "");
    ce_csl_names_result_free(r);
    override.given = CE_CSL_GIVEN_INITIALS;
    r = render(input, "name-form='short'", "", body, &ctx);
    sort_is(r, 0, CE_CSL_NAME_PERSONAL, "MARTINIÈRE", "de LA", "g. j.-l.", "III", "");
    ce_csl_names_result_free(r);
    ctx.overrides = NULL;
    ctx.override_count = 0;
    r = render(input, "name-form='short'", "", body, &ctx);
    sort_is(r, 0, CE_CSL_NAME_PERSONAL, "LA MARTINIÈRE", "", "", "", "");
    ce_csl_names_result_free(r);
    r = render("[{\"family\":\"Artagnan\",\"given\":\"Charles\",\"non-dropping-particle\":\"d’\"}]",
               "demote-non-dropping-particle='never'", "", "<names variable='author'/>", &ctx);
    sort_is(r, 0, CE_CSL_NAME_PERSONAL, "d’Artagnan", "", "Charles", "", "");
    ce_csl_names_result_free(r);
    r = render("[{\"family\":\"Family\",\"given\":\"émile ǳuro ßeta\","
               "\"dropping-particle\":\"d’\",\"non-dropping-particle\":\"al-\"}]",
               "initialize-with='. '", "", "<names variable='author'/>", &ctx);
    sort_is(r, 0, CE_CSL_NAME_PERSONAL, "Family", "d’al-", "É. ǲ. Ss.", "", "");
    ce_csl_names_result_free(r);
    r = render("[{\"family\":\"张\",\"given\":\"三\"},{\"family\":\"Mao\",\"given\":\"泽东\"},"
               "{\"literal\":\"The Royal Society & <友>\"}]",
               "initialize-with='. '", "", "<names variable='author'/>", &ctx);
    sort_is(r, 0, CE_CSL_NAME_PERSONAL, "张", "", "三", "", "");
    sort_is(r, 1, CE_CSL_NAME_PERSONAL, "Mao", "", "泽东", "", "");
    sort_is(r, 2, CE_CSL_NAME_LITERAL, "", "", "", "", "The Royal Society & <友>");
    ce_csl_names_result_free(r);
    /* Selected indices stay aligned with semantic identity through et-al use-last. */
    const char *three = "[{}, {\"family\":\"A\"},{\"family\":\"B\"},{\"family\":\"C\"}]";
    r = render(three, "", "",
               "<names variable='author'><name et-al-min='3' et-al-use-first='1' "
               "et-al-use-last='true'/></names>",
               NULL);
    assert(ce_csl_names_result_person_count(r) == 2);
    assert(ce_csl_names_result_person(r, 0)->person_index == 1);
    assert(ce_csl_names_result_person(r, 1)->person_index == 3);
    sort_is(r, 0, CE_CSL_NAME_PERSONAL, "A", "", "", "", "");
    sort_is(r, 1, CE_CSL_NAME_PERSONAL, "C", "", "", "", "");
    ce_csl_names_result_free(r);
    assert(!ce_csl_names_result_sort_person(NULL, 0));
}
static void test_substitute_and_invalid(void) {
    CeCslXml *x =
        style("initialize-with='. '", "et-al-min='3' et-al-use-first='1'",
              "<names variable='author'><name form='short'/><et-al font-style='italic'/><substitute><names "
              "variable='editor'/><text variable='title'/></substitute></names>");
    CeCslNameOptions outer = options(x), inner;
    const CeCslXmlNode *sub = outer.substitute;
    assert(sub && ce_csl_names_resolve(x->root, ce_csl_xml_child(x->root, "citation"), sub->children, &outer,
                                       &inner, error, sizeof(error)));
    assert(inner.form == CE_CSL_NAME_SHORT && inner.et_al == outer.et_al &&
           inner.initialize_with == outer.initialize_with);
    CeCslNamesData *d = data("[]", NULL);
    CeCslNamesBinding b = {"author", d};
    CeCslNamesResult *r = NULL;
    assert(ce_csl_names_render(&b, 1, &outer, locale, NULL, &r, error, sizeof(error)));
    plain_is(r, "");
    assert(ce_csl_names_result_summary(r)->needs_substitute);
    ce_csl_names_result_free(r);
    ce_csl_names_data_free(d);
    ce_csl_xml_free(x);
    const char *bad[] = {"<names variable='author' display='block'/>",
                         "<names variable='author'><name form='bogus'/></names>",
                         "<names variable='author'><name et-al-min='-1'/></names>",
                         "<names variable='author'><name><name-part name='suffix'/></name></names>",
                         "<names variable='author'><name/><name/></names>",
                         "<names variable='author'><et-al term='wrong'/></names>",
                         "<names variable='author'><label form='bad'/></names>",
                         "<names variable='author'><name delimiter-precedes-last='foo'/></names>"};
    for (size_t i = 0; i < COUNT(bad); ++i) {
        x = style("", "", bad[i]);
        CeCslNameOptions o = {.et_al_min = 777};
        const CeCslXmlNode *section = ce_csl_xml_child(x->root, "citation");
        assert(!ce_csl_names_resolve(x->root, section, section->children, NULL, &o, error, sizeof(error)) &&
               o.et_al_min == 777);
        ce_csl_xml_free(x);
    }
    x = style("", "", "<names variable='author'/>");
    CeCslNameOptions o = options(x);
    d = data("[{\"family\":\"van Doe\",\"parse-names\":true}]", NULL);
    b.data = d;
    r = NULL;
    assert(!ce_csl_names_render(&b, 1, &o, locale, NULL, &r, error, sizeof(error)) && !r &&
           strstr(error, "parse-names"));
    ce_csl_names_data_free(d);
    ce_csl_xml_free(x);
}
typedef union Header {
    struct {
        size_t bytes;
    } block;
    cJSON alignment;
} Header;
typedef struct Memory {
    size_t calls, fail_at, live, bytes, peak;
} Memory;
static void *allocate(void *context, size_t n) {
    Memory *m = context;
    if (m->calls++ == m->fail_at)
        return NULL;
    Header *p = malloc(sizeof(*p) + n);
    if (!p)
        return NULL;
    p->block.bytes = n;
    ++m->live;
    m->bytes += n;
    if (m->peak < m->bytes)
        m->peak = m->bytes;
    return p + 1;
}
static void release(void *context, void *pointer) {
    Memory *m = context;
    Header *p = (Header *)pointer - 1;
    assert(m->live && m->bytes >= p->block.bytes);
    --m->live;
    m->bytes -= p->block.bytes;
    free(p);
}
static void test_oom_limits(void) {
    cJSON *j = json("[{\"family\":\"Doe\",\"given\":\"Jane\",\"dropping-particle\":\"de\","
                    "\"non-dropping-particle\":\"van\",\"suffix\":\"Jr.\"},"
                    "{\"family\":\"张\",\"given\":\"三\"},{\"literal\":\"The <Group>\"}]");
    for (size_t at = 0; at < 8; ++at) {
        Memory m = {.fail_at = at};
        CeCslOutputAllocator a = {&m, allocate, release};
        CeCslNamesData *d = NULL;
        bool ok = ce_csl_names_data_load(j, NULL, &a, &d, error, sizeof(error));
        if (ok) {
            ce_csl_names_data_free(d);
            assert(!m.live);
            break;
        }
        assert(!d && !m.live);
        assert(at < 7);
    }
    CeCslNamesData *d = NULL;
    assert(ce_csl_names_data_load(j, NULL, NULL, &d, error, sizeof(error)));
    cJSON_Delete(j);
    CeCslXml *x = style("", "",
                        "<names variable='author' prefix='(' suffix=')'><name initialize-with='. ' "
                        "name-as-sort-order='all'><name-part name='family' font-style='italic' "
                        "text-case='uppercase'/></name></names>");
    CeCslNameOptions o = options(x);
    CeCslNamesBinding b = {"author", d};
    CeCslNamesResult *old = NULL;
    assert(ce_csl_names_render(&b, 1, &o, locale, NULL, &old, error, sizeof(error)));
    size_t success = 0;
    for (size_t at = 0; at < 1000; ++at) {
        Memory m = {.fail_at = at};
        CeCslOutputAllocator a = {&m, allocate, release};
        CeCslNameRenderContext context = {.allocator = &a};
        CeCslNamesResult *r = old;
        bool ok = ce_csl_names_render(&b, 1, &o, locale, &context, &r, error, sizeof(error));
        if (ok) {
            old = NULL;
            m.fail_at = SIZE_MAX;
            assert(m.live);
            plain_is(r, "(DOE, J. de VAN, Jr., 张三, The <Group>)");
            sort_is(r, 0, CE_CSL_NAME_PERSONAL, "DOE", "de VAN", "J.", "Jr.", "");
            sort_is(r, 2, CE_CSL_NAME_LITERAL, "", "", "", "", "The <Group>");
            ce_csl_names_result_free(r);
            assert(!m.live);
            success = at;
            break;
        }
        assert(r == old && !m.live);
        plain_is(old, "(DOE, J. de VAN, Jr., 张三, The <Group>)");
        sort_is(old, 0, CE_CSL_NAME_PERSONAL, "DOE", "de VAN", "J.", "Jr.", "");
    }
    assert(success > 20 && success < 1000);
    ce_csl_names_result_free(old);
    ce_csl_names_data_free(d);
    ce_csl_xml_free(x);
    j = cJSON_CreateArray();
    assert(j);
    for (size_t i = 0; i <= CE_CSL_NAMES_MAX_PEOPLE; ++i)
        assert(cJSON_AddItemToArray(j, cJSON_CreateObject()));
    size_t count = 777;
    assert(!ce_csl_names_inspect(j, &count, error, sizeof(error)) && count == 777);
    cJSON_Delete(j);
    char *huge = malloc(CE_CSL_NAMES_MAX_BYTES + 2);
    assert(huge);
    memset(huge, 'x', CE_CSL_NAMES_MAX_BYTES + 1);
    huge[CE_CSL_NAMES_MAX_BYTES + 1] = 0;
    j = cJSON_CreateArray();
    cJSON *p = cJSON_CreateObject();
    assert(j && p && cJSON_AddStringToObject(p, "family", huge) && cJSON_AddItemToArray(j, p));
    assert(!ce_csl_names_inspect(j, &count, error, sizeof(error)));
    cJSON_Delete(j);
    free(huge);
}
static void test_sort_component_limit(void) {
    size_t size = CE_CSL_NAMES_MAX_BYTES / 2;
    char *text = malloc(size + 1);
    assert(text);
    memset(text, 'x', size);
    text[size] = 0;
    cJSON *j = cJSON_CreateArray(), *person = cJSON_CreateObject();
    assert(j && person && cJSON_AddStringToObject(person, "literal", text));
    assert(cJSON_AddItemToArray(j, person));
    free(text);
    CeCslNamesData *d = NULL;
    assert(ce_csl_names_data_load(j, NULL, NULL, &d, error, sizeof(error)));
    cJSON_Delete(j);
    CeCslXml *x = style("", "", "<names variable='author editor translator' delimiter=''><name/></names>");
    CeCslNameOptions o = options(x);
    CeCslNamesBinding bindings[] = {{"author", d}, {"editor", d}, {"translator", d}};
    CeCslNamesResult *r = NULL;
    assert(ce_csl_names_render(bindings, 2, &o, locale, NULL, &r, error, sizeof(error)));
    assert(ce_csl_names_result_person_count(r) == 2);
    assert(ce_csl_names_result_sort_person(r, 1)->literal_length == size);
    assert(ce_csl_output_text_bytes(ce_csl_names_result_output(r)) == CE_CSL_NAMES_MAX_BYTES);
    CeCslNamesResult *old = r;
    assert(!ce_csl_names_render(bindings, 3, &o, locale, NULL, &r, error, sizeof(error)));
    assert(r == old && strstr(error, "combined name sort component size limit"));
    assert(ce_csl_names_result_sort_person(r, 0)->literal_length == size);
    ce_csl_names_result_free(r);
    ce_csl_names_data_free(d);
    ce_csl_xml_free(x);
}
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(!fseek(f, 0, SEEK_END));
    long end = ftell(f);
    assert(end >= 0 && end < 4 * 1024 * 1024);
    rewind(f);
    char *s = malloc((size_t)end + 1);
    assert(s && fread(s, 1, (size_t)end, f) == (size_t)end);
    s[end] = 0;
    assert(!fclose(f));
    return s;
}
static bool inherited(CeCslXml *x, const CeCslXmlNode *section, const CeCslXmlNode *n, CeCslNameOptions *o) {
    const CeCslXmlNode *p = n->parent;
    while (p && !ce_csl_xml_is(p, "names"))
        p = p->parent;
    CeCslNameOptions parent;
    const CeCslNameOptions *inherit = NULL;
    if (p && n->parent && ce_csl_xml_is(n->parent, "substitute")) {
        if (!inherited(x, section, p, &parent))
            return false;
        inherit = &parent;
    }
    return ce_csl_names_resolve(x->root, section, n, inherit, o, error, sizeof(error));
}
static void real_resources(int argc, char **argv) {
    CeCslXml *docs[8];
    size_t count = 0, nlocales = 0, rendered = 0;
    const CeCslXmlNode *roots[8];
    assert(argc <= 8);
    for (int i = 0; i < argc; ++i) {
        char *s = read_file(argv[i]);
        docs[count] = xml(s);
        free(s);
        if (ce_csl_xml_is(docs[count]->root, "locale"))
            roots[nlocales++] = docs[count]->root;
        ++count;
    }
    CeCslNamesData *d = data("[{\"family\":\"张\",\"given\":\"三\"},{\"family\":\"Doe\",\"given\":\"Jane\"},{"
                             "\"family\":\"Roe\",\"given\":\"John\"},{\"literal\":\"Research & <Group>\"}]",
                             NULL);
    CeCslLocaleCatalog catalog = {roots, nlocales, NULL, 0};
    for (size_t i = 0; i < count; ++i)
        if (ce_csl_xml_is(docs[i]->root, "style")) {
            CeCslLocale *l = ce_csl_locale_new(docs[i]->root, &catalog, NULL, NULL, error, sizeof(error));
            assert(l);
            const char *sections[] = {"citation", "bibliography"};
            for (size_t scope = 0; scope < 2; ++scope) {
                const CeCslXmlNode *section = ce_csl_xml_child(docs[i]->root, sections[scope]);
                if (!section)
                    continue;
                for (const CeCslXmlNode *n = docs[i]->allocated; n; n = n->allocated_next)
                    if (ce_csl_xml_is(n, "names")) {
                        CeCslNameOptions o;
                        if (!inherited(docs[i], section, n, &o)) {
                            fprintf(stderr, "Real resolve %s\n", error);
                            abort();
                        }
                        char variables[1024];
                        snprintf(variables, sizeof(variables), "%s", ce_csl_xml_attribute(n, "variable"));
                        CeCslNamesBinding bindings[64];
                        size_t nb = 0;
                        for (char *v = strtok(variables, " "); v; v = strtok(NULL, " ")) {
                            assert(nb < 64);
                            bindings[nb++] = (CeCslNamesBinding){v, d};
                        }
                        CeCslNamesResult *r = NULL;
                        CeCslNameRenderContext ctx = {
                            .style_default_locale = ce_csl_xml_attribute(docs[i]->root, "default-locale")};
                        if (!ce_csl_names_render(bindings, nb, &o, l, &ctx, &r, error, sizeof(error))) {
                            fprintf(stderr, "Real render %s\n", error);
                            abort();
                        }
                        assert(ce_csl_names_result_summary(r)->rendered);
                        ce_csl_names_result_free(r);
                        ++rendered;
                    }
            }
            ce_csl_locale_free(l);
        }
    ce_csl_names_data_free(d);
    for (size_t i = 0; i < count; ++i)
        ce_csl_xml_free(docs[i]);
    printf("Pinned styles: %zu names/section render checks passed\n", rendered);
}
int main(int argc, char **argv) {
    setup();
    test_parser();
    test_people();
    test_lists();
    test_roles_format_identity();
    test_scalar_and_empty();
    test_sort_components();
    test_substitute_and_invalid();
    test_oom_limits();
    test_sort_component_limit();
    if (argc > 1)
        real_resources(argc - 1, argv + 1);
    ce_csl_locale_free(locale);
    ce_csl_xml_free(locale_xml);
    puts("CSL names: identity, inheritance, initials/scripts, parts, lists/count, role labels, overrides, "
         "sort components, retention and OOM passed");
    return 0;
}
