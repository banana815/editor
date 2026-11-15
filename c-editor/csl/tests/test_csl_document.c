#include "csl_document.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[512];
static size_t cases;
typedef struct Fixture {
    CeCslStyle *style;
    CeCslLocale *locale;
    CeCslInput *input;
    CeCslDocument *result;
    CeCslDocumentOptions options;
} Fixture;
static const char *numeric_citation = "<citation><sort><key variable='citation-number'/></sort>"
    "<layout delimiter=', '><text variable='citation-number' prefix='[' suffix=']'/></layout></citation>";
static void fixture(Fixture *f, const char *style_class, const char *macros,
    const char *citation, const char *bibliography, const char *json) {
    memset(f, 0, sizeof(*f));
    const char *info = "<info><title>Own document regression</title><id>https://example.invalid/document</id>"
        "<updated>2026-01-01T00:00:00+00:00</updated></info><locale><terms>"
        "<term name='et-al'>et al.</term><term name='and'>and</term>"
        "<term name='open-quote'>“</term><term name='close-quote'>”</term>"
        "<term name='open-inner-quote'>‘</term><term name='close-inner-quote'>’</term></terms></locale>";
    size_t size = strlen(info) + strlen(style_class) + strlen(macros) + strlen(citation) + strlen(bibliography) + 256;
    char *xml = malloc(size); assert(xml);
    snprintf(xml, size, "<style xmlns='http://purl.org/net/xbiblio/csl' version='1.0' class='%s' initialize-with='. '>%s%s%s%s</style>",
        style_class, info, macros, citation, bibliography);
    bool ok = ce_csl_style_load(&f->style, xml, strlen(xml), error, sizeof(error));
    if (!ok) fprintf(stderr, "style: %s\n", error);
    assert(ok); free(xml);
    f->locale = ce_csl_locale_new(ce_csl_style_root(f->style), NULL, "en-US", NULL, error, sizeof(error)); assert(f->locale);
    assert(ce_csl_input_load(&f->input, json, strlen(json), error, sizeof(error)));
}
static bool build(Fixture *f, const CeCslOutputAllocator *allocator) {
    return ce_csl_document_build(f->style, f->locale, f->input, &f->options,
        allocator, &f->result, error, sizeof(error));
}
static void success(Fixture *f) {
    bool ok = build(f, NULL);
    if (!ok) fprintf(stderr, "document: %s\n", error);
    assert(ok); ++cases;
}
static void citation_is(const Fixture *f, size_t index, const char *expected) {
    const CeCslDocumentCitation *cite = ce_csl_document_citation(f->result, index); assert(cite);
    CeCslOutputString text = {0}; assert(ce_csl_output_plain(cite->output, &text, error, sizeof(error)));
    if (text.length != strlen(expected) || strcmp(text.data, expected))
        fprintf(stderr, "citation %zu: expected <%s> actual <%s>\n", index, expected, text.data);
    assert(text.length == strlen(expected) && !strcmp(text.data, expected)); ce_csl_output_string_free(&text);
}
static void order_is(const Fixture *f, size_t index, const size_t *order, size_t count) {
    const CeCslDocumentCitation *cite = ce_csl_document_citation(f->result, index); assert(cite && cite->cite_count == count);
    for (size_t i = 0; i < count; ++i) assert(cite->cite_order[i] == order[i]);
}
static void fixture_free(Fixture *f) {
    ce_csl_document_free(f->result); ce_csl_input_free(f->input);
    ce_csl_locale_free(f->locale); ce_csl_style_free(f->style);
}
static void test_eligibility_and_numbering(void) {
    const char *json = "{\"items\":[{\"id\":\"unused\",\"title\":\"Unused\"},{\"id\":\"a\",\"title\":\"Alpha\"},"
        "{\"id\":\"b\",\"title\":\"Beta\"},{\"id\":\"c\",\"title\":\"Gamma\"}],\"citations\":["
        "{\"citationID\":\"one\",\"citationItems\":[{\"id\":\"b\"}]},"
        "{\"citationID\":\"two\",\"citationItems\":[{\"id\":\"a\"}]},"
        "{\"citationID\":\"three\",\"citationItems\":[{\"id\":\"c\"}]},"
        "{\"citationID\":\"four\",\"citationItems\":[{\"id\":\"c\"},{\"id\":\"b\"},{\"id\":\"a\"}]}]}";
    Fixture f; fixture(&f, "in-text", "", numeric_citation,
        "<bibliography><layout><text variable='title'/></layout></bibliography>", json);
    success(&f); citation_is(&f, 0, "[1]"); citation_is(&f, 1, "[2]"); citation_is(&f, 3, "[1], [2], [3]");
    const size_t last[] = {1, 2, 0}; order_is(&f, 3, last, 3);
    const CeCslRegistry *registry = ce_csl_document_registry(f.result);
    assert(ce_csl_registry_count(registry) == 3 && ce_csl_registry_citation_number(registry, 0) == 0);
    assert(ce_csl_registry_citation_number(registry, 2) == 1 && ce_csl_registry_citation_number(registry, 1) == 2);
    size_t includes[] = {0, 0, 2}; f.options.include_items = includes; f.options.include_count = 3; success(&f);
    assert(ce_csl_registry_count(ce_csl_document_registry(f.result)) == 4);
    assert(ce_csl_registry_citation_number(ce_csl_document_registry(f.result), 0) == 4); fixture_free(&f);
    fixture(&f, "in-text", "", numeric_citation,
        "<bibliography><sort><key variable='title'/></sort><layout><text variable='title'/></layout></bibliography>", json);
    success(&f); citation_is(&f, 0, "[2]"); citation_is(&f, 1, "[1]"); citation_is(&f, 3, "[1], [2], [3]");
    const size_t sorted[] = {2, 1, 0}; order_is(&f, 3, sorted, 3); fixture_free(&f);
    fixture(&f, "in-text", "", numeric_citation, "", "{\"items\":[],\"citations\":[]}");
    success(&f); assert(!ce_csl_document_citation_count(f.result) && !ce_csl_registry_count(ce_csl_document_registry(f.result))); fixture_free(&f);
    fixture(&f, "in-text", "", numeric_citation,
        "<bibliography><layout><text variable='title'/></layout></bibliography>",
        "{\"items\":[{\"id\":\"a\",\"title\":\"Only included\"}],\"citations\":[]}");
    success(&f); assert(!ce_csl_registry_count(ce_csl_document_registry(f.result)));
    size_t only = 0; f.options.include_items = &only; f.options.include_count = 1; success(&f);
    assert(ce_csl_registry_count(ce_csl_document_registry(f.result)) == 1 && !ce_csl_document_citation_count(f.result)); fixture_free(&f);
}
static const char *ambiguous_citation = "<citation disambiguate-add-year-suffix='true'><layout prefix='(' suffix=')' delimiter='; '>"
    "<group delimiter=', '><names variable='author'><name form='short'/></names><group>"
    "<date variable='issued'><date-part name='year'/></date><text variable='year-suffix'/></group></group></layout></citation>";
