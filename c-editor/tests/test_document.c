#include "document.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CeDocument *load(const char *s, CeFormat f) {
    CeDocument *d = ce_document_new();
    char error[256];
    assert(d);
    if (!ce_document_import(d, s, f, error, sizeof error)) {
        fprintf(stderr, "Import failed: %s\n", error);
        abort();
    }
    return d;
}
static void exact_metadata_is(const CeDocument *d) {
    char *json=ce_document_export_json(d),*block=ce_document_block_json(d,0);
    assert(json && block);
    /* Match decimal spellings as well as parsed values: cJSON_Compare itself
     * treats some neighboring large integers as equal. */
    assert(strstr(json,"\"rootNumber\":4503599627370501"));
    assert(strstr(json,"\"nested\":[-4503599627370501,9007199254740991]"));
    assert(strstr(block,"\"nodeNumber\":4503599627370501"));
    cJSON *tree=cJSON_Parse(json); assert(tree);
    assert(cJSON_GetObjectItemCaseSensitive(tree,"rootNumber")->valuedouble==4503599627370501.0);
    cJSON *attrs=cJSON_GetObjectItemCaseSensitive(tree,"attrs");
    cJSON *nested=cJSON_GetObjectItemCaseSensitive(attrs,"nested");
    assert(cJSON_GetArrayItem(nested,0)->valuedouble==-4503599627370501.0);
    assert(cJSON_GetArrayItem(nested,1)->valuedouble==9007199254740991.0);
    cJSON_Delete(tree); free(json); free(block);
}
static void test_exact_unknown_integers(void) {
    /* This covers already representable safe integers. It does not promise
     * recovery of unsafe integers/decimal tokens rounded by initial import. */
    const char *source="{\"type\":\"doc\",\"rootNumber\":4503599627370501,"
        "\"attrs\":{\"nested\":[-4503599627370501,9007199254740991]},"
        "\"content\":[{\"type\":\"paragraph\",\"attrs\":{\"nodeNumber\":4503599627370501},"
        "\"content\":[{\"type\":\"text\",\"text\":\"Original\"}]}]}";
    CeDocument *d=load(source,CE_FORMAT_JSON); exact_metadata_is(d);
    char *before=ce_document_export_json(d); assert(before);
    CeDocument *round=load(before,CE_FORMAT_JSON); exact_metadata_is(round);
    char *again=ce_document_export_json(round); assert(again && !strcmp(before,again));
    free(again); ce_document_free(round);
    assert(ce_document_insert(d,ce_document_length(d)," edited")); exact_metadata_is(d);
    assert(ce_document_is_modified(d));
    assert(ce_document_undo(d)); exact_metadata_is(d);
    assert(!ce_document_is_modified(d));
    again=ce_document_export_json(d); assert(again && !strcmp(before,again)); free(again);
    assert(ce_document_redo(d)); exact_metadata_is(d);
    assert(!strcmp(ce_document_text(d),"Original edited"));
    assert(ce_document_apply_json(d,before)); exact_metadata_is(d);
    assert(!ce_document_is_modified(d));
    assert(ce_document_undo(d)); exact_metadata_is(d);
    assert(!strcmp(ce_document_text(d),"Original edited"));
    free(before); ce_document_free(d);
}
static void test_edit(void) {
    CeDocument *d = load("你好 world\nsecond", CE_FORMAT_TEXT);
    assert(ce_document_length(d) == 19);
    assert(!ce_document_is_modified(d));
    assert(!ce_document_insert(d, 1, "x"));
    assert(!ce_document_insert(d, 0, "\xc0\x80"));
    assert(ce_document_replace(d, 7, 12, "Earth"));
    assert(!strcmp(ce_document_text(d), "你好 Earth\nsecond"));
    assert(ce_document_is_modified(d));
    assert(ce_document_undo(d));
    assert(!strcmp(ce_document_text(d), "你好 world\nsecond"));
    assert(!ce_document_is_modified(d));
    assert(ce_document_redo(d));
    ce_document_mark_saved(d);
    assert(!ce_document_is_modified(d));
    assert(ce_document_delete(d, 6, 13));
    assert(!strcmp(ce_document_text(d), "你好second"));
    assert(ce_document_insert(d, 6, "\n中文\n"));
    assert(!strcmp(ce_document_text(d), "你好\n中文\nsecond"));
    assert(ce_document_replace(d, 0, ce_document_length(d), ""));
    assert(!strcmp(ce_document_text(d), ""));
    assert(ce_document_block_count(d) == 1);
    ce_document_free(d);
}
static void test_format(void) {
    CeDocument *d = load("alpha beta", CE_FORMAT_TEXT);
    assert(ce_document_style(d, 0, 5, CE_MARK_BOLD, NULL, true));
    assert(ce_document_style(d, 6, 10, CE_MARK_LINK, "https://example.org/?a=1&b=2", true));
    CeStyle st = ce_document_style_at(d, 2);
    assert(st.marks & CE_MARK_BOLD);
    st = ce_document_style_at(d, 7);
    assert(st.marks & CE_MARK_LINK);
    char *html = ce_document_export_html(d);
    assert(html);
    assert(strstr(html, "<strong>alpha</strong>"));
    assert(strstr(html, "&amp;b=2"));
    CeDocument *round = load(html, CE_FORMAT_HTML);
    assert(!strcmp(ce_document_text(d), ce_document_text(round)));
    st = ce_document_style_at(round, 7);
    assert(!strcmp(st.link, "https://example.org/?a=1&b=2"));
    free(html);
    assert(ce_document_style(d, 1, 4, CE_MARK_BOLD, NULL, false));
    st = ce_document_style_at(d, 2);
    assert(!(st.marks & CE_MARK_BOLD));
    st = ce_document_style_at(d, 0);
    assert(st.marks & CE_MARK_BOLD);
    assert(ce_document_set_block(d, 0, 0, CE_BLOCK_HEADING, 2));
    CeBlockInfo info;
    assert(ce_document_block_info(d, 0, &info));
    assert(info.kind == CE_BLOCK_HEADING && info.level == 2);
    ce_document_free(d);
    ce_document_free(round);
}
static void test_migration(void) {
    const char *s =
        "<h2 style=\"text-align: center\">标题</h2><ul><li><p><em>Nested</em></p><ol "
        "start=\"3\"><li><p><mark data-color=\"#f00\">text</mark></p></li></ol></li></ul><div "
        "data-type=\"zoteroCitationBlock\" citationid=\"cite-1\" endnotenumber=\"2\" "
        "itemkey=\"ABC\" title=\"A &amp; B\" note=\"摘录\" "
        "data-source-id=\"库/&quot;A&quot;&amp;条目\" data-library-id=\"9007199254740993\" "
        "data-csl-json=\"{&quot;title&quot;:&quot;文献 &amp; 资料&quot;,&quot;DOI&quot;:&quot;10.123/test&quot;}\"></div><div "
        "data-type=\"zoteroEndnoteSection\" style=\"apa\" "
        "entriesjson=\"[{&quot;number&quot;:1,&quot;entryHtml&quot;:&quot;&lt;em&gt;Title&lt;/"
        "em&gt;&quot;}]\"></div>";
    CeDocument *d = load(s, CE_FORMAT_HTML);
    assert(ce_document_block_count(d) == 5);
    char *j = ce_document_export_json(d);
    assert(strstr(j, "zoteroCitationBlock"));
    assert(strstr(j, "A & B"));
    assert(strstr(j, "entriesJson"));
    assert(strstr(j, "orderedList"));
    assert(strstr(j, "\"sourceId\":\"库/\\\"A\\\"&条目\""));
    assert(strstr(j, "\"libraryId\":\"9007199254740993\""));
    assert(strstr(j, "\"cslJson\":\"{\\\"title\\\":\\\"文献 & 资料\\\",\\\"DOI\\\":\\\"10.123/test\\\"}\""));
    char *h = ce_document_export_html(d);
    assert(h);
    assert(strstr(h, "data-source-id=\"库/&quot;A&quot;&amp;条目\""));
    assert(strstr(h, "data-library-id=\"9007199254740993\""));
    CeDocument *r = load(h, CE_FORMAT_HTML);
    char *j2 = ce_document_export_json(r);
    assert(!strcmp(j, j2));
    free(j);
    free(j2);
    free(h);
    char err[256];
    char *before = ce_document_export_json(d);
    assert(!ce_document_import(d, "<p><script>evil</script></p>", CE_FORMAT_HTML, err, sizeof err));
    j = ce_document_export_json(d);
    assert(!strcmp(before, j));
    free(before);
    free(j);
    assert(!ce_document_import(d, "<p style=\"font-size: 20px\">no loss</p>", CE_FORMAT_HTML, err,
                               sizeof err));
    assert(!ce_document_import(d, "<p>Unclosed", CE_FORMAT_HTML, err, sizeof err));
    assert(!ce_document_import(d, "{broken", CE_FORMAT_JSON, err, sizeof err));
    ce_document_free(d);
    ce_document_free(r);
}
static void test_unknown_json(void) {
    CeDocument *d = load("{\"doc\":{\"type\":\"doc\",\"content\":[{\"type\":\"alien\",\"attrs\":{"
                         "\"payload\":[1,2,3]}}]}}",
                         CE_FORMAT_JSON);
    char *j = ce_document_export_json(d);
    assert(strstr(j, "\"payload\":[1,2,3]"));
    free(j);
    assert(!ce_document_export_html(d));
    ce_document_free(d);
}
static void test_citation_identity(void) {
    CeDocument *d = load("<div data-type=\"zoteroCitationBlock\" itemkey=\"ABC\"></div>",
                         CE_FORMAT_HTML);
    char *json = ce_document_export_json(d);
    assert(json && !strstr(json, "sourceId") && !strstr(json, "libraryId"));
    free(json);
    ce_document_free(d);

    d = load("<div data-type=\"zoteroCitationBlock\" sourceid=\"legacy-id\" "
             "libraryid=\"42\"></div>", CE_FORMAT_HTML);
    json = ce_document_export_json(d);
    assert(json && strstr(json, "\"sourceId\":\"legacy-id\""));
    assert(strstr(json, "\"libraryId\":\"42\""));
    free(json);
    ce_document_free(d);

    /* Numeric legacy JSON remains available losslessly as JSON. HTML cannot
     * carry its original type, so it must fail without changing the document. */
    d = load("{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\","
             "\"attrs\":{\"libraryId\":42}}]}", CE_FORMAT_JSON);
    json = ce_document_export_json(d);
    assert(!ce_document_export_html(d));
    char *after = ce_document_export_json(d);
    assert(after && !strcmp(json, after));
    free(json); free(after);
    assert(!ce_document_is_modified(d));
    ce_document_free(d);
}
static void test_citation_html_metadata(void) {
    const char *html = "<html><body>\n<div data-type=\"notelabDocument\" data-citation-profile=\"apa&amp;&quot;&lt;&gt;&#39;\" "
        "data-citation-locale=\"zh-CN\"><p>Editable 中文</p><div data-type=\"zoteroCitationBlock\" citationid=\"c1\" "
        "data-library-id=\"9007199254740993\" data-source-id=\"来源/1\" data-csl-json=\"{&quot;title&quot;:&quot;原文&quot;}\" "
        "data-csl-render-json=\"{&quot;html&quot;:&quot;&lt;script&gt;bad&lt;/script&gt;&amp;&quot;,&quot;future&quot;:1}\"></div>"
        "<div data-type=\"zoteroEndnoteSection\" style=\"apa\" entriesjson=\"[]\" data-bibliography-mode=\"citations\" "
        "data-csl-render-json=\"opaque &lt;em&gt;文字&lt;/em&gt; &amp; &quot; &#39; {broken\"></div></div>\n</body></html>";
    CeDocument *d=load(html,CE_FORMAT_HTML); char *json=ce_document_export_json(d); assert(json);
    cJSON *tree=cJSON_Parse(json); assert(tree);
    cJSON *attrs=cJSON_GetObjectItemCaseSensitive(tree,"attrs");
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(attrs,"citationProfile")->valuestring,"apa&\"<>'"));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(attrs,"citationLocale")->valuestring,"zh-CN"));
    cJSON *content=cJSON_GetObjectItemCaseSensitive(tree,"content");
    attrs=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(content,1),"attrs");
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(attrs,"cslRenderJson")->valuestring,
        "{\"html\":\"<script>bad</script>&\",\"future\":1}"));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(attrs,"libraryId")->valuestring,"9007199254740993"));
    attrs=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(content,2),"attrs");
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(attrs,"bibliographyMode")->valuestring,"citations"));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(attrs,"cslRenderJson")->valuestring,"opaque <em>文字</em> & \" ' {broken"));
    cJSON_Delete(tree);
    char *exported=ce_document_export_html(d); assert(exported && !strncmp(exported,"<div data-type=\"notelabDocument\"",32));
    assert(strstr(exported,"data-csl-render-json=") && !strstr(exported,"<script>") && !strstr(exported,"<em>文字"));
    assert(strstr(exported,"data-bibliography-mode=\"citations\""));
    assert(strstr(exported,"data-citation-profile=\"apa&amp;&quot;&lt;&gt;&#39;\""));
    CeDocument *round=load(exported,CE_FORMAT_HTML); char *after=ce_document_export_json(round);
    assert(after && !strcmp(json,after)); free(after); ce_document_free(round);
    after=ce_document_export_json(d); assert(after && !strcmp(json,after)); free(after);
    free(json); free(exported); ce_document_free(d);
    d=load("<div data-type='notelabDocument' data-citation-locale=''><div data-type='zoteroEndnoteSection' "
        "data-csl-render-json=''></div></div>",CE_FORMAT_HTML);
    exported=ce_document_export_html(d); assert(exported && strstr(exported,"data-citation-locale=\"\"") &&
        strstr(exported,"data-csl-render-json=\"\"") && !strstr(exported,"data-citation-profile") &&
        !strstr(exported,"bibliography-mode"));
    json=ce_document_export_json(d); assert(json && !strstr(json,"bibliographyMode"));
    free(json); free(exported); ce_document_free(d);
    d=load("<p>legacy</p>",CE_FORMAT_HTML); exported=ce_document_export_html(d);
    assert(exported && !strcmp(exported,"<p>legacy</p>")); free(exported); ce_document_free(d);
    d=load("<div data-type='zoteroCitationBlock' citationId='legacy-html' data-citation-id='legacy-html' "
        "endnoteNumber='1' data-endnote-number='1' sourceid='source' data-source-id='source' "
        "libraryid='9007199254740993' data-library-id='9007199254740993' csljson='{}' data-csl-json='{}' "
        "cslrenderjson='{}' data-csl-render-json='{}'></div>",CE_FORMAT_HTML);
    json=ce_document_export_json(d); assert(json && strstr(json,"\"citationId\":\"legacy-html\"") &&
        strstr(json,"\"endnoteNumber\":1") && strstr(json,"\"libraryId\":\"9007199254740993\"") &&
        strstr(json,"\"cslJson\":\"{}\"") && strstr(json,"\"cslRenderJson\":\"{}\""));
    exported=ce_document_export_html(d); round=load(exported,CE_FORMAT_HTML); after=ce_document_export_json(round);
    assert(after && !strcmp(json,after)); free(after); free(json); free(exported); ce_document_free(round); ce_document_free(d);

    const char *bad_html[]={
        "<div data-type='notelabDocument'><div data-type='notelabDocument'></div></div>",
        "<div data-type='notelabDocument'></div><div data-type='notelabDocument'></div>",
        "<p>sibling</p><div data-type='notelabDocument'></div>",
        "<ul><li><div data-type='notelabDocument'></div></li></ul>",
        "<div data-type='zoteroCitationBlock'><div data-type='notelabDocument'></div></div>",
        "<div data-type='notelabDocument' data-citation-profile='apa' unknown='keep'></div>",
        "<div data-type='notelabDocument' data-citation-profile='apa' DATA-CITATION-PROFILE='ieee'></div>",
        "<p data-citation-profile='apa'>x</p>", "<p data-csl-render-json='{}'>x</p>",
        "<span data-type='notelabDocument' data-citation-profile='apa'></span>",
        "<div data-type='zoteroCitationBlock' data-csl-render-json='{}' data-csl-render-json='{}'></div>",
        "<div data-type='zoteroCitationBlock' data-csl-render-json='{}' cslrenderjson='[]'></div>",
        "<div data-type='zoteroCitationBlock' data-source-id='a' sourceid='b'></div>",
        "<div data-type='zoteroCitationBlock' citationid='a' data-citation-id='b'></div>",
        "<div data-type='zoteroEndnoteSection' data-csl-render-json='{}' unknown='keep'></div>",
        "<p data-bibliography-mode='citations'>x</p>",
        "<div data-type='zoteroCitationBlock' data-bibliography-mode='citations'></div>",
        "<div data-type='notelabDocument' data-bibliography-mode='citations'></div>",
        "<div data-type='zoteroEndnoteSection' data-bibliography-mode='future'></div>",
        "<div data-type='zoteroEndnoteSection' data-bibliography-mode=''></div>",
        "<div data-type='zoteroEndnoteSection' data-bibliography-mode='citations' DATA-BIBLIOGRAPHY-MODE='citations'></div>",
        "<div data-type='zoteroEndnoteSection' data-bibliography-mode='citations' unknown='keep'></div>"};
    d=load("Original",CE_FORMAT_TEXT); assert(ce_document_insert(d,8," saved")); ce_document_mark_saved(d);
    assert(ce_document_insert(d,ce_document_length(d)," redo") && ce_document_undo(d));
    json=ce_document_export_json(d); const char *borrowed=ce_document_text(d); char error[256];
    for(size_t i=0;i<sizeof(bad_html)/sizeof(*bad_html);++i) {
        assert(!ce_document_import(d,bad_html[i],CE_FORMAT_HTML,error,sizeof(error)) && error[0]);
        after=ce_document_export_json(d); assert(after && !strcmp(after,json)); free(after);
        assert(ce_document_text(d)==borrowed && !ce_document_is_modified(d) && ce_document_can_undo(d) && ce_document_can_redo(d));
    }
    free(json); ce_document_free(d);
    const char *bad_json[]={
        "{\"type\":\"doc\",\"attrs\":{\"citationProfile\":1}}",
        "{\"type\":\"doc\",\"attrs\":{\"citationLocale\":null}}",
        "{\"type\":\"doc\",\"attrs\":{\"citationProfile\":\"apa\",\"unknown\":{\"keep\":1}}}",
        "{\"type\":\"doc\",\"attrs\":{\"citationProfile\":\"apa\",\"citationProfile\":\"ieee\"}}",
        "{\"type\":\"doc\",\"content\":[{\"type\":\"doc\",\"attrs\":{\"citationProfile\":\"apa\"}}]}",
        "{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"cslRenderJson\":{}}}]}",
        "{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroEndnoteSection\",\"attrs\":{\"cslRenderJson\":1}}]}",
        "{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"cslRenderJson\":\"{}\",\"future\":[1,2]}}]}",
        "{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroEndnoteSection\",\"attrs\":{\"bibliographyMode\":1}}]}",
        "{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroEndnoteSection\",\"attrs\":{\"bibliographyMode\":null}}]}",
        "{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroEndnoteSection\",\"attrs\":{\"bibliographyMode\":\"future\"}}]}",
        "{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"bibliographyMode\":\"citations\"}}]}"};
    for(size_t i=0;i<sizeof(bad_json)/sizeof(*bad_json);++i) {
        d=load(bad_json[i],CE_FORMAT_JSON); json=ce_document_export_json(d);
        assert(!ce_document_export_html(d) && *ce_document_error(d));
        after=ce_document_export_json(d); assert(after && !strcmp(json,after) && !ce_document_is_modified(d));
        free(json); free(after); ce_document_free(d);
    }
}
static void test_search_history(void) {
    CeDocument *d = load("One one 中文 one", CE_FORMAT_TEXT);
    size_t a, b;
    assert(ce_document_find(d, "ONE", 1, false, &a, &b) && a == 4 && b == 7);
    assert(ce_document_find(d, "one", 0, true, &a, &b) && a == 15);
    assert(ce_document_find(d, "中文", 0, false, &a, &b) && a == 8);
    for (int i = 0; i < 150; i++)
        assert(ce_document_insert(d, 0, "x"));
    for (int i = 0; i < 128; i++)
        assert(ce_document_undo(d));
    assert(!ce_document_can_undo(d));
    assert(ce_document_can_redo(d));
    assert(ce_document_insert(d, 0, "y"));
    assert(!ce_document_can_redo(d));
    ce_document_free(d);
}
static void test_lists_and_random_edits(void) {
    CeDocument *d = load("first\nsecond", CE_FORMAT_TEXT);
    assert(ce_document_set_block(d, 0, ce_document_length(d), CE_BLOCK_ORDERED, 0));
    char *html = ce_document_export_html(d);
    assert(html);
    assert(!strcmp(html, "<ol><li><p>first</p></li><li><p>second</p></li></ol>"));
    free(html);
    assert(ce_document_delete(d, 3, 8));
    assert(!strcmp(ce_document_text(d), "fircond"));
    ce_document_free(d);
    d = load("", CE_FORMAT_TEXT);
    unsigned seed = 79;
    for (int step = 0; step < 500; step++) {
        const char *text = ce_document_text(d);
        size_t len = strlen(text);
        seed = seed * 1664525u + 1013904223u;
        size_t from = seed % (len + 1);
        seed = seed * 1664525u + 1013904223u;
        size_t to = from + seed % (len - from + 1);
        const char *ins = step % 3 == 0 ? "a\nb" : step % 3 == 1 ? "xy" : "";
        size_t n = strlen(ins);
        char *want = malloc(len - (to - from) + n + 1);
        assert(want);
        memcpy(want, text, from);
        memcpy(want + from, ins, n);
        strcpy(want + from + n, text + to);
        assert(ce_document_replace(d, from, to, ins));
        assert(!strcmp(want, ce_document_text(d)));
        free(want);
    }
    ce_document_free(d);
}
static void expect_html(CeDocument *d, const char *expected) {
    char *actual = ce_document_export_html(d);
    if (!actual || strcmp(actual, expected)) {
        fprintf(stderr, "Expected HTML: %s\nActual HTML: %s\nError: %s\n", expected,
                actual ? actual : "(null)", ce_document_error(d));
        abort();
    }
    free(actual);
}
static void expect_undo(CeDocument *d, const char *before) {
    char *after = ce_document_export_json(d);
    assert(ce_document_undo(d));
    char *restored = ce_document_export_json(d);
    assert(!strcmp(restored, before));
    free(restored);
    assert(ce_document_redo(d));
    restored = ce_document_export_json(d);
    assert(!strcmp(restored, after));
    free(restored);
    free(after);
}
static void test_clear_and_alignment(void) {
    CeDocument *d = load("<h2 style=\"text-align: right\"><strong>你好ab</strong></h2>"
                         "<p><em>tail</em></p>", CE_FORMAT_HTML);
    char *before = ce_document_export_json(d);
    assert(ce_document_clear_formatting(d, 3, 7));
    expect_html(d, "<h2 style=\"text-align: right\"><strong>你</strong>好a<strong>b</strong></h2>"
                   "<p><em>tail</em></p>");
    assert(!strcmp(ce_document_text(d), "你好ab\ntail"));
    expect_undo(d, before);
    free(before);
    before = ce_document_export_json(d);
    assert(ce_document_set_alignment(d, 0, 9, "center"));
    CeBlockInfo info;
    assert(ce_document_block_info(d, 0, &info));
    assert(info.kind == CE_BLOCK_HEADING && info.level == 2 && !strcmp(info.alignment, "center"));
    assert(ce_document_block_info(d, 1, &info) && !*info.alignment);
    expect_undo(d, before);
    free(before);
    before = ce_document_export_json(d);
    assert(!ce_document_set_alignment(d, 0, 2, "invalid"));
    char *after = ce_document_export_json(d);
    assert(!strcmp(before, after));
    free(before);
    free(after);
    ce_document_free(d);
    d = load("{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"attrs\":{\"custom\":7},"
             "\"content\":[{\"type\":\"text\",\"text\":\"abc\",\"marks\":[{\"type\":\"futureMark\"}]}]}]}",
             CE_FORMAT_JSON);
    assert(ce_document_set_alignment(d, 0, 0, "justify"));
    assert(ce_document_clear_formatting(d, 0, 3));
    after = ce_document_export_json(d);
    assert(strstr(after, "\"custom\":7") && strstr(after, "justify") && !strstr(after, "futureMark"));
    free(after);
    ce_document_free(d);
}
static const char *citation = "{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"c1\",\"note\":\"摘录\"}}";
static void test_atom_and_node_insertion(void) {
    CeDocument *d = load("<h2 style=\"text-align: center\"><strong>你好</strong></h2>", CE_FORMAT_HTML);
    char *before = ce_document_export_json(d);
    assert(ce_document_insert_node(d, 3, citation));
    assert(!strcmp(ce_document_text(d), "你\n\xef\xbf\xbc\n好"));
    expect_html(d, "<h2 style=\"text-align: center\"><strong>你</strong></h2>"
                   "<div data-type=\"zoteroCitationBlock\" citationId=\"c1\" note=\"摘录\"></div>"
                   "<h2 style=\"text-align: center\"><strong>好</strong></h2>");
    expect_undo(d, before);
    free(before);
    before = ce_document_export_json(d);
    assert(ce_document_insert(d, 4, "before"));
    assert(!strcmp(ce_document_text(d), "你\nbefore\n\xef\xbf\xbc\n好"));
    expect_undo(d, before);
    free(before);
    size_t atom = (size_t)(strstr(ce_document_text(d), "\xef\xbf\xbc") - ce_document_text(d));
    before = ce_document_export_json(d);
    assert(ce_document_insert(d, atom + 3, "after\nline"));
    assert(!strcmp(ce_document_text(d), "你\nbefore\n\xef\xbf\xbc\nafter\nline\n好"));
    expect_undo(d, before);
    free(before);
    assert(!ce_document_insert_node(d, atom + 1, citation));
    assert(!ce_document_insert_node(d, 0, "{\"type\":\"hardBreak\"}"));
    assert(!ce_document_insert_node(d, 0, "{\"type\":\"paragraph\"} trailing"));
    ce_document_free(d);
    d = load("<ul><li><p><em>ab</em></p></li></ul>", CE_FORMAT_HTML);
    assert(ce_document_insert_node(d, 0, citation));
    expect_html(d, "<ul><li><p></p><div data-type=\"zoteroCitationBlock\" citationId=\"c1\" "
                   "note=\"摘录\"></div><p><em>ab</em></p></li></ul>");
    ce_document_free(d);
}
static void test_edit_boundaries(void) {
    CeDocument *d = load("{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\","
                         "\"attrs\":{\"note\":\"keep me\"}}]}", CE_FORMAT_JSON);
    char *before = ce_document_export_json(d);
    assert(ce_document_insert(d, 0, "\n"));
    assert(!strcmp(ce_document_text(d), "\n\xef\xbf\xbc"));
    expect_undo(d, before);
    free(before);
    assert(ce_document_insert(d, ce_document_length(d), "\n"));
    assert(!strcmp(ce_document_text(d), "\n\xef\xbf\xbc\n"));
    before = ce_document_export_json(d);
    assert(!ce_document_insert_node(d, 0, "{\"type\":\"paragraph\",\"attrs\":{\"x\":\"\\u0000\"}}"));
    assert(!ce_document_set_block(d, 0, ce_document_length(d), CE_BLOCK_HEADING, 2));
    char *after = ce_document_export_json(d);
    assert(!strcmp(before, after));
    free(before);
    free(after);
    ce_document_free(d);
    d = load("<ul><li><p><em>ab</em></p><ul><li><p>child</p></li></ul></li></ul>", CE_FORMAT_HTML);
    before = ce_document_export_json(d);
    assert(ce_document_replace(d, 1, 2, "X\nY\nZ"));
    assert(!strcmp(ce_document_text(d), "aX\nY\nZ\nchild"));
    expect_html(d, "<ul><li><p><em>a</em><em>X</em></p></li><li><p><em>Y</em></p></li>"
                   "<li><p><em>Z</em></p><ul><li><p>child</p></li></ul></li></ul>");
    expect_undo(d, before);
    free(before);
    ce_document_free(d);
    d = load("<p>a</p><p>b</p>", CE_FORMAT_HTML);
    assert(ce_document_set_block(d, 0, 2, CE_BLOCK_HEADING, 2));
    expect_html(d, "<h2>a</h2><p>b</p>");
    ce_document_free(d);
    d = load("{\"type\":\"doc\",\"content\":[{\"type\":\"future\",\"content\":[]}]}", CE_FORMAT_JSON);
    assert(ce_document_set_block(d, 0, 0, CE_BLOCK_HEADING, 1));
    after = ce_document_export_json(d);
    assert(strstr(after, "future") && strstr(after, "heading"));
    free(after);
    ce_document_free(d);
}
static void test_list_structure_edits(void) {
    CeDocument *d = load("<ol start=\"3\"><li><p><strong>abcd</strong></p>"
                         "<ul><li><p>child</p></li></ul></li><li><p>next</p></li></ol>", CE_FORMAT_HTML);
    char *before = ce_document_export_json(d);
    assert(ce_document_insert(d, 2, "\n"));
    expect_html(d, "<ol start=\"3\"><li><p><strong>ab</strong></p></li>"
                   "<li><p><strong>cd</strong></p><ul><li><p>child</p></li></ul></li>"
                   "<li><p>next</p></li></ol>");
    expect_undo(d, before);
    free(before);
    before = ce_document_export_json(d);
    assert(ce_document_move_block(d, 1, 3));
    expect_html(d, "<ol start=\"3\"><li><p><strong>ab</strong></p></li>"
                   "<li><p>next</p></li><li><p><strong>cd</strong></p>"
                   "<ul><li><p>child</p></li></ul></li></ol>");
    expect_undo(d, before);
    free(before);
    ce_document_free(d);
    d = load("<ul><li><p>parent</p><ol start=\"4\"><li><p>inner</p></li>"
             "<li><p>later</p></li></ol></li><li><p>last</p></li></ul>", CE_FORMAT_HTML);
    before = ce_document_export_json(d);
    assert(ce_document_set_block(d, 7, 7, CE_BLOCK_BULLET, 0));
    expect_html(d, "<ul><li><p>parent</p><ul><li><p>inner</p></li>"
                   "<li><p>later</p></li></ul></li><li><p>last</p></li></ul>");
    expect_undo(d, before);
    free(before);
    before = ce_document_export_json(d);
    assert(ce_document_set_block(d, 7, 7, CE_BLOCK_HEADING, 3));
    expect_html(d, "<ul><li><p>parent</p></li></ul><h3>inner</h3>"
                   "<ul><li><p>later</p></li></ul><ul><li><p>last</p></li></ul>");
    assert(!strcmp(ce_document_text(d), "parent\ninner\nlater\nlast"));
    expect_undo(d, before);
    free(before);
    ce_document_free(d);
    d = load("<ol start=\"5\"><li><p>a</p></li><li><p>b</p></li><li><p>c</p></li></ol>", CE_FORMAT_HTML);
    assert(ce_document_set_block(d, 2, 2, CE_BLOCK_PARAGRAPH, 0));
    expect_html(d, "<ol start=\"5\"><li><p>a</p></li></ol><p>b</p>"
                   "<ol start=\"7\"><li><p>c</p></li></ol>");
    ce_document_free(d);
    d = load("<ul><li><p>parent</p><ul><li><p></p></li><li><p>child</p></li></ul></li></ul>", CE_FORMAT_HTML);
    before = ce_document_export_json(d);
    assert(ce_document_insert(d, 7, "\n"));
    expect_html(d, "<ul><li><p>parent</p></li><li><p></p><ul><li><p>child</p></li></ul></li></ul>");
    expect_undo(d, before);
    free(before);
    ce_document_free(d);
    d = load("<ul><li><p>a</p></li><li><p></p></li><li><p>b</p></li></ul>", CE_FORMAT_HTML);
    assert(ce_document_insert(d, 2, "\n"));
    expect_html(d, "<ul><li><p>a</p></li></ul><p></p><ul><li><p>b</p></li></ul>");
    ce_document_free(d);
}
static void test_document_json_transaction(void) {
    CeDocument *d = load("Original", CE_FORMAT_TEXT);
    assert(ce_document_insert(d, 8, " edited"));
    char *before = ce_document_export_json(d);
    const char *transformed = "{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"Original edited\"}]},{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"c1\",\"itemKey\":\"ABCDEFGH\",\"endnoteNumber\":1}}]}";
    assert(ce_document_apply_json(d, transformed));
    assert(ce_document_is_modified(d));
    char *after = ce_document_export_json(d);
    assert(!ce_document_apply_json(d, "{\"type\":\"paragraph\"}"));
    char *unchanged = ce_document_export_json(d);
    assert(!strcmp(after, unchanged)); free(unchanged);
    /* An identical sync must not consume another undo slot. */
    assert(ce_document_apply_json(d, after));
    assert(ce_document_undo(d));
    unchanged = ce_document_export_json(d);
    assert(!strcmp(before, unchanged)); free(unchanged);
    assert(ce_document_undo(d));
    assert(!ce_document_is_modified(d));
    assert(!strcmp(ce_document_text(d), "Original"));
    assert(ce_document_redo(d)); assert(ce_document_redo(d));
    unchanged = ce_document_export_json(d);
    assert(!strcmp(after, unchanged)); free(unchanged);
    free(before); free(after); ce_document_free(d);
}
static void test_moved_offset(void) {
    CeDocument *doc=load("<ul><li><p>A</p></li><li><p>B</p><ul><li><p>C</p></li></ul></li></ul>",CE_FORMAT_HTML);
    size_t offset=999;
    assert(ce_document_move_block_ex(doc,0,1,&offset));
    assert(!strcmp(ce_document_text(doc),"B\nC\nA") && offset==4);
    assert(ce_document_move_block_ex(doc,2,0,&offset));
    assert(!strcmp(ce_document_text(doc),"A\nB\nC") && offset==0);
    assert(ce_document_undo(doc) && !strcmp(ce_document_text(doc),"B\nC\nA"));
    ce_document_free(doc);
}
/* Keep failing after the selected allocation: recovery that reparses the old
 * JSON would fail too. All documents below use the same hooks for their whole
 * lifetime, including the printable cJSON buffers copied by document.c. */
