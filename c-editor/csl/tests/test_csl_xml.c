#include "csl_xml.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *xml = "<?xml version=\"1.0\"?><cs:style xmlns:cs=\"" CE_CSL_NAMESPACE "\" version=\"1.0\" xml:lang=\"zh-CN\"><cs:macro name=\"author\"><cs:text value=\" 中&amp;文🙂 \" prefix=\" \" suffix=\". \"/></cs:macro><cs:locale><cs:terms><cs:term name=\"and\">和&#x4E0E;</cs:term></cs:terms></cs:locale></cs:style>";
    char error[256];
    CeCslXml *doc = ce_csl_xml_parse(xml, strlen(xml), error, sizeof error);
    assert(doc && !*error && doc->node_count == 6 && ce_csl_xml_is(doc->root, "style"));
    assert(!strcmp(ce_csl_xml_attribute(doc->root, "xml:lang"), "zh-CN"));
    const CeCslXmlNode *macro = ce_csl_xml_child(doc->root, "macro");
    const CeCslXmlNode *text = ce_csl_xml_child(macro, "text");
    assert(!strcmp(ce_csl_xml_attribute(text, "value"), " 中&文🙂 "));
    assert(!strcmp(ce_csl_xml_attribute(text, "prefix"), " "));
    assert(!strcmp(ce_csl_xml_attribute(text, "suffix"), ". "));
    const CeCslXmlNode *term = ce_csl_xml_child(ce_csl_xml_child(ce_csl_xml_child(doc->root, "locale"), "terms"), "term");
    assert(!strcmp(term->text, "和与") && term->parent && text->line == 1);
    ce_csl_xml_free(doc);
    const char *bad[] = {"<style>", "<a x='1' x='2'/>", "<a>&missing;</a>",
        "<!DOCTYPE a [<!ENTITY e 'expanded'>]><a>&e;</a>",
        "<!DOCTYPE a SYSTEM 'file:///nonexistent-notelab-file'><a/>",
        "<a>\xc0\xaf</a>", "<a/><b/>"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        assert(!ce_csl_xml_parse(bad[i], strlen(bad[i]), error, sizeof error)); assert(*error);
    }
    assert(!ce_csl_xml_parse("<a>\0</a>", 8, error, sizeof error));
    assert(!ce_csl_xml_parse("<a/>", CE_CSL_XML_MAX_BYTES + 1, error, sizeof error));
    size_t depth = CE_CSL_XML_MAX_DEPTH + 1;
    char *nested = calloc(depth * 7 + 1, 1); assert(nested);
    for (size_t i = 0; i < depth; ++i) strcat(nested, "<a>");
    for (size_t i = 0; i < depth; ++i) strcat(nested, "</a>");
    assert(!ce_csl_xml_parse(nested, strlen(nested), error, sizeof error)); free(nested);
    doc = ce_csl_xml_parse("<style xmlns='urn:other'/>", 26, error, sizeof error);
    assert(doc && !ce_csl_xml_is(doc->root, "style")); ce_csl_xml_free(doc);
    for (int i = 1; i < argc; ++i) {
        FILE *file = fopen(argv[i], "rb"); assert(file);
        assert(!fseek(file, 0, SEEK_END)); long size = ftell(file);
        assert(size > 0 && (unsigned long)size <= CE_CSL_XML_MAX_BYTES);
        rewind(file); char *data = malloc((size_t)size); assert(data);
        assert(fread(data, 1, (size_t)size, file) == (size_t)size); assert(!fclose(file));
        doc = ce_csl_xml_parse(data, (size_t)size, error, sizeof error);
        if (!doc) fprintf(stderr, "%s: %s\n", argv[i], error);
        assert(doc && (ce_csl_xml_is(doc->root, "style") || ce_csl_xml_is(doc->root, "locale")));
        printf("Parsed %s: %zu elements\n", argv[i], doc->node_count);
        ce_csl_xml_free(doc); free(data);
    }
    puts("C CSL XML namespace, UTF-8, affix preservation and bounded rejection passed");
    return 0;
}