static const char *ambiguous_items = "\"items\":[{\"id\":\"z\",\"title\":\"Zulu\",\"year-suffix\":\"WRONG\","
    "\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],\"issued\":{\"date-parts\":[[2000]]}},"
    "{\"id\":\"a\",\"title\":\"Alpha\",\"year-suffix\":\"WRONG\",\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],"
    "\"issued\":{\"date-parts\":[[2000]]}}]";
static char *ambiguous_input(bool both) {
    const char *citations = both ? ",\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"z\"},{\"id\":\"a\"}]}]}" :
        ",\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"a\"}]}]}";
    size_t size = strlen(ambiguous_items) + strlen(citations) + 2; char *text = malloc(size); assert(text);
    snprintf(text, size, "{%s%s", ambiguous_items, citations); return text;
}
static void test_disambiguation_and_delete(void) {
    char *json = ambiguous_input(true); Fixture f;
    fixture(&f, "in-text", "", ambiguous_citation,
        "<bibliography><sort><key variable='title'/></sort><layout><text variable='title'/><text variable='year-suffix'/></layout></bibliography>", json); free(json);
    success(&f); citation_is(&f, 0, "(Doe, 2000b; Doe, 2000a)");
    const CeCslDisambiguation *d = ce_csl_document_disambiguation(f.result);
    assert(!strcmp(ce_csl_disambiguation_item(d, 0)->year_suffix, "b"));
    assert(!strcmp(ce_csl_disambiguation_item(d, 1)->year_suffix, "a"));
    CeCslInput *old_input = f.input; f.input = NULL; json = ambiguous_input(false);
    assert(ce_csl_input_load(&f.input, json, strlen(json), error, sizeof(error))); free(json);
    success(&f); ce_csl_input_free(old_input); citation_is(&f, 0, "(Doe, 2000)");
    d = ce_csl_document_disambiguation(f.result);
    assert(!ce_csl_disambiguation_item(d, 0) && !*ce_csl_disambiguation_item(d, 1)->year_suffix);
    assert(ce_csl_registry_citation_number(ce_csl_document_registry(f.result), 1) == 1);
    size_t include = 0; f.options.include_items = &include; f.options.include_count = 1; success(&f);
    citation_is(&f, 0, "(Doe, 2000a)"); fixture_free(&f);
    fixture(&f, "in-text", "", "<citation disambiguate-add-givenname='true' givenname-disambiguation-rule='primary-name-with-initials'>"
        "<layout delimiter='; '><group delimiter=', '><names variable='author'><name form='short'/></names>"
        "<date variable='issued'><date-part name='year'/></date></group></layout></citation>",
        "<bibliography><layout><text variable='title'/></layout></bibliography>",
        "{\"items\":[{\"id\":\"a\",\"title\":\"A\",\"author\":[{\"family\":\"Doe\",\"given\":\"Alice\"}],\"issued\":{\"date-parts\":[[2000]]}},"
        "{\"id\":\"b\",\"title\":\"B\",\"author\":[{\"family\":\"Doe\",\"given\":\"Bob\"}],\"issued\":{\"date-parts\":[[2001]]}}],"
        "\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"a\"},{\"id\":\"b\"}]}]}");
    success(&f); citation_is(&f, 0, "A. Doe, 2000; B. Doe, 2001");
    cJSON *revision = cJSON_Duplicate(ce_csl_input_data(f.input), true); assert(revision);
    cJSON *cluster = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(revision, "citations"), 0);
    cJSON_DeleteItemFromArray(cJSON_GetObjectItemCaseSensitive(cluster, "citationItems"), 1);
    char *revised = cJSON_PrintUnformatted(revision); assert(revised); cJSON_Delete(revision);
    old_input = f.input; f.input = NULL;
    assert(ce_csl_input_load(&f.input, revised, strlen(revised), error, sizeof(error))); cJSON_free(revised);
    success(&f); ce_csl_input_free(old_input); citation_is(&f, 0, "Doe, 2000"); fixture_free(&f);
}
static const char *group_input = "{\"items\":[{\"id\":\"a\",\"author\":[{\"family\":\"X\"}],\"editor\":[{\"family\":\"Y\"}]},"
    "{\"id\":\"b\",\"author\":[{\"family\":\"Y\"}],\"editor\":[{\"family\":\"Y\"}]}],"
    "\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"a\"},{\"id\":\"b\"},{\"id\":\"a\"}]}]}";
