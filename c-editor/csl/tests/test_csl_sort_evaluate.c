#include "csl_sort_evaluate.h"
#include "csl_document.h"
#include "csl_json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[512];
typedef struct Fixture {
    CeCslStyle *style;
    CeCslLocale *locale;
    CeCslInputItem item;
    CeCslGeneratedVariables generated;
    CeCslEvalRequest request;
    const CeCslXmlNode *key;
} Fixture;
static void item(Fixture *f, const char *json) {
    cJSON_Delete((cJSON *)f->item.data);
    f->item.data = cJSON_Parse(json); assert(f->item.data);
}
static void fixture(Fixture *f, const char *macros, const char *key, const char *json) {
    memset(f, 0, sizeof(*f));
    const char *head = "<style xmlns='http://purl.org/net/xbiblio/csl' version='1.0' class='in-text'>"
        "<info><title>Own sort regression</title><id>https://example.invalid/sort</id>"
        "<updated>2026-01-01T00:00:00+00:00</updated></info>"
        "<locale><date form='numeric'><date-part name='year'/><date-part name='month' form='numeric'/><date-part name='day'/></date>"
        "<terms><term name='and'>and</term><term name='et-al'>et al.</term>"
        "<term name='bc'> BC</term><term name='ad'> AD</term>"
        "<term name='open-quote'>“</term><term name='close-quote'>”</term></terms></locale>";
    size_t size = strlen(head) + strlen(macros) + strlen(key) + 256;
    char *xml = malloc(size); assert(xml);
    snprintf(xml, size, "%s%s<citation><sort>%s</sort><layout><text value='unused'/></layout></citation></style>", head, macros, key);
    bool ok = ce_csl_style_load(&f->style, xml, strlen(xml), error, sizeof(error));
    if (!ok) fprintf(stderr, "style: %s\n", error);
    assert(ok); free(xml);
    f->locale = ce_csl_locale_new(ce_csl_style_root(f->style), NULL, "en-US", NULL, error, sizeof(error));
    if (!f->locale) fprintf(stderr, "locale: %s\n", error);
    assert(f->locale);
    f->item.id = "one"; item(f, json);
    f->generated = (CeCslGeneratedVariables){.citation_number = {true, true, 1},
        .first_reference_note_number = {true, false, 0}, .year_suffix = {true, "", 0}, .citation_label = {true, "", 0}};
    f->request = (CeCslEvalRequest){.style = f->style, .locale = f->locale,
        .section = ce_csl_style_citation(f->style), .position = CE_CSL_POSITION_FIRST,
        .variables = {.item = &f->item, .generated = &f->generated}};
    f->key = ce_csl_xml_child(f->request.section, "sort")->children;
}
static void fixture_free(Fixture *f) {
    cJSON_Delete((cJSON *)f->item.data); ce_csl_locale_free(f->locale); ce_csl_style_free(f->style);
}
static void evaluate(Fixture *f, CeCslSortEvaluated *out) {
    bool ok = ce_csl_sort_evaluate(&f->request, f->key, CE_CSL_COLLATION_DEFAULT, NULL, out, error, sizeof(error));
    if (!ok) fprintf(stderr, "sort evaluate: %s\n", error);
    assert(ok);
}
static int compare(const CeCslSortEvaluated *a, const CeCslSortEvaluated *b) {
    assert(a->domain == b->domain && !a->missing && !b->missing);
    size_t n = a->key.length < b->key.length ? a->key.length : b->key.length;
    int d = n ? memcmp(a->key.data, b->key.data, n) : 0;
    return d ? d : (a->key.length > b->key.length) - (a->key.length < b->key.length);
}
static void expect_order(CeCslSortEvaluated *keys, size_t rows, size_t columns, const size_t *expected) {
    CeCslSortOrder order = {0};
    bool ok = ce_csl_sort_evaluated_order(keys, rows, columns, NULL, &order, error, sizeof(error));
    if (!ok) fprintf(stderr, "sort order: %s\n", error);
    assert(ok && order.count == rows);
    for (size_t i = 0; i < rows; ++i) {
        if (order.indices[i] != expected[i]) fprintf(stderr, "row %zu: got %zu expected %zu\n", i, order.indices[i], expected[i]);
        assert(order.indices[i] == expected[i]);
    }
    ce_csl_sort_order_free(&order);
}
static void test_scalars(void) {
    Fixture f; CeCslSortEvaluated keys[5] = {0};
    fixture(&f, "", "<key variable='volume' sort='descending'/>", "{\"volume\":\"184467440737095516160\"}");
    evaluate(&f, &keys[0]);
    item(&f, "{\"volume\":\"-0007\"}"); evaluate(&f, &keys[1]);
    item(&f, "{\"volume\":\"20\"}"); evaluate(&f, &keys[2]);
    item(&f, "{\"volume\":\"+020\"}"); evaluate(&f, &keys[3]);
    item(&f, "{}"); evaluate(&f, &keys[4]);
    const size_t expected[] = {0, 2, 3, 1, 4}; expect_order(keys, 5, 1, expected);
    item(&f, "{\"volume\":\"S20\"}");
    evaluate(&f, &keys[0]); assert(!compare(&keys[0], &keys[2]));
    item(&f, "{\"volume\":\"forthcoming\"}"); evaluate(&f, &keys[1]);
    const size_t mixed[] = {1, 0, 2, 3, 4}; expect_order(keys, 5, 1, mixed);
    CeCslSortOrder order = {0};
    keys[1].language = CE_CSL_COLLATION_TR;
    assert(!ce_csl_sort_evaluated_order(keys, 5, 1, NULL, &order, error, sizeof(error)) && strstr(error, "incompatible"));
    for (size_t i = 0; i < 5; ++i) ce_csl_sort_evaluated_free(&keys[i]);
    fixture_free(&f);

    fixture(&f, "", "<key variable='title'/>", "{\"title\":\"<i>École</i>\"}");
    evaluate(&f, &keys[0]); item(&f, "{\"title\":\"éCOLE\"}"); evaluate(&f, &keys[1]);
    assert(!compare(&keys[0], &keys[1])); item(&f, "{\"title\":\"Ecole\"}"); evaluate(&f, &keys[2]);
    assert(compare(&keys[2], &keys[0]) < 0);
    item(&f, "{\"title\":\"\u200d\"}"); evaluate(&f, &keys[3]); assert(!keys[3].missing);
    for (size_t i = 0; i < 5; ++i) ce_csl_sort_evaluated_free(&keys[i]);
    fixture_free(&f);
}
static void test_names(void) {
    Fixture f; CeCslSortEvaluated keys[5] = {0};
    fixture(&f, "", "<key variable='author' names-min='1' names-use-first='1'/>",
        "{\"author\":[{\"family\":\"Doe\",\"given\":\"A\"},{\"family\":\"Zed\"}]}");
    evaluate(&f, &keys[0]);
    item(&f, "{\"author\":[{\"family\":\"Doe\",\"given\":\"A\"},{\"family\":\"Able\"}]}"); evaluate(&f, &keys[1]);
    assert(compare(&keys[1], &keys[0]) < 0); /* Direct keys keep the entire list. */
    item(&f, "{\"author\":[{\"literal\":\"The Academy\"}]}"); evaluate(&f, &keys[2]);
    item(&f, "{\"author\":[{\"literal\":\"academy\"}]}"); evaluate(&f, &keys[3]); assert(!compare(&keys[2], &keys[3]));
    item(&f, "{\"author\":[{\"literal\":\"Theater\"}]}"); evaluate(&f, &keys[4]); assert(compare(&keys[2], &keys[4]) < 0);
    for (size_t i = 0; i < 5; ++i) ce_csl_sort_evaluated_free(&keys[i]);
    fixture_free(&f);
    fixture(&f, "<macro name='author'><names variable='author'><name form='short' et-al-min='2' et-al-use-first='1'/>"
        "<substitute><text variable='title'/></substitute></names></macro>", "<key macro='author'/>",
        "{\"author\":[{\"family\":\"Moss\",\"given\":\"A\"},{\"family\":\"Zed\"}]}");
    evaluate(&f, &keys[0]);
    item(&f, "{\"author\":[{\"family\":\"Moss\",\"given\":\"B\"},{\"family\":\"Able\"}]}"); evaluate(&f, &keys[1]);
    assert(!compare(&keys[0], &keys[1]));
    item(&f, "{\"title\":\"Alpha\"}"); evaluate(&f, &keys[2]);
    item(&f, "{\"title\":\"Zulu\"}"); evaluate(&f, &keys[3]);
    const size_t expected[] = {2, 0, 1, 3}; expect_order(keys, 4, 1, expected);
    for (size_t i = 0; i < 5; ++i) ce_csl_sort_evaluated_free(&keys[i]);
    fixture_free(&f);
}
static void volume_item(Fixture *f, const char *volume) {
    item(f,"{}");
    if (volume) assert(cJSON_AddStringToObject((cJSON *)f->item.data,"volume",volume));
}
static void test_composite_numbers(void) {
    /* Own fixtures, checked against citeproc@2.4.63's direct-variable black box
     * in both input orders. CSL sorting-macros says cs:number uses the SAME
     * numeric rule; reference macro sorting instead distinguishes affixes and
     * later numbers. We intentionally follow the normative shared rule here.
     * Reference's extraction from invalid numeric prose/decimals is NOT used. */
    const char *values[] = {"S12","12-13","2","10","0012","12b","A2","2, 20",
        "2 & 4","S10","第12卷"," \t2\xE2\x80\x83– 4 ","Alpha word","Zulu word",NULL};
    const size_t ascending[] = {2,6,7,8,11,3,9,0,1,4,5,10,12,13,14};
    const size_t descending[] = {13,12,0,1,4,5,10,3,9,2,6,7,8,11,14};
    for (unsigned macro = 0; macro < 2; ++macro) {
        Fixture f; fixture(&f,macro ? "<macro name='n'><number variable='volume' form='roman'/></macro>" : "",
                          macro ? "<key macro='n'/>" : "<key variable='volume'/>","{}");
        CeCslSortEvaluated keys[15] = {0};
        for (size_t i = 0; i < 15; ++i) {
            volume_item(&f,values[i]); evaluate(&f,&keys[i]);
            if (values[i]) assert(keys[i].domain==CE_CSL_SORT_NUMBER);
        }
        expect_order(keys,15,1,ascending);
        for (size_t i = 0; i < 15; ++i) keys[i].descending=true;
        expect_order(keys,15,1,descending);
        for (size_t i = 0; i < 15; ++i) ce_csl_sort_evaluated_free(&keys[i]);
        const char *text[] = {"12 special","12.5","1e3","Special edition 12","<b>special edition 12</b>"};
        volume_item(&f,"999999999999999999999999999999999999999999"); evaluate(&f,&keys[0]);
        for (size_t i = 0; i < sizeof text/sizeof *text; ++i) {
            volume_item(&f,text[i]); evaluate(&f,&keys[1]);
            assert(compare(&keys[0],&keys[1])<0); /* Entire text, not 12/12/1. */
        }
        volume_item(&f,text[3]); evaluate(&f,&keys[2]); assert(!compare(&keys[1],&keys[2]));
        volume_item(&f,"<i>S0012</i>-13"); evaluate(&f,&keys[1]);
        volume_item(&f,"12"); evaluate(&f,&keys[2]); assert(!compare(&keys[1],&keys[2]));
        volume_item(&f,"2-3x4"); evaluate(&f,&keys[1]);
        assert(compare(&keys[0],&keys[1])<0); /* Invalid later component blocks extraction. */
        volume_item(&f,"<i></i>"); evaluate(&f,&keys[1]); assert(keys[1].missing);
        for (size_t i = 0; i < 15; ++i) ce_csl_sort_evaluated_free(&keys[i]);
        fixture_free(&f);
    }
}
static void test_literal_dates(void) {
    const char *values[] = {"{\"issued\":{\"date-parts\":[[2020]]}}",
        "{\"issued\":{\"literal\":\"in press\"}}","{}",
        "{\"issued\":{\"literal\":\"1900\"}}","{\"issued\":{\"date-parts\":[[-100]]}}",
        "{\"issued\":{\"date-parts\":[[2019]]}}","{\"issued\":{\"literal\":\"forthcoming\"}}"};
    const size_t ascending[] = {4,5,0,1,2,3,6}, descending[] = {0,5,4,1,2,3,6};
    for (unsigned macro = 0; macro < 2; ++macro) {
        Fixture f; fixture(&f,macro ? "<macro name='d'><date variable='issued'><date-part name='year'/></date></macro>" : "",
                          macro ? "<key macro='d'/><key variable='title'/>" : "<key variable='issued'/><key variable='title'/>","{}");
        CeCslSortEvaluated keys[7] = {0};
        const CeCslXmlNode *date_key = f.key;
        for (size_t i = 0; i < 7; ++i) {
            item(&f,values[i]); evaluate(&f,&keys[i]);
            if (i==1 || i==2 || i==3 || i==6) assert(keys[i].missing && !keys[i].key.data && !keys[i].key.length);
        }
        expect_order(keys,7,1,ascending);
        for (size_t i = 0; i < 7; ++i) keys[i].descending=true;
        expect_order(keys,7,1,descending);
        for (size_t i = 0; i < 7; ++i) ce_csl_sort_evaluated_free(&keys[i]);
        /* Literal and absent dates tie on chronology, but later style keys
         * still order them. Source literals, including digit-looking literals,
         * remain untouched and are never inferred to be calendar years. */
        CeCslSortEvaluated matrix[6] = {0};
        const char *titled[] = {"{\"title\":\"Zulu\",\"issued\":{\"literal\":\"1900\"}}",
            "{\"title\":\"Alpha\"}","{\"title\":\"Middle\",\"issued\":{\"literal\":\"in press\"}}"};
        for (size_t i = 0; i < 3; ++i) {
            item(&f,titled[i]); char *before = ce_csl_json_print_exact(f.item.data); assert(before);
            f.key=date_key; evaluate(&f,&matrix[i*2]); f.key=date_key->next; evaluate(&f,&matrix[i*2+1]);
            char *after = ce_csl_json_print_exact(f.item.data); assert(after && !strcmp(before,after));
            cJSON_free(before); cJSON_free(after);
        }
        const size_t by_title[] = {1,2,0}; expect_order(matrix,3,2,by_title);
        for (size_t i = 0; i < 6; ++i) ce_csl_sort_evaluated_free(&matrix[i]);
        fixture_free(&f);
    }
}
static void test_dates_and_macros(void) {
    Fixture f; CeCslSortEvaluated keys[6] = {0};
    fixture(&f, "", "<key variable='issued'/>", "{\"issued\":{\"date-parts\":[[-100]]}}");
    evaluate(&f, &keys[0]); item(&f, "{\"issued\":{\"date-parts\":[[-50]]}}"); evaluate(&f, &keys[1]);
    item(&f, "{\"issued\":{\"date-parts\":[[2000]]}}"); evaluate(&f, &keys[2]);
    item(&f, "{\"issued\":{\"date-parts\":[[2000],[2001]]}}"); evaluate(&f, &keys[3]);
    item(&f, "{\"issued\":{\"date-parts\":[[2000,1,1]]}}"); evaluate(&f, &keys[4]);
    item(&f, "{\"issued\":{\"date-parts\":[[2000,21]]}}"); evaluate(&f, &keys[5]); assert(!compare(&keys[2], &keys[5]));
    const size_t expected[] = {0, 1, 2, 5, 3, 4}; expect_order(keys, 6, 1, expected);
    for (size_t i = 0; i < 6; ++i) ce_csl_sort_evaluated_free(&keys[i]);
    fixture_free(&f);
    fixture(&f, "<macro name='date'><choose><if type='book'><date variable='issued' form='numeric' date-parts='year'/></if>"
        "<else><date variable='issued'><date-part name='year'/><date-part name='month' form='numeric'/><date-part name='day'/></date></else>"
        "</choose></macro>", "<key macro='date'/>", "{\"type\":\"book\",\"issued\":{\"date-parts\":[[2001,12,31]]}}");
    evaluate(&f, &keys[0]);
    item(&f, "{\"type\":\"article-journal\",\"issued\":{\"date-parts\":[[2001,1,1]]}}"); evaluate(&f, &keys[1]);
    assert(compare(&keys[0], &keys[1]) < 0);
    for (size_t i = 0; i < 6; ++i) ce_csl_sort_evaluated_free(&keys[i]);
    fixture_free(&f);
    fixture(&f, "<macro name='number'><number variable='volume' form='roman'/></macro>", "<key macro='number'/>", "{\"volume\":\"4000\"}");
    evaluate(&f, &keys[0]); item(&f, "{\"volume\":\"20\"}"); evaluate(&f, &keys[1]);
    assert(keys[0].domain == CE_CSL_SORT_NUMBER && compare(&keys[1], &keys[0]) < 0);
    for (size_t i = 0; i < 6; ++i) ce_csl_sort_evaluated_free(&keys[i]);
    fixture_free(&f);
    fixture(&f, "<macro name='mix'><names variable='author'><name/></names><text variable='title'/></macro>",
        "<key macro='mix'/>", "{\"author\":[{\"family\":\"Doe\"}],\"title\":\"Book\"}");
    assert(!ce_csl_sort_evaluate(&f.request, f.key, 0, NULL, &keys[0], error, sizeof(error)) && strstr(error, "Unsupported")); fixture_free(&f);
}
static void test_selected_parts_and_counts(void) {
    Fixture f; CeCslSortEvaluated keys[4] = {0};
    fixture(&f, "<macro name='date'><date variable='issued'><date-part name='month' form='numeric'/><date-part name='day'/></date></macro>",
        "<key macro='date'/>", "{\"issued\":{\"date-parts\":[[2024,1,2]]}}");
    evaluate(&f, &keys[0]); item(&f, "{\"issued\":{\"date-parts\":[[1900,2,1]]}}"); evaluate(&f, &keys[1]);
    assert(keys[0].domain == CE_CSL_SORT_MONTH_DAY && compare(&keys[0], &keys[1]) < 0);
    item(&f, "{\"issued\":{\"date-parts\":[[2000]]}}"); evaluate(&f, &keys[2]); assert(keys[2].missing);
    item(&f, "{\"issued\":{\"date-parts\":[[1900,1,2]]}}"); evaluate(&f, &keys[3]); assert(!compare(&keys[0], &keys[3]));
    for (size_t i = 0; i < 4; ++i) ce_csl_sort_evaluated_free(&keys[i]);
    fixture_free(&f);
    fixture(&f, "<macro name='count'><names variable='author'><name form='count'/></names></macro>",
        "<key macro='count' names-min='3' names-use-first='1' names-use-last='true'/>",
        "{\"author\":[{\"family\":\"A\"},{\"family\":\"B\"},{\"family\":\"C\"}]}");
    evaluate(&f, &keys[0]); assert(keys[0].domain == CE_CSL_SORT_NUMBER);
    item(&f, "{\"author\":[{\"family\":\"A\"},{\"family\":\"B\"}]}"); evaluate(&f, &keys[1]);
    assert(!compare(&keys[0], &keys[1])); /* Truncated prefix + visible last = two. */
    item(&f, "{\"author\":[{\"family\":\"A\"}]}"); evaluate(&f, &keys[2]); assert(compare(&keys[2], &keys[1]) < 0);
    for (size_t i = 0; i < 4; ++i) ce_csl_sort_evaluated_free(&keys[i]);
    fixture_free(&f);
    fixture(&f, "<macro name='text'><group delimiter=' / '><text variable='title' text-case='uppercase'/><text variable='publisher'/></group></macro>",
        "<key macro='text'/>", "{\"title\":\"<i>Alpha</i>\",\"publisher\":\"Beta\"}");
    evaluate(&f, &keys[0]);
    fixture_free(&f);
    fixture(&f, "", "<key variable='title'/>", "{\"title\":\"ALPHA / Beta\"}"); evaluate(&f, &keys[1]);
    assert(!compare(&keys[0], &keys[1]));
    for (size_t i = 0; i < 4; ++i) ce_csl_sort_evaluated_free(&keys[i]);
    fixture_free(&f);
    fixture(&f, "<macro name='number'><number variable='volume' prefix='A'/></macro>",
        "<key macro='number'/>", "{\"volume\":\"1\"}");
    assert(!ce_csl_sort_evaluate(&f.request, f.key, 0, NULL, &keys[0], error, sizeof(error)) && strstr(error, "decoration")); fixture_free(&f);
}
static void test_boundaries(void) {
    Fixture f; fixture(&f, "", "<key variable='volume'/>", "{}");
    CeCslSortEvaluated out = {0};
    char *digits = malloc(CE_CSL_SORT_MAX_INTEGER_DIGITS + 2); assert(digits);
    memset(digits, '9', CE_CSL_SORT_MAX_INTEGER_DIGITS + 1); digits[CE_CSL_SORT_MAX_INTEGER_DIGITS] = 0;
    cJSON *n = cJSON_CreateString(digits); assert(n && cJSON_AddItemToObject((cJSON *)f.item.data, "volume", n));
    evaluate(&f, &out); unsigned char *old = out.key.data;
    digits[CE_CSL_SORT_MAX_INTEGER_DIGITS] = '9'; digits[CE_CSL_SORT_MAX_INTEGER_DIGITS + 1] = 0;
    assert(cJSON_SetValuestring(n, digits));
    assert(!ce_csl_sort_evaluate(&f.request, f.key, 0, NULL, &out, error, sizeof(error)) && out.key.data == old);
    free(digits); ce_csl_sort_evaluated_free(&out); fixture_free(&f);
    /* The first complete component has the same exact-digit bound even when
     * its decimal is surrounded by Unicode letter affixes or a later range. */
    for (unsigned macro = 0; macro < 2; ++macro) {
        fixture(&f, macro ? "<macro name='n'><number variable='volume'/></macro>" : "",
                macro ? "<key macro='n'/>" : "<key variable='volume'/>", "{}");
        size_t limit = CE_CSL_SORT_MAX_INTEGER_DIGITS;
        char *affixed = malloc(limit + 32); assert(affixed);
        memcpy(affixed, "第", 3); memset(affixed + 3, '9', limit);
        memcpy(affixed + 3 + limit, "卷-2", sizeof "卷-2");
        volume_item(&f, affixed); evaluate(&f, &out); old = out.key.data;
        affixed[3 + limit] = '9';
        memcpy(affixed + 4 + limit, "卷-2", sizeof "卷-2");
        volume_item(&f, affixed);
        assert(!ce_csl_sort_evaluate(&f.request, f.key, 0, NULL, &out, error, sizeof error));
        assert(out.key.data == old && *error);
        free(affixed); ce_csl_sort_evaluated_free(&out); fixture_free(&f);
    }
    fixture(&f, "", "<key variable='title'/>", "{}");
    char *text = malloc(CE_CSL_COLLATION_MAX_TEXT_BYTES + 2); assert(text);
    memset(text, 'a', CE_CSL_COLLATION_MAX_TEXT_BYTES + 1); text[CE_CSL_COLLATION_MAX_TEXT_BYTES + 1] = 0;
    n = cJSON_CreateString(text); assert(n && cJSON_AddItemToObject((cJSON *)f.item.data, "title", n));
    assert(!ce_csl_sort_evaluate(&f.request, f.key, 0, NULL, &out, error, sizeof(error)) && strstr(error, "64 KiB"));
    assert(!ce_csl_sort_evaluate(NULL, f.key, 0, NULL, &out, error, sizeof(error)));
    assert(!ce_csl_sort_evaluate(&f.request, ce_csl_style_root(f.style), 0, NULL, &out, error, sizeof(error)));
    free(text); fixture_free(&f);
    fixture(&f, "", "<key variable='author'/>", "{\"author\":[{\"literal\":\"the \"}]}");
    evaluate(&f, &out); assert(out.missing); ce_csl_sort_evaluated_free(&out); fixture_free(&f);
}
typedef union Block { long double align; void *pointer; unsigned char bytes[32]; } Block;
typedef struct Memory { size_t calls, fail, live; bool sticky; } Memory;
static void *allocate(void *context, size_t size) {
    Memory *m = context; ++m->calls;
    if (m->fail && (m->sticky ? m->calls >= m->fail : m->calls == m->fail)) return NULL;
    Block *p = malloc(sizeof(*p) + size);
    if (!p) return NULL;
    ++m->live; return p + 1;
}
static void release(void *context, void *memory) {
    if (!memory) return;
    Memory *m = context; assert(m->live); --m->live; free((Block *)memory - 1);
}
static size_t test_oom(void) {
    Fixture f; fixture(&f, "<macro name='author'><names variable='author'><name initialize-with='. '/>"
        "<substitute><names variable='editor'/><text variable='title'/></substitute></names></macro>",
        "<key macro='author'/>", "{\"editor\":[{\"family\":\"École\",\"given\":\"Anna\"},{\"family\":\"Brown\",\"given\":\"Bob\"}]}");
    Memory m = {0}; CeCslOutputAllocator a = {&m, allocate, release}; CeCslSortEvaluated out = {0};
    assert(ce_csl_sort_evaluate(&f.request, f.key, 0, &a, &out, error, sizeof(error)));
    size_t total = m.calls; unsigned char *old = out.key.data;
    for (size_t i = 1; i <= total; ++i) {
        m.calls = 0; m.fail = i;
        assert(!ce_csl_sort_evaluate(&f.request, f.key, 0, &a, &out, error, sizeof(error)));
        assert(out.key.data == old && m.live == 1);
    }
    m.fail = 0; ce_csl_sort_evaluated_free(&out); assert(!m.live); fixture_free(&f);
    /* Exercise finalizer-owned objects as well as the typed names path. */
    fixture(&f, "<macro name='text'><group delimiter='; '><text variable='title' text-case='uppercase'/><text variable='publisher'/></group></macro>",
        "<key macro='text'/>", "{\"title\":\"Straße <i>École</i>\",\"publisher\":\"House\"}");
    m.calls = 0; assert(ce_csl_sort_evaluate(&f.request, f.key, 0, &a, &out, error, sizeof(error)));
    size_t text_total = m.calls; old = out.key.data;
    for (size_t i = 1; i <= text_total; ++i) {
        m.calls = 0; m.fail = i;
        assert(!ce_csl_sort_evaluate(&f.request, f.key, 0, &a, &out, error, sizeof(error)));
        assert(out.key.data == old && m.live == 1);
    }
    m.fail = 0; ce_csl_sort_evaluated_free(&out); assert(!m.live); fixture_free(&f);
    fixture(&f, "", "<key variable='volume'/>", "{\"volume\":\"2\"}");
    CeCslSortEvaluated keys[2] = {0}; evaluate(&f, &keys[0]);
    volume_item(&f,"Special edition"); evaluate(&f, &keys[1]);
    CeCslSortOrder order = {0}; m.calls = 0;
    assert(ce_csl_sort_evaluated_order(keys, 2, 1, &a, &order, error, sizeof(error)));
    size_t order_total = m.calls; size_t *old_order = order.indices;
    for (size_t i = 1; i <= order_total; ++i) {
        m.calls = 0; m.fail = i;
        assert(!ce_csl_sort_evaluated_order(keys, 2, 1, &a, &order, error, sizeof(error)));
        assert(order.indices == old_order && m.live == 1);
    }
    m.fail = 0; ce_csl_sort_order_free(&order); assert(!m.live);
    ce_csl_sort_evaluated_free(&keys[0]); ce_csl_sort_evaluated_free(&keys[1]); fixture_free(&f);
    return total + text_total + order_total;
}
static size_t test_composite_oom(void) {
    size_t total = 0;
    const char *values[] = {"S00012-13","12 special","<i>第0012卷</i> & 20"};
    for (size_t variant = 0; variant < 4; ++variant) {
        Fixture f;
        fixture(&f,variant==3 ? "<macro name='d'><date variable='issued'><date-part name='year'/></date></macro>" : "",
                variant==3 ? "<key macro='d'/>" : "<key variable='volume'/>",
                variant==3 ? "{\"issued\":{\"date-parts\":[[2020]]}}" : "{\"volume\":\"1\"}");
        Memory m = {.sticky=true}; CeCslOutputAllocator allocator = {&m,allocate,release};
        CeCslSortEvaluated out = {0}, candidate = {0};
        assert(ce_csl_sort_evaluate(&f.request,f.key,0,&allocator,&out,error,sizeof error));
        unsigned char *old = out.key.data; assert(old && m.live==1);
        if (variant==3) item(&f,"{\"issued\":{\"literal\":\"in press\"}}");
        else volume_item(&f,values[variant]);
        m.calls=0;
        assert(ce_csl_sort_evaluate(&f.request,f.key,0,&allocator,&candidate,error,sizeof error));
        size_t count=m.calls; ce_csl_sort_evaluated_free(&candidate); assert(m.live==1);
        char *before=ce_csl_json_print_exact(f.item.data); assert(before);
        for (size_t at=1; at<=count; ++at) {
            m.calls=0; m.fail=at;
            assert(!ce_csl_sort_evaluate(&f.request,f.key,0,&allocator,&out,error,sizeof error));
            assert(*error && out.key.data==old && m.live==1);
        }
        char *after=ce_csl_json_print_exact(f.item.data); assert(after && !strcmp(before,after));
        cJSON_free(before); cJSON_free(after);
        total+=count; m.fail=0; ce_csl_sort_evaluated_free(&out); assert(!m.live); fixture_free(&f);
    }
    return total;
}
static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb"); assert(file); assert(!fseek(file, 0, SEEK_END));
    long length = ftell(file); assert(length >= 0); rewind(file);
    char *data = malloc((size_t)length + 1); assert(data);
    assert(fread(data, 1, (size_t)length, file) == (size_t)length); fclose(file); data[length] = 0; return data;
}
static char *document_sort_input(bool dates) {
    const char *volumes[] = {"S12","2","10","12-13","12a","Special edition",NULL,"第12卷"};
    const char *date_values[] = {"{\"date-parts\":[[2020]]}","{\"literal\":\"in press\"}",
        "{\"literal\":\"forthcoming\"}",NULL,"{\"date-parts\":[[2019]]}"};
    const char *titles[] = {"Same study","Alpha study","Beta study","Delta study","Gamma study"};
    cJSON *root=cJSON_Parse("{\"items\":[],\"citations\":[]}"); assert(root);
    for (size_t i=0; i<(dates ? 5u : 8u); ++i) {
        cJSON *one=cJSON_Parse("{\"type\":\"article-journal\",\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],"
                              "\"container-title\":\"Journal of Research\"}"); assert(one);
        char id[32]; snprintf(id,sizeof id,"i%zu",i);
        assert(cJSON_AddStringToObject(one,"id",id));
        assert(cJSON_AddStringToObject(one,"title",dates ? titles[i] : "Same study"));
        if (dates) {
            if (date_values[i]) assert(cJSON_AddItemToObject(one,"issued",cJSON_Parse(date_values[i])));
        } else {
            assert(cJSON_AddItemToObject(one,"issued",cJSON_Parse("{\"date-parts\":[[2024]]}")));
            if (volumes[i]) assert(cJSON_AddStringToObject(one,"volume",volumes[i]));
        }
        assert(cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root,"items"),one));
        cJSON *cluster=cJSON_Parse("{\"citationItems\":[{}]}"); assert(cluster);
        assert(cJSON_AddStringToObject(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(cluster,"citationItems"),0),"id",id));
        snprintf(id,sizeof id,"c%zu",i); assert(cJSON_AddStringToObject(cluster,"citationID",id));
        assert(cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root,"citations"),cluster));
    }
    char *json=ce_csl_json_print_exact(root); assert(json); cJSON_Delete(root); return json;
}
typedef struct CancelProbe { size_t calls,at; } CancelProbe;
static bool cancel_probe(void *context) {
    CancelProbe *probe=context; ++probe->calls; return probe->at && probe->calls>=probe->at;
}
static void real_document_sorts(const CeCslStyle *style,const CeCslLocale *locale,size_t profile) {
    /* Original whole-document fixtures and order/citation strings independently
     * checked against citeproc@2.4.63 using the five pinned resource files.
     * These exercise actual APA sort + disambiguation + finalization; the
     * earlier 540 materializer cases do not exercise this document pipeline. */
    for (unsigned dates=0; dates<2; ++dates) {
        char *json=document_sort_input(dates!=0); CeCslInput *input=NULL;
        assert(ce_csl_input_load(&input,json,strlen(json),error,sizeof error));
        cJSON *source=cJSON_Parse(json); assert(source);
        cJSON_free(json);
        CeCslDocument *document=NULL;
        bool ok=ce_csl_document_build(style,locale,input,NULL,NULL,&document,error,sizeof error);
        if (!ok) fprintf(stderr,"Profile %zu %s document: %s\n",profile,dates ? "dates" : "volumes",error);
        size_t count=dates ? 5 : 8;
        assert(ok && ce_csl_document_citation_count(document)==count &&
               ce_csl_document_bibliography_count(document)==count);
        const size_t apa_volume[]={1,2,0,3,4,7,5,6}, apa_date[]={3,4,0,1,2};
        const char *apa_volume_cites[]={"(Doe, 2024c)","(Doe, 2024a)","(Doe, 2024b)","(Doe, 2024d)",
            "(Doe, 2024e)","(Doe, 2024g)","(Doe, 2024h)","(Doe, 2024f)"};
        const char *apa_date_cites[]={"(Doe, 2020)","(Doe, in press)","(Doe, forthcoming)","(Doe, n.d.)","(Doe, 2019)"};
        const char *first_bib_volume[]={"Doe, J. (2024a). Same study. Journal of Research, 2.",
            "[1] J. Doe, “Same study,” Journal of Research, vol. S12, 2024.",
            "[1] DOE J. Same study[J]. Journal of Research, 2024, S12."};
        const char *first_bib_date[]={"Doe, J. (n.d.). Delta study. Journal of Research.",
            "[1] J. Doe, “Same study,” Journal of Research, 2020.",
            "[1] DOE J. Same study[J]. Journal of Research, 2020."};
        for (size_t i=0; i<count; ++i) {
            const CeCslDocumentBibliography *row=ce_csl_document_bibliography(document,i); assert(row);
            assert(row->item_index==(profile ? i : dates ? apa_date[i] : apa_volume[i]));
            CeCslOutputString citation={0};
            assert(ce_csl_document_citation_plain(document,i,NULL,&citation,error,sizeof error));
            char numeric[32]; snprintf(numeric,sizeof numeric,"[%zu]",i+1);
            const char *expected=profile ? numeric : dates ? apa_date_cites[i] : apa_volume_cites[i];
            if (strcmp(citation.data,expected))
                fprintf(stderr,"Profile %zu %s cite %zu expected <%s> actual <%s>\n",profile,
                        dates ? "dates" : "volumes",i,expected,citation.data);
            assert(!strcmp(citation.data,expected));
            ce_csl_output_string_free(&citation);
            const CeCslInputItem *item=ce_csl_input_item(input,i); assert(item);
            char *before=ce_csl_json_print_exact(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(source,"items"),(int)i));
            char *after=ce_csl_json_print_exact(item->data); assert(before && after && !strcmp(before,after));
            cJSON_free(before); cJSON_free(after);
        }
        const CeCslBibliographyEntry *entries[]={ce_csl_document_bibliography(document,0)->entry};
        CeCslOutputString bibliography={0};
        assert(ce_csl_bibliography_plain(entries,1,CE_CSL_BIBLIOGRAPHY_PLAIN_COMPACT,NULL,&bibliography,error,sizeof error));
        assert(!strcmp(bibliography.data,dates ? first_bib_date[profile] : first_bib_volume[profile]));
        ce_csl_output_string_free(&bibliography);
        if (dates) {
            size_t index=profile ? 1 : 3;
            entries[0]=ce_csl_document_bibliography(document,index)->entry;
            assert(ce_csl_bibliography_plain(entries,1,CE_CSL_BIBLIOGRAPHY_PLAIN_COMPACT,NULL,&bibliography,error,sizeof error));
            assert(strstr(bibliography.data,"in press") && strstr(bibliography.data,"Alpha study"));
            ce_csl_output_string_free(&bibliography);
        }
        if (!profile) {
            CancelProbe probe={0}; CeCslDocumentOptions options={.cancel_context=&probe,.cancelled=cancel_probe};
            CeCslDocument *sample=NULL;
            assert(ce_csl_document_build(style,locale,input,&options,NULL,&sample,error,sizeof error));
            size_t calls=probe.calls; assert(calls); ce_csl_document_free(sample);
            /* Real APA probes also check cancellation inside dependency
             * allocation hooks (hundreds of thousands of calls). Sample
             * bounded checkpoints throughout this complete pipeline; the
             * module allocation-failure loops above remain exhaustive. */
            assert(calls>=8);
            const size_t checkpoints[]={1,calls/8,calls/4,calls/2,calls*3/4,calls-2,calls-1,calls};
            for (size_t c=0; c<sizeof checkpoints/sizeof *checkpoints; ++c) {
                size_t stop=checkpoints[c];
                probe=(CancelProbe){0,stop}; sample=document;
                assert(!ce_csl_document_build(style,locale,input,&options,NULL,&sample,error,sizeof error));
                assert(sample==document && *error && probe.calls>=stop);
            }
            printf("real APA %s document: 8 cancellation checkpoints across %zu calls retained old result\n",dates ? "dates" : "volumes",calls);
        }
        ce_csl_document_free(document); ce_csl_input_free(input); cJSON_Delete(source);
        printf("real profile %zu %s document: %zu complete citations and bibliography rows passed\n",profile,dates ? "dates" : "volumes",count);
    }
}
static void real_profiles(char **argv) {
    CeCslXml *locale_xml[2]; const CeCslXmlNode *roots[2];
    for (size_t i = 0; i < 2; ++i) {
        char *text = read_file(argv[3 + i]); locale_xml[i] = ce_csl_xml_parse(text, strlen(text), error, sizeof(error));
        free(text); assert(locale_xml[i]); roots[i] = locale_xml[i]->root;
    }
    CeCslLocaleCatalog catalog = {.roots = roots, .root_count = 2};
    const char *data[] = {
        "{\"type\":\"book\",\"title\":\"Beta study\",\"author\":[{\"family\":\"Smith\",\"given\":\"Ann\"}],\"issued\":{\"date-parts\":[[2020,12,31]]},\"volume\":\"10\",\"part-number\":\"2\",\"archive\":\"Collection\",\"archive-place\":\"Paris\"}",
        "{\"type\":\"book\",\"title\":\"Alpha handbook\",\"issued\":{\"date-parts\":[[2021]]},\"volume\":\"2\",\"part-number\":\"1\",\"original-date\":{\"date-parts\":[[1999]]}}",
        "{\"type\":\"book\",\"title\":\"Earlier study\",\"author\":[{\"family\":\"Smith\",\"given\":\"Ann\"}],\"issued\":{\"date-parts\":[[2019]]},\"event-date\":{\"date-parts\":[[2018,3,4]]}}",
        "{\"type\":\"webpage\",\"title\":\"Zulu report\",\"issued\":{\"date-parts\":[[2022,2,10]]},\"status\":\"forthcoming\"}",
        "{\"type\":\"book\",\"title\":\"Edited source\",\"editor\":[{\"family\":\"Moss\",\"given\":\"Jane\"}],\"issued\":{\"date-parts\":[[2000]]}}"
    };
    for (size_t s = 0; s < 3; ++s) {
        Fixture f = {0}; char *text = read_file(argv[s]);
        assert(ce_csl_style_load(&f.style, text, strlen(text), error, sizeof(error))); free(text);
        f.locale = ce_csl_locale_new(ce_csl_style_root(f.style), &catalog, NULL, NULL, error, sizeof(error)); assert(f.locale);
        f.item.id = "one";
        f.generated = (CeCslGeneratedVariables){.citation_number = {true, true, 1}, .first_reference_note_number = {true, false, 0},
            .year_suffix = {true, "", 0}, .citation_label = {true, "", 0}};
        f.request = (CeCslEvalRequest){.style = f.style, .locale = f.locale, .position = CE_CSL_POSITION_FIRST,
            .variables = {.item = &f.item, .generated = &f.generated}};
        for (size_t section = 0; section < 2; ++section) {
            f.request.section = section ? ce_csl_style_bibliography(f.style) : ce_csl_style_citation(f.style);
            const CeCslXmlNode *sort = ce_csl_xml_child(f.request.section, "sort");
            size_t columns = 0; for (const CeCslXmlNode *k = sort ? sort->children : NULL; k; k = k->next) ++columns;
            if (!columns) continue;
            CeCslSortEvaluated keys[5 * 64] = {0};
            for (size_t row = 0; row < 5; ++row) {
                item(&f, data[row]); f.generated.citation_number.value = 5 - row;
                size_t col = 0;
                for (f.key = sort->children; f.key; f.key = f.key->next, ++col) {
                    bool ok = ce_csl_sort_evaluate(&f.request, f.key, 0, NULL, &keys[row * columns + col], error, sizeof(error));
                    if (!ok) fprintf(stderr, "profile %zu section %zu row %zu key %zu: %s\n", s, section, row, col, error);
                    assert(ok);
                }
            }
            const size_t apa[] = {1, 4, 2, 0, 3}, numeric[] = {4, 3, 2, 1, 0};
            expect_order(keys, 5, columns, s == 0 ? apa : numeric);
            for (size_t i = 0; i < 5 * columns; ++i) ce_csl_sort_evaluated_free(&keys[i]);
            printf("real profile %zu %s: %zu keys x 5 rows passed\n", s, section ? "bibliography" : "citation", columns);
        }
        real_document_sorts(f.style,f.locale,s);
        fixture_free(&f);
    }
    for (size_t i = 0; i < 2; ++i) ce_csl_xml_free(locale_xml[i]);
}
int main(int argc, char **argv) {
    test_scalars(); test_names(); test_composite_numbers(); test_literal_dates();
    test_dates_and_macros(); test_selected_parts_and_counts(); test_boundaries();
    size_t allocations = test_oom() + test_composite_oom();
    if (argc == 6) real_profiles(argv + 1); else assert(argc == 1);
    printf("CSL sort evaluation passed (%zu allocation failures)\n", allocations); return 0;
}
