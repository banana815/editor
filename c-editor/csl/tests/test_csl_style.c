#include "csl_style.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[256];
#define OK(call) do { bool success = (call); if (!success) fprintf(stderr, "%s: %s\n", #call, error); assert(success); } while (0)
#define OPEN "<style xmlns=\"" CE_CSL_NAMESPACE "\" version=\"1.0\" class=\"in-text\">"
#define INFO "<info><id>urn:test:style</id><title>Test style</title><updated>2026-11-15T00:00:00Z</updated></info>"
#define CITE "<citation><layout><text value=\"ok\"/></layout></citation>"
static const char rich[] =
    "<style xmlns=\"" CE_CSL_NAMESPACE "\" version=\"1.0\" class=\"note\" default-locale=\"zh-CN\" initialize-with=\". \">"
    "<info><id> urn:test:owned </id><title xml:lang=\"zh-CN\"> 中文 &amp; 标题 </title>"
    "<author><name> 作者 &amp; Author </name><email>a@example.org</email></author>"
    "<link rel=\"documentation\" href=\"https://example.org/style\"> documentation </link>"
    "<rights license=\"urn:license:test\">Test</rights><updated>2026-11-15T00:00:00Z</updated></info>"
    "<locale><terms><term name=\"and\"> &amp; </term></terms></locale>"
    "<locale xml:lang=\"zh-CN\"><date form=\"text\"><date-part name=\"year\" suffix=\"年 \"/></date>"
    "<terms><term name=\"page\" form=\"short\"><single> 页 </single><multiple> 页码 </multiple></term></terms></locale>"
    "<macro name=\"unused\"><text value=\"unused\"/></macro>"
    "<macro name=\"main\"><choose><if type=\"book\"><text macro=\"leaf\"/></if>"
    "<else-if variable=\"title\"><text macro=\"leaf\"/></else-if><else><text value=\"?\"/></else></choose></macro>"
    "<macro name=\"leaf\"><text variable=\"title\" prefix=\" \" suffix=\". \" font-style=\"italic\"/></macro>"
    "<citation><sort><key macro=\"main\"/></sort><layout delimiter=\"; \" prefix=\"(\" suffix=\")\"><text macro=\"main\"/></layout></citation>"
    "<bibliography hanging-indent=\"true\"><layout><group display=\"block\"><text macro=\"main\"/></group></layout></bibliography></style>";