typedef union { cJSON alignment; size_t magic; } TransactionAllocation;
static size_t transaction_calls, transaction_fail_at, transaction_live, transaction_rejected;
static void *transaction_allocate(size_t size) {
    ++transaction_calls;
    if (transaction_fail_at && transaction_calls >= transaction_fail_at) {
        ++transaction_rejected; return NULL;
    }
    TransactionAllocation *p = malloc(sizeof *p + size);
    if (!p) return NULL;
    p->magic = 0x7a11ca7eu; ++transaction_live; return p + 1;
}
static void transaction_free(void *pointer) {
    if (!pointer) return;
    TransactionAllocation *p = (TransactionAllocation *)pointer - 1;
    assert(p->magic == 0x7a11ca7eu && transaction_live);
    --transaction_live; free(p);
}
static CeDocument *transaction_document(void) {
    CeDocument *d = load("<p><em>Original</em></p>", CE_FORMAT_HTML);
    assert(ce_document_insert(d, ce_document_length(d), " saved"));
    ce_document_mark_saved(d);
    assert(ce_document_insert(d, ce_document_length(d), " redo"));
    assert(ce_document_undo(d));
    assert(!ce_document_is_modified(d) && ce_document_can_undo(d) && ce_document_can_redo(d));
    return d;
}
static void transaction_sweep(const char *target, bool no_op) {
    CeDocument *probe = transaction_document();
    char *same = no_op ? ce_document_export_json(probe) : NULL;
    transaction_calls = 0;
    assert(ce_document_apply_json(probe, no_op ? same : target));
    size_t calls = transaction_calls; free(same); ce_document_free(probe);
    assert(!transaction_live);
    size_t multiple_failures = 0;
    for (size_t fail_at = 1; fail_at <= calls; ++fail_at) {
        CeDocument *d = transaction_document();
        char *before = ce_document_export_json(d), *html = ce_document_export_html(d);
        const char *text = ce_document_text(d);
        size_t blocks = ce_document_block_count(d), spans = ce_document_span_count(d);
        CeStyle style = ce_document_style_at(d, 1);
        assert(before && html);
        transaction_calls = transaction_rejected = 0; transaction_fail_at = fail_at;
        bool ok = ce_document_apply_json(d, no_op ? before : target);
        transaction_fail_at = 0;
        if (ok) fprintf(stderr, "apply_json unexpectedly succeeded at sticky failure %zu/%zu\n", fail_at, calls);
        assert(!ok && *ce_document_error(d));
        if (transaction_rejected > 1) ++multiple_failures;
        char *after = ce_document_export_json(d), *after_html = ce_document_export_html(d);
        assert(after && after_html && !strcmp(before, after) && !strcmp(html, after_html));
        assert(ce_document_text(d) == text && ce_document_block_count(d) == blocks && ce_document_span_count(d) == spans);
        assert(ce_document_style_at(d, 1).marks == style.marks);
        assert(!ce_document_is_modified(d) && ce_document_can_undo(d) && ce_document_can_redo(d));
        /* Both histories survived, with their original savepoint semantics. */
        assert(ce_document_redo(d) && !strcmp(ce_document_text(d), "Original saved redo"));
        assert(ce_document_undo(d) && !ce_document_is_modified(d));
        assert(ce_document_undo(d) && !strcmp(ce_document_text(d), "Original"));
        free(before); free(html); free(after); free(after_html); ce_document_free(d);
        assert(!transaction_live);
    }
    assert(calls && multiple_failures);
    printf("apply_json sticky OOM: %zu allocation points (%zu with repeated failure) passed\n", calls, multiple_failures);
}
static void test_document_json_transaction_oom(void) {
    cJSON_Hooks hooks = {transaction_allocate, transaction_free}; cJSON_InitHooks(&hooks);
    transaction_sweep("{\"type\":\"doc\",\"content\":[{\"type\":\"heading\",\"attrs\":{\"level\":2},\"content\":[{\"type\":\"text\",\"text\":\"replacement\"}]},{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"c1\",\"endnoteNumber\":9}}]}", false);
    transaction_sweep("{\"doc\":{\"type\":\"doc\",\"content\":[]}}", false);
    transaction_sweep("{\"type\":\"doc\"}", false);
    transaction_sweep(NULL, true);
    CeDocument *d = transaction_document(); char *before = ce_document_export_json(d);
    const char *text = ce_document_text(d);
    assert(ce_document_apply_json(d, before) && ce_document_text(d) == text);
    assert(ce_document_can_redo(d) && !ce_document_is_modified(d));
    assert(ce_document_redo(d) && !strcmp(ce_document_text(d), "Original saved redo"));
    free(before); ce_document_free(d);
    /* Capacity eviction remains exactly one oldest undo entry per commit. */
    d = load("0", CE_FORMAT_TEXT);
    for (size_t i = 1; i <= 129; ++i) {
        char value[192];
        snprintf(value, sizeof value, "{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"%zu\"}]}]}", i);
        assert(ce_document_apply_json(d, value));
    }
    for (size_t i = 0; i < 128; ++i) assert(ce_document_undo(d));
    assert(!ce_document_can_undo(d) && !strcmp(ce_document_text(d), "1"));
    for (size_t i = 0; i < 128; ++i) assert(ce_document_redo(d));
    assert(!ce_document_can_redo(d) && !strcmp(ce_document_text(d), "129"));
    ce_document_free(d); assert(!transaction_live); cJSON_InitHooks(NULL);
}
int main(void) {
    test_exact_unknown_integers();
    test_edit();
    test_format();
    test_migration();
    test_unknown_json();
    test_citation_identity();
    test_citation_html_metadata();
    test_search_history();
    test_lists_and_random_edits();
    test_clear_and_alignment();
    test_atom_and_node_insertion();
    test_list_structure_edits();
    test_edit_boundaries();
    test_moved_offset();
    test_document_json_transaction();
    test_document_json_transaction_oom();
    puts("document tests passed");
    return 0;
}
