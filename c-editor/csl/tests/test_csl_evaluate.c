#include "csl_evaluate.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[512];
typedef struct Fixture {
    CeCslStyle *style;
    CeCslLocale *locale;
    cJSON *item_json, *cite_json;
    CeCslInputItem item;
    CeCslInputCite cite;
    CeCslGeneratedVariables generated;
    CeCslEvalRequest request;
    CeCslEvaluation *result;
} Fixture;
static void fixture_options(Fixture *f, const char *style_options, const char *macros, const char *section, const char *item, const char *cite) {
    memset(f, 0, sizeof(*f));
    const char *head = "<style xmlns='http://purl.org/net/xbiblio/csl' version='1.0' class='in-text' default-locale='en-US' %s>"
        "<info><title>Owned evaluator regression</title><id>https://example.invalid/evaluator</id>"
        "<updated>2026-01-01T00:00:00+00:00</updated></info>"
        "<locale><terms><term name='open-quote'>“</term><term name='close-quote'>”</term>"
        "<term name='open-inner-quote'>‘</term><term name='close-inner-quote'>’</term>"
        "<term name='page'><single>page</single><multiple>pages</multiple></term>"
        "<term name='volume'><single>volume</single><multiple>volumes</multiple></term>"
        "<term name='edition' gender='feminine'>edition</term>"
        "<term name='ordinal'>th</term><term name='ordinal-01' match='last-digit'>st</term>"
        "<term name='et-al'>et al.</term><term name='and'>and</term><term name='month-01'>January</term>"
        "<term name='no date' form='short'>n.d.</term>"
        "</terms></locale>";
    bool bibliography = !strncmp(section, "<bibliography", 13);
    const char *citation = bibliography ? "<citation><layout><text value='unused'/></layout></citation>" : "";
    size_t size = strlen(head) + strlen(style_options ? style_options : "") + strlen(macros ? macros : "") + strlen(section) + strlen(citation) + 16;
    char *xml = malloc(size); assert(xml);
    int used = snprintf(xml, size, head, style_options ? style_options : ""); assert(used > 0 && (size_t)used < size);
    snprintf(xml + used, size - (size_t)used, "%s%s%s</style>", macros ? macros : "", citation, section);
    bool ok = ce_csl_style_load(&f->style, xml, strlen(xml), error, sizeof(error));
    if (!ok) fprintf(stderr, "style: %s\n", error);
    assert(ok); free(xml);
    f->locale = ce_csl_locale_new(ce_csl_style_root(f->style), NULL, "en-US", NULL, error, sizeof(error));
    if (!f->locale) fprintf(stderr, "locale: %s\n", error);
    assert(f->locale);
    f->item_json = cJSON_Parse(item ? item : "{}"); assert(f->item_json);
    f->item = (CeCslInputItem){.id = "owned-item", .data = f->item_json};
    if (cite) { f->cite_json = cJSON_Parse(cite); assert(f->cite_json); f->cite.data = f->cite_json; }
    f->generated.citation_number = (CeCslGeneratedNumber){true, true, 7};
    f->generated.first_reference_note_number = (CeCslGeneratedNumber){true, false, 0};
    f->generated.year_suffix = (CeCslGeneratedText){true, "", 0};
    f->generated.citation_label = (CeCslGeneratedText){true, "", 0};
    f->request = (CeCslEvalRequest){.style = f->style, .locale = f->locale,
        .section = ce_csl_style_citation(f->style), .position = CE_CSL_POSITION_FIRST,
        .variables = {.item = &f->item, .cite = cite ? &f->cite : NULL, .generated = &f->generated}};
    if (bibliography) f->request.section = ce_csl_style_bibliography(f->style);
}
static void fixture(Fixture *f, const char *macros, const char *section, const char *item, const char *cite) {
    fixture_options(f, NULL, macros, section, item, cite);
}
static void fixture_free(Fixture *f) {
    ce_csl_evaluation_free(f->result); ce_csl_locale_free(f->locale); ce_csl_style_free(f->style);
    cJSON_Delete(f->item_json); cJSON_Delete(f->cite_json);
}
static void evaluate(Fixture *f) {
    bool ok = ce_csl_evaluate(&f->request, NULL, &f->result, error, sizeof(error));
    if (!ok) fprintf(stderr, "evaluate: %s\n", error);
    assert(ok);
}
static void text_is(const CeCslInline *value, const char *expected) {
    size_t size; const char *text = ce_csl_inline_text(value, &size);
    if (!text || size != strlen(expected) || memcmp(text, expected, size))
        fprintf(stderr, "expected <%s>, actual <%.*s>\n", expected, (int)size, text ? text : "NULL");
    assert(text && size == strlen(expected) && !memcmp(text, expected, size));
}
static void preview_is(Fixture *f, const char *expected) { evaluate(f); text_is(ce_csl_eval_node_preview(ce_csl_evaluation_root(f->result)), expected); }
static const CeCslEvalNode *child(const CeCslEvalNode *n, size_t index) {
    n = ce_csl_eval_node_info(n)->first_child;
    while (n && index--) n = ce_csl_eval_node_info(n)->next;
    return n;
}
static void test_groups(void) {
    Fixture f;
    fixture(&f, NULL, "<citation><layout><group delimiter='|'><text value='unwanted'/><text variable='title' strip-periods='true'/></group></layout></citation>", "{\"title\":\".\"}", NULL);
    preview_is(&f, "");
    const CeCslEvalNode *g = child(ce_csl_evaluation_root(f.result), 0);
    const CeCslEvalNodeInfo *info = ce_csl_eval_node_info(g);
    assert(info->metadata_present && info->variable_called && !info->rendered_variable_present);
    assert(info->suppression == CE_CSL_EVAL_ALL_GROUP_VARIABLES_EMPTY);
    text_is(ce_csl_eval_node_content(child(g, 1)), ".");
    fixture_free(&f);
    fixture(&f, "<macro name='fixed'><text value='macro'/></macro>",
        "<citation><layout><group delimiter='|'><text variable='title'/><group><text value='nested'/></group><text macro='fixed'/></group></layout></citation>", "{}", NULL);
    preview_is(&f, "nested|macro"); fixture_free(&f);
    fixture(&f, NULL, "<citation><layout><text value='kept'/><group><text value='unwanted'/><date variable='issued'><date-part name='month'/></date></group></layout></citation>", "{\"issued\":{\"date-parts\":[[2025]]}}", NULL);
    preview_is(&f, "kept");
    const CeCslEvalNode *date = child(child(ce_csl_evaluation_root(f.result), 1), 1);
    assert(ce_csl_eval_node_date_value(date)->endpoints[0].year == 2025);
    assert(ce_csl_eval_node_date_parts(date) == CE_CSL_DATE_PART_MONTH);
    assert(!ce_csl_eval_node_date(date)->rendered && ce_csl_eval_node_info(date)->metadata_present);
    fixture_free(&f);
    fixture(&f, NULL, "<citation><layout><date variable='issued'><date-part name='year'/></date></layout></citation>", "{\"issued\":{\"date-parts\":[[0]]}}", NULL);
    preview_is(&f, ""); date = child(ce_csl_evaluation_root(f.result), 0);
    assert(ce_csl_eval_node_date_value(date)->endpoint_count == 1);
    assert(ce_csl_eval_node_date_value(date)->endpoints[0].year == 0);
    assert(!ce_csl_eval_node_date_value(date)->present);
    fixture_free(&f);
}
static bool suppress_suffix(void *context, const char *name, bool *value, char *message, size_t capacity) {
    (void)context; (void)message; (void)capacity;
    *value = !strcmp(name, "year-suffix");
    return true;
}
static void test_year_suffix_groups(void) {
    /* Own regression, informed by official test-suite 6eefc5b fixture
     * date_YearSuffixImplicitWithNoDateOneOnly and isolated citeproc 2.4.63
     * probes. A suffix success is not a claim that any letters rendered. */
    Fixture f;
    const char *section = "<citation><layout><group delimiter='-'><choose><if variable='issued'>"
        "<date variable='issued'><date-part name='year'/></date><text variable='year-suffix'/></if>"
        "<else><text term='no date' form='short'/><text variable='year-suffix'/></else>"
        "</choose></group></layout></citation>";
    fixture(&f, NULL, section, "{}", NULL);
    preview_is(&f, "n.d.");
    const CeCslEvalNode *group = child(ce_csl_evaluation_root(f.result), 0);
    const CeCslEvalNodeInfo *info = ce_csl_eval_node_info(group);
    assert(info->variable_called && !info->metadata_present && !info->rendered_variable_present);
    assert(info->nonempty && info->suppression == CE_CSL_EVAL_NOT_SUPPRESSED);
    const CeCslEvalNode *suffix = child(child(group, 0), 1);
    assert(ce_csl_eval_node_variable(suffix, 0)->value.state == CE_CSL_VARIABLE_EMPTY);
    assert(!ce_csl_eval_node_variable(suffix, 0)->rendered);
    f.generated.year_suffix = (CeCslGeneratedText){true, "a", 1};
    preview_is(&f, "n.d.-a");
    info = ce_csl_eval_node_info(child(ce_csl_evaluation_root(f.result), 0));
    assert(info->metadata_present && info->rendered_variable_present);
    f.generated.year_suffix = (CeCslGeneratedText){true, "", 0};
    f.request.variables.suppressed = suppress_suffix;
    preview_is(&f, "");
    f.request.variables.suppressed = NULL;
    preview_is(&f, "n.d.");
    fixture_free(&f);

    fixture(&f, NULL, "<citation><layout><group><text value='kept'/><text variable='URL'/>"
        "<text variable='year-suffix'/></group><group><text value='drop'/><text variable='URL'/></group>"
        "</layout></citation>", "{}", NULL);
    preview_is(&f, "kept");
    assert(ce_csl_eval_node_info(child(ce_csl_evaluation_root(f.result), 1))->suppression ==
           CE_CSL_EVAL_ALL_GROUP_VARIABLES_EMPTY);
    fixture_free(&f);

    fixture(&f, NULL, "<citation><layout><group><text variable='URL'/><group>"
        "<text term='no date' form='short'/><text variable='year-suffix'/></group></group>"
        "</layout></citation>", "{}", NULL);
    preview_is(&f, "n.d."); fixture_free(&f);

    /* No success leaks from unchosen branches, empty container boundaries,
     * or an empty substitute candidate whose transaction was discarded. */
    fixture(&f, "<macro name='empty'><text variable='year-suffix'/></macro>",
        "<citation><layout><group><text value='drop1'/><text variable='URL'/><choose>"
        "<if variable='title'><text variable='year-suffix'/></if></choose></group>"
        "<group><text value='drop2'/><text variable='URL'/><text macro='empty'/></group>"
        "<group><text value='drop3'/><text variable='URL'/><group><text variable='year-suffix'/></group></group>"
        "<group><names variable='author'><name/><substitute><text variable='year-suffix'/></substitute>"
        "</names><text value='drop4'/></group></layout></citation>", "{}", NULL);
    preview_is(&f, ""); fixture_free(&f);

    /* A genuinely rendered substitute consumes the suffix: a later call is
     * suppressed and must not rescue its surrounding constant text. */
    fixture(&f, NULL, "<citation><layout><names variable='author'><name/><substitute>"
        "<text variable='year-suffix'/></substitute></names><group><text value='drop'/>"
        "<text variable='year-suffix'/></group></layout></citation>", "{}", NULL);
    f.generated.year_suffix = (CeCslGeneratedText){true, "b", 1};
    preview_is(&f, "b");
    suffix = child(child(ce_csl_evaluation_root(f.result), 1), 1);
    assert(ce_csl_eval_node_variable(suffix, 0)->value.state == CE_CSL_VARIABLE_SUPPRESSED);
    fixture_free(&f);
}
static void test_delimiters_and_conditions(void) {
    Fixture f;
    fixture(&f, "<macro name='pair'><text value='X'/><text value='Y'/></macro>",
        "<citation><layout prefix='(' suffix=')' delimiter='; '><group delimiter='|'>"
        "<text value='A'/><choose><if variable='title'><text value='B'/><text value='C'/></if><else><text value='wrong'/></else></choose>"
        "<group><text value='D'/><text value='E'/></group><text macro='pair'/></group><text variable='citation-number'/></layout></citation>", "{\"title\":\"yes\"}", NULL);
    preview_is(&f, "A|B|C|DE|XY7");
    const CeCslEvalNodeInfo *root = ce_csl_eval_node_info(ce_csl_evaluation_root(f.result));
    assert(root->decoration.cluster_envelope && !strcmp(root->decoration.delimiter, "; "));
    fixture_free(&f);
    fixture(&f, NULL, "<citation><layout><choose><if position='ibid-with-locator'><text value='I'/></if><else-if is-numeric='volume' variable='issued' match='all'><text value='N'/></else-if><else><text value='E'/></else></choose></layout></citation>", "{\"volume\":\"D2\",\"issued\":{\"date-parts\":[[2020]]}}", NULL);
    preview_is(&f, "N"); f.request.position = CE_CSL_POSITION_IBID_WITH_LOCATOR;
    preview_is(&f, "I"); fixture_free(&f);
}
static void test_substitute(void) {
    Fixture f;
    fixture(&f, NULL, "<citation><layout><group delimiter='|'><names variable='author'><name/><substitute><names variable='editor'/><text variable='title'/></substitute></names><names variable='editor'><name/></names><text variable='title'/></group></layout></citation>", "{\"editor\":[{\"family\":\"Doe\"}],\"title\":\"Book\"}", NULL);
    preview_is(&f, "Doe|Book");
    const CeCslEvalNode *g = child(ce_csl_evaluation_root(f.result), 0), *names = child(g, 0);
    assert(ce_csl_eval_node_info(names)->used_substitute && !ce_csl_eval_node_content(names));
    assert(ce_csl_eval_node_variable(child(g, 1), 0)->value.state == CE_CSL_VARIABLE_SUPPRESSED);
    fixture_free(&f);
    fixture(&f, "<macro name='empty'><text variable='title' strip-periods='true'/></macro>",
        "<citation><layout><group delimiter='|'><names variable='author'><name/><substitute><text macro='empty'/><text variable='page-first'/></substitute></names><text variable='page'/><text variable='title'/></group></layout></citation>", "{\"title\":\".\",\"page\":\"S001-S009\"}", NULL);
    preview_is(&f, "S001|.");
    assert(ce_csl_evaluation_summary(f.result)->node_count == 6);
    g = child(ce_csl_evaluation_root(f.result), 0);
    assert(ce_csl_eval_node_variable(child(g, 1), 0)->value.state == CE_CSL_VARIABLE_SUPPRESSED);
    assert(ce_csl_eval_node_variable(child(g, 2), 0)->value.state == CE_CSL_VARIABLE_VALUE);
    fixture_free(&f);
    fixture(&f, NULL, "<citation><layout><group delimiter='|'><text variable='title'/><names variable='author'><name/><substitute><text variable='title'/></substitute></names><text variable='title'/></group></layout></citation>", "{\"title\":\"Book\"}", NULL);
    preview_is(&f, "Book|Book"); fixture_free(&f);
    fixture(&f, "<macro name='fixed'><text value='Anon'/></macro>",
        "<citation><layout><group><names variable='author'><name/><substitute><text macro='fixed'/></substitute></names><text value='!'/></group></layout></citation>", "{}", NULL);
    preview_is(&f, "Anon!"); fixture_free(&f);
    fixture(&f, NULL,
        "<citation><layout><group><names variable='author'><name/><substitute><group><text value='Anon'/></group></substitute></names><text value='!'/></group></layout></citation>", "{}", NULL);
    preview_is(&f, "Anon!"); fixture_free(&f);
    fixture(&f, NULL,
        "<citation><layout><group><names variable='author'><name/><substitute><text value='Anon'/></substitute></names><text value='!'/></group></layout></citation>", "{}", NULL);
    preview_is(&f, ""); fixture_free(&f);
    fixture(&f, NULL, "<citation><layout><names variable='author'><name/><substitute><label variable='page'/></substitute></names><text variable='page' prefix='|'/></layout></citation>", "{\"page\":\"5\"}", NULL);
    preview_is(&f, "page|5"); fixture_free(&f);
    fixture(&f, NULL, "<citation><layout><group><label variable='page'/><text variable='URL'/></group><group><label variable='page'/><text value='!'/></group></layout></citation>", "{\"page\":\"5\"}", NULL);
    preview_is(&f, "page!");
    const CeCslEvalNode *label = child(child(ce_csl_evaluation_root(f.result), 1), 0);
    assert(ce_csl_eval_node_info(label)->metadata_present && !ce_csl_eval_node_info(label)->variable_called);
    assert(!ce_csl_eval_node_variable(label, 0)->rendered);
    fixture_free(&f);
}
static void test_typed_and_inline(void) {
    Fixture f;
    fixture(&f, "<macro name='title'><text variable='title' prefix='pre ' suffix=' post'/></macro>",
        "<bibliography><layout><group delimiter='|' font-style='italic' display='block'><text macro='title' text-case='uppercase' prefix='outer '/><number variable='edition' form='ordinal'/><label variable='page'/><text variable='page'/><date variable='issued' prefix='[' suffix=']'><date-part name='year'/></date></group></layout></bibliography>",
        "{\"title\":\"hello <span class='nocase'>iPhone</span> <i>world</i>\",\"edition\":2,\"page\":\"3-5\",\"issued\":{\"date-parts\":[[2025,1,2]]}}", NULL);
    preview_is(&f, "outer PRE HELLO iPhone WORLD POST|2th|pages|3-5|[2025]");
    const CeCslEvalNode *g = child(ce_csl_evaluation_root(f.result), 0);
    assert(ce_csl_eval_node_info(g)->decoration.display == CE_CSL_EVAL_DISPLAY_BLOCK);
    const CeCslEvalNode *leaf = child(child(g, 0), 0);
    text_is(ce_csl_eval_node_content(leaf), "hello iPhone world");
    const CeCslInline *body = ce_csl_eval_node_content(leaf);
    bool protected_seen = false, flipped_seen = false;
    for (size_t i = 0; i < ce_csl_inline_run_count(body); ++i) {
        const CeCslInlineRun *run = ce_csl_inline_run(body, i);
        protected_seen |= run->nocase; flipped_seen |= !(run->format & CE_CSL_ITALIC);
    }
    assert(protected_seen && flipped_seen);
    assert(ce_csl_eval_node_number(child(g, 1))->numeric);
    assert(ce_csl_eval_node_date_value(child(g, 4))->endpoints[0].month == 1);
    assert(ce_csl_eval_node_date_parts(child(g, 4)) == CE_CSL_DATE_PART_YEAR);
    assert(ce_csl_evaluation_locale(f.result) == f.locale);
    CeCslEvaluation *old = f.result; f.request.variables.cite = &f.cite;
    assert(!ce_csl_evaluate(&f.request, NULL, &f.result, error, sizeof(error)) && f.result == old && strstr(error, "current cite"));
    fixture_free(&f);
    fixture(&f, NULL, "<citation><layout><label variable='locator' suffix=' '/><text variable='locator'/></layout></citation>", "{}", "{\"locator\":\"3-5\"}");
    preview_is(&f, "pages 3–5"); fixture_free(&f);
}
typedef struct Adjust { const CeCslXmlNode *source, *calls[2]; size_t count; CeCslNameOverride override; } Adjust;
static bool adjust(void *context, const CeCslEvalOccurrence *occurrence,
    const CeCslNamesBinding *bindings, size_t count, const CeCslNameOptions *options,
    CeCslEvalNameAdjustment *out, char *err, size_t cap) {
    (void)options; (void)err; (void)cap;
    Adjust *a = context;
    assert(count == 1 && ce_csl_names_data_count(bindings[0].data) == 2 && occurrence->macro_depth == 1 && a->count < 2);
    if (a->source) assert(a->source == occurrence->source);
    a->source = occurrence->source; a->calls[a->count] = occurrence->macro_calls[0];
    a->override = (CeCslNameOverride){0, 0, CE_CSL_GIVEN_FULL};
    *out = (CeCslEvalNameAdjustment){.minimum_names = a->count ? 2 : 0, .overrides = &a->override, .override_count = 1};
    ++a->count; return true;
}
static void test_occurrences(void) {
    Fixture f;
    fixture(&f, "<macro name='author'><names variable='author'><name initialize-with='. ' et-al-min='2' et-al-use-first='1'/></names></macro>",
        "<citation><layout><group delimiter='|'><text macro='author'/><text macro='author'/></group></layout></citation>",
        "{\"author\":[{\"family\":\"Doe\",\"given\":\"John\"},{\"family\":\"Roe\",\"given\":\"Jane\"}]}", NULL);
    Adjust a = {0}; f.request.name_context = &a; f.request.adjust_names = adjust;
    preview_is(&f, "John Doe et al.|John Doe, J. Roe"); assert(a.count == 2 && a.calls[0] != a.calls[1]);
    const CeCslEvalNode *names = child(child(child(ce_csl_evaluation_root(f.result), 0), 0), 0);
    const CeCslNamesResult *result = ce_csl_eval_node_names(names);
    assert(result && ce_csl_names_data_count(ce_csl_names_result_data(result, 0)) == 2);
    assert(ce_csl_names_result_person_count(result) == 1);
    assert(ce_csl_eval_node_info(names)->occurrence.macro_calls[0] == a.calls[0]);
    fixture_free(&f);
}
typedef struct Allocator { size_t calls, fail_at, live; bool sticky; } Allocator;
static void *test_allocate(void *context, size_t bytes) {
    Allocator *a = context;
    ++a->calls;
    if (a->fail_at && (a->sticky ? a->calls >= a->fail_at : a->calls == a->fail_at)) return NULL;
    void *p = malloc(bytes); if (p) ++a->live; return p;
}
static void test_free(void *context, void *memory) {
    Allocator *a = context; if (memory) { assert(a->live); --a->live; free(memory); }
}
static void test_atomic_and_oom(void) {
    Fixture f;
    fixture(&f, "<macro name='x'><text variable='title' text-case='uppercase'/></macro>",
        "<citation><layout><group delimiter='|'><names variable='author'><name/><substitute><text variable='publisher'/><names variable='editor'/></substitute></names><text macro='x' quotes='true'/><date variable='issued'><date-part name='year'/></date></group></layout></citation>",
        "{\"editor\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],\"title\":\"hello <i>world</i>\",\"issued\":{\"date-parts\":[[2025]]}}", NULL);
    preview_is(&f, "Jane Doe|“HELLO WORLD”|2025");
    CeCslEvaluation *old = f.result;
    Allocator a = {0}; CeCslOutputAllocator hooks = {&a, test_allocate, test_free};
    CeCslEvaluation *probe = NULL;
    assert(ce_csl_evaluate(&f.request, &hooks, &probe, error, sizeof(error)));
    size_t calls = a.calls; ce_csl_evaluation_free(probe); assert(!a.live);
    char *before = cJSON_PrintUnformatted(f.item_json); assert(before);
    for (size_t i = 1; i <= calls; ++i) {
        a = (Allocator){.fail_at = i};
        bool ok = ce_csl_evaluate(&f.request, &hooks, &f.result, error, sizeof(error));
        if (ok || !*error || f.result != old || a.live)
            fprintf(stderr, "allocation %zu/%zu: ok=%d error=<%s> replaced=%d live=%zu\n", i, calls,
                ok, error, f.result != old, a.live);
        assert(!ok && *error && f.result == old && !a.live);
    }
    char *after = cJSON_PrintUnformatted(f.item_json); assert(after && !strcmp(before, after));
    free(before); free(after);
    cJSON *editor = cJSON_GetObjectItemCaseSensitive(f.item_json, "editor");
    cJSON_ReplaceItemInObject(cJSON_GetArrayItem(editor, 0), "given", cJSON_CreateString("<i>Jane</i>"));
    assert(!ce_csl_evaluate(&f.request, NULL, &f.result, error, sizeof(error)) && f.result == old && strstr(error, "markup"));
    fixture_free(&f);

    fixture(&f, NULL, "<citation><layout><group delimiter='-'><text term='no date' form='short'/>"
        "<choose><if variable='title'><text variable='year-suffix'/></if></choose>"
        "</group></layout></citation>", "{\"title\":\"Present\"}", NULL);
    preview_is(&f, "n.d."); old = f.result;
    a = (Allocator){.sticky = true}; probe = NULL;
    assert(ce_csl_evaluate(&f.request, &hooks, &probe, error, sizeof error));
    calls = a.calls; ce_csl_evaluation_free(probe); assert(!a.live);
    for (size_t i = 1; i <= calls; ++i) {
        a = (Allocator){.fail_at = i, .sticky = true};
        assert(!ce_csl_evaluate(&f.request, &hooks, &f.result, error, sizeof error));
        assert(f.result == old && !a.live && *error);
        text_is(ce_csl_eval_node_preview(ce_csl_evaluation_root(f.result)), "n.d.");
    }
    printf("No-date evaluator: %zu sticky allocation failures retained old result\n", calls);
    fixture_free(&f);
}
typedef struct Provider { char text[32]; bool fail, suppress; } Provider;
static CeCslVariableStatus provider(void *context, const char *variable, const cJSON *long_value,
    CeCslVariableShortValue *value, char *err, size_t cap) {
    (void)variable; (void)long_value;
    Provider *p = context;
    if (p->fail) { snprintf(err, cap, "owned provider failure"); return CE_CSL_VARIABLE_UNSUPPORTED; }
    *value = (CeCslVariableShortValue){true, p->text, strlen(p->text)};
    return CE_CSL_VARIABLE_OK;
}
static bool suppression(void *context, const char *variable, bool *value, char *err, size_t cap) {
    (void)variable; (void)err; (void)cap;
    *value = ((Provider *)context)->suppress; return true;
}
static void test_providers_and_limits(void) {
    Fixture f;
    fixture(&f, NULL, "<citation><layout><text variable='title' form='short'/></layout></citation>", "{\"title\":\"Original\"}", NULL);
    Provider p = {.text = "Abbreviation"};
    f.request.variables.context = &p;
    f.request.variables.short_value = provider;
    f.request.variables.suppressed = suppression;
    char language[] = "tr"; f.request.item_language = language;
    preview_is(&f, "Abbreviation");
    const CeCslEvalNode *leaf = child(ce_csl_evaluation_root(f.result), 0);
    strcpy(p.text, "Changed"); language[0] = 'x';
    size_t size;
    const char *text = ce_csl_variable_text(&ce_csl_eval_node_variable(leaf, 0)->value, &size);
    assert(size == 12 && !memcmp(text, "Abbreviation", size));
    assert(!strcmp(ce_csl_evaluation_item_language(f.result), "tr"));
    p.text[0] = 0; preview_is(&f, "");
    const CeCslVariableValue *empty_value = &ce_csl_eval_node_variable(child(ce_csl_evaluation_root(f.result), 0), 0)->value;
    assert(empty_value->borrowed_text && empty_value->borrowed_text != p.text);
    p.text[0] = 'X'; assert(!*empty_value->borrowed_text);
    CeCslEvaluation *old = f.result; p.fail = true;
    assert(!ce_csl_evaluate(&f.request, NULL, &f.result, error, sizeof(error)) && f.result == old && strstr(error, "provider"));
    p.fail = false; p.suppress = true; preview_is(&f, "");
    p.suppress = false; f.request.variables.short_value = NULL;
    f.request.position = CE_CSL_POSITION_FIRST | CE_CSL_POSITION_IBID;
    old = f.result;
    assert(!ce_csl_evaluate(&f.request, NULL, &f.result, error, sizeof(error)) && f.result == old && strstr(error, "position"));
    f.request.position = CE_CSL_POSITION_FIRST;
    size_t half = CE_CSL_EVALUATE_MAX_BYTES / 2;
    char *large = malloc(half + 2); assert(large); memset(large, 'a', half + 1); large[half] = 0;
    cJSON_ReplaceItemInObject(f.item_json, "title", cJSON_CreateString(large));
    evaluate(&f); old = f.result;
    assert(ce_csl_evaluation_summary(f.result)->leaf_bytes == CE_CSL_EVALUATE_MAX_BYTES);
    assert(ce_csl_inline_text(ce_csl_eval_node_preview(ce_csl_evaluation_root(f.result)), &size) && size == half);
    large[half] = 'a'; large[half + 1] = 0;
    cJSON_ReplaceItemInObject(f.item_json, "title", cJSON_CreateString(large)); free(large);
    assert(!ce_csl_evaluate(&f.request, NULL, &f.result, error, sizeof(error)) && f.result == old && strstr(error, "payload"));
    fixture_free(&f);

    fixture(&f, NULL, "<citation><layout><names variable='author'><name initialize-with='. ' et-al-min='3' et-al-use-first='2' et-al-subsequent-min='2' et-al-subsequent-use-first='1'/></names></layout></citation>",
        "{\"author\":[{\"family\":\"One\"},{\"family\":\"Two\"}]}", NULL);
    preview_is(&f, "One, Two"); f.request.position = CE_CSL_POSITION_IBID_WITH_LOCATOR;
    preview_is(&f, "One et al."); fixture_free(&f);

    size_t capacity = 30000; char *macros = malloc(capacity); assert(macros); size_t used = 0;
    for (size_t i = 0; i < 140; ++i)
        used += (size_t)snprintf(macros + used, capacity - used,
            "<macro name='m%zu'><text macro='m%zu'/></macro>", i, i + 1);
    snprintf(macros + used, capacity - used, "<macro name='m140'><text value='last'/></macro>");
    fixture(&f, macros, "<citation><layout><text macro='m0'/></layout></citation>", "{}", NULL);
    assert(!ce_csl_evaluate(&f.request, NULL, &f.result, error, sizeof(error)) && !f.result && strstr(error, "depth"));
    fixture_free(&f);
    used = 0;
    for (size_t i = 0; i < 70; ++i)
        used += (size_t)snprintf(macros + used, capacity - used,
            "<macro name='m%zu'><names variable='author'><name/><substitute><text macro='m%zu'/></substitute></names></macro>", i, i + 1);
    snprintf(macros + used, capacity - used, "<macro name='m70'><text value='last'/></macro>");
    fixture(&f, macros, "<citation><layout><text macro='m0'/></layout></citation>", "{}", NULL);
    assert(!ce_csl_evaluate(&f.request, NULL, &f.result, error, sizeof(error)) && !f.result && strstr(error, "depth"));
    fixture_free(&f);
    used = 0;
    for (size_t i = 0; i < 17; ++i)
        used += (size_t)snprintf(macros + used, capacity - used,
            "<macro name='m%zu'><text macro='m%zu'/><text macro='m%zu'/></macro>", i, i + 1, i + 1);
    snprintf(macros + used, capacity - used, "<macro name='m17'><text value=''/></macro>");
    fixture(&f, macros, "<citation><layout><text macro='m0'/></layout></citation>", "{}", NULL);
    assert(!ce_csl_evaluate(&f.request, NULL, &f.result, error, sizeof(error)) && !f.result && strstr(error, "node limit"));
    fixture_free(&f);
    used = 0;
    for (size_t i = 0; i < 12; ++i)
        used += (size_t)snprintf(macros + used, capacity - used,
            "<macro name='m%zu'><text macro='m%zu'/><text macro='m%zu'/></macro>", i, i + 1, i + 1);
    snprintf(macros + used, capacity - used, "<macro name='m12'><text value=''/></macro>");
    char section[2048] = "<citation><layout><names variable='author'><name/><substitute>";
    for (size_t i = 0; i < 16; ++i) strcat(section, "<text macro='m0'/>");
    strcat(section, "</substitute></names></layout></citation>");
    fixture(&f, macros, section, "{}", NULL); free(macros);
    assert(!ce_csl_evaluate(&f.request, NULL, &f.result, error, sizeof(error)) && !f.result && strstr(error, "node limit"));
    fixture_free(&f);
}
static void test_pages(void) {
    Fixture f;
    const char *section = "<citation><layout><group delimiter='|'><text variable='page'/><number variable='page'/><text variable='locator'/></group></layout></citation>";
    fixture_options(&f, "page-range-format='minimal'", NULL, section,
        "{\"page\":\" 101 - 109,201-209 &301-309 \"}", "{\"locator\":\"321-328\",\"label\":\"page\"}");
    preview_is(&f, " 101 – 9,201–9 &301–9 |101–9, 201–9 & 301–9|321–8");
    assert(ce_csl_evaluation_summary(f.result)->missing_term);
    const CeCslEvalNode *g = child(ce_csl_evaluation_root(f.result), 0);
    const CeCslVariableValue *v = &ce_csl_eval_node_variable(child(g, 1), 0)->value;
    size_t bytes = 0; const char *raw = ce_csl_variable_text(v, &bytes);
    assert(bytes == strlen(" 101 - 109,201-209 &301-309 ") && !memcmp(raw, " 101 - 109,201-209 &301-309 ", bytes));
    assert(ce_csl_eval_node_number(child(g, 1))->number_count == 6);
    cJSON_ReplaceItemInObject(f.cite_json, "label", cJSON_CreateString("section"));
    preview_is(&f, " 101 – 9,201–9 &301–9 |101–9, 201–9 & 301–9|321–328");
    f.request.sorting = true;
    preview_is(&f, " 101 - 109,201-209 &301-309 |101-109, 201-209 & 301-309|321-328");
    fixture_free(&f);
    fixture_options(&f, "page-range-format='expanded'",
        "<locale xml:lang='en-US'><terms><term name='page-range-delimiter'>至</term></terms></locale>", section,
        "{\"page\":\"甲199章-甲02章\"}", "{\"locator\":\"101-109\",\"label\":\"page\"}");
    preview_is(&f, "甲199章至甲202章|甲199章至甲202章|101至109");
    assert(!ce_csl_evaluation_summary(f.result)->missing_term);
    CeCslEvaluation *old = f.result;
    cJSON_ReplaceItemInObject(f.item_json, "page", cJSON_CreateString("<i>101-109</i>"));
    assert(!ce_csl_evaluate(&f.request, NULL, &f.result, error, sizeof(error)) && f.result == old && strstr(error, "markup"));
    cJSON_ReplaceItemInObject(f.item_json, "page", cJSON_CreateString("1-2-3"));
    assert(!ce_csl_evaluate(&f.request, NULL, &f.result, error, sizeof(error)) && f.result == old && strstr(error, "chained"));
    fixture_free(&f);
    fixture_options(&f, "page-range-format='minimal'", NULL,
        "<citation><layout><text variable='page'/></layout></citation>", "{\"page\":\"pre-publication\"}", NULL);
    preview_is(&f, "pre-publication"); fixture_free(&f);
    fixture(&f, NULL, section, "{\"page\":\"101-109\"}", "{\"locator\":\"101-109\"}");
    preview_is(&f, "101-109|101-109|101–109"); fixture_free(&f);
}
static void test_sorting_numbers(void) {
    const char *values[] = {"0", "4000", "1844674407370955161600001"};
    for (size_t i=0;i<sizeof(values)/sizeof(*values);++i) {
        Fixture f; char item[160]; snprintf(item,sizeof(item),"{\"edition\":\"%s\"}",values[i]);
        fixture(&f,"<macro name='numeric'><number variable='edition' form='roman'/></macro>",
            "<citation><layout><text macro='numeric'/></layout></citation>",item,NULL);
        f.request.sorting=true; preview_is(&f,values[i]);
        const CeCslEvalNode *number=child(child(ce_csl_evaluation_root(f.result),0),0);
        const CeCslEvalVariableUse *use=ce_csl_eval_node_variable(number,0);
        size_t bytes=0; const char *raw=ce_csl_variable_text(&use->value,&bytes);
        assert(bytes==strlen(values[i]) && !memcmp(raw,values[i],bytes));
        assert(cJSON_IsString(use->value.node) && !strcmp(use->value.node->valuestring,values[i]));
        assert(!strcmp(ce_csl_xml_attribute(ce_csl_eval_node_info(number)->occurrence.source,"form"),"roman"));
        CeCslEvaluation *old=f.result; f.request.sorting=false;
        assert(!ce_csl_evaluate(&f.request,NULL,&f.result,error,sizeof(error)) && f.result==old && *error);
        fixture_free(&f);
    }
    Fixture f;
    fixture(&f,"<macro name='authors'><names variable='author'><name et-al-min='4' et-al-use-first='1'/></names></macro>",
        "<citation><layout><text macro='authors'/></layout></citation>",
        "{\"author\":[{\"family\":\"One\"},{\"family\":\"Two\"},{\"family\":\"Three\"},{\"family\":\"Four\"}]}",NULL);
    f.request.sorting=true; f.request.has_sort_min=true; f.request.sort_min=2;
    f.request.has_sort_first=true; f.request.sort_first=2; f.request.has_sort_last=true; f.request.sort_last=false;
    preview_is(&f,"One, Two");
    const CeCslNamesResult *names=ce_csl_eval_node_names(child(child(ce_csl_evaluation_root(f.result),0),0));
    assert(ce_csl_names_result_person_count(names)==2);
    assert(ce_csl_names_data_count(ce_csl_names_result_data(names,0))==4);
    fixture_free(&f);
}
static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long size = ftell(file); assert(size >= 0);
    rewind(file); char *text = malloc((size_t)size + 1); assert(text);
    assert(fread(text, 1, (size_t)size, file) == (size_t)size); fclose(file); text[size] = 0; return text;
}
static void real_resources(int argc, char **argv) {
    assert(argc == 5);
    CeCslXml *xml[2]; const CeCslXmlNode *roots[2];
    for (size_t i = 0; i < 2; ++i) {
        char *text = read_file(argv[i + 3]); xml[i] = ce_csl_xml_parse(text, strlen(text), error, sizeof(error));
        assert(xml[i]); roots[i] = xml[i]->root; free(text);
    }
    CeCslLocaleCatalog catalog = {.roots = roots, .root_count = 2};
    const char *types[] = {"article", "article-journal", "article-magazine", "article-newspaper", "bill", "book", "broadcast",
        "chapter", "classic", "collection", "dataset", "document", "entry", "entry-dictionary", "entry-encyclopedia", "event",
        "figure", "graphic", "hearing", "interview", "legal_case", "legislation", "manuscript", "map", "motion_picture",
        "musical_score", "pamphlet", "paper-conference", "patent", "performance", "periodical", "personal_communication",
        "post", "post-weblog", "regulation", "report", "review", "review-book", "software", "song", "speech", "standard",
        "thesis", "treaty", "webpage"};
    size_t checks = 0, node_count = 0;
    for (size_t style_index = 0; style_index < 3; ++style_index) {
        Fixture f = {0}; char *text = read_file(argv[style_index]);
        assert(ce_csl_style_load(&f.style, text, strlen(text), error, sizeof(error))); free(text);
        f.locale = ce_csl_locale_new(ce_csl_style_root(f.style), &catalog,
            style_index == 2 ? "zh-CN" : "en-US", NULL, error, sizeof(error)); assert(f.locale);
        f.item_json = cJSON_Parse("{\"id\":\"matrix\",\"type\":\"book\",\"title\":\"a <i>known</i> <span class='nocase'>iPhone</span> study\","
            "\"container-title\":\"Journal of tests\",\"publisher\":\"Example Press\",\"publisher-place\":\"Example City\","
            "\"page\":\"123\",\"volume\":\"2\",\"issue\":\"3\",\"edition\":\"2\",\"number\":\"4\",\"version\":\"5\","
            "\"DOI\":\"10.1234/example\",\"URL\":\"https://example.invalid/resource\",\"genre\":\"Report\","
            "\"event-title\":\"Example Meeting\",\"event-place\":\"Example City\",\"status\":\"published\","
            "\"author\":[{\"given\":\"Jane\",\"family\":\"Doe\"},{\"given\":\"John\",\"family\":\"Roe\"}],"
            "\"editor\":[{\"given\":\"Alex\",\"family\":\"Smith\"}],\"translator\":[{\"given\":\"Sam\",\"family\":\"Brown\"}],"
            "\"issued\":{\"date-parts\":[[2025,3,14]]},\"accessed\":{\"date-parts\":[[2026,1,2]]},"
            "\"original-date\":{\"date-parts\":[[2020]]},\"event-date\":{\"date-parts\":[[2025,2,12]]}}");
        assert(f.item_json); f.item = (CeCslInputItem){.id = "matrix", .data = f.item_json};
        f.cite_json = cJSON_Parse("{\"locator\":\"20-22\",\"label\":\"page\"}"); assert(f.cite_json); f.cite.data = f.cite_json;
        f.generated = (CeCslGeneratedVariables){.citation_number = {true,true,7},
            .first_reference_note_number = {true,false,0}, .year_suffix = {true,"a",1}, .citation_label = {true,"DR25",4}};
        f.request = (CeCslEvalRequest){.style=f.style,.locale=f.locale,.position=CE_CSL_POSITION_FIRST,
            .variables={.item=&f.item,.cite=&f.cite,.generated=&f.generated}};
        for (size_t variant = 0; variant < 2; ++variant) {
            if (variant) {
                cJSON_DeleteItemFromObjectCaseSensitive(f.item_json, "author");
                cJSON_DeleteItemFromObjectCaseSensitive(f.item_json, "editor");
                cJSON_DeleteItemFromObjectCaseSensitive(f.item_json, "translator");
                cJSON_DeleteItemFromObjectCaseSensitive(f.item_json, "issued");
            }
            for (size_t type = 0; type < sizeof(types)/sizeof(*types); ++type) {
                cJSON_ReplaceItemInObject(f.item_json, "type", cJSON_CreateString(types[type]));
                for (size_t section = 0; section < 2; ++section) {
                    f.request.section = section ? ce_csl_style_bibliography(f.style) : ce_csl_style_citation(f.style);
                    f.request.variables.cite = section ? NULL : &f.cite;
                    if (!f.request.section) continue;
                    if (!ce_csl_evaluate(&f.request, NULL, &f.result, error, sizeof(error))) {
                        fprintf(stderr, "Real evaluation %s variant=%zu type=%s section=%zu: %s\n",
                            argv[style_index], variant, types[type], section, error); abort();
                    }
                    assert(ce_csl_evaluation_summary(f.result)->nonempty);
                    node_count += ce_csl_evaluation_summary(f.result)->node_count; ++checks;
                }
            }
        }
        fixture_free(&f);
    }
    for (size_t i = 0; i < 2; ++i) ce_csl_xml_free(xml[i]);
    printf("Pinned styles: %zu typed semantic evaluations / %zu nodes passed (not final-format conformance)\n", checks, node_count);
}
int main(int argc, char **argv) {
    test_groups(); test_year_suffix_groups(); test_delimiters_and_conditions(); test_substitute();
    test_typed_and_inline(); test_occurrences(); test_atomic_and_oom(); test_providers_and_limits();
    test_sorting_numbers(); test_pages();
    if (argc > 1) real_resources(argc - 1, argv + 1);
    puts("CSL semantic evaluation: groups, transactions, typed leaves, inline, occurrences and OOM passed");
    return 0;
}
