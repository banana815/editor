#include "csl_sort_evaluate.h"
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
    unsigned char *old = keys[0].key.data;
    item(&f, "{\"volume\":\"S20\"}");
    assert(!ce_csl_sort_evaluate(&f.request, f.key, 0, NULL, &keys[0], error, sizeof(error)) && keys[0].key.data == old && strstr(error, "Unsupported"));
    item(&f, "{\"volume\":\"forthcoming\"}"); evaluate(&f, &keys[1]);
    CeCslSortOrder order = {0};
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
    assert(keys[0].domain == CE_CSL_SORT_INTEGER && compare(&keys[1], &keys[0]) < 0);
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
    evaluate(&f, &keys[0]); assert(keys[0].domain == CE_CSL_SORT_INTEGER);
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
typedef struct Memory { size_t calls, fail, live; } Memory;
static void *allocate(void *context, size_t size) {
    Memory *m = context; if (++m->calls == m->fail) return NULL;
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
    CeCslSortEvaluated keys[2] = {0}; evaluate(&f, &keys[0]); evaluate(&f, &keys[1]);
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
static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb"); assert(file); assert(!fseek(file, 0, SEEK_END));
    long length = ftell(file); assert(length >= 0); rewind(file);
    char *data = malloc((size_t)length + 1); assert(data);
    assert(fread(data, 1, (size_t)length, file) == (size_t)length); fclose(file); data[length] = 0; return data;
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
        fixture_free(&f);
    }
    for (size_t i = 0; i < 2; ++i) ce_csl_xml_free(locale_xml[i]);
}
int main(int argc, char **argv) {
    test_scalars(); test_names(); test_dates_and_macros(); test_selected_parts_and_counts(); test_boundaries();
    size_t allocations = test_oom();
    if (argc == 6) real_profiles(argv + 1); else assert(argc == 1);
    printf("CSL sort evaluation passed (%zu allocation failures)\n", allocations); return 0;
}
