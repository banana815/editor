#include "csl_bibliography.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char error[512];
#define OK(call)                                                                                             \
    do {                                                                                                     \
        bool ok_ = (call);                                                                                   \
        if (!ok_)                                                                                            \
            fprintf(stderr, "%s: %s\n", #call, error);                                                       \
        assert(ok_);                                                                                         \
    } while (0)
typedef struct Fixture {
    CeCslStyle *style;
    CeCslLocale *locale;
    CeCslXml *locale_xml;
    cJSON *json;
    CeCslInputItem item;
    CeCslGeneratedVariables generated;
    CeCslEvaluation *evaluation;
    CeCslFinalized *finalized;
} Fixture;
static void evaluate(Fixture *f) {
    CeCslEvalRequest r = {.style = f->style,
                          .locale = f->locale,
                          .section = ce_csl_style_bibliography(f->style),
                          .variables = {.item = &f->item, .generated = &f->generated}};
    OK(ce_csl_evaluate(&r, NULL, &f->evaluation, error, sizeof(error)));
    OK(ce_csl_finalize(f->evaluation, NULL, &f->finalized, error, sizeof(error)));
}
static void fixture(Fixture *f, const char *options, const char *macros, const char *layout,
                    const char *item) {
    memset(f, 0, sizeof(*f));
    const char *head =
        "<style xmlns='http://purl.org/net/xbiblio/csl' version='1.0' class='in-text'><info><title>Owned "
        "bibliography "
        "adapter</title><id>https://example.invalid/bibliography</id><updated>2026-01-01T00:00:00+00:00</"
        "updated></info><locale><style-options punctuation-in-quote='true'/><terms><term "
        "name='open-quote'>“</term><term name='close-quote'>”</term><term name='and'>and</term><term "
        "name='et-al'>et al.</term></terms></locale><citation><layout><text "
        "value='unused'/></layout></citation>";
    size_t size = strlen(head) + strlen(options) + strlen(macros) + strlen(layout) + 100;
    char *xml = malloc(size);
    assert(xml);
    snprintf(xml, size, "%s%s<bibliography %s>%s</bibliography></style>", head, macros, options, layout);
    OK(ce_csl_style_load(&f->style, xml, strlen(xml), error, sizeof(error)));
    free(xml);
    f->locale = ce_csl_locale_new(ce_csl_style_root(f->style), NULL, "en-US", NULL, error, sizeof(error));
    assert(f->locale);
    f->json = cJSON_Parse(item);
    assert(f->json);
    f->item = (CeCslInputItem){.id = "a", .data = f->json};
    f->generated.citation_number = (CeCslGeneratedNumber){true, true, 1};
    f->generated.first_reference_note_number = (CeCslGeneratedNumber){true, false, 0};
    f->generated.year_suffix = (CeCslGeneratedText){true, "", 0};
    f->generated.citation_label = (CeCslGeneratedText){true, "", 0};
    evaluate(f);
}
static void fixture_free(Fixture *f) {
    ce_csl_finalized_free(f->finalized);
    ce_csl_evaluation_free(f->evaluation);
    ce_csl_locale_free(f->locale);
    ce_csl_style_free(f->style);
    ce_csl_xml_free(f->locale_xml);
    cJSON_Delete(f->json);
}
static CeCslBibliographyEntry *entry(Fixture *f, size_t index) {
    CeCslBibliographyEntry *e = NULL;
    OK(ce_csl_bibliography_entry_build(f->style, f->finalized, index, NULL, &e, error, sizeof(error)));
    return e;
}
static void cell_is(const CeCslBibliographyCell *cell, const char *expected) {
    assert(cell && cell->output);
    CeCslOutputString text = {0};
    OK(ce_csl_output_plain(cell->output, &text, error, sizeof(error)));
    if (strcmp(text.data, expected))
        fprintf(stderr, "cell <%s>, expected <%s>\n", text.data, expected);
    assert(!strcmp(text.data, expected));
    ce_csl_output_string_free(&text);
}
static void plain_is(const CeCslBibliographyEntry *const *entries, size_t count,
                     CeCslBibliographyPlainMode mode, const char *expected) {
    CeCslOutputString out = {0};
    OK(ce_csl_bibliography_plain(entries, count, mode, NULL, &out, error, sizeof(error)));
    if (strcmp(out.data, expected))
        fprintf(stderr, "plain <%s>, expected <%s>\n", out.data, expected);
    assert(!strcmp(out.data, expected));
    ce_csl_output_string_free(&out);
}
static void test_flow_lifetime(void) {
    Fixture f;
    fixture(&f, "hanging-indent='true' line-spacing='+2' entry-spacing='+0'", "",
            "<layout><text variable='title' font-style='italic'/></layout>",
            "{\"title\":\"中文🙂 <literal>&\\\"\"}");
    CeCslBibliographyEntry *e = entry(&f, 17);
    size_t bytes = ce_csl_bibliography_entry_text_bytes(e);
    assert(bytes == strlen("中文🙂 <literal>&\""));
    assert(ce_csl_bibliography_entry_source_item_index(e) == 17);
    const CeCslBibliographyOptions *o = ce_csl_bibliography_entry_options(e);
    assert(o->hanging_indent && o->line_spacing == 2 && !o->entry_spacing);
    assert(ce_csl_bibliography_entry_row_count(e) == 1);
    const CeCslBibliographyRow *r = ce_csl_bibliography_entry_row(e, 0);
    assert(r->kind == CE_CSL_BIBLIOGRAPHY_FLOW && !r->left.output);
    cell_is(&r->body, "中文🙂 <literal>&\"");
    assert(ce_csl_output_run(r->body.output, 0)->format == CE_CSL_ITALIC);
    assert(ce_csl_bibliography_entry_source_count(e) > 0);
    size_t node = ce_csl_bibliography_entry_source(e, 0)->source_node_index;
    fixture_free(&f);
    assert(ce_csl_bibliography_entry_source(e, 0)->source_node_index == node);
    const CeCslBibliographyEntry *array[] = {e, e};
    plain_is(array, 2, CE_CSL_BIBLIOGRAPHY_PLAIN_TABS, "中文🙂 <literal>&\"\n中文🙂 <literal>&\"");
    CeCslOutputString html = {0};
    OK(ce_csl_bibliography_html(array, 2, NULL, NULL, &html, error, sizeof(error)));
    assert(strstr(html.data, "line-height:2.400em") && strstr(html.data, "text-indent:-2.000em") &&
           strstr(html.data, "font-style:italic"));
    assert(strstr(html.data, "&lt;literal&gt;&amp;&quot;") && !strstr(html.data, "<literal>"));
    assert(strstr(html.data, "data-source-item-index=\"17\""));
    ce_csl_output_string_free(&html);
    ce_csl_bibliography_entry_free(e);
    fixture(&f, "hanging-indent='true' line-spacing='+2' entry-spacing='+0'", "",
            "<layout><text variable='title'/></layout>", "{}");
    e = entry(&f, 9);
    fixture_free(&f);
    assert(!ce_csl_bibliography_entry_row_count(e) && !ce_csl_bibliography_entry_text_bytes(e));
    assert(ce_csl_bibliography_entry_source_item_index(e) == 9);
    array[0] = e;
    plain_is(array, 1, CE_CSL_BIBLIOGRAPHY_PLAIN_TABS, "");
    OK(ce_csl_bibliography_html(array, 1, NULL, NULL, &html, error, sizeof(error)));
    assert(!html.length);
    ce_csl_output_string_free(&html);
    ce_csl_bibliography_entry_free(e);
    plain_is(NULL, 0, CE_CSL_BIBLIOGRAPHY_PLAIN_TABS, "");
}
static void fields(const char *body, const char *macros, const char *expected_left,
                   const char *expected_right) {
    Fixture f;
    char layout[4096];
    snprintf(layout, sizeof(layout), "<layout>%s</layout>", body);
    fixture(&f, "second-field-align='flush' entry-spacing='0'", macros, layout,
            "{\"title\":\"Title [1]🙂\"}");
    CeCslBibliographyEntry *e = entry(&f, 3);
    fixture_free(&f);
    const CeCslBibliographyRow *r = ce_csl_bibliography_entry_row(e, 0);
    assert(r && r->kind == CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS);
    cell_is(&r->left, expected_left);
    cell_is(&r->body, expected_right);
    ce_csl_bibliography_entry_free(e);
}
static void test_fields(void) {
    fields("<text variable='citation-number' prefix='[' suffix=']'/><text variable='title'/>", "", "[1]",
           "Title [1]🙂");
    fields("<group delimiter=' '><text variable='citation-number' prefix='[' suffix=']'/><text "
           "variable='title'/></group><text value='tail'/>",
           "", "[1] Title [1]🙂", "tail");
    fields("<choose><if variable='title'><text variable='citation-number' prefix='[' suffix=']'/><text "
           "variable='title'/></if></choose><text value='tail'/>",
           "", "[1]", "Title [1]🙂tail");
    fields("<text macro='n'/><text value='tail'/>",
           "<macro name='n'><text variable='citation-number' prefix='[' suffix=']'/><text "
           "variable='title'/></macro>",
           "[1]Title [1]🙂", "tail");
    fields("<text variable='publisher'/><text variable='citation-number' prefix='[' suffix=']'/><text "
           "variable='title'/>",
           "", "", "[1]Title [1]🙂");
    fields("<choose><if variable='publisher'><text value='absent'/></if></choose><text "
           "variable='citation-number'/><text variable='title'/>",
           "", "1", "Title [1]🙂");
    Fixture f;
    fixture(&f, "second-field-align='margin' entry-spacing='1'", "",
            "<layout suffix='.'><text variable='citation-number' prefix='[' suffix=']'/><text "
            "variable='title'/></layout>",
            "{\"title\":\"Body\"}");
    f.generated.citation_number.value = 9;
    evaluate(&f);
    CeCslBibliographyEntry *a = entry(&f, 0);
    f.generated.citation_number.value = 10;
    evaluate(&f);
    CeCslBibliographyEntry *b = entry(&f, 1);
    fixture_free(&f);
    const CeCslBibliographyEntry *array[] = {a, b};
    plain_is(array, 2, CE_CSL_BIBLIOGRAPHY_PLAIN_TABS, "[9]\tBody.\n\n[10]\tBody.");
    plain_is(array, 2, CE_CSL_BIBLIOGRAPHY_PLAIN_COMPACT, "[9] Body.\n\n[10] Body.");
    CeCslOutputString html = {0};
    assert(!ce_csl_bibliography_html(array, 2, NULL, NULL, &html, error, sizeof(error)) && !html.data &&
           strstr(error, "measured"));
    CeCslBibliographyHtmlOptions metrics = {.label_width_milli_em = 3000};
    OK(ce_csl_bibliography_html(array, 2, &metrics, NULL, &html, error, sizeof(error)));
    assert(strstr(html.data, "margin-inline-start:-3.500em") &&
           strstr(html.data, "width:calc(100% + 3.500em)"));
    assert(strstr(html.data, "padding-bottom:1.200em"));
    ce_csl_output_string_free(&html);
    ce_csl_bibliography_entry_free(a);
    ce_csl_bibliography_entry_free(b);
}
static void test_display(void) {
    Fixture f;
    fixture(&f, "entry-spacing='0'", "",
            "<layout><text value='Heading' display='block'/><text variable='citation-number' "
            "display='left-margin'/><text variable='title' display='right-inline'/><text variable='abstract' "
            "display='indent'/><text value='solo' display='right-inline'/></layout>",
            "{\"title\":\"A title\",\"abstract\":\"annotated🙂\"}");
    CeCslBibliographyEntry *e = entry(&f, 0);
    fixture_free(&f);
    assert(ce_csl_bibliography_entry_row_count(e) == 4);
    assert(ce_csl_bibliography_entry_row(e, 1)->kind == CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS);
    assert(ce_csl_bibliography_entry_row(e, 2)->kind == CE_CSL_BIBLIOGRAPHY_INDENT);
    assert(ce_csl_bibliography_entry_row(e, 3)->kind == CE_CSL_BIBLIOGRAPHY_BLOCK);
    const CeCslBibliographyEntry *array[] = {e};
    plain_is(array, 1, CE_CSL_BIBLIOGRAPHY_PLAIN_COMPACT, "Heading\n1 A title\nannotated🙂\nsolo");
    CeCslOutputString html = {0};
    OK(ce_csl_bibliography_html(array, 1, NULL, NULL, &html, error, sizeof(error)));
    assert(strstr(html.data, "padding-inline-start:2.000em"));
    ce_csl_output_string_free(&html);
    ce_csl_bibliography_entry_free(e);
}
static void rejects(const char *options, const char *layout, const char *expected) {
    Fixture f;
    fixture(&f, options, "", layout, "{\"title\":\"A\",\"volume\":\"B\"}");
    CeCslBibliographyEntry *e = NULL;
    assert(!ce_csl_bibliography_entry_build(f.style, f.finalized, 0, NULL, &e, error, sizeof(error)) && !e);
    if (!strstr(error, expected))
        fprintf(stderr, "error %s, expected %s\n", error, expected);
    assert(strstr(error, expected));
    fixture_free(&f);
}
static void test_unsupported(void) {
    rejects("", "<layout><group display='block'><text variable='title' display='indent'/></group></layout>",
            "nested");
    rejects("second-field-align='flush'", "<layout><text variable='title' display='block'/></layout>",
            "alignment");
    rejects("hanging-indent='true' second-field-align='flush'", "<layout><text variable='title'/></layout>",
            "simultaneous");
    rejects("", "<layout><text value='uncontrolled'/><text variable='title' display='block'/></layout>",
            "uncontrolled");
    rejects("",
            "<layout><text variable='title' display='left-margin' quotes='true'/><text variable='volume' "
            "display='right-inline' prefix=','/></layout>",
            "overlapping");
    rejects("second-field-align='flush'",
            "<layout><text variable='title' quotes='true'/><text variable='volume' prefix=','/></layout>",
            "overlapping");
    rejects("",
            "<layout><group delimiter='; '><text variable='title' display='block'/><text variable='volume' "
            "display='block'/></group></layout>",
            "decorated");
}
typedef union Header {
    unsigned char padding[32];
    size_t size;
    long double alignment;
    void *pointer;
} Header;
typedef struct Memory {
    size_t calls, fail_at, live, bytes;
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
static void test_oom_limits(void) {
    Fixture f;
    fixture(&f, "second-field-align='flush' entry-spacing='0'", "",
            "<layout suffix='.'><text variable='citation-number' prefix='[' suffix=']'/><text "
            "variable='title' font-style='italic'/></layout>",
            "{\"title\":\"中文 &amp; <literal>🙂\"}");
    CeCslBibliographyEntry *old = entry(&f, 1);
    size_t success = 0;
    for (size_t at = 0; at < 1000; ++at) {
        Memory memory = {.fail_at = at};
        CeCslOutputAllocator a = {&memory, allocate, release};
        CeCslBibliographyEntry *out = old;
        bool ok = ce_csl_bibliography_entry_build(f.style, f.finalized, 2, &a, &out, error, sizeof(error));
        if (ok) {
            old = NULL;
            assert(ce_csl_bibliography_entry_source_item_index(out) == 2);
            ce_csl_bibliography_entry_free(out);
            assert(!memory.live && !memory.bytes);
            success = at;
            break;
        }
        assert(out == old && !memory.live && !memory.bytes);
        assert(ce_csl_bibliography_entry_source_item_index(old) == 1);
    }
    assert(success > 10 && success < 1000);
    printf("Bibliography entry allocation failpoints: %zu\n", success);
    old = entry(&f, 1);
    fixture_free(&f);
    const CeCslBibliographyEntry *array[] = {old, old};
    for (unsigned html = 0; html < 2; ++html) {
        CeCslOutputString saved = {0};
        OK(ce_csl_bibliography_plain(array, 1, CE_CSL_BIBLIOGRAPHY_PLAIN_TABS, NULL, &saved, error,
                                     sizeof(error)));
        for (size_t at = 0; at < 1000; ++at) {
            Memory memory = {.fail_at = at};
            CeCslOutputAllocator a = {&memory, allocate, release};
            char *before = saved.data;
            bool ok = html ? ce_csl_bibliography_html(array, 2, NULL, &a, &saved, error, sizeof(error))
                           : ce_csl_bibliography_plain(array, 2, CE_CSL_BIBLIOGRAPHY_PLAIN_TABS, &a, &saved,
                                                       error, sizeof(error));
            if (ok) {
                ce_csl_output_string_free(&saved);
                assert(!memory.live && !memory.bytes);
                success = at;
                break;
            }
            assert(saved.data == before && !memory.live && !memory.bytes);
            assert(at < 999);
        }
        assert(success > 0);
        printf("Bibliography %s export allocation failpoints: %zu\n", html ? "HTML" : "plain", success);
    }
    CeCslOutputString saved = {0};
    OK(ce_csl_bibliography_plain(array, 1, CE_CSL_BIBLIOGRAPHY_PLAIN_TABS, NULL, &saved, error,
                                 sizeof(error)));
    char *before = saved.data;
    assert(!ce_csl_bibliography_plain(array, 2, (CeCslBibliographyPlainMode)99, NULL, &saved, error,
                                      sizeof(error)) &&
           saved.data == before);
    CeCslBibliographyHtmlOptions metrics = {.indent_milli_em = UINT32_MAX};
    assert(!ce_csl_bibliography_html(array, 2, &metrics, NULL, &saved, error, sizeof(error)) &&
           saved.data == before);
    ce_csl_output_string_free(&saved);
    ce_csl_bibliography_entry_free(old);
    /* Repeated entries charge document payload again, without copying all
     * evaluations or requiring any live source objects. */
    fixture(&f, "entry-spacing='0'", "", "<layout><text variable='title'/></layout>", "{\"title\":\"a\"}");
    size_t length = 400000;
    char *title = malloc(length + 1);
    assert(title);
    memset(title, 'x', length);
    title[length] = 0;
    assert(cJSON_ReplaceItemInObject(f.json, "title", cJSON_CreateString(title)));
    free(title);
    evaluate(&f);
    old = entry(&f, 0);
    fixture_free(&f);
    const CeCslBibliographyEntry *many[43];
    for (size_t i = 0; i < 43; ++i)
        many[i] = old;
    assert(!ce_csl_bibliography_plain(many, 43, CE_CSL_BIBLIOGRAPHY_PLAIN_TABS, NULL, &saved, error,
                                      sizeof(error)) &&
           !saved.data && strstr(error, "aggregate"));
    assert(!ce_csl_bibliography_html(many, 43, NULL, NULL, &saved, error, sizeof(error)) && !saved.data &&
           strstr(error, "aggregate"));
    ce_csl_bibliography_entry_free(old);
    fixture(&f, "entry-spacing='2147483647'", "", "<layout><text value='x'/></layout>", "{}");
    old = entry(&f, 0);
    fixture_free(&f);
    array[0] = array[1] = old;
    assert(!ce_csl_bibliography_plain(array, 2, CE_CSL_BIBLIOGRAPHY_PLAIN_TABS, NULL, &saved, error,
                                      sizeof(error)) &&
           !saved.data && strstr(error, "spacing"));
    ce_csl_bibliography_entry_free(old);
}
static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size >= 0);
    rewind(file);
    char *text = malloc((size_t)size + 1);
    assert(text);
    assert(fread(text, 1, (size_t)size, file) == (size_t)size);
    assert(!fclose(file));
    text[size] = 0;
    return text;
}
static void real_styles(char **paths) {
    CeCslXml *locales[2];
    const CeCslXmlNode *roots[2];
    for (size_t i = 0; i < 2; ++i) {
        char *xml = read_file(paths[3 + i]);
        locales[i] = ce_csl_xml_parse(xml, strlen(xml), error, sizeof(error));
        free(xml);
        assert(locales[i]);
        roots[i] = locales[i]->root;
    }
    CeCslLocaleCatalog catalog = {.roots = roots, .root_count = 2};
    const char *golden[] = {
        "Doe, J., & Roe, J. (2024). an example of reproducible research (2nd ed.). Example Press.",
        "[1] J. Doe and J. Roe, an example of reproducible research, 2nd ed. Oxford: Example Press, 2024.",
        "[1] DOE J, ROE J. an example of reproducible research[M]. 2 版. Oxford: Example Press, 2024."};
    for (size_t i = 0; i < 3; ++i) {
        Fixture f = {0};
        char *xml = read_file(paths[i]);
        OK(ce_csl_style_load(&f.style, xml, strlen(xml), error, sizeof(error)));
        free(xml);
        f.locale = ce_csl_locale_new(ce_csl_style_root(f.style), &catalog, NULL, NULL, error, sizeof(error));
        assert(f.locale);
        f.json =
            cJSON_Parse("{\"id\":\"book\",\"type\":\"book\",\"title\":\"an example of reproducible "
                        "research\",\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"},{\"family\":\"Roe\","
                        "\"given\":\"John\"}],\"issued\":{\"date-parts\":[[2024]]},\"publisher\":\"Example "
                        "Press\",\"publisher-place\":\"Oxford\",\"edition\":\"2\"}");
        assert(f.json);
        f.item = (CeCslInputItem){.id = "book", .data = f.json};
        f.generated.citation_number = (CeCslGeneratedNumber){true, true, 1};
        f.generated.first_reference_note_number = (CeCslGeneratedNumber){true, false, 0};
        f.generated.year_suffix = (CeCslGeneratedText){true, "", 0};
        f.generated.citation_label = (CeCslGeneratedText){true, "", 0};
        evaluate(&f);
        CeCslBibliographyEntry *e = entry(&f, i);
        const CeCslBibliographyEntry *array[] = {e};
        const CeCslBibliographyOptions *o = ce_csl_bibliography_entry_options(e);
        assert(!o->entry_spacing);
        assert(o->hanging_indent == (i == 0));
        assert(o->line_spacing == (i == 0 ? 2u : 1u));
        assert(o->alignment == (i == 0 ? CE_CSL_BIBLIOGRAPHY_ALIGN_NONE : CE_CSL_BIBLIOGRAPHY_ALIGN_FLUSH));
        fixture_free(&f);
        plain_is(array, 1, CE_CSL_BIBLIOGRAPHY_PLAIN_COMPACT, golden[i]);
        if (i) {
            cell_is(&ce_csl_bibliography_entry_row(e, 0)->left, "[1]");
            assert(ce_csl_bibliography_entry_row(e, 0)->body.source_node_index == SIZE_MAX);
        }
        CeCslOutputString html = {0};
        OK(ce_csl_bibliography_html(array, 1, NULL, NULL, &html, error, sizeof(error)));
        assert(strstr(html.data, "csl-bibliography"));
        ce_csl_output_string_free(&html);
        ce_csl_bibliography_entry_free(e);
    }
    for (size_t i = 0; i < 2; ++i)
        ce_csl_xml_free(locales[i]);
    puts("Pinned APA/IEEE/GB bibliography entries passed");
}
int main(int argc, char **argv) {
    test_flow_lifetime();
    test_fields();
    test_display();
    test_unsupported();
    test_oom_limits();
    if (argc == 6)
        real_styles(argv + 1);
    puts("CSL bibliography: streaming ownership, typed columns/display, exports, limits and OOM passed");
    return 0;
}
