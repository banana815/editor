#include "document_publish.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char error[512];
static CeDocument *load(const char *json) {
    CeDocument *doc=ce_document_new(); assert(doc);
    assert(ce_document_import(doc,json,CE_FORMAT_JSON,error,sizeof error)); return doc;
}
static char *render(CeDocument *doc,CePublishFormat format) {
    char *out=ce_document_publish(doc,format,NULL,NULL,error,sizeof error);
    if(!out) fprintf(stderr,"Publish failed: %s\n",error);
    assert(out); return out;
}
static void article(void) {
    CeDocument *doc=load("{\"type\":\"doc\",\"content\":["
        "{\"type\":\"heading\",\"attrs\":{\"level\":1},\"content\":[{\"type\":\"text\",\"text\":\"文章 <script>\"}]},"
        "{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"bold\",\"marks\":[{\"type\":\"bold\"}]},"
        "{\"type\":\"hardBreak\"},{\"type\":\"text\",\"text\":\"[literal]* & 中文\"}]},"
        "{\"type\":\"orderedList\",\"attrs\":{\"start\":3},\"content\":[{\"type\":\"listItem\",\"content\":["
        "{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"parent\"}]},"
        "{\"type\":\"bulletList\",\"content\":[{\"type\":\"listItem\",\"content\":["
        "{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"child\"}]}]}]}]}]}]}");
    char *before=ce_document_export_json(doc),*md=render(doc,CE_PUBLISH_MARKDOWN),*html=render(doc,CE_PUBLISH_HTML);
    assert(strstr(md,"# 文章 &lt;script&gt;\n\n**bold**  \n\\[literal\\]\\* &amp; 中文"));
    assert(strstr(md,"3. parent\n   \n   - child"));
    assert(strstr(html,"<h1>文章 &lt;script&gt;</h1>"));
    assert(strstr(html,"<ol start=\"3\"><li><p>parent</p>"));
    assert(strstr(html,"<strong>bold</strong><br>[literal]* &amp; 中文"));
    char *after=ce_document_export_json(doc); assert(!strcmp(before,after));
    assert(!ce_document_can_undo(doc) && !ce_document_is_modified(doc));
    free(before); free(after); free(md); free(html); ce_document_free(doc);
}
static void styled_and_links(void) {
    CeDocument *doc=load("{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"attrs\":{\"textAlign\":\"center\"},"
        "\"content\":[{\"type\":\"text\",\"text\":\"a & b\",\"marks\":[{\"type\":\"underline\"},"
        "{\"type\":\"highlight\",\"attrs\":{\"color\":\"#ffcc00\"}},"
        "{\"type\":\"link\",\"attrs\":{\"href\":\"https://example.com/?a=1&b=2\"}}]}]}]}");
    char *out=render(doc,CE_PUBLISH_MARKDOWN);
    assert(strstr(out,"<p style=\"text-align:center\"><u><mark style=\"background-color:#ffcc00\"><a href=\"https://example.com/?a=1&amp;b=2\">a &amp; b</a></mark></u></p>"));
    free(out); ce_document_free(doc);
}
static void rejection(const char *json) {
    CeDocument *doc=load(json); char *before=ce_document_export_json(doc);
    for(int kind=CE_PUBLISH_MARKDOWN;kind<=CE_PUBLISH_HTML;kind++) {
        assert(!ce_document_publish(doc,(CePublishFormat)kind,NULL,NULL,error,sizeof error)); assert(*error);
        char *after=ce_document_export_json(doc); assert(!strcmp(before,after)); free(after);
    }
    free(before); ce_document_free(doc);
}
static void unsupported(void) {
    rejection("{\"type\":\"doc\",\"content\":[{\"type\":\"image\",\"attrs\":{\"src\":\"https://example.com/image.png\"}}]}");
    rejection("{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"unsafe\",\"marks\":[{\"type\":\"link\",\"attrs\":{\"href\":\"javascript:alert(1)\"}}]}]}]}");
    rejection("{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"unsafe\",\"marks\":[{\"type\":\"textStyle\",\"attrs\":{\"color\":\"red; background:url(evil)\"}}]}]}]}");
    rejection("{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"unknown\",\"marks\":[{\"type\":\"unknownMark\"}]}]}]}");
    rejection("{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"x\",\"cslRenderJson\":\"POISON\"}}]}");
}
int main(void) { article(); styled_and_links(); unsupported(); return 0; }
