#include "csl_cluster.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[512];
static size_t cases;
enum { MAX_ROWS = 12 };
typedef struct Fixture {
    CeCslStyle *style; CeCslLocale *locale;
    CeCslInputItem items[MAX_ROWS]; CeCslInputCite cites[MAX_ROWS];
    CeCslEvaluation *evaluations[MAX_ROWS]; CeCslFinalized *finals[MAX_ROWS];
    CeCslClusterEntry entries[MAX_ROWS]; CeCslCluster *result;
    size_t count;
} Fixture;
static void initialize(Fixture *f, size_t count) {
    f->count = count;
    for (size_t i = 0; i < count; ++i) {
        f->items[i] = (CeCslInputItem){.id = "fixture", .data = cJSON_CreateObject()};
        f->cites[i] = (CeCslInputCite){.item_index = i, .data = cJSON_CreateObject()};
        assert(f->items[i].data && f->cites[i].data);
        f->entries[i] = (CeCslClusterEntry){.source_cite_index = 10 + i, .cite = &f->cites[i],
            .citation_number = {true, true, i + 1}};
    }
}
static void fixture(Fixture *f, const char *attributes, const char *macros, const char *body, size_t count) {
    memset(f, 0, sizeof(*f));
    const char *head = "<style xmlns='http://purl.org/net/xbiblio/csl' version='1.0' class='in-text'>"
        "<info><title>Own cluster regression</title><id>https://example.invalid/cluster</id>"
        "<updated>2026-01-01T00:00:00+00:00</updated></info>"
        "<locale><style-options punctuation-in-quote='true'/><terms><term name='open-quote'>“</term>"
        "<term name='close-quote'>”</term><term name='open-inner-quote'>‘</term><term name='close-inner-quote'>’</term>"
        "<term name='et-al'>et al.</term><term name='and'>and</term></terms></locale>";
    size_t size = strlen(head) + strlen(attributes) + strlen(macros) + strlen(body) + 256;
    char *xml = malloc(size); assert(xml);
    snprintf(xml, size, "%s%s<citation %s><layout prefix='(' suffix=')' delimiter='; '>%s</layout></citation></style>",
        head, macros, attributes, body);
    bool ok = ce_csl_style_load(&f->style, xml, strlen(xml), error, sizeof(error));
    if (!ok) fprintf(stderr, "style: %s\n", error);
    assert(ok); free(xml);
    f->locale = ce_csl_locale_new(ce_csl_style_root(f->style), NULL, "en-US", NULL, error, sizeof(error));
    assert(f->locale); initialize(f, count);
}
static void item(Fixture *f, size_t row, const char *json) {
    cJSON_Delete((cJSON *)f->items[row].data); f->items[row].data = cJSON_Parse(json); assert(f->items[row].data);
}
static void name_year(Fixture *f, size_t row, const char *name, unsigned year) {
    cJSON *data = (cJSON *)f->items[row].data;
    assert(cJSON_AddStringToObject(data, "type", "book"));
    if (name) {
        cJSON *names = cJSON_AddArrayToObject(data, "author"), *person = cJSON_CreateObject();
        assert(names && person && cJSON_AddItemToArray(names, person));
        assert(cJSON_AddStringToObject(person, "family", name));
    }
    if (year) {
        cJSON *date = cJSON_AddObjectToObject(data, "issued"), *parts = cJSON_AddArrayToObject(date, "date-parts");
        cJSON *point = cJSON_CreateArray(); assert(parts && point && cJSON_AddItemToArray(parts, point));
        assert(cJSON_AddItemToArray(point, cJSON_CreateNumber(year)));
    }
}
static void cite_text(Fixture *f, size_t row, const char *key, const char *text) {
    assert(cJSON_AddStringToObject((cJSON *)f->cites[row].data, key, text));
}
static void cite_bool(Fixture *f, size_t row, const char *key, bool value) {
    assert(cJSON_AddBoolToObject((cJSON *)f->cites[row].data, key, value));
}
static void evaluate(Fixture *f) {
    for (size_t i = 0; i < f->count; ++i) {
        CeCslGeneratedVariables generated = {.citation_number = f->entries[i].citation_number,
            .first_reference_note_number = {true, false, 0}, .year_suffix = {true, "", 0}, .citation_label = {true, "", 0}};
        CeCslEvalRequest request = {.style = f->style, .locale = f->locale,
            .section = ce_csl_style_citation(f->style), .position = CE_CSL_POSITION_FIRST,
            .variables = {.item = &f->items[i], .cite = &f->cites[i], .generated = &generated}};
        bool ok = ce_csl_evaluate(&request, NULL, &f->evaluations[i], error, sizeof(error)) &&
            ce_csl_finalize(f->evaluations[i], NULL, &f->finals[i], error, sizeof(error));
        if (!ok) fprintf(stderr, "evaluate: %s\n", error);
        assert(ok); f->entries[i].evaluation = f->evaluations[i]; f->entries[i].finalized = f->finals[i];
    }
}
static CeCslClusterStatus build(Fixture *f, const CeCslClusterOptions *options,
    const CeCslOutputAllocator *allocator) {
    return ce_csl_cluster_build(f->style, f->locale, f->entries, f->count,
        options, allocator, &f->result, error, sizeof(error));
}
static void text_is(Fixture *f, const char *expected) {
    size_t size = 0; const char *text = ce_csl_inline_text(ce_csl_cluster_inline(f->result), &size);
    if (!text || size != strlen(expected) || memcmp(text, expected, size))
        fprintf(stderr, "expected <%s>, actual <%.*s>\n", expected, (int)size, text ? text : "NULL");
    assert(text && size == strlen(expected) && !memcmp(text, expected, size));
}
static void success(Fixture *f, const char *expected) {
    evaluate(f); CeCslClusterStatus status = build(f, NULL, NULL);
    if (status != CE_CSL_CLUSTER_OK) fprintf(stderr, "cluster %d: %s\n", status, error);
    assert(status == CE_CSL_CLUSTER_OK); text_is(f, expected); ++cases;
}
static void fixture_free(Fixture *f) {
    ce_csl_cluster_free(f->result);
    for (size_t i = 0; i < f->count; ++i) {
        ce_csl_finalized_free(f->finals[i]); ce_csl_evaluation_free(f->evaluations[i]);
        cJSON_Delete((cJSON *)f->items[i].data); cJSON_Delete((cJSON *)f->cites[i].data);
    }
    ce_csl_locale_free(f->locale); ce_csl_style_free(f->style);
}
static const char *number_body = "<text variable='citation-number'/>";
static const char *name_body = "<group delimiter=', '><names variable='author'><name form='short'/><substitute>"
    "<text variable='title'/></substitute></names><date variable='issued'><date-part name='year'/></date>"
    "<text variable='locator' prefix='p. '/></group>";
