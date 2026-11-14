#include "citations.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

static char error[256];
/* cJSON hooks allocate nodes and character buffers; align the prefix for the
 * actual node type without relying on MSVC's unavailable C max_align_t. */
typedef union { cJSON alignment; size_t magic; } JsonAllocation;
static size_t json_live_allocations;
static void *json_custom_allocate(size_t size) {
    JsonAllocation *allocation=malloc(sizeof *allocation+size);
    if (!allocation) return NULL;
    allocation->magic=0xc17a710u; ++json_live_allocations;
    return allocation+1;
}
static void json_custom_free(void *pointer) {
    if (!pointer) return;
    JsonAllocation *allocation=(JsonAllocation *)pointer-1;
    assert(allocation->magic==0xc17a710u && json_live_allocations);
    --json_live_allocations; free(allocation);
}
#define OK(call) do { bool success=(call); if(!success) fprintf(stderr,"%s: %s\n",#call,error); assert(success); } while(0)
static cJSON *get(const cJSON *node,const char *key) { return cJSON_GetObjectItemCaseSensitive(node,key); }
static const char *str(const cJSON *node,const char *key) {
    cJSON *v=get(node,key); assert(cJSON_IsString(v)); return v->valuestring;
}
static void set_string(cJSON *object,const char *key,const char *value) {
    cJSON_DeleteItemFromObjectCaseSensitive(object,key);
    assert(cJSON_AddStringToObject(object,key,value));
}
static cJSON *parse(const char *json) { cJSON *root=cJSON_Parse(json); assert(root); return root; }
static cJSON *document(void) { return parse("{\"type\":\"doc\",\"content\":[]}"); }
static void append(cJSON *root,cJSON *node) { assert(cJSON_AddItemToArray(get(root,"content"),node)); }
static cJSON *citation(const char *id,const char *uri,const char *key) {
    cJSON *node=parse("{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"endnoteNumber\":99,\"title\":\"中文标题\",\"author\":\"作者\",\"year\":\"2024\",\"page\":\"old page\",\"note\":\"old note\"}}");
    cJSON *a=get(node,"attrs");
    if(id) set_string(a,"citationId",id);
    if(uri) set_string(a,"itemUri",uri);
    if(key) set_string(a,"itemKey",key);
    return node;
}
static cJSON *section(const char *entries) {
    cJSON *node=parse("{\"type\":\"zoteroEndnoteSection\",\"attrs\":{\"style\":\"apa\"}}");
    set_string(get(node,"attrs"),"entriesJson",entries); return node;
}
static CeDocument *load(cJSON *root) {
    CeDocument *doc=ce_document_new(); assert(doc);
    char *json=cJSON_PrintUnformatted(root); assert(json);
    OK(ce_document_import(doc,json,CE_FORMAT_JSON,error,sizeof error));
    free(json); cJSON_Delete(root); return doc;
}
static cJSON *read_doc(CeDocument *doc) {
    char *json=ce_document_export_json(doc); assert(json);
    cJSON *root=parse(json); free(json); return root;
}
static cJSON *nth_type_impl(cJSON *root,const char *type,size_t *number) {
    cJSON *t=get(root,"type");
    if(cJSON_IsString(t) && !strcmp(t->valuestring,type)) {
        if(!*number) return root;
        --*number;
    }
    cJSON *child;
    cJSON_ArrayForEach(child,get(root,"content")) {
        cJSON *found=nth_type_impl(child,type,number); if(found) return found;
    }
    return NULL;
}
static cJSON *nth(cJSON *root,const char *type,size_t number) { return nth_type_impl(root,type,&number); }
static cJSON *attrs(cJSON *root,size_t number) {
    cJSON *node=nth(root,"zoteroCitationBlock",number); assert(node); return get(node,"attrs");
}
static cJSON *entries(cJSON *root) {
    cJSON *node=nth(root,"zoteroEndnoteSection",0); assert(node);
    cJSON *result=parse(str(get(node,"attrs"),"entriesJson")); assert(cJSON_IsArray(result)); return result;
}
static void same_document(CeDocument *doc,const char *expected) {
    char *actual=ce_document_export_json(doc); assert(actual && !strcmp(actual,expected)); free(actual);
}
static void single_undo(CeDocument *doc,const char *before) {
    char *after=ce_document_export_json(doc); assert(after && strcmp(before,after));
    assert(ce_document_is_modified(doc)); assert(ce_document_undo(doc));
    same_document(doc,before); assert(!ce_document_is_modified(doc));
    assert(!ce_document_can_undo(doc)); assert(ce_document_redo(doc)); same_document(doc,after); free(after);
}
static void test_insert_and_update(void) {
    CeDocument *doc=ce_document_new(); assert(doc);
    OK(ce_document_import(doc,"<p><strong>你好</strong></p>",CE_FORMAT_HTML,error,sizeof error));
    char *before=ce_document_export_json(doc),*id=NULL;
    CeZoteroItem item={0}; item.item_key="ABCD1234"; item.item_uri="zotero://select/groups/7/items/ABCD1234";
    item.pdf_uri="zotero://open-pdf/groups/7/items/PDF12345"; item.title="中文 <script>&\"'";
    item.author="作者 & 合著者"; item.year="2025"; item.cite_key="author2025";
    item.csl_json="{\"id\":\"保留\",\"title\":\"原始 <&>\",\"custom\":{\"nested\":[1,2,3]}}";
    OK(ce_citation_insert(doc,strlen("你"),&item,"12<13","摘录 \"x\" & y",&id,error,sizeof error));
    assert(id && *id); assert(!strcmp(ce_document_text(doc),"你\n\xef\xbf\xbc\n好"));
    assert(ce_document_style_at(doc,0).marks & CE_MARK_BOLD);
    assert(ce_document_style_at(doc,strlen("你\n\xef\xbf\xbc\n")).marks & CE_MARK_BOLD);
    cJSON *root=read_doc(doc),*a=attrs(root,0);
    assert(!strcmp(str(a,"citationId"),id)); assert(!strcmp(str(a,"title"),item.title));
    assert(!strcmp(str(a,"cslJson"),item.csl_json));
    assert(!strcmp(str(a,"itemUri"),item.item_uri)); assert(!strcmp(str(a,"pdfUri"),item.pdf_uri));
    assert(!strcmp(str(a,"page"),"12<13")); assert(!strcmp(str(a,"note"),"摘录 \"x\" & y"));
    assert(get(a,"endnoteNumber")->valueint==0); cJSON_Delete(root);
    char *html=ce_document_export_html(doc); assert(html && strstr(html,"&lt;script&gt;") && strstr(html,"&amp;")); free(html);
    single_undo(doc,before); free(before);
    /* Updating the quote is separate from explicit bibliography synchronization. */
    ce_document_mark_saved(doc); before=ce_document_export_json(doc);
    OK(ce_citation_update(doc,id,NULL,"新摘录 <&>",error,sizeof error));
    root=read_doc(doc); a=attrs(root,0);
    assert(!strcmp(str(a,"page"),"12<13")); assert(!strcmp(str(a,"note"),"新摘录 <&>")); cJSON_Delete(root);
    assert(ce_document_undo(doc)); same_document(doc,before); assert(!ce_document_is_modified(doc));
    assert(ce_document_can_undo(doc)); assert(ce_document_redo(doc));
    OK(ce_citation_update(doc,id,"","",error,sizeof error));
    root=read_doc(doc); a=attrs(root,0);
    assert(!*str(a,"page") && !*str(a,"note")); cJSON_Delete(root);
    free(before); free(id); ce_document_free(doc);
}
static void test_insert_validation_and_history(void) {
    CeDocument *doc=ce_document_new(); assert(doc);
    OK(ce_document_import(doc,"中文",CE_FORMAT_TEXT,error,sizeof error));
    assert(ce_document_insert(doc,0,"A")); ce_document_mark_saved(doc);
    char *before=ce_document_export_json(doc); char sentinel[]="unchanged"; char *id=sentinel;
    CeZoteroItem item={0}; item.title="Valid";
    assert(!ce_citation_insert(doc,2,&item,"","",&id,error,sizeof error));
    assert(*error && id==sentinel); same_document(doc,before); assert(!ce_document_is_modified(doc));
    item.title="bad\xff";
    assert(!ce_citation_insert(doc,1,&item,"","",&id,error,sizeof error));
    assert(*error && id==sentinel); same_document(doc,before);
    item.title="Valid";
    OK(ce_citation_insert(doc,1,&item,"","",&id,error,sizeof error)); assert(id!=sentinel);
    assert(ce_document_undo(doc)); same_document(doc,before); assert(!ce_document_is_modified(doc));
    assert(ce_document_undo(doc)); assert(!strcmp(ce_document_text(doc),"中文"));
    assert(ce_document_redo(doc)); same_document(doc,before); assert(ce_document_redo(doc));
    free(id); free(before); ce_document_free(doc);

    cJSON *root=document(); append(root,citation("cite-c-1","uri:first","KEY")); doc=load(root); id=NULL;
    OK(ce_citation_insert(doc,ce_document_length(doc),&item,NULL,NULL,&id,error,sizeof error));
    assert(strcmp(id,"cite-c-1")); free(id); ce_document_free(doc);
}
static void test_sync_numbering_identity_and_repairs(void) {
    cJSON *root=document();
    append(root,citation("dup","zotero://select/library/items/SAMEKEY","SAMEKEY"));
    cJSON *nested=parse("{\"type\":\"bulletList\",\"content\":[{\"type\":\"listItem\",\"content\":[]}]}");
    cJSON *children=get(cJSON_GetArrayItem(get(nested,"content"),0),"content");
    assert(cJSON_AddItemToArray(children,citation(NULL,"zotero://select/library/items/SAMEKEY","SAMEKEY")));
    append(root,nested);
    append(root,citation("dup","zotero://select/groups/7/items/SAMEKEY","SAMEKEY"));
    append(root,citation("cite-c-1","zotero://select/groups/8/items/SAMEKEY","SAMEKEY"));
    append(root,citation("plain",NULL,"SAMEKEY"));
    cJSON *node=citation("sourceA",NULL,"SAMEKEY"); set_string(get(node,"attrs"),"sourceId","source:7/A"); append(root,node);
    node=citation("sourceA2",NULL,"DIFFERENTKEY"); set_string(get(node,"attrs"),"sourceId","source:7/A"); append(root,node);
    node=citation("sourceB",NULL,"SAMEKEY"); set_string(get(node,"attrs"),"sourceId","source:8/A"); append(root,node);
    node=citation("library7",NULL,"SAMEKEY"); assert(cJSON_AddNumberToObject(get(node,"attrs"),"libraryId",7)); append(root,node);
    node=citation("library8",NULL,"SAMEKEY"); assert(cJSON_AddNumberToObject(get(node,"attrs"),"libraryId",8)); append(root,node);
    node=citation("library7again",NULL,"SAMEKEY"); assert(cJSON_AddNumberToObject(get(node,"attrs"),"libraryId",7)); append(root,node);
    append(root,citation(NULL,NULL,NULL)); append(root,citation(NULL,NULL,NULL));
    CeDocument *doc=load(root); char *before=ce_document_export_json(doc); CeCitationSyncResult stats={0};
    OK(ce_citations_sync_approximate(doc,CE_ZOTERO_APPROXIMATE_APA,&stats,error,sizeof error));
    assert(stats.citation_count==13 && stats.reference_count==10 && stats.repaired_ids==4);
    root=read_doc(doc); const int numbers[]={1,1,2,3,4,5,5,6,7,8,7,9,10};
    for(size_t i=0;i<sizeof numbers/sizeof *numbers;i++) {
        cJSON *a=attrs(root,i); assert(get(a,"endnoteNumber")->valueint==numbers[i]);
        assert(*str(a,"citationId"));
        for(size_t j=0;j<i;j++) assert(strcmp(str(a,"citationId"),str(attrs(root,j),"citationId")));
    }
    assert(!strcmp(str(attrs(root,3),"citationId"),"cite-c-1"));
    cJSON *list=entries(root); assert(cJSON_GetArraySize(list)==10);
    for(int i=0;i<10;i++) {
        cJSON *entry=cJSON_GetArrayItem(list,i);
        assert(get(entry,"number")->valueint==i+1);
        assert(!strcmp(str(entry,"formatting"),"approximate"));
        assert(strstr(str(entry,"entryHtml"),"中文标题"));
    }
    cJSON_Delete(list); cJSON_Delete(root); single_undo(doc,before); free(before);
    /* Synchronizing an unchanged bibliography is a no-op in the undo history. */
    ce_document_mark_saved(doc); before=ce_document_export_json(doc);
    OK(ce_citations_sync_approximate(doc,CE_ZOTERO_APPROXIMATE_APA,&stats,error,sizeof error));
    same_document(doc,before); assert(!ce_document_is_modified(doc));
    assert(ce_document_undo(doc)); assert(!nth((root=read_doc(doc)),"zoteroEndnoteSection",0));
    cJSON_Delete(root); free(before); ce_document_free(doc);
}
static void test_known_source_metadata(void) {
    CeDocument *doc=load(document()); CeZoteroItem item={0};
    item.item_key="SAMEKEY"; item.title="Book"; item.library_id=INT64_C(9007199254740993);
    OK(ce_citation_insert(doc,0,&item,NULL,NULL,NULL,error,sizeof error));
    item.library_id=INT64_C(9007199254740994);
    OK(ce_citation_insert(doc,ce_document_length(doc),&item,NULL,NULL,NULL,error,sizeof error));
    item.id="specific source 中文"; item.library_id=0;
    OK(ce_citation_insert(doc,ce_document_length(doc),&item,NULL,NULL,NULL,error,sizeof error));
    cJSON *root=read_doc(doc);
    assert(!strcmp(str(attrs(root,0),"libraryId"),"9007199254740993"));
    assert(!strcmp(str(attrs(root,1),"libraryId"),"9007199254740994"));
    assert(!get(attrs(root,0),"sourceId")); assert(!get(attrs(root,2),"libraryId"));
    assert(!strcmp(str(attrs(root,2),"sourceId"),item.id)); cJSON_Delete(root);
    item.id="42"; item.library_id=7;
    OK(ce_citation_insert(doc,ce_document_length(doc),&item,NULL,NULL,NULL,error,sizeof error));
    item.library_id=8;
    OK(ce_citation_insert(doc,ce_document_length(doc),&item,NULL,NULL,NULL,error,sizeof error));
    item.id="43";
    OK(ce_citation_insert(doc,ce_document_length(doc),&item,NULL,NULL,NULL,error,sizeof error));
    item.item_key="OTHERKEY";
    OK(ce_citation_insert(doc,ce_document_length(doc),&item,NULL,NULL,NULL,error,sizeof error));
    CeCitationSyncResult stats={0};
    OK(ce_citations_sync_approximate(doc,CE_ZOTERO_APPROXIMATE_APA,&stats,error,sizeof error));
    assert(stats.reference_count==6); /* Different source IDs within one library stay distinct. */
    root=read_doc(doc); assert(get(attrs(root,5),"endnoteNumber")->valueint==6);
    assert(get(attrs(root,6),"endnoteNumber")->valueint==6); cJSON_Delete(root);
    char *html=ce_document_export_html(doc); assert(html);
    CeDocument *roundtrip=ce_document_new(); assert(roundtrip);
    OK(ce_document_import(roundtrip,html,CE_FORMAT_HTML,error,sizeof error));
    root=read_doc(roundtrip); assert(!strcmp(str(attrs(root,0),"libraryId"),"9007199254740993"));
    assert(!strcmp(str(attrs(root,3),"sourceId"),"42"));
    assert(!get(attrs(root,0),"sourceId") && !get(attrs(root,2),"libraryId"));
    cJSON_Delete(root); free(html); ce_document_free(roundtrip); ce_document_free(doc);
}
static void test_metadata_preservation_and_escaping(void) {
    cJSON *root=document(),*node=citation("one","uri:library/one","KEY");
    cJSON *a=get(node,"attrs");
    set_string(a,"title","中文 <script>alert(\"x\")</script> & 'quoted'");
    set_string(a,"author","<b>作者</b>"); set_string(a,"customCitation","untouched"); append(root,node);
    append(root,parse("{\"type\":\"futureBlock\",\"attrs\":{\"opaque\":{\"k\":[1,2,3]}},\"futurePayload\":\"保留\"}"));
    node=section("[{\"citationId\":\"one\",\"itemUri\":\"uri:wrong-library\",\"wrongMetadata\":true},{\"citationId\":\"old\",\"itemUri\":\"uri:library/one\",\"customEntry\":{\"notes\":\"保留\"}}]");
    set_string(get(node,"attrs"),"customSection","保留尾注设置");
    assert(cJSON_AddNumberToObject(node,"futureVersion",2)); append(root,node);
    CeDocument *doc=load(root); char *before=ce_document_export_json(doc); CeCitationSyncResult stats={0};
    OK(ce_citation_update(doc,"one","新页码","新摘录 <>&",error,sizeof error));
    root=read_doc(doc); assert(!strcmp(str(attrs(root,0),"customCitation"),"untouched"));
    assert(nth(root,"futureBlock",0)); cJSON_Delete(root);
    assert(ce_document_undo(doc)); same_document(doc,before);
    OK(ce_citations_sync_approximate(doc,CE_ZOTERO_APPROXIMATE_APA,&stats,error,sizeof error));
    root=read_doc(doc); cJSON *list=entries(root),*entry=cJSON_GetArrayItem(list,0);
    assert(!strcmp(str(get(entry,"customEntry"),"notes"),"保留")); assert(!get(entry,"wrongMetadata"));
    const char *html=str(entry,"entryHtml");
    assert(strstr(html,"&lt;script&gt;") && strstr(html,"&amp;") && strstr(html,"&quot;") && strstr(html,"&#39;"));
    assert(strstr(html,"&lt;b&gt;作者&lt;/b&gt;") && !strchr(html,'<'));
    assert(!strcmp(str(entry,"formatting"),"approximate"));
    node=nth(root,"zoteroEndnoteSection",0);
    assert(get(node,"futureVersion")->valueint==2);
    assert(!strcmp(str(get(node,"attrs"),"customSection"),"保留尾注设置"));
    assert(!strcmp(str(nth(root,"futureBlock",0),"futurePayload"),"保留"));
    assert(cJSON_GetArraySize(get(get(get(nth(root,"futureBlock",0),"attrs"),"opaque"),"k"))==3);
    assert(!strcmp(str(attrs(root,0),"customCitation"),"untouched"));
    cJSON_Delete(list); cJSON_Delete(root); single_undo(doc,before); free(before); ce_document_free(doc);

    /* Legacy HTML round-trip carries escaped plain formatter output, never raw HTML. */
    root=document(); node=citation("safe","zotero://select/library/items/SAFE","SAFE");
    set_string(get(node,"attrs"),"title","中文 <script>&\"'"); append(root,node); doc=load(root);
    OK(ce_citations_sync_approximate(doc,CE_ZOTERO_APPROXIMATE_APA,NULL,error,sizeof error));
    char *exported=ce_document_export_html(doc); assert(exported && !strstr(exported,"<script>"));
    CeDocument *roundtrip=ce_document_new(); assert(roundtrip);
    OK(ce_document_import(roundtrip,exported,CE_FORMAT_HTML,error,sizeof error));
    root=read_doc(roundtrip); list=entries(root); entry=cJSON_GetArrayItem(list,0);
    assert(strstr(str(entry,"entryHtml"),"中文 &lt;script&gt;&amp;&quot;&#39;"));
    cJSON_Delete(list); cJSON_Delete(root); free(exported); ce_document_free(roundtrip); ce_document_free(doc);
}
static void test_remove_old_sections_without_citations(void) {
    cJSON *root=document();
    append(root,parse("{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"正文\"}]}"));
    cJSON *node=section("malformed old content"); set_string(get(node,"attrs"),"custom","delete explicitly"); append(root,node);
    cJSON *container=parse("{\"type\":\"futureContainer\",\"attrs\":{\"opaque\":\"keep\"},\"content\":[]}");
    append(container,section("[]")); append(container,parse("{\"type\":\"futureAtom\",\"attrs\":{\"v\":1}}")); append(root,container);
    CeDocument *doc=load(root); char *before=ce_document_export_json(doc); CeCitationSyncResult stats={0};
    OK(ce_citations_sync_approximate(doc,CE_ZOTERO_APPROXIMATE_APA,&stats,error,sizeof error));
    assert(!stats.citation_count && !stats.reference_count && stats.removed_sections==2);
    root=read_doc(doc); assert(!nth(root,"zoteroEndnoteSection",0));
    assert(nth(root,"futureAtom",0)); assert(!strcmp(str(get(nth(root,"futureContainer",0),"attrs"),"opaque"),"keep"));
    cJSON_Delete(root); single_undo(doc,before); free(before); ce_document_free(doc);
}
static void test_sync_reorder_styles_and_extra_sections(void) {
    cJSON *root=document(); append(root,citation("A","uri:A","A")); append(root,citation("B","uri:B","B"));
    append(root,citation("A2","uri:A","A")); append(root,section("[]")); append(root,section("[]"));
    CeDocument *doc=load(root); CeCitationSyncResult stats={0};
    OK(ce_citations_sync_approximate(doc,CE_ZOTERO_APPROXIMATE_IEEE,&stats,error,sizeof error));
    assert(stats.removed_sections==1 && stats.reference_count==2);
    root=read_doc(doc); assert(!nth(root,"zoteroEndnoteSection",1));
    cJSON *list=entries(root); assert(strstr(str(cJSON_GetArrayItem(list,0),"entryHtml"),"[1]"));
    cJSON_Delete(list); cJSON_Delete(root);
    assert(ce_document_move_block(doc,0,2));
    ce_document_mark_saved(doc); char *before=ce_document_export_json(doc);
    OK(ce_citations_sync_approximate(doc,CE_ZOTERO_APPROXIMATE_GBT_7714,&stats,error,sizeof error));
    root=read_doc(doc); list=entries(root);
    assert(!strcmp(str(attrs(root,0),"itemUri"),"uri:B"));
    assert(get(attrs(root,0),"endnoteNumber")->valueint==1);
    assert(get(attrs(root,1),"endnoteNumber")->valueint==2 && get(attrs(root,2),"endnoteNumber")->valueint==2);
    assert(!strcmp(str(cJSON_GetArrayItem(list,0),"key"),"B"));
    assert(!strcmp(str(get(nth(root,"zoteroEndnoteSection",0),"attrs"),"style"),"gb-t-7714"));
    cJSON_Delete(list); cJSON_Delete(root); assert(ce_document_undo(doc)); same_document(doc,before);
    assert(!ce_document_is_modified(doc)); free(before); ce_document_free(doc);
}
static void assert_sync_failure(CeDocument *doc,CeZoteroApproximateStyle style) {
    char *before=ce_document_export_json(doc); CeCitationSyncResult result={9,8,7,6};
    assert(!ce_citations_sync_approximate(doc,style,&result,error,sizeof error));
    assert(*error && result.citation_count==9 && result.reference_count==8 && result.repaired_ids==7 && result.removed_sections==6);
    same_document(doc,before); assert(!ce_document_is_modified(doc)); assert(!ce_document_can_undo(doc)); free(before);
}
static void test_failures_are_atomic(void) {
    cJSON *root=document(); append(root,citation("duplicate","uri:A","A")); append(root,citation("duplicate","uri:B","B"));
    CeDocument *doc=load(root); char *before=ce_document_export_json(doc);
    assert(!ce_citation_update(doc,"duplicate","42","changed",error,sizeof error)); assert(*error); same_document(doc,before);
    assert(!ce_citation_update(doc,"missing","42",NULL,error,sizeof error)); assert(*error); same_document(doc,before);
    assert_sync_failure(doc,(CeZoteroApproximateStyle)999); free(before); ce_document_free(doc);

    root=document(); append(root,citation("unique","uri:A","A")); doc=load(root); before=ce_document_export_json(doc);
    assert(!ce_citation_update(doc,"unique","bad\xff",NULL,error,sizeof error)); assert(*error); same_document(doc,before);
    assert(!ce_document_can_undo(doc));
    OK(ce_citation_update(doc,"unique",NULL,NULL,error,sizeof error)); same_document(doc,before); assert(!ce_document_can_undo(doc));
    free(before); ce_document_free(doc);

    const char *bad_entries[]={"not json","{}","[null]","[123]"};
    for(size_t i=0;i<sizeof bad_entries/sizeof *bad_entries;i++) {
        root=document(); append(root,citation(NULL,"uri:A","A")); append(root,section(bad_entries[i]));
        doc=load(root); assert_sync_failure(doc,CE_ZOTERO_APPROXIMATE_APA); ce_document_free(doc);
    }
    root=document(); append(root,citation(NULL,"uri:A","A"));
    cJSON *node=section("[]"); cJSON_DeleteItemFromObjectCaseSensitive(get(node,"attrs"),"entriesJson");
    assert(cJSON_AddNumberToObject(get(node,"attrs"),"entriesJson",23)); append(root,node);
    doc=load(root); assert_sync_failure(doc,CE_ZOTERO_APPROXIMATE_APA); ce_document_free(doc);

    for(int custom=0;custom<2;custom++) {
        root=document(); append(root,citation(NULL,"uri:A","A")); append(root,section("[]"));
        node=section(custom?"[{\"customEntry\":\"must not silently disappear\"}]":"[]");
        if(!custom) set_string(get(node,"attrs"),"customSection","keep");
        append(root,node); doc=load(root); assert_sync_failure(doc,CE_ZOTERO_APPROXIMATE_APA); ce_document_free(doc);
    }
}
static void test_json_allocator_contract(void) {
    cJSON_Hooks hooks={json_custom_allocate,json_custom_free};
    cJSON_InitHooks(&hooks);
    CeDocument *doc=ce_document_new(); assert(doc);
    OK(ce_document_import(doc,"中文正文",CE_FORMAT_TEXT,error,sizeof error));
    CeZoteroItem item={0}; item.id="source"; item.title="文献"; item.author="作者";
    char *id=NULL;
    OK(ce_citation_insert(doc,6,&item,"9","摘录",&id,error,sizeof error));
    char *json=ce_document_export_json(doc); assert(json); free(json);
    char *block=ce_document_block_json(doc,1); assert(block); free(block);
    OK(ce_citation_update(doc,id,"10",NULL,error,sizeof error));
    OK(ce_citations_sync_approximate(doc,CE_ZOTERO_APPROXIMATE_APA,NULL,error,sizeof error));
    assert(ce_document_undo(doc) && ce_document_redo(doc));
    ce_document_mark_saved(doc); assert(!ce_document_is_modified(doc));
    char *html=ce_document_export_html(doc); assert(html); free(html);
    free(id); ce_document_free(doc);
    assert(!json_live_allocations);
    cJSON_InitHooks(NULL);
}
int main(void) {
    test_insert_and_update(); test_insert_validation_and_history();
    test_sync_numbering_identity_and_repairs(); test_known_source_metadata();
    test_metadata_preservation_and_escaping(); test_remove_old_sections_without_citations();
    test_sync_reorder_styles_and_extra_sections(); test_failures_are_atomic();
    test_json_allocator_contract();
    puts("citation insertion, editing and explicit approximate bibliography tests passed"); return 0;
}
