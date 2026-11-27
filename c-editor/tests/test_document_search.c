#include "document_search.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t allocation,fail_at;
static void *failing_malloc(size_t size) {
    ++allocation;
    if(fail_at && allocation>=fail_at) return NULL;
    return malloc(size);
}
static CeDocument *load(const char *text,CeFormat format) {
    CeDocument *doc=ce_document_new(); char error[256];
    assert(doc && ce_document_import(doc,text,format,error,sizeof error));
    ce_document_mark_saved(doc); return doc;
}
static void unchanged(CeDocument *doc,const char *before) {
    char *after=ce_document_export_json(doc); assert(after && !strcmp(before,after)); free(after);
}
int main(void) {
    const char *fixture="{\"type\":\"doc\",\"attrs\":{\"unknown\":4503599627370501},\"content\":["
        "{\"type\":\"heading\",\"attrs\":{\"level\":2,\"custom\":\"keep\"},\"content\":["
        "{\"type\":\"text\",\"text\":\"hel\",\"marks\":[{\"type\":\"bold\"}],\"customText\":true},"
        "{\"type\":\"text\",\"text\":\"lo HELLO\",\"marks\":[{\"type\":\"link\",\"attrs\":{\"href\":\"https://example.org\"}}]}]},"
        "{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"note\":\"hello\",\"unknown\":\"keep atom\"}},"
        "{\"type\":\"bulletList\",\"attrs\":{\"unknown\":9},\"content\":[{\"type\":\"listItem\",\"content\":["
        "{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"中文 hello\"}]}]}]}]}";
    CeDocument *doc=load(fixture,CE_FORMAT_JSON); char error[256];
    char *before=ce_document_export_json(doc); assert(before);
    size_t count=99,cursor=99,start,end;
    assert(ce_document_search_next(doc,"HELLO",1,false,&start,&end) && start==6 && end==11);
    assert(!ce_document_search_next(doc,"keep atom",0,true,&start,&end));
    assert(!ce_document_search_next(doc,"\xEF\xBF\xBC",0,true,&start,&end));
    assert(ce_document_search_replace(doc,"hello","世界",0,true,&count,&cursor,error,sizeof error));
    assert(count==3 && strstr(ce_document_text(doc),"世界 世界") && strstr(ce_document_text(doc),"中文 世界"));
    assert(cursor==ce_document_length(doc));
    assert(ce_document_style_at(doc,0).marks&CE_MARK_BOLD);
    assert(ce_document_style_at(doc,7).marks&CE_MARK_LINK);
    char *after=ce_document_export_json(doc); assert(after);
    assert(strstr(after,"4503599627370501") && strstr(after,"customText") && strstr(after,"keep atom") && strstr(after,"\"note\":\"hello\""));
    assert(ce_document_undo(doc)); unchanged(doc,before); assert(!ce_document_can_undo(doc) && !ce_document_is_modified(doc));
    assert(ce_document_redo(doc)); unchanged(doc,after); free(after);
    assert(ce_document_undo(doc));
    assert(!ce_document_search_replace(doc,"","x",0,true,&count,&cursor,error,sizeof error)); unchanged(doc,before);
    assert(!ce_document_search_replace(doc,"hello","bad\nline",0,true,&count,&cursor,error,sizeof error)); unchanged(doc,before);
    assert(!ce_document_search_replace(doc,"hello","\xED\xA0\x80",0,true,&count,&cursor,error,sizeof error)); unchanged(doc,before);
    assert(ce_document_search_replace(doc,"missing","x",0,true,&count,&cursor,error,sizeof error) && !count);
    unchanged(doc,before); assert(!ce_document_can_undo(doc));
    assert(ce_document_search_replace(doc,"中文","",0,false,&count,&cursor,error,sizeof error) && count==1);
    assert(!strstr(ce_document_text(doc),"中文")); assert(ce_document_undo(doc)); unchanged(doc,before);
    ce_document_free(doc);
    doc=load("aaaa\naaaa",CE_FORMAT_TEXT);
    assert(ce_document_search_next(doc,"aa",1,false,&start,&end) && start==1 && end==3);
    assert(ce_document_search_replace(doc,"aa","aaa",0,true,&count,&cursor,error,sizeof error));
    assert(count==4 && !strcmp(ce_document_text(doc),"aaaaaa\naaaaaa"));
    assert(ce_document_undo(doc) && !ce_document_can_undo(doc));
    assert(ce_document_search_replace(doc,"aa","X",1,false,&count,&cursor,error,sizeof error));
    assert(count==1 && cursor==2 && !strcmp(ce_document_text(doc),"aXa\naaaa"));
    assert(!ce_document_search_query_valid("a\na") && !ce_document_search_query_valid("\xC0\xAF"));
    ce_document_free(doc);
    /* Fail every cJSON allocation persistently, including final apply_json.
     * The live document must never require allocation to roll back. */
    cJSON_Hooks hooks={failing_malloc,free}; bool reached_success=false;
    for(size_t failure=1;failure<10000;failure++) {
        doc=load(fixture,CE_FORMAT_JSON); allocation=0; fail_at=failure; cJSON_InitHooks(&hooks);
        count=999; bool ok=ce_document_search_replace(doc,"hello","replacement",0,true,&count,&cursor,error,sizeof error);
        cJSON_InitHooks(NULL); fail_at=0;
        if(!ok) { unchanged(doc,before); assert(count==999 && !ce_document_can_undo(doc) && !ce_document_is_modified(doc)); }
        else { assert(count==3 && ce_document_undo(doc)); unchanged(doc,before); reached_success=true; }
        ce_document_free(doc); if(reached_success) break;
    }
    assert(reached_success); free(before);
    puts("document search and transactional replacement tests passed"); return 0;
}