typedef struct NumericCase {
    uint64_t numbers[6]; size_t count;
    int prefix, suffix, locator; bool show_locator;
    const char *expected;
} NumericCase;
static void test_numeric(void) {
    static const NumericCase tests[] = {
        {{1},1,-1,-1,-1,false,"(1)"}, {{1,2},2,-1,-1,-1,false,"(1; 2)"},
        {{1,2,3},3,-1,-1,-1,false,"(1–3)"}, {{1,2,3,4},4,-1,-1,-1,false,"(1–4)"},
        {{1,2,3,5},4,-1,-1,-1,false,"(1–3; 5)"}, {{1,2,3,5,6,7},6,-1,-1,-1,false,"(1–3; 5–7)"},
        {{3,2,1},3,-1,-1,-1,false,"(3; 2; 1)"}, {{1,3,4},3,-1,-1,-1,false,"(1; 3; 4)"},
        {{1,2,2,3,4},5,-1,-1,-1,false,"(1; 2; 2–4)"}, {{1,1,1},3,-1,-1,-1,false,"(1; 1; 1)"},
        {{UINT64_MAX-2,UINT64_MAX-1,UINT64_MAX},3,-1,-1,-1,false,"(18446744073709551613–18446744073709551615)"},
        {{UINT64_MAX,1,2},3,-1,-1,-1,false,"(18446744073709551615; 1; 2)"},
        {{1,2,3},3,0,-1,-1,false,"(see 1–3)"}, {{1,2,3},3,1,-1,-1,false,"(1; see 2; 3)"},
        {{1,2,3},3,2,-1,-1,false,"(1; 2; see 3)"}, {{1,2,3,4},4,1,-1,-1,false,"(1; see 2–4)"},
        {{1,2,3},3,-1,0,-1,false,"(1 tail; 2; 3)"}, {{1,2,3},3,-1,1,-1,false,"(1; 2 tail; 3)"},
        {{1,2,3},3,-1,2,-1,false,"(1; 2; 3 tail)"}, {{1,2,3,4},4,-1,0,-1,false,"(1 tail; 2–4)"},
        {{1,2,3,4},4,-1,3,-1,false,"(1–3; 4 tail)"},
        {{1,2,3},3,-1,-1,0,true,"(1, p. 4; 2; 3)"}, {{1,2,3},3,-1,-1,1,true,"(1; 2, p. 4; 3)"},
        {{1,2,3},3,-1,-1,2,true,"(1; 2; 3, p. 4)"}, {{1,2,3},3,-1,-1,1,false,"(1–3)"}
    };
    for (size_t t = 0; t < sizeof(tests)/sizeof(*tests); ++t) {
        const NumericCase *test = &tests[t]; Fixture f;
        fixture(&f, "collapse='citation-number'", "", test->show_locator ?
            "<group delimiter=', '><text variable='citation-number'/><text variable='locator' prefix='p. '/></group>" : number_body,
            test->count);
        for (size_t i = 0; i < test->count; ++i) f.entries[i].citation_number.value = test->numbers[i];
        if (test->prefix >= 0) cite_text(&f, (size_t)test->prefix, "prefix", "see ");
        if (test->suffix >= 0) cite_text(&f, (size_t)test->suffix, "suffix", " tail");
        if (test->locator >= 0) cite_text(&f, (size_t)test->locator, "locator", "4");
        success(&f, test->expected);
        for (size_t i = 0; i < test->count; ++i) assert(ce_csl_cluster_order(f.result, i) == i);
        fixture_free(&f);
    }
    Fixture f; fixture(&f, "collapse='citation-number' after-collapse-delimiter=' / '", "", number_body, 4);
    f.entries[3].citation_number.value = 5; success(&f, "(1–3 / 5)");
    const CeCslClusterGroup *group = ce_csl_cluster_group(f.result, 0);
    assert(group->kind == CE_CSL_CLUSTER_NUMERIC_RANGE && group->member_count == 3 && group->piece_count == 2);
    assert(ce_csl_cluster_piece(f.result, 0)->source_cite_index == 10);
    assert(ce_csl_cluster_piece(f.result, 1)->source_cite_index == 12);
    bool start = false, end = false;
    const CeCslFinalized *final = ce_csl_cluster_finalized(f.result);
    for (size_t i = 0; i < ce_csl_finalized_span_count(final); ++i) {
        const CeCslFinalSpan *span = ce_csl_finalized_span(final, i);
        start |= span->source_cite_index == 10; end |= span->source_cite_index == 12;
        assert(span->source_cite_index != 11);
    }
    assert(start && end); fixture_free(&f);
    fixture(&f, "collapse='citation-number'", "", "<text variable='citation-number' prefix='[' suffix=']'/>", 3);
    success(&f, "([1]–[3])"); fixture_free(&f);
    fixture(&f, "collapse='citation-number'", "", "<text value='Ref '/><text variable='citation-number'/>", 3);
    success(&f, "(Ref 1; Ref 2; Ref 3)"); fixture_free(&f);
    fixture(&f, "collapse='citation-number'", "", "<choose><if variable='title'><text variable='citation-number' prefix='A'/></if>"
        "<else><text variable='citation-number'/></else></choose>", 3);
    item(&f, 1, "{\"title\":\"x\"}"); success(&f, "(1; A2; 3)"); fixture_free(&f);
    fixture(&f, "collapse='citation-number'", "", "<number variable='citation-number' form='roman'/>", 3);
    success(&f, "(i–iii)"); fixture_free(&f);
}
static void test_grouping(void) {
    const char *attrs[] = {"", "cite-group-delimiter=' / '", "collapse='year'", "collapse='year' after-collapse-delimiter=' / '"};
    const char *expected[] = {"(Doe, 2000; Roe, 2001; Doe, 2002; Poe, 2003; Doe, 2004)",
        "(Doe, 2000 / Doe, 2002 / Doe, 2004; Roe, 2001; Poe, 2003)",
        "(Doe, 2000, 2002, 2004; Roe, 2001; Poe, 2003)",
        "(Doe, 2000, 2002, 2004 / Roe, 2001; Poe, 2003)"};
    const char *names[] = {"Doe", "Roe", "Doe", "Poe", "Doe"};
    for (size_t a = 0; a < 4; ++a) {
        Fixture f; fixture(&f, attrs[a], "", name_body, 5);
        for (size_t i = 0; i < 5; ++i) name_year(&f, i, names[i], 2000 + (unsigned)i);
        success(&f, expected[a]);
        const size_t grouped[] = {0, 2, 4, 1, 3};
        for (size_t i = 0; i < 5; ++i) assert(ce_csl_cluster_order(f.result, i) == (a ? grouped[i] : i));
        fixture_free(&f);
    }
    Fixture f; fixture(&f, "collapse='year'", "", name_body, 3);
    for (size_t i = 0; i < 3; ++i) name_year(&f, i, "Doe", 2000 + (unsigned)i);
    cite_text(&f, 1, "prefix", "see "); cite_text(&f, 1, "locator", "4"); cite_text(&f, 2, "suffix", " tail");
    success(&f, "(Doe, 2000, see 2001, p. 4, 2002 tail)"); fixture_free(&f);
    fixture(&f, "collapse='year'", "", name_body, 2);
    name_year(&f, 0, "Doe", 2000); name_year(&f, 1, "doe", 2001);
    success(&f, "(Doe, 2000; doe, 2001)"); fixture_free(&f);
    fixture(&f, "collapse='year'", "", name_body, 2);
    name_year(&f, 0, NULL, 2000); name_year(&f, 1, NULL, 2001);
    cite_text(&f, 0, "prefix", "first "); success(&f, "(first 2000; 2001)"); fixture_free(&f);
    fixture(&f, "collapse='year'", "", name_body, 2);
    item(&f, 0, "{\"title\":\"Common\",\"issued\":{\"date-parts\":[[2000]]}}");
    item(&f, 1, "{\"title\":\"Common\",\"issued\":{\"date-parts\":[[2001]]}}");
    success(&f, "(Common, 2000, 2001)"); fixture_free(&f);
    fixture(&f, "collapse='year'", "", "<group delimiter=', '><names variable='author'><name form='short'/></names>"
        "<names variable='editor'><name form='short'/></names><date variable='issued'><date-part name='year'/></date></group>", 2);
    item(&f, 0, "{\"author\":[{\"family\":\"Doe\"}],\"editor\":[{\"family\":\"X\"}],\"issued\":{\"date-parts\":[[2000]]}}");
    item(&f, 1, "{\"author\":[{\"family\":\"Doe\"}],\"editor\":[{\"family\":\"Y\"}],\"issued\":{\"date-parts\":[[2001]]}}");
    success(&f, "(Doe, X, 2000, Y, 2001)"); fixture_free(&f);
    fixture(&f, "collapse='year'", "", "<group delimiter=', '><names variable='author'><name form='short'/><substitute>"
        "<names variable='editor'/></substitute></names><names variable='editor'><name form='short'/></names>"
        "<date variable='issued'><date-part name='year'/></date></group>", 2);
    item(&f, 0, "{\"editor\":[{\"family\":\"Doe\"}],\"issued\":{\"date-parts\":[[2000]]}}");
    item(&f, 1, "{\"editor\":[{\"family\":\"Doe\"}],\"issued\":{\"date-parts\":[[2001]]}}");
    success(&f, "(Doe, 2000, 2001)"); fixture_free(&f);
    fixture(&f, "collapse='year'", "", name_body, 3);
    name_year(&f, 0, "Doe", 0); name_year(&f, 1, "Doe", 0); name_year(&f, 2, "Roe", 2000);
    success(&f, "(Doe; Roe, 2000)"); assert(!ce_csl_cluster_piece(f.result, 1)->visible); fixture_free(&f);
    fixture(&f, "collapse='year' after-collapse-delimiter=' / '", "", name_body, 4);
    name_year(&f, 0, "A", 2000); name_year(&f, 1, "B", 0); name_year(&f, 2, "B", 0); name_year(&f, 3, "C", 2001);
    cite_bool(&f, 1, "suppress-author", true); success(&f, "(A, 2000; C, 2001)");
    assert(!ce_csl_cluster_group(f.result, 1)->visible); fixture_free(&f);
    fixture(&f, "collapse='year-suffix'", "", name_body, 2);
    name_year(&f, 0, "Doe", 2000); name_year(&f, 1, "Doe", 2001);
    success(&f, "(Doe, 2000, 2001)"); fixture_free(&f);
    fixture(&f, "collapse='year-suffix-ranged' disambiguate-add-year-suffix='false'", "", name_body, 2);
    name_year(&f, 0, "Doe", 2000); name_year(&f, 1, "Doe", 2001);
    success(&f, "(Doe, 2000, 2001)"); fixture_free(&f);
}
static void test_flags_and_quotes(void) {
    Fixture f; fixture(&f, "", "", name_body, 1); name_year(&f, 0, "Doe", 2000);
    cite_bool(&f, 0, "suppress-author", true); success(&f, "(2000)"); fixture_free(&f);
    fixture(&f, "collapse='year'", "", name_body, 2);
    name_year(&f, 0, "Doe", 2000); name_year(&f, 1, "Doe", 2001); cite_bool(&f, 0, "suppress-author", true);
    success(&f, "(2000, 2001)"); fixture_free(&f);
    fixture(&f, "", "", number_body, 1); cite_text(&f, 0, "prefix", "<i>x</i>"); cite_text(&f, 0, "suffix", "tail");
    success(&f, "(<i>x</i>1tail)"); fixture_free(&f);
    fixture(&f, "", "", "<text variable='title' quotes='true'/>", 2);
    item(&f, 0, "{\"title\":\"First\"}"); item(&f, 1, "{\"title\":\"Second\"}");
    success(&f, "(“First”; “Second”)"); fixture_free(&f);
    fixture(&f, "cite-group-delimiter=', '", "", "<names variable='author'><name form='short'/></names><text variable='title' quotes='true'/>", 2);
    item(&f, 0, "{\"author\":[{\"family\":\"Doe\"}],\"title\":\"First\"}");
    item(&f, 1, "{\"author\":[{\"family\":\"Doe\"}],\"title\":\"Second\"}");
    success(&f, "(Doe“First,” Doe“Second”)"); fixture_free(&f);
    fixture(&f, "collapse='year'", "", "<group delimiter=', '><choose><if variable='title'>"
        "<names variable='author' font-style='italic'><name form='short'/></names></if><else>"
        "<names variable='author'><name form='short'/></names></else></choose>"
        "<date variable='issued'><date-part name='year'/></date></group>", 2);
    name_year(&f, 0, "Doe", 2000); name_year(&f, 1, "Doe", 2001);
    assert(cJSON_AddStringToObject((cJSON *)f.items[0].data, "title", "x"));
    success(&f, "(Doe, 2000, 2001)"); fixture_free(&f);
    fixture(&f, "collapse='year'", "", name_body, 2);
    name_year(&f, 0, "École", 2000); name_year(&f, 1, "E\xcc\x81" "cole", 2001);
    success(&f, "(École, 2000; E\xcc\x81" "cole, 2001)"); fixture_free(&f);
}
static bool cancelled(void *context) { size_t *calls = context; return ++*calls > 2; }
typedef struct Cancel { size_t calls, limit; } Cancel;
static bool cancel_after(void *context) { Cancel *c = context; return ++c->calls > c->limit; }
static void test_rejections(void) {
    Fixture f; fixture(&f, "collapse='year'", "", name_body, 2);
    name_year(&f, 0, "Doe", 2000); name_year(&f, 1, "Doe", 2001); success(&f, "(Doe, 2000, 2001)");
    CeCslCluster *old = f.result;
    CeCslClusterOptions limits[] = {{.max_key_bytes = 2}, {.max_keys_bytes = 5}, {.max_compared_bytes = 1},
        {.max_work_units = 1}, {.max_workspace_bytes = 1}};
    for (size_t i = 0; i < sizeof(limits)/sizeof(*limits); ++i) {
        assert(build(&f, &limits[i], NULL) == CE_CSL_CLUSTER_LIMIT && f.result == old); ++cases;
    }
    CeCslClusterOptions too_large = {.max_work_units = CE_CSL_CLUSTER_MAX_WORK + 1};
    assert(build(&f, &too_large, NULL) == CE_CSL_CLUSTER_INVALID && f.result == old); ++cases;
    size_t calls = 0; CeCslClusterOptions cancel = {.cancel_context = &calls, .cancelled = cancelled};
    assert(build(&f, &cancel, NULL) == CE_CSL_CLUSTER_CANCELLED && f.result == old); ++cases;
    f.entries[0].finalized = f.finals[1]; assert(build(&f, NULL, NULL) == CE_CSL_CLUSTER_INVALID && f.result == old); ++cases;
    f.entries[0].finalized = f.finals[0]; f.entries[0].source_cite_index = SIZE_MAX;
    assert(build(&f, NULL, NULL) == CE_CSL_CLUSTER_INVALID && f.result == old); ++cases; f.entries[0].source_cite_index = 10;
    cite_bool(&f, 0, "author-only", true); assert(build(&f, NULL, NULL) == CE_CSL_CLUSTER_UNSUPPORTED && f.result == old); ++cases;
    fixture_free(&f);
    fixture(&f, "collapse='year-suffix' disambiguate-add-year-suffix='true'", "", name_body, 1);
    name_year(&f, 0, "Doe", 2000); evaluate(&f); assert(build(&f, NULL, NULL) == CE_CSL_CLUSTER_UNSUPPORTED); ++cases; fixture_free(&f);
    fixture(&f, "collapse='citation-number'", "", number_body, 3); evaluate(&f);
    f.entries[0].citation_number.value = 99; assert(build(&f, NULL, NULL) == CE_CSL_CLUSTER_INVALID); ++cases; fixture_free(&f);
}
typedef union Header { long double alignment; void *pointer; unsigned char bytes[32]; } Header;
typedef struct Memory { size_t calls, fail, live; } Memory;
static void *fault_allocate(void *context, size_t size) {
    Memory *m = context; if (++m->calls == m->fail) return NULL;
    Header *h = malloc(sizeof(*h) + size); if (!h) return NULL;
    ++m->live; return h + 1;
}
static void fault_free(void *context, void *memory) {
    if (!memory) return;
    Memory *m = context; assert(m->live); --m->live; free((Header *)memory - 1);
}
static size_t test_oom(void) {
    size_t total = 0;
    for (size_t mode = 0; mode < 2; ++mode) {
        Fixture f; fixture(&f, mode ? "collapse='citation-number'" : "collapse='year'", "", mode ? number_body : name_body, 4);
        for (size_t i = 0; i < 4; ++i) name_year(&f, i, i == 1 ? "Roe" : "Doe", 2000 + (unsigned)i);
        evaluate(&f); Memory memory = {0}; CeCslOutputAllocator a = {&memory, fault_allocate, fault_free};
        assert(build(&f, NULL, &a) == CE_CSL_CLUSTER_OK); size_t count = memory.calls, live = memory.live;
        CeCslCluster *old = f.result;
        for (size_t i = 1; i <= count; ++i) {
            memory.calls = 0; memory.fail = i;
            CeCslClusterStatus status = build(&f, NULL, &a);
            if (status != CE_CSL_CLUSTER_OOM) fprintf(stderr, "OOM %zu/%zu returned %d: %s\n", i, count, status, error);
            assert(status == CE_CSL_CLUSTER_OOM && f.result == old && memory.live == live);
        }
        memory.fail = 0;
        Cancel cancel = {0, SIZE_MAX}; CeCslClusterOptions options = {.cancel_context = &cancel, .cancelled = cancel_after};
        assert(build(&f, &options, &a) == CE_CSL_CLUSTER_OK);
        size_t last_work = cancel.calls; old = f.result; live = memory.live;
        cancel.calls = 0; cancel.limit = last_work - 1;
        assert(build(&f, &options, &a) == CE_CSL_CLUSTER_CANCELLED && f.result == old && memory.live == live);
        ++cases; fixture_free(&f); assert(!memory.live); total += count;
    }
    return total;
}
static void test_large_group(void) {
    Fixture f; fixture(&f, "collapse='year'", "", "<names variable='author'><name form='short'/></names>", 1);
    name_year(&f, 0, "Doe", 0); evaluate(&f);
    const size_t count = 4096;
    CeCslClusterEntry *entries = malloc(count * sizeof(*entries)); assert(entries);
    for (size_t i = 0; i < count; ++i) { entries[i] = f.entries[0]; entries[i].source_cite_index = i; }
    CeCslClusterStatus status = ce_csl_cluster_build(f.style, f.locale, entries, count,
        NULL, NULL, &f.result, error, sizeof(error));
    if (status != CE_CSL_CLUSTER_OK) fprintf(stderr, "large group: %d %s\n", status, error);
    assert(status == CE_CSL_CLUSTER_OK); text_is(&f, "(Doe)");
    assert(ce_csl_cluster_count(f.result) == count && ce_csl_cluster_group_count(f.result) == 1);
    for (size_t i = 0; i < count; ++i) {
        assert(ce_csl_cluster_order(f.result, i) == i);
        assert(ce_csl_cluster_piece(f.result, i)->visible == (i == 0));
    }
    assert(ce_csl_cluster_order(f.result, count) == SIZE_MAX && !ce_csl_cluster_group(f.result, 1));
    CeCslCluster *old = f.result;
    assert(ce_csl_cluster_build(f.style, f.locale, entries, 0, NULL, NULL, &f.result, error, sizeof(error)) == CE_CSL_CLUSTER_INVALID);
    assert(ce_csl_cluster_build(f.style, f.locale, entries, CE_CSL_INPUT_MAX_CITES + 1, NULL, NULL, &f.result, error, sizeof(error)) == CE_CSL_CLUSTER_INVALID);
    assert(f.result == old); free(entries); fixture_free(&f); ++cases;
}
static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb"); assert(file); assert(!fseek(file, 0, SEEK_END));
    long length = ftell(file); assert(length >= 0); rewind(file);
    char *data = malloc((size_t)length + 1); assert(data);
    assert(fread(data, 1, (size_t)length, file) == (size_t)length); fclose(file); data[length] = 0; return data;
}
static void real_profiles(char **argv) {
    CeCslXml *xml[2]; const CeCslXmlNode *roots[2];
    for (size_t i = 0; i < 2; ++i) { char *text = read_file(argv[i + 3]); xml[i] = ce_csl_xml_parse(text, strlen(text), error, sizeof(error)); free(text); assert(xml[i]); roots[i] = xml[i]->root; }
    CeCslLocaleCatalog catalog = {.roots = roots, .root_count = 2};
    const char *expected[] = {"(Doe, 2019, 2020; Roe, 2021)", "[1], [2], [3]", "[1–3]"};
    for (size_t s = 0; s < 3; ++s) {
        Fixture f = {0}; char *text = read_file(argv[s]);
        assert(ce_csl_style_load(&f.style, text, strlen(text), error, sizeof(error))); free(text);
        f.locale = ce_csl_locale_new(ce_csl_style_root(f.style), &catalog, NULL, NULL, error, sizeof(error)); assert(f.locale);
        initialize(&f, 3); name_year(&f, 0, "Doe", 2019); name_year(&f, 1, "Doe", 2020); name_year(&f, 2, "Roe", 2021);
        success(&f, expected[s]);
        if (s == 2) for (size_t i = 0; i < ce_csl_output_run_count(ce_csl_cluster_output(f.result)); ++i)
            assert(ce_csl_output_run(ce_csl_cluster_output(f.result), i)->format & CE_CSL_SUPERSCRIPT);
        fixture_free(&f); printf("real cluster profile %zu passed\n", s);
    }
    for (size_t i = 0; i < 2; ++i) ce_csl_xml_free(xml[i]);
}
int main(int argc, char **argv) {
    test_numeric(); test_grouping(); test_flags_and_quotes(); test_rejections(); test_large_group(); size_t failures = test_oom();
    if (argc == 6) real_profiles(argv + 1); else assert(argc == 1);
    printf("CSL cluster passed (%zu owned cases, %zu allocation failures)\n", cases, failures); return 0;
}