static void test_positions_and_grouping(void) {
    Fixture f; fixture(&f, "in-text", "", "<citation collapse='year'><layout prefix='(' suffix=')' delimiter='; '>"
        "<names variable='author' suffix=' '><name form='short'/></names><choose><if position='ibid'><text value='I'/></if>"
        "<else><text value='N'/></else></choose></layout></citation>", "", group_input);
    success(&f); citation_is(&f, 0, "(X N, I; Y N)"); const size_t order[] = {0, 2, 1}; order_is(&f, 0, order, 3);
    assert(ce_csl_document_pass_count(f.result) >= 2 && ce_csl_document_pass_count(f.result) <= CE_CSL_DOCUMENT_MAX_PASSES);
    CeCslDocument *old = f.result; f.options.max_passes = 1; assert(!build(&f, NULL) && f.result == old); ++cases; fixture_free(&f);
    fixture(&f, "in-text", "", "<citation collapse='year'><layout delimiter='; '><group delimiter=' '><choose>"
        "<if position='ibid'><names variable='editor'><name form='short'/></names></if><else>"
        "<names variable='author'><name form='short'/></names></else></choose><text value='entry'/></group></layout></citation>", "", group_input);
    assert(!build(&f, NULL) && error[0] && !f.result); ++cases; fixture_free(&f);
    const char *notes = "{\"items\":[{\"id\":\"a\",\"first-reference-note-number\":99},{\"id\":\"b\"}],\"citations\":["
        "{\"citationID\":\"n1\",\"properties\":{\"noteIndex\":1},\"citationItems\":[{\"id\":\"a\",\"position\":3,\"near-note\":true}]},"
        "{\"citationID\":\"n2\",\"properties\":{\"noteIndex\":2},\"citationItems\":[{\"id\":\"a\",\"locator\":\"5\"}]},"
        "{\"citationID\":\"n3\",\"properties\":{\"noteIndex\":3},\"citationItems\":[{\"id\":\"b\"}]},"
        "{\"citationID\":\"n4\",\"properties\":{\"noteIndex\":4},\"citationItems\":[{\"id\":\"a\",\"locator\":\"5\"}]},"
        "{\"citationID\":\"t1\",\"citationItems\":[{\"id\":\"a\"}]},{\"citationID\":\"t2\",\"citationItems\":[{\"id\":\"a\"}]}]}";
    fixture(&f, "note", "", "<citation><layout><text variable='first-reference-note-number' suffix='|'/><choose>"
        "<if position='first'><text value='F'/></if><else-if position='ibid-with-locator'><text value='L'/></else-if>"
        "<else-if position='ibid'><text value='I'/></else-if><else-if position='near-note'><text value='N'/></else-if>"
        "<else><text value='S'/></else></choose></layout></citation>", "", notes);
    success(&f); const char *expected[] = {"F", "1|L", "F", "1|N", "F", "I"};
    for (size_t i = 0; i < 6; ++i) citation_is(&f, i, expected[i]);
    fixture_free(&f);
}
static const char *one_input = "{\"items\":[{\"id\":\"a\",\"title\":\"Alpha\"}],"
    "\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"a\"}]}]}";