static char *wrap(const char *body) {
    size_t size = strlen(OPEN INFO) + strlen(body) + strlen("</style>");
    char *result = malloc(size + 1);
    assert(result);
    snprintf(result, size + 1, "%s%s%s", OPEN INFO, body, "</style>");
    return result;
}
static char *replace(const char *text, const char *old, const char *replacement) {
    const char *at = strstr(text, old);
    assert(at);
    size_t prefix = (size_t)(at - text), old_size = strlen(old), new_size = strlen(replacement);
    size_t size = strlen(text) - old_size + new_size;
    char *result = malloc(size + 1);
    assert(result);
    memcpy(result, text, prefix);
    memcpy(result + prefix, replacement, new_size);
    strcpy(result + prefix + new_size, at + old_size);
    return result;
}
static void test_owned_views(void) {
    CeCslStyle *style = NULL;
    char *input = malloc(sizeof rich);
    assert(input); memcpy(input, rich, sizeof rich);
    OK(ce_csl_style_load(&style, input, strlen(input), error, sizeof error));
    memset(input, 'x', sizeof rich - 1); free(input);
    assert(!strcmp(ce_csl_style_id(style), " urn:test:owned "));
    assert(!strcmp(ce_csl_style_title(style), " 中文 & 标题 "));
    assert(!strcmp(ce_csl_style_default_locale(style), "zh-CN"));
    assert(ce_csl_style_citation(style) && ce_csl_style_bibliography(style));
    assert(ce_csl_style_macro_count(style) == 3);
    const CeCslXmlNode *leaf = ce_csl_style_macro(style, "leaf");
    assert(leaf && ce_csl_xml_is(leaf, "macro") && ce_csl_style_macro(style, "unused"));
    assert(!ce_csl_style_macro(style, "LEAF") && !ce_csl_style_macro(style, NULL));
    const CeCslXmlNode *text = ce_csl_xml_child(leaf, "text");
    assert(!strcmp(ce_csl_xml_attribute(text, "prefix"), " "));
    assert(!strcmp(ce_csl_xml_attribute(text, "suffix"), ". "));
    assert(!strcmp(ce_csl_xml_attribute(ce_csl_style_root(style), "initialize-with"), ". "));
    const CeCslXmlNode *author = ce_csl_xml_child(ce_csl_xml_child(ce_csl_style_root(style), "info"), "author");
    assert(!strcmp(ce_csl_xml_child(author, "name")->text, " 作者 & Author "));
    assert(ce_csl_style_inline_locale_count(style) == 2 && !ce_csl_style_inline_locale(style, 2));
    const CeCslXmlNode *generic = ce_csl_style_inline_locale(style, 0);
    assert(!ce_csl_xml_attribute(generic, "xml:lang"));
    assert(!strcmp(ce_csl_xml_child(ce_csl_xml_child(generic, "terms"), "term")->text, " & "));
    assert(!strcmp(ce_csl_xml_attribute(ce_csl_style_inline_locale(style, 1), "xml:lang"), "zh-CN"));
    OK(ce_csl_style_load(&style, OPEN INFO CITE "</style>", strlen(OPEN INFO CITE "</style>"), error, sizeof error));
    assert(!ce_csl_style_bibliography(style) && !ce_csl_style_default_locale(style));
    assert(!ce_csl_style_macro_count(style) && !ce_csl_style_inline_locale_count(style));
    ce_csl_style_free(style);
    assert(!ce_csl_style_root(NULL) && !ce_csl_style_id(NULL) && !ce_csl_style_title(NULL));
    assert(!ce_csl_style_citation(NULL) && !ce_csl_style_bibliography(NULL));
    assert(!ce_csl_style_default_locale(NULL) && !ce_csl_style_macro(NULL, "x"));
    assert(!ce_csl_style_inline_locale(NULL, 0) && !ce_csl_style_macro_count(NULL));
    ce_csl_style_free(NULL);
}
static void rejected(CeCslStyle **slot, const char *xml) {
    CeCslStyle *old = *slot;
    const CeCslXmlNode *root = ce_csl_style_root(old), *macro = ce_csl_style_macro(old, "leaf");
    const char *title = ce_csl_style_title(old);
    assert(!ce_csl_style_load(slot, xml, strlen(xml), error, sizeof error));
    assert(*error && *slot == old && ce_csl_style_root(*slot) == root);
    assert(ce_csl_style_macro(*slot, "leaf") == macro && ce_csl_style_title(*slot) == title);
}
static void test_invalid_syntax(void) {
    CeCslStyle *style = NULL;
    OK(ce_csl_style_load(&style, rich, strlen(rich), error, sizeof error));
    const struct { const char *old, *replacement; } changes[] = {
        {"version=\"1.0\"", "version=\"1.0.2\""}, {"version=\"1.0\"", "version=\"1.0.1\""},
        {"version=\"1.0\"", ""}, {"class=\"note\"", "class=\"numeric\""}, {"class=\"note\"", ""},
        {"xmlns=\"" CE_CSL_NAMESPACE "\"", "xmlns=\"urn:other\""},
        {"<text variable=\"title\"", "<text xmlns=\"urn:extension\" variable=\"title\""},
        {"<text variable=\"title\"", "<text xmlns=\"\" variable=\"title\""},
        {"font-style=\"italic\"", "font-style=\"italics\""}, {"font-style=\"italic\"", "font-weigth=\"bold\""},
        {"font-style=\"italic\"", "x:font-style=\"italic\" xmlns:x=\"urn:extension\""},
        {"name=\"leaf\"", "name=\"leaf duplicate\""}, {"name=\"leaf\"", "name=\"\""},
        {"name=\"leaf\"", "name=\"main\""}, {"<macro name=\"leaf\">", "<macro>"},
        {"macro=\"leaf\"", "macro=\"missing\""}, {"macro=\"main\"", "macro=\"missing-in-sort\""},
        {"<text variable=\"title\"", "<text variable=\"not-a-variable\""},
        {"<text variable=\"title\"", "<text variable=\"title page\""},
        {"<text variable=\"title\"", "<text variable=\"title\" value=\"double\""},
        {"<text variable=\"title\"", "<text variable=\"title\" plural=\"true\""},
        {"type=\"book\"", "type=\"book alien\""}, {"type=\"book\"", "type=\"\""},
        {"type=\"book\"", "match=\"all\""}, {"type=\"book\"", "type=\"book\" match=\"some\""},
        {"hanging-indent=\"true\"", "hanging-indent=\"yes\""},
        {"hanging-indent=\"true\"", "line-spacing=\"0\""},
        {"hanging-indent=\"true\"", "et-al-min=\"-1\""},
        {"hanging-indent=\"true\"", "et-al-min=\"99999999999999999999999\""},
        {"default-locale=\"zh-CN\"", "default-locale=\"zh_CN\""},
        {"<author><name>", "<author><name variable=\"author\">"},
        {"name=\"page\"", "name=\"unknown-term\""}, {"name=\"page\"", "name=\"locator\""},
        {"form=\"short\"", "form=\"banana\""},
        {"<id> urn:test:owned </id>", ""}, {"<updated>2026-11-15T00:00:00Z</updated>", ""},
        {"<single> 页 </single><multiple> 页码 </multiple>", "<multiple> 页码 </multiple><single> 页 </single>"},
        {"<date-part name=\"year\"", "<date-part name=\"year\" form=\"numeric\""},
        {"<group display=\"block\">", "<group display=\"table\">"},
        {"<citation>", "<citation unsupported=\"true\">"},
        {"<info>", "<info extra=\"value\">"}, {"rel=\"documentation\"", "rel=\"independent-parent\""}
    };
    for (size_t i = 0; i < sizeof changes / sizeof *changes; ++i) {
        char *bad = replace(rich, changes[i].old, changes[i].replacement);
        rejected(&style, bad); free(bad);
    }
    const char *bodies[] = {
        "", CITE CITE, "<citation><layout/></citation>",
        "<citation><layout><unknown/></layout></citation>",
        "<citation><layout><text/></layout></citation>",
        "<citation><layout><text value=\"a\"><text value=\"b\"/></text></layout></citation>",
        "<citation><layout>unexpected<text value=\"a\"/></layout></citation>",
        "<citation><layout><choose><else><text value=\"x\"/></else></choose></layout></citation>",
        "<citation><layout><choose><if type=\"book\"/><else/><else-if type=\"article\"/></choose></layout></citation>",
        "<citation><layout><choose><if type=\"book\"/><if type=\"article\"/></choose></layout></citation>",
        "<citation><layout><names variable=\"title\"/></layout></citation>",
        "<citation><layout><names variable=\"author\"><name><name-part name=\"family\"/><name-part name=\"family\"/></name></names></layout></citation>",
        "<citation><layout><names variable=\"author\"><label variable=\"page\"/></names></layout></citation>",
        "<citation><layout><names variable=\"author\"><substitute><text value=\"x\"/></substitute><name/></names></layout></citation>",
        "<citation><layout><label/></layout></citation>",
        "<citation><layout><number variable=\"issued\"/></layout></citation>",
        "<citation><layout><date variable=\"issued\"/></layout></citation>",
        "<citation><layout><date variable=\"issued\" form=\"text\"><date-part name=\"year\" prefix=\" \"/></date></layout></citation>",
        "<citation><layout><date variable=\"issued\"><date-part name=\"month\"/><date-part name=\"month\"/></date></layout></citation>",
        "<citation><layout><text value=\"ok\"/></layout><sort><key variable=\"title\"/></sort></citation>",
        "<citation><sort><key variable=\"title\" macro=\"main\"/></sort><layout><text value=\"ok\"/></layout></citation>",
        CITE "<bibliography><layout><text value=\"b\"/></layout></bibliography><bibliography><layout><text value=\"b\"/></layout></bibliography>",
        CITE "<bibliography><layout delimiter=\";\"><text value=\"b\"/></layout></bibliography>",
        CITE "<locale><date form=\"text\"><date-part name=\"year\"/></date><date form=\"text\"><date-part name=\"month\"/></date></locale>",
        CITE "<locale><terms><term name=\"and\">mixed<single>x</single><multiple>y</multiple></term></terms></locale>"
    };
    for (size_t i = 0; i < sizeof bodies / sizeof *bodies; ++i) {
        char *bad = wrap(bodies[i]); rejected(&style, bad); free(bad);
    }
    rejected(&style, "<!DOCTYPE style SYSTEM 'https://example.org/remote.dtd'>" OPEN INFO CITE "</style>");
    rejected(&style, "<style xmlns=\"" CE_CSL_NAMESPACE "\" version=\"1.0\"><info><id>x</id><title>dependent</title>"
        "<updated>2026-11-15T00:00:00Z</updated><link rel=\"independent-parent\" href=\"https://example.org/style\"/></info></style>");
    assert(!ce_csl_style_load(&style, "x", CE_CSL_XML_MAX_BYTES + 1, error, sizeof error));
    assert(!ce_csl_style_load(&style, "a\0b", 3, error, sizeof error));
    assert(!ce_csl_style_load(NULL, rich, strlen(rich), error, sizeof error));
    ce_csl_style_free(style);
}
static void test_all_macro_branches(void) {
    CeCslStyle *style = NULL;
    OK(ce_csl_style_load(&style, rich, strlen(rich), error, sizeof error));
    const char *bad_bodies[] = {
        "<macro name=\"a\"><text macro=\"a\"/></macro>" CITE,
        "<macro name=\"a\"><text macro=\"b\"/></macro><macro name=\"b\"><text macro=\"a\"/></macro>" CITE,
        "<macro name=\"unused\"><choose><if type=\"book\"><text value=\"ok\"/></if><else><text macro=\"absent\"/></else></choose></macro>" CITE,
        "<macro name=\"a\"><choose><if type=\"book\"><text value=\"ok\"/></if><else-if variable=\"title\"><text macro=\"b\"/></else-if></choose></macro><macro name=\"b\"><text macro=\"a\"/></macro>" CITE,
        CITE "<bibliography><sort><key macro=\"missing\"/></sort><layout><text value=\"b\"/></layout></bibliography>"
    };
    for (size_t i = 0; i < sizeof bad_bodies / sizeof *bad_bodies; ++i) {
        char *bad = wrap(bad_bodies[i]); rejected(&style, bad); free(bad);
    }
    const char *body = "<macro name=\"终点\"><text value=\"end\"/></macro>"
        "<macro name=\"left\"><text macro=\"终点\"/></macro><macro name=\"right\"><text macro=\"终点\"/></macro>"
        "<macro name=\"root\"><group><text macro=\"left\"/><text macro=\"right\"/></group></macro>"
        "<citation><sort><key macro=\"root\"/></sort><layout><text macro=\"root\"/></layout></citation>";
    char *xml = wrap(body);
    OK(ce_csl_style_load(&style, xml, strlen(xml), error, sizeof error));
    assert(ce_csl_style_macro(style, "终点") && ce_csl_style_macro_count(style) == 4);
    free(xml); ce_csl_style_free(style);
}
/* These small shapes exercise the empty metadata and locale overrides used by
 * official test-suite fixtures number_PageRange / locale_OverloadWithEmptyString.
 * Optional argv fixtures below load the actual upstream bytes as well. */
