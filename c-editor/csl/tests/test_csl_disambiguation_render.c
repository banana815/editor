#include "csl_disambiguation_render.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[512];
#define OK(call)                                                                                             \
    do {                                                                                                     \
        bool success_ = (call);                                                                              \
        if (!success_)                                                                                       \
            fprintf(stderr, "%s: %s\n", #call, error);                                                       \
        assert(success_);                                                                                    \
    } while (0)
typedef struct Fixture {
    CeCslStyle *style;
    CeCslLocale *locale;
    CeCslXml *locale_xml;
    CeCslInput *input;
    CeCslRegistry *registry;
    CeCslDisambiguationRenderConfig config;
} Fixture;
static const char *head =
    "<style xmlns='http://purl.org/net/xbiblio/csl' version='1.0' class='in-text' default-locale='en-US' "
    "initialize-with='. '><info><title>Owned disambiguation "
    "regression</title><id>https://example.invalid/disambiguation</id><updated>2026-01-01T00:00:00+00:00</"
    "updated></info><locale><terms><term name='et-al'>et al.</term><term name='and'>and</term><term "
    "name='open-quote'>“</term><term name='close-quote'>”</term><term name='open-inner-quote'>‘</term><term "
    "name='close-inner-quote'>’</term></terms></locale>";
static void input(Fixture *f, const char *items, size_t cited) {
    cJSON *root = cJSON_CreateObject(), *array = cJSON_Parse(items), *citations = cJSON_CreateArray();
    assert(root && array && citations && cJSON_IsArray(array));
    assert(cJSON_AddItemToObject(root, "items", array));
    assert(cJSON_AddItemToObject(root, "citations", citations));
    if (cited) {
        cJSON *cluster = cJSON_CreateObject(), *cites = cJSON_CreateArray();
        assert(cluster && cites && cJSON_AddStringToObject(cluster, "citationID", "c"));
        assert(cJSON_AddItemToObject(cluster, "citationItems", cites));
        assert(cJSON_AddItemToArray(citations, cluster));
        for (size_t i = 0; i < cited; ++i) {
            cJSON *cite = cJSON_CreateObject();
            const cJSON *item = cJSON_GetArrayItem(array, (int)i);
            assert(cite && item &&
                   cJSON_AddStringToObject(cite, "id",
                                           cJSON_GetObjectItemCaseSensitive(item, "id")->valuestring));
            assert(cJSON_AddStringToObject(cite, "locator", "999"));
            assert(cJSON_AddItemToArray(cites, cite));
        }
    }
    char *json = cJSON_PrintUnformatted(root);
    assert(json);
    OK(ce_csl_input_load(&f->input, json, strlen(json), error, sizeof(error)));
    free(json);
    cJSON_Delete(root);
    size_t includes[32], count = ce_csl_input_item_count(f->input);
    assert(count <= 32);
    for (size_t i = 0; i < count; ++i)
        includes[i] = i;
    CeCslRegistryOptions options = {.include_items = includes, .include_count = count};
    OK(ce_csl_registry_build(f->input, &options, NULL, &f->registry, error, sizeof(error)));
    f->config.input = f->input;
    f->config.registry = f->registry;
}
static void fixture(Fixture *f, const char *macros, const char *sections, const char *items, size_t cited) {
    memset(f, 0, sizeof(*f));
    size_t size = strlen(head) + strlen(macros) + strlen(sections) + 16;
    char *xml = malloc(size);
    assert(xml);
    snprintf(xml, size, "%s%s%s</style>", head, macros, sections);
    OK(ce_csl_style_load(&f->style, xml, strlen(xml), error, sizeof(error)));
    free(xml);
    f->locale = ce_csl_locale_new(ce_csl_style_root(f->style), NULL, "en-US", NULL, error, sizeof(error));
    assert(f->locale);
    f->config.style = f->style;
    f->config.locale = f->locale;
    input(f, items, cited);
}
static void fixture_free(Fixture *f) {
    ce_csl_registry_free(f->registry);
    ce_csl_input_free(f->input);
    ce_csl_locale_free(f->locale);
    ce_csl_style_free(f->style);
    ce_csl_xml_free(f->locale_xml);
}
static CeCslDisambiguationProbe *probe(Fixture *f, size_t item, CeCslDisambiguationProfile profile,
                                       const CeCslDisambiguationItem *state) {
    CeCslDisambiguationItem original = {.item_index = item};
    CeCslDisambiguationRequest request = {
        .item_index = item, .profile = profile, .state = state ? state : &original};
    CeCslDisambiguationProbe *p = NULL;
    CeCslDisambiguationStatus status =
        ce_csl_disambiguation_render(&f->config, &request, NULL, &p, error, sizeof(error));
    if (status != CE_CSL_DISAMBIGUATION_OK)
        fprintf(stderr, "provider status %d: %s\n", (int)status, error);
    assert(status == CE_CSL_DISAMBIGUATION_OK && p);
    return p;
}
static void key_is(CeCslDisambiguationBytes key, const char *expected) {
    if (key.size != strlen(expected) || (key.size && memcmp(key.data, expected, key.size)))
        fprintf(stderr, "expected <%s>, actual <%.*s>\n", expected, (int)key.size,
                key.data ? (const char *)key.data : "");
    assert(key.size == strlen(expected) && (!key.size || !memcmp(key.data, expected, key.size)));
}
static void test_context(void) {
    Fixture f;
    fixture(
        &f, "<macro name='n'><names variable='author'><name form='short'/></names></macro>",
        "<citation><layout delimiter='; '><text macro='n' text-case='uppercase' strip-periods='true'/><text "
        "macro='n' prefix='|'/><text variable='year-suffix'/><text variable='locator'/></layout></citation>",
        "[{\"id\":\"a\",\"year-suffix\":\"WRONG\",\"author\":[{\"family\":\"Doe\",\"given\":\"Alice\"}]}]",
        1);
    CeCslDisambiguationProbe *p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, NULL);
    key_is(p->key, "DOE|Doe");
    assert(p->names_count == 2);
    key_is(p->names[0].people[0].style_key, "DOE");
    key_is(p->names[0].people[0].initials_key, "A DOE");
    key_is(p->names[1].people[0].initials_key, "A. Doe");
    assert(p->names[0].people[0].primary && !p->names[1].people[0].primary);
    assert(p->names[0].occurrence.source == p->names[1].occurrence.source);
    assert(p->names[0].occurrence.macro_depth == 1 && p->names[1].occurrence.macro_depth == 1);
    assert(p->names[0].occurrence.macro_calls[0] != p->names[1].occurrence.macro_calls[0]);
    CeCslDisambiguationAdjustment adjustment = {.profile = CE_CSL_DISAMBIGUATION_FIRST,
                                                .occurrence = p->names[1].occurrence};
    CeCslNameOverride override = {0, 0, CE_CSL_GIVEN_INITIALS};
    adjustment.overrides = &override;
    adjustment.override_count = 1;
    CeCslDisambiguationItem state = {.item_index = 0, .adjustments = &adjustment, .adjustment_count = 1};
    strcpy(state.year_suffix, "a");
    CeCslDisambiguationProbe *q = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, &state);
    memset(&state, 0xaa, sizeof(state));
    memset(&override, 0xaa, sizeof(override));
    key_is(q->key, "DOE|A. Doea");
    assert(q->names[1].people[0].given_initialized);
    key_is(q->names[1].people[0].initials_key, "A. Doe");
    assert(ce_csl_disambiguation_render_evaluation(q) && ce_csl_disambiguation_render_finalized(q));
    CeCslDisambiguationRenderConfig saved = f.config;
    memset(&f.config, 0xaa, sizeof(f.config));
    CeCslOutputString exported = {0};
    OK(ce_csl_output_plain(ce_csl_finalized_output(ce_csl_disambiguation_render_finalized(q)), &exported,
                           error, sizeof(error)));
    assert(!strcmp(exported.data, "DOE|A. Doea"));
    ce_csl_output_string_free(&exported);
    f.config = saved;
    ce_csl_disambiguation_render_release(p);
    ce_csl_disambiguation_render_release(q);
    fixture_free(&f);
    /* Parent lowercase must use Greek final-sigma context across name/value. */
    fixture(&f,
            "<macro name='n'><names variable='author'><name form='short' initialize-with=''/></names><text "
            "value='Α'/></macro>",
            "<citation><layout><text macro='n' text-case='lowercase'/></layout></citation>",
            "[{\"id\":\"a\",\"author\":[{\"family\":\"ΟΣ\",\"given\":\"Βήτα\"}]}]", 1);
    p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, NULL);
    key_is(p->key, "οσα");
    key_is(p->names[0].people[0].style_key, "οσ");
    key_is(p->names[0].people[0].initials_key, "β οσ");
    ce_csl_disambiguation_render_release(p);
    fixture_free(&f);
}
static void test_selection_and_capability(void) {
    Fixture f;
    fixture(
        &f, "",
        "<citation><layout><names variable='author editor'><name form='short' et-al-min='3' "
        "et-al-use-first='1' et-al-use-last='true'/></names></layout></citation><bibliography><layout><names "
        "variable='author'><name/></names></layout></bibliography>",
        "[{\"id\":\"a\",\"author\":[{}, "
        "{\"family\":\"A\",\"given\":\"Alice\"},{\"family\":\"张\",\"given\":\"伟\"},{\"family\":\"C\","
        "\"given\":\"Chris\"}],\"editor\":[{\"family\":\"D\",\"given\":\"Daniel\"}]}]",
        1);
    CeCslDisambiguationProbe *p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, NULL);
    assert(p->names_count == 1 && p->names[0].person_count == 5 && p->names[0].next_minimum_names == 3);
    const CeCslDisambiguationPerson *persons = p->names[0].people;
    assert(persons[0].identity->kind == CE_CSL_NAME_EMPTY && !persons[0].rendered);
    assert(persons[1].primary && persons[1].can_initials);
    assert(persons[2].person_index == 2 && !persons[2].rendered && !persons[2].can_initials);
    key_is(persons[2].style_key, "");
    key_is(persons[2].initials_key, "");
    assert(persons[3].rendered && persons[3].can_initials && persons[4].binding == 1);
    ce_csl_disambiguation_render_release(p);
    p = probe(&f, 0, CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY, NULL);
    assert(p->names[0].people[1].given_initialized && !p->names[0].people[2].can_initials);
    ce_csl_disambiguation_render_release(p);
    fixture_free(&f);
    fixture(&f, "",
            "<citation><layout><names variable='author'><name/><substitute><names "
            "variable='editor'/></substitute></names><names "
            "variable='translator'><name/></names></layout></citation>",
            "[{\"id\":\"a\",\"editor\":[{\"family\":\"E\",\"given\":\"Ella\"}],\"translator\":[{\"literal\":"
            "\"机构\"}]}]",
            1);
    p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, NULL);
    assert(p->names_count == 2 && !strcmp(p->names[0].people[0].source_variable, "editor"));
    assert(p->names[0].people[0].primary && !p->names[1].people[0].primary &&
           !p->names[1].people[0].can_initials);
    ce_csl_disambiguation_render_release(p);
    fixture_free(&f);
}
static void test_binding_edges(void) {
    Fixture f;
    fixture(&f, "",
            "<citation><layout><names variable='editor translator'><name form='count' et-al-min='3' "
            "et-al-use-first='1' et-al-use-last='true'/></names></layout></citation>",
            "[{\"id\":\"a\",\"editor\":[{\"family\":\"A\"},{\"family\":\"B\"},{\"family\":\"C\"}],"
            "\"translator\":[{\"family\":\"A\"},{\"family\":\"B\"},{\"family\":\"C\"}]}]",
            1);
    CeCslDisambiguationProbe *p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, NULL);
    key_is(p->key, "2");
    assert(p->names_count == 1 && p->names[0].person_count == 6);
    assert(p->names[0].next_minimum_names == 3);
    for (size_t i = 0; i < 6; ++i) {
        assert(!p->names[0].people[i].rendered && !p->names[0].people[i].primary);
        key_is(p->names[0].people[i].style_key, "");
    }
    assert(p->names[0].people[3].binding == 1);
    ce_csl_disambiguation_render_release(p);
    fixture_free(&f);
    fixture(&f, "",
            "<citation><layout><names variable='author'><name form='short' et-al-min='3' et-al-use-first='2' "
            "et-al-subsequent-min='3' et-al-subsequent-use-first='1'/></names></layout></citation>",
            "[{\"id\":\"a\",\"author\":[{\"family\":\"A\",\"given\":\"Alice\"},{\"family\":\"B\",\"given\":"
            "\"Bob\"},{\"family\":\"C\",\"given\":\"Chris\"}]}]",
            1);
    p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, NULL);
    assert(p->names[0].next_minimum_names == 3);
    assert(p->names[0].people[2].can_initials && !p->names[0].people[2].rendered);
    key_is(p->names[0].people[2].initials_key, "");
    ce_csl_disambiguation_render_release(p);
    p = probe(&f, 0, CE_CSL_DISAMBIGUATION_SUBSEQUENT, NULL);
    assert(p->names[0].next_minimum_names == 2);
    assert(p->names[0].people[1].can_initials && !p->names[0].people[1].rendered);
    ce_csl_disambiguation_render_release(p);
    CeCslDisambiguationItem state = {.item_index = 0};
    CeCslDisambiguationRequest request = {
        .item_index = 0, .profile = CE_CSL_DISAMBIGUATION_FIRST, .state = &state};
    f.config.max_people = 2;
    p = NULL;
    assert(ce_csl_disambiguation_render(&f.config, &request, NULL, &p, error, sizeof(error)) ==
               CE_CSL_DISAMBIGUATION_LIMIT &&
           !p);
    f.config.max_people = 0;
    f.config.max_variants = 1;
    assert(ce_csl_disambiguation_render(&f.config, &request, NULL, &p, error, sizeof(error)) ==
               CE_CSL_DISAMBIGUATION_LIMIT &&
           !p);
    fixture_free(&f);
    /* A fully stripped first person's empty span must not claim primary. */
    fixture(
        &f,
        "<macro name='n'><names variable='author'><name form='short'/></names><names variable='editor'><name "
        "form='short'/></names></macro>",
        "<citation><layout><text macro='n' strip-periods='true'/></layout></citation>",
        "[{\"id\":\"a\",\"author\":[{\"family\":\".\"}],\"editor\":[{\"family\":\"E\",\"given\":\"Ella\"}]}]",
        1);
    p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, NULL);
    key_is(p->key, "E");
    assert(!p->names[0].people[0].primary && p->names[1].people[0].primary);
    ce_csl_disambiguation_render_release(p);
    fixture_free(&f);
}
static CeCslDisambiguation *build(Fixture *f, bool names, bool suffix, unsigned profiles) {
    CeCslDisambiguationProvider provider = ce_csl_disambiguation_render_provider(&f->config);
    CeCslDisambiguationOptions options = {.add_given_names = true,
                                          .add_names = names,
                                          .add_year_suffix = suffix,
                                          .given_rule = CE_CSL_DISAMBIGUATE_PRIMARY_NAME_INITIALS,
                                          .profiles = profiles};
    CeCslDisambiguation *result = NULL;
    CeCslDisambiguationStatus status = ce_csl_disambiguation_build(f->input, f->registry, &options, &provider,
                                                                   NULL, &result, error, sizeof(error));
    if (status != CE_CSL_DISAMBIGUATION_OK)
        fprintf(stderr, "driver %d: %s\n", (int)status, error);
    assert(status == CE_CSL_DISAMBIGUATION_OK && result);
    return result;
}
static void test_driver(void) {
    Fixture f;
    fixture(&f, "",
            "<citation><layout><names variable='author'><name form='short'/></names><date variable='issued' "
            "prefix=', '><date-part name='year'/></date><text "
            "variable='year-suffix'/></layout></citation><bibliography><layout><names "
            "variable='author'><name/></names><date variable='issued' prefix=', '><date-part "
            "name='year'/></date><text variable='year-suffix'/></layout></bibliography>",
            "[{\"id\":\"a\",\"author\":[{\"family\":\"Doe\",\"given\":\"Alice\"}],\"issued\":{\"date-parts\":"
            "[[2024]]}},{\"id\":\"b\",\"author\":[{\"family\":\"Doe\",\"given\":\"Bob\"}],\"issued\":{\"date-"
            "parts\":[[2024]]}}]",
            1);
    CeCslDisambiguation *d = build(&f, true, true,
                                   CE_CSL_DISAMBIGUATION_FIRST | CE_CSL_DISAMBIGUATION_SUBSEQUENT |
                                       CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY);
    assert(!ce_csl_disambiguation_unresolved_count(d));
    CeCslDisambiguationProbe *p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, ce_csl_disambiguation_item(d, 0));
    key_is(p->key, "A. Doe, 2024");
    ce_csl_disambiguation_render_release(p);
    p = probe(&f, 1, CE_CSL_DISAMBIGUATION_SUBSEQUENT, ce_csl_disambiguation_item(d, 1));
    key_is(p->key, "B. Doe, 2024");
    ce_csl_disambiguation_render_release(p);
    ce_csl_disambiguation_free(d);
    input(&f,
          "[{\"id\":\"a\",\"author\":[{\"family\":\"Doe\",\"given\":\"Alice\"}],\"issued\":{\"date-parts\":[["
          "2024]]}},{\"id\":\"b\",\"author\":[{\"family\":\"Doe\",\"given\":\"Andrew\"}],\"issued\":{\"date-"
          "parts\":[[2024]]}}]",
          1);
    d = build(&f, true, true, CE_CSL_DISAMBIGUATION_FIRST | CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY);
    assert(!strcmp(ce_csl_disambiguation_item(d, 0)->year_suffix, "a"));
    assert(!strcmp(ce_csl_disambiguation_item(d, 1)->year_suffix, "b"));
    ce_csl_disambiguation_free(d);
    input(&f,
          "[{\"id\":\"a\",\"author\":[{\"family\":\"Doe\",\"given\":\"Alice\"}],\"issued\":{\"date-parts\":[["
          "2024]]}}]",
          1);
    d = build(&f, true, true, CE_CSL_DISAMBIGUATION_FIRST | CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY);
    assert(!*ce_csl_disambiguation_item(d, 0)->year_suffix);
    p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, ce_csl_disambiguation_item(d, 0));
    key_is(p->key, "Doe, 2024");
    ce_csl_disambiguation_render_release(p);
    ce_csl_disambiguation_free(d);
    fixture_free(&f);
    fixture(&f, "",
            "<citation><layout><names variable='author'><name form='short' et-al-min='3' et-al-use-first='1' "
            "et-al-use-last='true'/></names><text "
            "variable='year-suffix'/></layout></citation><bibliography><layout><names "
            "variable='author'><name/></names><text variable='year-suffix'/></layout></bibliography>",
            "[{\"id\":\"a\",\"author\":[{\"family\":\"A\"},{\"family\":\"B\"},{\"family\":\"C\"}]},{\"id\":"
            "\"b\",\"author\":[{\"family\":\"A\"},{\"family\":\"D\"},{\"family\":\"C\"}]}]",
            1);
    d = build(&f, true, true, CE_CSL_DISAMBIGUATION_FIRST | CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY);
    p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, ce_csl_disambiguation_item(d, 0));
    key_is(p->key, "A, B, C");
    assert(p->names[0].people[2].rendered && !p->names[0].next_minimum_names);
    ce_csl_disambiguation_render_release(p);
    ce_csl_disambiguation_free(d);
    fixture_free(&f);
}
typedef union Header {
    size_t size;
    long double alignment;
    void *pointer_alignment;
} Header;
typedef struct Memory {
    size_t calls, fail_at, live, bytes, peak;
} Memory;
static void *allocate(void *context, size_t size) {
    Memory *m = context;
    if (m->calls++ == m->fail_at)
        return NULL;
    Header *h = malloc(sizeof(*h) + size);
    assert(h);
    h->size = size;
    ++m->live;
    m->bytes += size;
    if (m->peak < m->bytes)
        m->peak = m->bytes;
    return h + 1;
}
static void release(void *context, void *pointer) {
    if (!pointer)
        return;
    Memory *m = context;
    Header *h = (Header *)pointer - 1;
    --m->live;
    m->bytes -= h->size;
    free(h);
}
static bool cancelled(void *context) {
    size_t *left = context;
    return !(*left)--;
}
static void test_errors_oom(void) {
    Fixture f;
    fixture(&f, "",
            "<citation><layout><names variable='author'><name form='short'/></names></layout></citation>",
            "[{\"id\":\"a\",\"author\":[{\"family\":\"Doe\",\"given\":\"Alice\"}]}]", 1);
    CeCslDisambiguationItem state = {.item_index = 0};
    CeCslDisambiguationRequest request = {
        .item_index = 0, .profile = CE_CSL_DISAMBIGUATION_FIRST, .state = &state};
    CeCslDisambiguationProbe *p = NULL;
    size_t success = 0;
    for (size_t at = 0; at < 20000; ++at) {
        Memory memory = {.fail_at = at};
        CeCslOutputAllocator allocator = {&memory, allocate, release};
        CeCslDisambiguationStatus status =
            ce_csl_disambiguation_render(&f.config, &request, &allocator, &p, error, sizeof(error));
        if (status == CE_CSL_DISAMBIGUATION_OK) {
            key_is(p->key, "Doe");
            key_is(p->names[0].people[0].initials_key, "A. Doe");
            ce_csl_disambiguation_render_release(p);
            p = NULL;
            assert(!memory.live && !memory.bytes);
            success = at;
            break;
        }
        if (status != CE_CSL_DISAMBIGUATION_OOM)
            fprintf(stderr, "failpoint %zu status %d: %s\n", at, (int)status, error);
        assert(status == CE_CSL_DISAMBIGUATION_OOM && !p && !memory.live && !memory.bytes);
    }
    assert(success > 100 && success < 20000);
    printf("Provider allocation failpoints: %zu\n", success);
    size_t ticks = 5;
    f.config.cancel_context = &ticks;
    f.config.cancelled = cancelled;
    assert(ce_csl_disambiguation_render(&f.config, &request, NULL, &p, error, sizeof(error)) ==
               CE_CSL_DISAMBIGUATION_CANCELLED &&
           !p);
    f.config.cancelled = NULL;
    f.config.max_nodes = 1;
    assert(ce_csl_disambiguation_render(&f.config, &request, NULL, &p, error, sizeof(error)) ==
               CE_CSL_DISAMBIGUATION_LIMIT &&
           !p);
    f.config.max_nodes = 0;
    f.config.max_work = 1;
    assert(ce_csl_disambiguation_render(&f.config, &request, NULL, &p, error, sizeof(error)) ==
               CE_CSL_DISAMBIGUATION_LIMIT &&
           !p);
    f.config.max_work = 0;
    strcpy(state.year_suffix, "a");
    assert(ce_csl_disambiguation_render(&f.config, &request, NULL, &p, error, sizeof(error)) ==
               CE_CSL_DISAMBIGUATION_UNSUPPORTED &&
           !p);
    state.year_suffix[0] = 0;
    state.item_index = 1;
    assert(ce_csl_disambiguation_render(&f.config, &request, NULL, &p, error, sizeof(error)) ==
               CE_CSL_DISAMBIGUATION_INVALID &&
           !p);
    state.item_index = 0;
    request.profile = CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY;
    assert(ce_csl_disambiguation_render(&f.config, &request, NULL, &p, error, sizeof(error)) ==
               CE_CSL_DISAMBIGUATION_UNSUPPORTED &&
           !p);
    fixture_free(&f);
    /* Explicit suffix through a macro in an unchosen branch is reachable but
     * is not invented by the provider when the actual branch omits it. */
    fixture(&f, "<macro name='suffix'><text variable='year-suffix'/></macro>",
            "<citation><layout><choose><if variable='title'><text macro='suffix'/></if><else><text "
            "value='X'/></else></choose></layout></citation>",
            "[{\"id\":\"a\"}]", 1);
    strcpy(state.year_suffix, "b");
    p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, &state);
    key_is(p->key, "X");
    ce_csl_disambiguation_render_release(p);
    fixture_free(&f);
    /* Expanded strings stay in the shared provider/driver allocator; there is
     * no direct malloc fallback when a higher layer refuses allocation. */
    fixture(&f, "",
            "<citation><layout><names variable='author'><name form='short'/></names><text "
            "variable='year-suffix'/></layout></citation>",
            "[{\"id\":\"a\",\"author\":[{\"family\":\"Doe\",\"given\":\"Alice\"}]},{\"id\":\"b\",\"author\":["
            "{\"family\":\"Doe\",\"given\":\"Bob\"}]}]",
            2);
    CeCslDisambiguationProvider provider = ce_csl_disambiguation_render_provider(&f.config);
    CeCslDisambiguationOptions options = {.add_given_names = true,
                                          .given_rule = CE_CSL_DISAMBIGUATE_PRIMARY_NAME_INITIALS};
    CeCslDisambiguation *old = build(&f, false, false, CE_CSL_DISAMBIGUATION_FIRST), *result;
    for (size_t at = 0; at < 30000; ++at) {
        Memory memory = {.fail_at = at};
        CeCslOutputAllocator allocator = {&memory, allocate, release};
        result = old;
        CeCslDisambiguationStatus status = ce_csl_disambiguation_build(
            f.input, f.registry, &options, &provider, &allocator, &result, error, sizeof(error));
        if (status == CE_CSL_DISAMBIGUATION_OK) {
            old = NULL;
            ce_csl_disambiguation_free(result);
            assert(!memory.live);
            success = at;
            break;
        }
        assert(result == old && !memory.live && !memory.bytes);
        assert(ce_csl_disambiguation_item(old, 0)->adjustment_count);
        assert(at < 29999);
    }
    printf("Driver + provider allocation failpoints: %zu\n", success);
    ce_csl_disambiguation_free(old);
    fixture_free(&f);
}
static void test_key_limit(void) {
    Fixture f;
    fixture(&f, "", "<citation><layout><text variable='title'/></layout></citation>", "[{\"id\":\"a\"}]", 1);
    size_t maximum = CE_CSL_DISAMBIGUATION_MAX_KEY_BYTES;
    char *items = malloc(maximum + 100);
    assert(items);
    const char *prefix = "[{\"id\":\"a\",\"title\":\"";
    size_t offset = strlen(prefix);
    memcpy(items, prefix, offset);
    memset(items + offset, 'A', maximum);
    strcpy(items + offset + maximum, "\"}]");
    input(&f, items, 1);
    CeCslDisambiguationProbe *p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, NULL);
    assert(p->key.size == maximum);
    ce_csl_disambiguation_render_release(p);
    p = NULL;
    items[offset + maximum] = 'A';
    strcpy(items + offset + maximum + 1, "\"}]");
    input(&f, items, 1);
    CeCslDisambiguationItem state = {.item_index = 0};
    CeCslDisambiguationRequest request = {
        .item_index = 0, .profile = CE_CSL_DISAMBIGUATION_FIRST, .state = &state};
    assert(ce_csl_disambiguation_render(&f.config, &request, NULL, &p, error, sizeof(error)) ==
               CE_CSL_DISAMBIGUATION_LIMIT &&
           !p);
    free(items);
    fixture_free(&f);
}
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(!fseek(f, 0, SEEK_END));
    long size = ftell(f);
    assert(size >= 0);
    rewind(f);
    char *text = malloc((size_t)size + 1);
    assert(text);
    assert(fread(text, 1, (size_t)size, f) == (size_t)size);
    assert(!fclose(f));
    text[size] = 0;
    return text;
}
static void test_apa(const char *path, const char *locale_path) {
    Fixture f = {0};
    char *xml = read_file(path);
    OK(ce_csl_style_load(&f.style, xml, strlen(xml), error, sizeof(error)));
    free(xml);
    xml = read_file(locale_path);
    f.locale_xml = ce_csl_xml_parse(xml, strlen(xml), error, sizeof(error));
    assert(f.locale_xml);
    free(xml);
    const CeCslXmlNode *roots[] = {f.locale_xml->root};
    CeCslLocaleCatalog catalog = {.roots = roots, .root_count = 1};
    f.locale = ce_csl_locale_new(ce_csl_style_root(f.style), &catalog, "en-US", NULL, error, sizeof(error));
    assert(f.locale);
    f.config.style = f.style;
    f.config.locale = f.locale;
    input(&f,
          "[{\"id\":\"a\",\"type\":\"book\",\"title\":\"Example "
          "A\",\"author\":[{\"family\":\"Doe\",\"given\":\"Alice\"}],\"issued\":{\"date-parts\":[[2024]]}},{"
          "\"id\":\"b\",\"type\":\"book\",\"title\":\"Example "
          "B\",\"author\":[{\"family\":\"Doe\",\"given\":\"Bob\"}],\"issued\":{\"date-parts\":[[2024]]}}]",
          2);
    CeCslDisambiguation *d =
        build(&f, true, true, CE_CSL_DISAMBIGUATION_FIRST | CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY);
    assert(!ce_csl_disambiguation_unresolved_count(d));
    CeCslDisambiguationProbe *p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, ce_csl_disambiguation_item(d, 0));
    key_is(p->key, "A. Doe, 2024");
    ce_csl_disambiguation_render_release(p);
    ce_csl_disambiguation_free(d);
    input(&f,
          "[{\"id\":\"a\",\"type\":\"book\",\"title\":\"Example "
          "A\",\"author\":[{\"family\":\"Doe\",\"given\":\"Alice\"}],\"issued\":{\"date-parts\":[[2024]]}},{"
          "\"id\":\"b\",\"type\":\"book\",\"title\":\"Example "
          "B\",\"author\":[{\"family\":\"Doe\",\"given\":\"Andrew\"}],\"issued\":{\"date-parts\":[[2024]]}}]",
          2);
    d = build(&f, true, true, CE_CSL_DISAMBIGUATION_FIRST | CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY);
    assert(!strcmp(ce_csl_disambiguation_item(d, 0)->year_suffix, "a"));
    assert(!strcmp(ce_csl_disambiguation_item(d, 1)->year_suffix, "b"));
    ce_csl_disambiguation_free(d);
    input(&f,
          "[{\"id\":\"a\",\"type\":\"book\",\"title\":\"Example "
          "A\",\"author\":[{\"family\":\"Doe\",\"given\":\"Alice\"}],\"issued\":{\"date-parts\":[[2024]]}}]",
          1);
    d = build(&f, true, true, CE_CSL_DISAMBIGUATION_FIRST | CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY);
    p = probe(&f, 0, CE_CSL_DISAMBIGUATION_FIRST, ce_csl_disambiguation_item(d, 0));
    key_is(p->key, "Doe, 2024");
    ce_csl_disambiguation_render_release(p);
    ce_csl_disambiguation_free(d);
    fixture_free(&f);
    puts("Pinned APA: distinct initials, same-initial suffixes, deletion rebuild passed");
}
int main(int argc, char **argv) {
    test_context();
    test_selection_and_capability();
    test_binding_edges();
    test_driver();
    test_errors_oom();
    test_key_limit();
    if (argc == 3)
        test_apa(argv[1], argv[2]);
    puts("CSL real disambiguation provider: finalized keys, names/identity, limits and OOM passed");
    return 0;
}
