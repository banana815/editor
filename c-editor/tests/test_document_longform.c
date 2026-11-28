#include "document.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static CeDocument *load(const char *s,CeFormat format) {
    CeDocument *d=ce_document_new(); char error[256];
    assert(d && ce_document_import(d,s,format,error,sizeof error)); return d;
}
static void kind(CeDocument *d,size_t i,CeBlockKind k) {
    CeBlockInfo b; assert(ce_document_block_info(d,i,&b) && b.kind==k);
}
static void same(CeDocument *d,const char *json) {
    char *s=ce_document_export_json(d); assert(s && !strcmp(s,json)); free(s);
}
static void quote_code(void) {
    CeDocument *d=load("hello",CE_FORMAT_TEXT);
    assert(ce_document_set_writing_block(d,0,CE_BLOCK_QUOTE,NULL,0)); kind(d,0,CE_BLOCK_QUOTE);
    assert(ce_document_insert(d,5,"\n")); kind(d,1,CE_BLOCK_QUOTE);
    size_t caret=99; assert(ce_document_exit_writing_block(d,6,&caret) && caret==6); kind(d,1,CE_BLOCK_PARAGRAPH);
    assert(ce_document_undo(d)); kind(d,1,CE_BLOCK_QUOTE);
    assert(ce_document_redo(d)); kind(d,1,CE_BLOCK_PARAGRAPH);
    assert(ce_document_set_writing_block(d,6,CE_BLOCK_CODE,"c++",0));
    assert(ce_document_insert(d,6,"if (x) {\n  y();\n}"));
    assert(ce_document_block_count(d)==2); kind(d,1,CE_BLOCK_CODE);
    assert(ce_document_exit_writing_block(d,ce_document_length(d),&caret)); kind(d,2,CE_BLOCK_PARAGRAPH);
    assert(caret==ce_document_length(d));
    char *html=ce_document_export_html(d); assert(html && strstr(html,"<blockquote>") && strstr(html,"<pre data-language=\"c++\"><code>"));
    CeDocument *round=load(html,CE_FORMAT_HTML); assert(!strcmp(ce_document_text(d),ce_document_text(round))); kind(round,1,CE_BLOCK_CODE);
    free(html); ce_document_free(round); ce_document_free(d);
}
static void rules_lists_atoms(void) {
    CeDocument *d=load("",CE_FORMAT_TEXT);
    assert(ce_document_set_writing_block(d,0,CE_BLOCK_ORDERED,NULL,42));
    assert(ce_document_insert(d,0,"A\nB\n")); assert(ce_document_block_count(d)==3);
    kind(d,2,CE_BLOCK_ORDERED); assert(ce_document_insert(d,4,"\n")); kind(d,2,CE_BLOCK_PARAGRAPH);
    assert(ce_document_set_writing_block(d,4,CE_BLOCK_RULE,NULL,0)); kind(d,2,CE_BLOCK_RULE);
    assert(ce_document_insert(d,7,"\n")); kind(d,3,CE_BLOCK_PARAGRAPH);
    char *html=ce_document_export_html(d); assert(html && strstr(html,"<hr>"));
    CeDocument *round=load(html,CE_FORMAT_HTML); kind(round,2,CE_BLOCK_RULE);
    free(html); ce_document_free(round);
    assert(ce_document_insert_node(d,8,"{\"type\":\"image\",\"attrs\":{\"src\":\"notelab-asset:abc.png\",\"alt\":\"A\"}}"));
    kind(d,3,CE_BLOCK_IMAGE); assert(!ce_document_insert(d,9,"x"));
    char *image_before=ce_document_export_json(d); assert(image_before);
    assert(ce_document_replace_block_json(d,3,"{\"type\":\"image\",\"attrs\":{\"src\":\"notelab-asset:abc.png\",\"alt\":\"Changed\",\"width\":600}}"));
    kind(d,3,CE_BLOCK_IMAGE); assert(ce_document_undo(d)); same(d,image_before); free(image_before);
    ce_document_free(d);
}
static size_t allocations,limit;
static void *limited(size_t size) { return allocations++>=limit?NULL:malloc(size); }
static void atomic_failures(void) {
    const char *source="{\"type\":\"doc\",\"legacy\":{\"exact\":9007199254740991},\"content\":[{\"type\":\"paragraph\",\"attrs\":{\"custom\":\"preserve\"},\"content\":[{\"type\":\"text\",\"text\":\"hello\"}]}]}";
    for(int mode=0;mode<3;mode++) {
        bool reached=false;
        for(limit=0;limit<1500;limit++) {
            CeDocument *d=load(source,CE_FORMAT_JSON); char *before=ce_document_export_json(d); assert(before);
            allocations=0; cJSON_Hooks hooks={limited,free}; cJSON_InitHooks(&hooks);
            bool ok=ce_document_set_writing_block(d,0,mode==0?CE_BLOCK_QUOTE:mode==1?CE_BLOCK_CODE:CE_BLOCK_ORDERED,"c",3);
            cJSON_InitHooks(NULL);
            if(!ok) { same(d,before); assert(!ce_document_can_undo(d) && !ce_document_is_modified(d)); }
            else { assert(ce_document_undo(d)); same(d,before); reached=true; }
            free(before); ce_document_free(d); if(reached) break;
        }
        assert(reached);
    }
    CeDocument *d=load("{\"type\":\"doc\",\"content\":[{\"type\":\"blockquote\",\"attrs\":{\"source\":\"keep\"},\"content\":[{\"type\":\"paragraph\"}]}]}",CE_FORMAT_JSON);
    char *before=ce_document_export_json(d); assert(!ce_document_exit_writing_block(d,0,NULL)); same(d,before);
    free(before); ce_document_free(d);
}
int main(void) { quote_code(); rules_lists_atoms(); atomic_failures(); puts("longform document tests passed"); return 0; }