static void bibliography_is(const Fixture *f, size_t index, const char *expected) {
    const CeCslDocumentBibliography *row = ce_csl_document_bibliography(f->result, index); assert(row && row->entry);
    assert(ce_csl_bibliography_entry_source_item_index(row->entry) == row->item_index);
    CeCslOutputString plain = {0}; const CeCslBibliographyEntry *entries[] = {row->entry};
    assert(ce_csl_bibliography_plain(entries, 1, CE_CSL_BIBLIOGRAPHY_PLAIN_COMPACT, NULL, &plain, error, sizeof(error)));
    if (strcmp(plain.data, expected)) fprintf(stderr, "bibliography %zu: expected <%s> actual <%s>\n", index, expected, plain.data);
    assert(!strcmp(plain.data, expected)); ce_csl_output_string_free(&plain);
}
static void test_bibliography_and_literals(void) {
    Fixture f; fixture(&f, "in-text", "", numeric_citation,
        "<bibliography second-field-align='flush' entry-spacing='0'><layout><text variable='citation-number' prefix='[' suffix=']'/>"
        "<text variable='title'/></layout></bibliography>", one_input);
    success(&f); bibliography_is(&f, 0, "[1] Alpha");
    assert(ce_csl_document_bibliography_count(f.result) == 1 && ce_csl_document_visible_bibliography_count(f.result) == 1);
    const CeCslDocumentBibliography *row = ce_csl_document_bibliography(f.result, 0);
    assert(row->item_index == 0 && row->citation_number == 1 && row->visible);
    assert(ce_csl_bibliography_entry_options(row->entry)->alignment == CE_CSL_BIBLIOGRAPHY_ALIGN_FLUSH);
    assert(ce_csl_bibliography_entry_row(row->entry, 0)->kind == CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS);
    assert(!ce_csl_document_citation(f.result, 1) && !ce_csl_document_bibliography(f.result, 1)); fixture_free(&f);
    fixture(&f, "in-text", "", numeric_citation,
        "<bibliography><layout><text variable='abstract'/></layout></bibliography>", one_input);
    success(&f); row = ce_csl_document_bibliography(f.result, 0);
    assert(row && !row->visible && !ce_csl_document_visible_bibliography_count(f.result)); bibliography_is(&f, 0, ""); fixture_free(&f);
    fixture(&f, "in-text", "", "<citation><layout prefix='(' suffix=')' delimiter='; '><text variable='title'/></layout></citation>", "",
        "{\"items\":[{\"id\":\"a\",\"title\":\"<i>Alpha</i>\"}],\"citations\":[{\"citationID\":\"c\","
        "\"citationItems\":[{\"id\":\"a\",\"prefix\":\"see <b>\",\"suffix\":\" </b>!\"}]}]}");
    success(&f); citation_is(&f, 0, "(see <b>Alpha </b>!)");
    assert(!ce_csl_document_bibliography_count(f.result)); fixture_free(&f);
    assert(!ce_csl_document_citation_count(NULL) && !ce_csl_document_bibliography_count(NULL));
    assert(!ce_csl_document_visible_bibliography_count(NULL) && !ce_csl_document_pass_count(NULL));
    assert(!ce_csl_document_citation(NULL, 0) && !ce_csl_document_bibliography(NULL, 0));
    assert(!ce_csl_document_registry(NULL) && !ce_csl_document_disambiguation(NULL)); ce_csl_document_free(NULL); ++cases;
    fixture(&f, "in-text", "", "<citation collapse='year'><sort><key variable='title'/></sort><layout delimiter='; '>"
        "<names variable='author' suffix=' '><name form='short'/></names><text variable='title'/></layout></citation>", "",
        "{\"items\":[{\"id\":\"a\",\"title\":\"Zulu\",\"author\":[{\"family\":\"Doe\"}]},"
        "{\"id\":\"b\",\"title\":\"Beta\",\"author\":[{\"family\":\"Roe\"}]},{\"id\":\"c\",\"title\":\"Alpha\",\"author\":[{\"family\":\"Doe\"}]}],"
        "\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"a\"},{\"id\":\"b\"},{\"id\":\"c\"}]}]}");
    success(&f); citation_is(&f, 0, "Doe Alpha, Zulu; Roe Beta");
    const size_t composed[] = {2, 0, 1}; order_is(&f, 0, composed, 3); fixture_free(&f);
}
static void test_note_epochs_and_xml_values(void) {
    const char *notes = "{\"items\":[{\"id\":\"a\"},{\"id\":\"b\"}],\"citations\":["
        "{\"citationID\":\"n1\",\"properties\":{\"noteIndex\":1},\"citationItems\":[{\"id\":\"a\"}]},"
        "{\"citationID\":\"n2\",\"properties\":{\"noteIndex\":2},\"citationItems\":[{\"id\":\"b\"}]},"
        "{\"citationID\":\"n3\",\"properties\":{\"noteIndex\":3},\"citationItems\":[{\"id\":\"a\"}]}]}";
    Fixture f; fixture(&f, "note", "", "<citation near-note-distance='+2'><layout><choose><if position='near-note'>"
        "<text value='near'/></if><else><text value='far'/></else></choose></layout></citation>", "", notes);
    success(&f); citation_is(&f, 2, "near"); fixture_free(&f);
    fixture(&f, "note", "", "<citation near-note-distance='0'><layout><choose><if position='near-note'>"
        "<text value='near'/></if><else><text value='far'/></else></choose></layout></citation>", "", notes);
    success(&f); citation_is(&f, 2, "far"); fixture_free(&f);
    fixture(&f, "note", "", "<citation><layout><text variable='first-reference-note-number'/><choose>"
        "<if position='first'><text value='first'/></if><else><text value='later'/></else></choose></layout></citation>", "",
        "{\"items\":[{\"id\":\"a\"}],\"citations\":[{\"citationID\":\"n2\",\"properties\":{\"noteIndex\":2},"
        "\"citationItems\":[{\"id\":\"a\"}]},{\"citationID\":\"n1\",\"properties\":{\"noteIndex\":1},\"citationItems\":[{\"id\":\"a\"}]}]}");
    assert(!build(&f, NULL) && !f.result && error[0]); ++cases;
    f.options.allow_note_number_reset = true; success(&f); citation_is(&f, 0, "first"); citation_is(&f, 1, "first"); fixture_free(&f);
    char *json = ambiguous_input(true);
    fixture(&f, "in-text", "", "<citation disambiguate-add-year-suffix='1'><layout delimiter='; '><names variable='author'>"
        "<name form='short'/></names><text variable='year-suffix'/></layout></citation>", "", json); free(json);
    success(&f); citation_is(&f, 0, "Doea; Doeb"); fixture_free(&f);
}
static void test_subsequent_and_unresolved(void) {
    Fixture f; fixture(&f, "in-text", "", "<citation disambiguate-add-names='true'><layout delimiter='; '>"
        "<names variable='author'><name form='short' et-al-min='3' et-al-use-first='1' et-al-subsequent-min='2' "
        "et-al-subsequent-use-first='1'/></names></layout></citation>", "",
        "{\"items\":[{\"id\":\"a\",\"author\":[{\"family\":\"Doe\"},{\"family\":\"Coe\"}]},"
        "{\"id\":\"b\",\"author\":[{\"family\":\"Doe\"},{\"family\":\"Roe\"}]}],\"citations\":["
        "{\"citationID\":\"a1\",\"citationItems\":[{\"id\":\"a\"}]},{\"citationID\":\"b1\",\"citationItems\":[{\"id\":\"b\"}]},"
        "{\"citationID\":\"a2\",\"citationItems\":[{\"id\":\"a\"}]}]}");
    success(&f); citation_is(&f, 0, "Doe, Coe"); citation_is(&f, 1, "Doe, Roe"); citation_is(&f, 2, "Doe, Coe");
    assert(!ce_csl_disambiguation_unresolved_count(ce_csl_document_disambiguation(f.result))); fixture_free(&f);
    fixture(&f, "in-text", "", "<citation><layout delimiter='; '><text variable='title'/></layout></citation>", "",
        "{\"items\":[{\"id\":\"a\",\"title\":\"Twin\"},{\"id\":\"b\",\"title\":\"Twin\"}],\"citations\":["
        "{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"a\"},{\"id\":\"b\"}]}]}");
    success(&f); citation_is(&f, 0, "Twin; Twin");
    assert(ce_csl_disambiguation_unresolved_count(ce_csl_document_disambiguation(f.result)) > 0); fixture_free(&f);
}
static void rejected(Fixture *f) {
    CeCslDocument *old = f->result;
    assert(!build(f, NULL) && f->result == old && error[0]); ++cases;
}
static void test_unsupported_and_limits(void) {
    const char *keys[] = {"<key variable='year-suffix'/>", "<key variable='first-reference-note-number'/>",
        "<key macro='outer'/>"};
    const char *macro = "<macro name='outer'><text macro='inner'/></macro><macro name='inner'><choose>"
        "<if position='ibid'><text value='I'/></if><else><text variable='title'/></else></choose></macro>";
    for (size_t i = 0; i < 3; ++i) {
        char citation[512]; snprintf(citation, sizeof(citation), "<citation><sort>%s</sort><layout><text variable='title'/></layout></citation>", keys[i]);
        Fixture f; fixture(&f, "in-text", macro, citation, "", one_input); rejected(&f); fixture_free(&f);
    }
    Fixture f; fixture(&f, "in-text", "<macro name='branch'><choose><if disambiguate='true'><text variable='title'/></if>"
        "<else><text value='plain'/></else></choose></macro>",
        "<citation><sort><key macro='branch'/></sort><layout><text variable='title'/></layout></citation>", "", one_input);
    rejected(&f); fixture_free(&f);
    fixture(&f, "in-text", "", numeric_citation,
        "<bibliography><sort><key variable='citation-number'/></sort><layout><text variable='title'/></layout></bibliography>", one_input);
    rejected(&f); fixture_free(&f);
    fixture(&f, "in-text", "", numeric_citation,
        "<bibliography subsequent-author-substitute='---'><layout><text variable='title'/></layout></bibliography>", one_input);
    rejected(&f); fixture_free(&f);
    char *ambiguous = ambiguous_input(true);
    fixture(&f, "in-text", "", ambiguous_citation,
        "<bibliography><layout><text variable='title'/></layout></bibliography>", ambiguous); free(ambiguous);
    rejected(&f); fixture_free(&f);
    fixture(&f, "in-text", "", numeric_citation, "",
        "{\"items\":[{\"id\":\"a\"}],\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"a\",\"author-only\":true}]}]}");
    rejected(&f); fixture_free(&f);
    const char *properties[] = {"\"mode\":\"author-only\"", "\"mode\":\"composite\"", "\"unsorted\":true"};
    for (size_t i = 0; i < 3; ++i) {
        char json[512]; snprintf(json, sizeof(json), "{\"items\":[{\"id\":\"a\"}],\"citations\":[{\"citationID\":\"c\","
            "\"properties\":{%s},\"citationItems\":[{\"id\":\"a\"}]}]}", properties[i]);
        fixture(&f, "in-text", "", numeric_citation, "", json); rejected(&f); fixture_free(&f);
    }
    fixture(&f, "in-text", "", "<citation><sort><key variable='title'/><key variable='citation-number'/></sort>"
        "<layout><text variable='title'/></layout></citation>", "<bibliography><layout><text variable='title'/></layout></bibliography>", one_input);
    success(&f); CeCslDocument *old = f.result;
    f.options.max_workspace_bytes = 1024; rejected(&f); f.options = (CeCslDocumentOptions){0};
    f.options.max_output_bytes = 1; rejected(&f); f.options = (CeCslDocumentOptions){0};
    f.options.max_output_bytes = 5; rejected(&f); f.options = (CeCslDocumentOptions){0};
    f.options.max_evaluations = 1; rejected(&f); f.options = (CeCslDocumentOptions){0};
    f.options.max_sort_cells = 1; rejected(&f); f.options = (CeCslDocumentOptions){0};
    f.options.max_passes = CE_CSL_DOCUMENT_MAX_PASSES + 1; rejected(&f); f.options = (CeCslDocumentOptions){0};
    f.options.max_output_bytes = CE_CSL_DOCUMENT_MAX_OUTPUT_BYTES + 1; rejected(&f); f.options = (CeCslDocumentOptions){0};
    f.options.collation_language = (CeCslCollationLanguage)99; rejected(&f); f.options = (CeCslDocumentOptions){0};
    f.options.include_count = 1; rejected(&f); f.options = (CeCslDocumentOptions){0};
    size_t invalid = 1; f.options.include_items = &invalid; f.options.include_count = 1; rejected(&f); f.options = (CeCslDocumentOptions){0};
    assert(!ce_csl_document_build(NULL, f.locale, f.input, NULL, NULL, &f.result, error, sizeof(error)) && f.result == old);
    assert(!ce_csl_document_build(f.style, NULL, f.input, NULL, NULL, &f.result, error, sizeof(error)) && f.result == old);
    assert(!ce_csl_document_build(f.style, f.locale, NULL, NULL, NULL, &f.result, error, sizeof(error)) && f.result == old);
    assert(!ce_csl_document_build(f.style, f.locale, f.input, NULL, NULL, NULL, error, sizeof(error)));
    CeCslOutputAllocator invalid_allocator = {0}; assert(!build(&f, &invalid_allocator) && f.result == old);
    citation_is(&f, 0, "Alpha"); bibliography_is(&f, 0, "Alpha"); ++cases; fixture_free(&f);
}
typedef union AllocationHeader {
    long double alignment;
    void *pointer;
    unsigned char padding[32];
    struct { size_t bytes; unsigned long magic; } value;
} AllocationHeader;
typedef struct Memory { size_t calls, fail_at, live, bytes, peak; } Memory;
static void *test_allocate(void *context, size_t bytes) {
    Memory *m = context;
    if (m->calls++ == m->fail_at) return NULL;
    assert(bytes <= SIZE_MAX - sizeof(AllocationHeader));
    AllocationHeader *h = malloc(sizeof(*h) + bytes); assert(h);
    h->value.bytes = bytes; h->value.magic = 0xc51d0cUL; ++m->live; m->bytes += bytes;
    if (m->bytes > m->peak) m->peak = m->bytes;
    return h + 1;
}
static void test_free(void *context, void *memory) {
    if (!memory) return;
    Memory *m = context; AllocationHeader *h = (AllocationHeader *)memory - 1;
    assert(h->value.magic == 0xc51d0cUL && m->live && m->bytes >= h->value.bytes);
    h->value.magic = 0; --m->live; m->bytes -= h->value.bytes; free(h);
}
typedef bool (*CitationExport)(const CeCslDocument *, size_t, const CeCslOutputAllocator *, CeCslOutputString *, char *, size_t);
static size_t test_independent_exports(void) {
    Fixture f; fixture(&f, "in-text", "", "<citation><layout prefix='(' suffix=')'>"
        "<text variable='title' font-style='italic'/></layout></citation>", "",
        "{\"items\":[{\"id\":\"a\",\"title\":\"Alpha & β\"}],\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"a\"}]}]}");
    Memory source = {.fail_at = SIZE_MAX}; CeCslOutputAllocator source_allocator = {&source, test_allocate, test_free};
    assert(build(&f, &source_allocator)); assert(source.live);
    const size_t source_live = source.live, source_calls = source.calls, source_bytes = source.bytes;
    CitationExport exports[] = {ce_csl_document_citation_plain, ce_csl_document_citation_html};
    const char *expected[] = {"(Alpha & β)", "<span style=\"white-space:pre-wrap\">(<em>Alpha &amp; β</em>)</span>"};
    CeCslOutputString defaults[2] = {0}, customs[2] = {0};
    Memory custom_memory[2] = {0};
    CeCslOutputAllocator custom_allocators[2]; size_t failures = 0;
    for (size_t i = 0; i < 2; ++i) {
        assert(exports[i](f.result, 0, NULL, &defaults[i], error, sizeof(error)));
        assert(!strcmp(defaults[i].data, expected[i]));
        char *old_data = defaults[i].data; size_t old_length = defaults[i].length;
        assert(!exports[i](NULL, 0, NULL, &defaults[i], error, sizeof(error)) && error[0]);
        assert(!exports[i](f.result, 1, NULL, &defaults[i], error, sizeof(error)) && error[0]);
        assert(!exports[i](f.result, SIZE_MAX, NULL, &defaults[i], NULL, 0));
        assert(!exports[i](f.result, 0, NULL, NULL, error, sizeof(error)) && error[0]);
        CeCslOutputAllocator bad = {0};
        assert(!exports[i](f.result, 0, &bad, &defaults[i], error, sizeof(error)) && error[0]);
        assert(defaults[i].data == old_data && defaults[i].length == old_length && !strcmp(defaults[i].data, expected[i]));
        Memory memory = {.fail_at = SIZE_MAX}; CeCslOutputAllocator allocator = {&memory, test_allocate, test_free};
        CeCslOutputString probe = {0}; assert(exports[i](f.result, 0, &allocator, &probe, error, sizeof(error)));
        size_t calls = memory.calls; assert(calls && calls < 100 && memory.live == 1);
        ce_csl_output_string_free(&probe); assert(!memory.live && !memory.bytes);
        for (size_t at = 0; at < calls; ++at) {
            memory = (Memory){.fail_at = at};
            assert(!exports[i](f.result, 0, &allocator, &defaults[i], error, sizeof(error)) && error[0]);
            assert(!memory.live && !memory.bytes && defaults[i].data == old_data && defaults[i].length == old_length);
            assert(!strcmp(defaults[i].data, expected[i])); ++failures;
        }
        custom_memory[i].fail_at = SIZE_MAX;
        custom_allocators[i] = (CeCslOutputAllocator){&custom_memory[i], test_allocate, test_free};
        assert(exports[i](f.result, 0, &custom_allocators[i], &customs[i], error, sizeof(error)));
        assert(custom_memory[i].live == 1 && !strcmp(customs[i].data, expected[i]));
        /* Replacing an independently exported string must release it through
         * its original allocator, even when the new export chooses another. */
        memory = (Memory){.fail_at = SIZE_MAX};
        assert(exports[i](f.result, 0, &allocator, &probe, error, sizeof(error)));
        assert(memory.live == 1);
        assert(exports[i](f.result, 0, NULL, &probe, error, sizeof(error)));
        assert(!memory.live && !memory.bytes && !strcmp(probe.data, expected[i])); ce_csl_output_string_free(&probe);
        ++cases;
    }
    assert(source.live == source_live && source.bytes == source_bytes && source.calls == source_calls);
    fixture_free(&f); assert(!source.live && !source.bytes);
    for (size_t i = 0; i < 2; ++i) {
        assert(defaults[i].length == strlen(expected[i]) && !strcmp(defaults[i].data, expected[i]));
        assert(customs[i].length == strlen(expected[i]) && !strcmp(customs[i].data, expected[i]));
        ce_csl_output_string_free(&defaults[i]); ce_csl_output_string_free(&customs[i]);
        assert(!custom_memory[i].live && !custom_memory[i].bytes);
    }
    fixture(&f, "in-text", "", "<citation><layout><text variable='abstract'/></layout></citation>", "", one_input);
    success(&f); assert(ce_csl_document_citation_plain(f.result, 0, NULL, &defaults[0], error, sizeof(error)));
    assert(ce_csl_document_citation_html(f.result, 0, NULL, &defaults[1], error, sizeof(error)));
    fixture_free(&f); assert(defaults[0].data && !defaults[0].length && !*defaults[0].data);
    assert(!strcmp(defaults[1].data, "<span style=\"white-space:pre-wrap\"></span>"));
    ce_csl_output_string_free(&defaults[0]); ce_csl_output_string_free(&defaults[1]);
    printf("independent citation exports: %zu allocation failures\n", failures); return failures;
}
typedef struct Cancel { size_t calls, stop; } Cancel;
static bool cancelled(void *context) { Cancel *c = context; return ++c->calls == c->stop; }
static size_t test_atomic_oom_and_cancel(void) {
    Fixture f; char *json = ambiguous_input(true);
    fixture(&f, "in-text", "", ambiguous_citation,
        "<bibliography><sort><key variable='title'/></sort><layout><group delimiter=' '><text variable='citation-number'/>"
        "<text variable='title'/><text variable='year-suffix'/></group></layout></bibliography>", json); free(json);
    char *before = cJSON_PrintUnformatted(ce_csl_input_data(f.input)); assert(before);
    success(&f); CeCslDocument *old = f.result;
    Memory m = {.fail_at = SIZE_MAX}; CeCslOutputAllocator allocator = {&m, test_allocate, test_free};
    CeCslDocument *probe = NULL;
    assert(ce_csl_document_build(f.style, f.locale, f.input, NULL, &allocator, &probe, error, sizeof(error)));
    size_t calls = m.calls, peak = m.peak; assert(calls && calls < 50000 && m.live);
    ce_csl_document_free(probe); assert(!m.live && !m.bytes);
    for (size_t at = 0; at < calls; ++at) {
        m = (Memory){.fail_at = at};
        bool ok = build(&f, &allocator);
        if (ok) fprintf(stderr, "allocation failure %zu unexpectedly succeeded\n", at);
        assert(!ok && f.result == old && error[0] && !m.live && !m.bytes);
        citation_is(&f, 0, "(Doe, 2000b; Doe, 2000a)");
    }
    Cancel cancel = {.stop = SIZE_MAX}; f.options.cancel_context = &cancel; f.options.cancelled = cancelled;
    m = (Memory){.fail_at = SIZE_MAX}; assert(build(&f, &allocator)); old = f.result;
    size_t checkpoints = cancel.calls, live = m.live, bytes = m.bytes; assert(checkpoints > 5);
    for (size_t at = 1; at <= checkpoints; ++at) {
        cancel = (Cancel){.stop = at};
        assert(!build(&f, &allocator) && f.result == old && error[0] && m.live == live && m.bytes == bytes);
    }
    char *after = cJSON_PrintUnformatted(ce_csl_input_data(f.input)); assert(after && !strcmp(before, after));
    cJSON_free(before); cJSON_free(after);
    citation_is(&f, 0, "(Doe, 2000b; Doe, 2000a)"); bibliography_is(&f, 0, "1 Alpha a");
    f.options = (CeCslDocumentOptions){0}; fixture_free(&f); assert(!m.live && !m.bytes);
    printf("document OOM: %zu failures, %zu cancellation checkpoints, peak %zu bytes\n", calls, checkpoints, peak);
    ++cases; return calls;
}
static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb"); assert(file); assert(!fseek(file, 0, SEEK_END));
    long length = ftell(file); assert(length >= 0); rewind(file);
    char *data = malloc((size_t)length + 1); assert(data);
    assert(fread(data, 1, (size_t)length, file) == (size_t)length); assert(!fclose(file)); data[length] = 0; return data;
}
static void real_fixture(Fixture *f, const char *path, const CeCslLocaleCatalog *catalog, const char *json) {
    memset(f, 0, sizeof(*f)); char *text = read_file(path);
    assert(ce_csl_style_load(&f->style, text, strlen(text), error, sizeof(error))); free(text);
    f->locale = ce_csl_locale_new(ce_csl_style_root(f->style), catalog, NULL, NULL, error, sizeof(error)); assert(f->locale);
    assert(ce_csl_input_load(&f->input, json, strlen(json), error, sizeof(error)));
}
static void real_profiles(char **argv) {
    CeCslXml *xml[2]; const CeCslXmlNode *roots[2];
    for (size_t i = 0; i < 2; ++i) {
        char *text = read_file(argv[i + 3]); xml[i] = ce_csl_xml_parse(text, strlen(text), error, sizeof(error)); free(text);
        assert(xml[i]); roots[i] = xml[i]->root;
    }
    CeCslLocaleCatalog catalog = {.roots = roots, .root_count = 2};
    const char *json = "{\"items\":[{\"id\":\"unused\",\"type\":\"book\",\"title\":\"Unused\"},"
        "{\"id\":\"a\",\"type\":\"book\",\"title\":\"Example A\",\"publisher\":\"Press\",\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],\"issued\":{\"date-parts\":[[2019]]}},"
        "{\"id\":\"b\",\"type\":\"book\",\"title\":\"Example B\",\"publisher\":\"Press\",\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],\"issued\":{\"date-parts\":[[2020]]}},"
        "{\"id\":\"c\",\"type\":\"book\",\"title\":\"Example C\",\"publisher\":\"Press\",\"author\":[{\"family\":\"Roe\",\"given\":\"Richard\"}],\"issued\":{\"date-parts\":[[2021]]}}],"
        "\"citations\":[{\"citationID\":\"one\",\"citationItems\":[{\"id\":\"b\"}]},{\"citationID\":\"two\",\"citationItems\":[{\"id\":\"a\"}]},"
        "{\"citationID\":\"three\",\"citationItems\":[{\"id\":\"c\"}]},{\"citationID\":\"four\",\"citationItems\":[{\"id\":\"c\"},{\"id\":\"b\"},{\"id\":\"a\"}]}]}";
    const char *expected[] = {"(Doe, 2019, 2020; Roe, 2021)", "[1], [2], [3]", "[1–3]"};
    const size_t apa_order[] = {2, 1, 0}, numeric_order[] = {1, 2, 0};
    for (size_t s = 0; s < 3; ++s) {
        Fixture f; real_fixture(&f, argv[s], &catalog, json); success(&f); citation_is(&f, 3, expected[s]);
        order_is(&f, 3, s ? numeric_order : apa_order, 3);
        assert(ce_csl_registry_count(ce_csl_document_registry(f.result)) == 3);
        assert(!ce_csl_registry_citation_number(ce_csl_document_registry(f.result), 0));
        assert(ce_csl_document_bibliography_count(f.result) == 3 && ce_csl_document_visible_bibliography_count(f.result) == 3);
        const CeCslBibliographyEntry *entries[3];
        for (size_t i = 0; i < 3; ++i) {
            const CeCslDocumentBibliography *row = ce_csl_document_bibliography(f.result, i);
            assert(row && row->citation_number == i + 1 && row->visible);
            assert(row->item_index == (s ? (i == 0 ? 2 : i == 1 ? 1 : 3) : i + 1)); entries[i] = row->entry;
        }
        CeCslOutputString plain = {0}, html = {0};
        assert(ce_csl_bibliography_plain(entries, 3, CE_CSL_BIBLIOGRAPHY_PLAIN_COMPACT, NULL, &plain, error, sizeof(error)));
        assert(strstr(plain.data, "Example A") && strstr(plain.data, "Example B") && strstr(plain.data, "Example C") && !strstr(plain.data, "Unused"));
        assert(ce_csl_bibliography_html(entries, 3, NULL, NULL, &html, error, sizeof(error)) && html.length);
        const char *first_bibliography[] = {"Doe, J. (2019). Example A. Press.",
            "[1] J. Doe, Example B. Press, 2020.", "[1] DOE J. Example B[M]. Press, 2020."};
        bibliography_is(&f, 0, first_bibliography[s]);
        if (s == 2) {
            const CeCslOutput *output = ce_csl_document_citation(f.result, 3)->output;
            for (size_t i = 0; i < ce_csl_output_run_count(output); ++i) assert(ce_csl_output_run(output, i)->format & CE_CSL_SUPERSCRIPT);
        }
        ce_csl_output_string_free(&plain); ce_csl_output_string_free(&html); fixture_free(&f);
        printf("real document profile %zu passed\n", s);
    }
    Fixture f; real_fixture(&f, argv[0], &catalog,
        "{\"items\":[{\"id\":\"z\",\"type\":\"book\",\"title\":\"Zulu\",\"author\":[{\"family\":\"Doe\",\"given\":\"Alice\"}],\"issued\":{\"date-parts\":[[2024]]}},"
        "{\"id\":\"a\",\"type\":\"book\",\"title\":\"Alpha\",\"author\":[{\"family\":\"Doe\",\"given\":\"Andrew\"}],\"issued\":{\"date-parts\":[[2024]]}}],"
        "\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"z\"},{\"id\":\"a\"}]}]}");
    success(&f); assert(!ce_csl_disambiguation_unresolved_count(ce_csl_document_disambiguation(f.result)));
    /* APA's macro keeps initialized authors; equal cite keys preserve z,a,
     * while the bibliography's later title key assigns Alpha=a, Zulu=b. */
    citation_is(&f, 0, "(Doe, 2024b, 2024a)");
    bibliography_is(&f, 0, "Doe, A. (2024a). Alpha.");
    bibliography_is(&f, 1, "Doe, A. (2024b). Zulu."); fixture_free(&f);
    real_fixture(&f, argv[0], &catalog,
        "{\"items\":[{\"id\":\"private\",\"type\":\"personal_communication\",\"title\":\"Private communication\","
        "\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],\"issued\":{\"date-parts\":[[2024,1,2]]}}],"
        "\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"private\"}]}]}");
    success(&f); assert(ce_csl_document_bibliography_count(f.result) == 1 && !ce_csl_document_visible_bibliography_count(f.result));
    bibliography_is(&f, 0, ""); assert(ce_csl_document_citation(f.result, 0)->cite_count == 1); fixture_free(&f);
    for (size_t i = 0; i < 2; ++i) ce_csl_xml_free(xml[i]);
}
int main(int argc, char **argv) {
    test_eligibility_and_numbering(); test_disambiguation_and_delete(); test_positions_and_grouping();
    test_bibliography_and_literals(); test_note_epochs_and_xml_values(); test_subsequent_and_unresolved(); test_unsupported_and_limits();
    size_t failures = test_atomic_oom_and_cancel() + test_independent_exports();
    if (argc == 6) real_profiles(argv + 1); else assert(argc == 1);
    printf("CSL document passed (%zu owned cases, %zu allocation failures)\n", cases, failures); return 0;
}