static void test_tiny_fixture_shapes(void) {
    const char *xml = OPEN "<info><id/><title/><updated>2009-08-10T04:49:00+09:00</updated></info>"
        "<locale xml:lang=\"en\"><terms><term name=\"et-al\"/></terms></locale>"
        "<citation><layout><text variable=\"page\"/></layout></citation></style>";
    CeCslStyle *style = NULL;
    OK(ce_csl_style_load(&style, xml, strlen(xml), error, sizeof error));
    assert(!*ce_csl_style_id(style) && !*ce_csl_style_title(style));
    assert(ce_csl_style_inline_locale_count(style) == 1);
    ce_csl_style_free(style);
}
static void add(char *buffer, size_t *used, const char *text) {
    size_t size = strlen(text);
    assert(size < CE_CSL_XML_MAX_BYTES - *used);
    memcpy(buffer + *used, text, size + 1); *used += size;
}
static void test_bounds_and_deep_graph(void) {
    char *xml = malloc(CE_CSL_XML_MAX_BYTES);
    assert(xml);
    size_t used = 0;
    add(xml, &used, OPEN INFO);
    for (size_t i = 0; i < CE_CSL_STYLE_MAX_MACROS; ++i) {
        char macro[180];
        if (i + 1 < CE_CSL_STYLE_MAX_MACROS)
            snprintf(macro, sizeof macro, "<macro name=\"m%zu\"><text macro=\"m%zu\"/></macro>", i, i + 1);
        else snprintf(macro, sizeof macro, "<macro name=\"m%zu\"><text value=\"end\"/></macro>", i);
        add(xml, &used, macro);
    }
    add(xml, &used, "<citation><layout><text macro=\"m0\"/></layout></citation></style>");
    CeCslStyle *style = NULL;
    OK(ce_csl_style_load(&style, xml, used, error, sizeof error));
    assert(ce_csl_style_macro_count(style) == CE_CSL_STYLE_MAX_MACROS);
    char *too_many = replace(xml, "</style>", "<macro name=\"one-too-many\"><text value=\"x\"/></macro></style>");
    rejected(&style, too_many); free(too_many);
    char *cycle = replace(xml, "value=\"end\"", "macro=\"m0\"");
    rejected(&style, cycle); assert(strstr(error, "Cyclic")); free(cycle);

    used = 0; add(xml, &used, OPEN INFO CITE);
    for (size_t i = 0; i <= CE_CSL_STYLE_MAX_INLINE_LOCALES; ++i) add(xml, &used, "<locale/>");
    add(xml, &used, "</style>"); rejected(&style, xml);
    used = 0; add(xml, &used, OPEN INFO "<macro name=\"x\"><text value=\"x\"/></macro><citation><layout>");
    for (size_t i = 0; i <= CE_CSL_STYLE_MAX_REFERENCES; ++i) add(xml, &used, "<text macro=\"x\"/>");
    add(xml, &used, "</layout></citation></style>"); rejected(&style, xml);
    assert(strstr(error, "reference limit"));
    free(xml); ce_csl_style_free(style);
}

