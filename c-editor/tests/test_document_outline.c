#include "document_outline.h"
#include "cJSON.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static CeDocument *load(const char *json) {
    CeDocument *doc=ce_document_new(); char error[256];
    assert(doc && ce_document_import(doc,json,CE_FORMAT_JSON,error,sizeof error));
    return doc;
}
static void unchanged(CeDocument *doc,const char *before) {
    char *after=ce_document_export_json(doc); assert(after && !strcmp(before,after)); free(after);
}
static size_t allocations,limit;
static void *limited(size_t bytes) { return allocations++>=limit?NULL:malloc(bytes); }
static void exact_and_failure(void) {
    const char *fixture="{\"type\":\"doc\",\"legacy\":{\"precise\":1.0000000000000002,\"integer\":4503599627370501},\"content\":["
        "{\"type\":\"heading\",\"attrs\":{\"level\":1,\"precise\":1.0000000000000002},\"content\":[{\"type\":\"text\",\"text\":\"A\"}]},"
        "{\"type\":\"futureNode\",\"attrs\":{\"integer\":4503599627370501}},"
        "{\"type\":\"heading\",\"attrs\":{\"level\":1},\"content\":[{\"type\":\"text\",\"text\":\"B\"}]}]}";
    CeDocument *doc=load(fixture); char *before=ce_document_export_json(doc),error[256]; size_t offset;
    assert(before && ce_outline_move(doc,0,2,true,&offset,error,sizeof error));
    char *moved=ce_document_export_json(doc);
    assert(moved && strstr(moved,"1.0000000000000002") && strstr(moved,"4503599627370501"));
    assert(!strncmp(ce_document_text(doc),"B\nA\n",4));
    assert(ce_document_undo(doc)); unchanged(doc,before); assert(ce_document_redo(doc)); unchanged(doc,moved);
    free(before); free(moved); ce_document_free(doc);
    bool reached=false;
    for(limit=0;limit<3000;limit++) {
        doc=load(fixture); before=ce_document_export_json(doc); offset=123456;
        allocations=0; cJSON_Hooks hooks={limited,free}; cJSON_InitHooks(&hooks);
        bool ok=ce_outline_move(doc,0,2,true,&offset,error,sizeof error);
        cJSON_InitHooks(NULL);
        if(!ok) { unchanged(doc,before); assert(offset==123456 && !ce_document_can_undo(doc) && !ce_document_is_modified(doc)); }
        else { assert(ce_document_undo(doc)); unchanged(doc,before); reached=true; }
        free(before); ce_document_free(doc); if(reached) break;
    }
    assert(reached);
    doc=load(fixture); CeOutline outline={0}; assert(ce_outline_build(doc,&outline));
    CeOutlineEntry *entries=outline.entries; size_t count=outline.count;
    limit=allocations=0; cJSON_Hooks hooks={limited,free}; cJSON_InitHooks(&hooks);
    assert(!ce_outline_build(doc,&outline)); cJSON_InitHooks(NULL);
    assert(outline.entries==entries && outline.count==count);
    ce_outline_free(&outline); ce_document_free(doc);
}
int main(void) {
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);
#endif
    exact_and_failure();
    CeDocument *doc=load("{\"type\":\"doc\",\"custom\":{\"keep\":true},\"content\":["
        "{\"type\":\"heading\",\"attrs\":{\"level\":1},\"content\":[{\"type\":\"text\",\"text\":\"A\"}]},"
        "{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"intro\"}]},"
        "{\"type\":\"heading\",\"attrs\":{\"level\":2,\"id\":\"section-a1\"},\"content\":[{\"type\":\"text\",\"text\":\"A1 中文\"}]},"
        "{\"type\":\"bulletList\",\"extra\":\"wrapper\",\"content\":[{\"type\":\"listItem\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"list body\",\"marks\":[{\"type\":\"bold\"}]}]}]}]},"
        "{\"type\":\"heading\",\"attrs\":{\"level\":3},\"content\":[{\"type\":\"text\",\"text\":\"A1a\"}]},"
        "{\"type\":\"futureWidget\",\"attrs\":{\"secret\":\"retained\"}},"
        "{\"type\":\"heading\",\"attrs\":{\"level\":2},\"content\":[{\"type\":\"text\",\"text\":\"A2\"}]},"
        "{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"body2\"}]},"
        "{\"type\":\"heading\",\"attrs\":{\"level\":1},\"content\":[{\"type\":\"text\",\"text\":\"B\"}]},"
        "{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"last\"}]}]}");
    CeOutline outline={0}; assert(ce_outline_build(doc,&outline));
    assert(outline.count==5 && outline.entries[0].parent==SIZE_MAX);
    assert(outline.entries[1].parent==0 && outline.entries[2].parent==1 && outline.entries[3].parent==0);
    assert(outline.entries[0].children && outline.entries[1].children && !outline.entries[2].children);
    assert(outline.entries[1].section_end==outline.entries[3].start);
    assert(outline.entries[0].section_end==outline.entries[4].start);
    char *before=ce_document_export_json(doc),error[256]; size_t offset=SIZE_MAX;
    assert(before && ce_outline_move(doc,2,6,true,&offset,error,sizeof error));
    assert(!strcmp(ce_document_text(doc),"A\nintro\nA2\nbody2\nA1 中文\nlist body\nA1a\n\xef\xbf\xbc\nB\nlast"));
    assert(!strncmp(ce_document_text(doc)+offset,"A1 中文",9));
    char *moved=ce_document_export_json(doc);
    assert(strstr(moved,"wrapper") && strstr(moved,"retained") && strstr(moved,"section-a1") && strstr(moved,"bold"));
    assert(ce_document_undo(doc)); unchanged(doc,before); assert(!ce_document_is_modified(doc));
    assert(ce_document_redo(doc)); unchanged(doc,moved); assert(ce_document_undo(doc));
    offset=12345;
    assert(!ce_outline_move(doc,2,4,false,&offset,error,sizeof error)); unchanged(doc,before); assert(offset==12345);
    assert(ce_outline_move(doc,0,8,true,&offset,error,sizeof error));
    assert(!strncmp(ce_document_text(doc),"B\nlast\nA\nintro\n",15));
    assert(ce_document_undo(doc)); unchanged(doc,before);
    uint64_t revision=ce_document_revision(doc);
    assert(ce_outline_move(doc,0,0,true,&offset,error,sizeof error));
    assert(revision==ce_document_revision(doc)); unchanged(doc,before);
    free(before); free(moved); ce_outline_free(&outline); ce_document_free(doc);

    doc=load("{\"type\":\"doc\",\"content\":[{\"type\":\"orderedList\",\"attrs\":{\"start\":7,\"keep\":\"yes\"},\"content\":["
        "{\"type\":\"listItem\",\"custom\":\"first\",\"content\":[{\"type\":\"heading\",\"attrs\":{\"level\":2},\"content\":[{\"type\":\"text\",\"text\":\"one\"}]},{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"body-one\"}]}]},"
        "{\"type\":\"listItem\",\"content\":[{\"type\":\"heading\",\"attrs\":{\"level\":2},\"content\":[{\"type\":\"text\",\"text\":\"two\"}]},{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"body-two\"}]}]}]},"
        "{\"type\":\"heading\",\"attrs\":{\"level\":1},\"content\":[{\"type\":\"text\",\"text\":\"outside\"}]}]}");
    before=ce_document_export_json(doc);
    assert(ce_outline_move(doc,0,2,true,&offset,error,sizeof error));
    assert(!strcmp(ce_document_text(doc),"two\nbody-two\none\nbody-one\noutside"));
    assert(offset==13); assert(ce_document_undo(doc)); unchanged(doc,before);
    assert(!ce_outline_move(doc,0,4,false,&offset,error,sizeof error)); unchanged(doc,before);
    assert(!ce_outline_move(doc,1,2,true,&offset,error,sizeof error)); unchanged(doc,before);
    free(before); ce_document_free(doc); return 0;
}