typedef union Allocation {
    struct { size_t size, magic; } header;
    long double alignment;
    void *pointer;
} Allocation;
typedef struct Memory { size_t calls, fail_at, live, bytes; } Memory;
static void *test_allocate(void *context, size_t size) {
    Memory *memory = context;
    if (++memory->calls == memory->fail_at) return NULL;
    assert(size <= SIZE_MAX - sizeof(Allocation));
    Allocation *allocation = malloc(sizeof *allocation + size);
    assert(allocation); allocation->header.size = size; allocation->header.magic = 0xc51a;
    ++memory->live; memory->bytes += size;
    return allocation + 1;
}
static void test_deallocate(void *context, void *pointer) {
    Memory *memory = context;
    Allocation *allocation = (Allocation *)pointer - 1;
    assert(pointer && allocation->header.magic == 0xc51a && memory->live);
    assert(memory->bytes >= allocation->header.size);
    --memory->live; memory->bytes -= allocation->header.size; allocation->header.magic = 0;
    free(allocation);
}
static void test_oom_rollback(void) {
    CeCslStyle *style = NULL;
    Memory memory = {0}, prior_memory = {0};
    CeCslStyleAllocator allocator = {&memory, test_allocate, test_deallocate};
    CeCslStyleAllocator prior_allocator = {&prior_memory, test_allocate, test_deallocate};
    OK(ce_csl_style_load_with_allocator(&style, rich, strlen(rich), &prior_allocator, error, sizeof error));
    size_t old_live = prior_memory.live, old_bytes = prior_memory.bytes;
    CeCslStyle *old = style;
    const CeCslXmlNode *root = ce_csl_style_root(old);
    bool succeeded = false;
    for (size_t point = 1; point < 12; ++point) {
        memory.calls = 0; memory.fail_at = point;
        bool success = ce_csl_style_load_with_allocator(&style, rich, strlen(rich), &allocator, error, sizeof error);
        if (!success) {
            assert(*error && !memory.live && !memory.bytes);
            assert(style == old && ce_csl_style_root(style) == root);
            assert(prior_memory.live == old_live && prior_memory.bytes == old_bytes);
        } else {
            assert(!*error && !prior_memory.live && !prior_memory.bytes);
            succeeded = true; break;
        }
    }
    assert(succeeded);
    memory.fail_at = 0;
    size_t live = memory.live, bytes = memory.bytes;
    char *cycle = replace(rich, "<text value=\"unused\"/>", "<text macro=\"unused\"/>");
    old = style;
    assert(!ce_csl_style_load_with_allocator(&style, cycle, strlen(cycle), &allocator, error, sizeof error));
    assert(style == old && memory.live == live && memory.bytes == bytes);
    free(cycle);
    allocator.allocate = NULL;
    assert(!ce_csl_style_load_with_allocator(&style, rich, strlen(rich), &allocator, error, sizeof error));
    assert(style == old && memory.live == live && memory.bytes == bytes);
    ce_csl_style_free(style);
    assert(!memory.live && !memory.bytes);
}
static void test_file(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long length = ftell(file);
    assert(length > 0 && (size_t)length <= CE_CSL_XML_MAX_BYTES);
    rewind(file);
    char *data = malloc((size_t)length + 1);
    assert(data && fread(data, 1, (size_t)length, file) == (size_t)length);
    fclose(file); data[length] = 0;
    char *start = strstr(data, ">>===== CSL =====>>");
    size_t size = (size_t)length;
    if (start) {
        start += strlen(">>===== CSL =====>>");
        char *end = strstr(start, "<<===== CSL =====<<");
        assert(end); size = (size_t)(end - start);
    } else start = data;
    CeCslStyle *style = NULL;
    if (!ce_csl_style_load(&style, start, size, error, sizeof error)) {
        fprintf(stderr, "%s: %s\n", path, error); abort();
    }
    printf("Validated %s (%zu macros)\n", path, ce_csl_style_macro_count(style));
    ce_csl_style_free(style); free(data);
}
int main(int argc, char **argv) {
    test_owned_views();
    test_invalid_syntax();
    test_all_macro_branches();
    test_tiny_fixture_shapes();
    test_bounds_and_deep_graph();
    test_oom_rollback();
    for (int i = 1; i < argc; ++i) test_file(argv[i]);
    puts("C CSL style ownership, vocabulary, all-branch macro graph, bounds and atomic OOM passed");
    return 0;
}
