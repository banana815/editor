#include "bibliography_selection.h"
#include "citation_session.h"
#include "citations.h"
#include "csl_json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The helper and strict JSON/document allocations fail together. Do not also
 * compile bibliography_selection.c into this executable. */
static size_t calls,fail_at,live;
static void *allocations[32768];
static void *fault_malloc(size_t size) {
    if(++calls>=fail_at && fail_at) return NULL;
    void *p=malloc(size); if(p) { assert(live<32768); allocations[live++]=p; } return p;
}
static void fault_free(void *p) {
    if(!p) return;
    for(size_t i=0;i<live;i++) if(allocations[i]==p) { allocations[i]=allocations[--live]; break; }
    free(p);
}
static void *fault_calloc(size_t n,size_t size) { assert(!n || size<=SIZE_MAX/n); void *p=fault_malloc(n*size); if(p) memset(p,0,n*size); return p; }
static void *fault_realloc(void *p,size_t size) {
    if(!p) return fault_malloc(size);
    if(++calls>=fail_at && fail_at) return NULL;
    size_t i=0; while(i<live && allocations[i]!=p) ++i; assert(i<live);
    void *next=realloc(p,size); if(next) allocations[i]=next; return next;
}
#define malloc fault_malloc
#define calloc fault_calloc
#define realloc fault_realloc
#define free fault_free
#include "../src/bibliography_selection.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
static char err[512];
static CeZoteroItem item_a={.library_id=1,.id="a",.item_key="A",.item_uri="http://zotero.org/users/local/items/A",
    .title="Archive book",.author="Adams",.year="2020",.cite_key="Adams2020",
    .csl_json="{\"id\":\"a\",\"type\":\"book\",\"title\":\"Archive book\",\"author\":[{\"family\":\"Adams\",\"given\":\"Alice\"}],\"issued\":{\"date-parts\":[[2020]]},\"publisher\":\"Press\"}"};
static CeZoteroItem item_b={.library_id=9223372036854775807LL,.id="b",.item_key="B",.item_uri="http://zotero.org/users/local/items/B",
    .title="Second book",.author="Brown",.year="2021",.cite_key="Brown2021",
    .csl_json="{\"id\":\"b\",\"type\":\"book\",\"title\":\"Second book\",\"author\":[{\"family\":\"Brown\",\"given\":\"Bob\"}],\"issued\":{\"date-parts\":[[2021]]},\"publisher\":\"Press\"}"};
static CeDocument *plain(void) { CeDocument *d=ce_document_new(); assert(d && ce_document_import(d,"Prose",CE_FORMAT_TEXT,err,sizeof err)); return d; }
static cJSON *tree(const CeDocument *d) {
    char *json=ce_document_export_json(d); cJSON *root=NULL;
    assert(json && ce_csl_json_parse_strict(json,strlen(json),&root,err,sizeof err)); free(json); return root;
}
static CeDocument *clone(const CeDocument *d) {
    char *json=ce_document_export_json(d); CeDocument *next=ce_document_new();
    assert(json && next && ce_document_import(next,json,CE_FORMAT_JSON,err,sizeof err)); free(json); return next;
}
static void replace_tree(CeDocument *d,cJSON *root) {
    char *json=ce_csl_json_print_exact(root); assert(json && ce_document_apply_json(d,json)); cJSON_free(json);
}
static cJSON *section(cJSON *root) {
    cJSON *content=cJSON_GetObjectItemCaseSensitive(root,"content");
    for(cJSON *n=content?content->child:NULL;n;n=n->next) if(!strcmp(str(n,"type"),"zoteroEndnoteSection")) return n;
    return NULL;
}
static cJSON *selected_sources(cJSON *root) {
    const char *json=str(get(section(root),"attrs"),"selectedItemsJson"); cJSON *out=NULL;
    assert(ce_csl_json_parse_strict(json,strlen(json),&out,err,sizeof err)); return out;
}
static void prepared(CeDocument *doc,CeCitationProfile profile,size_t cites,size_t refs,size_t selected,bool intent) {
    CeCitationStyle *style=NULL; CeCitationPrepared *p=NULL;
    assert(ce_citation_style_load(profile,NULL,NULL,&style,err,sizeof err));
    if(!ce_citations_prepare(doc,style,NULL,NULL,&p,err,sizeof err)) { fprintf(stderr,"prepare: %s\n",err); abort(); }
    const CeCitationProcessStats *stats=ce_citation_prepared_stats(p);
    assert(stats->citation_count==cites && stats->reference_count==refs && stats->selected_count==selected && stats->selected_bibliography==intent);
    assert(ce_document_apply_json(doc,ce_citation_prepared_json(p)));
    char digest[65]; assert(ce_citations_source_digest(doc,style,NULL,digest,err,sizeof err));
    assert(!strcmp(digest,ce_citation_prepared_digest(p)));
    const CeCitationCache *cache=ce_citation_prepared_bibliography(p); assert(cache && ce_citation_cache_entry_count(cache)==refs);
    for(size_t i=0;i<refs;i++) {
        const CeCitationCacheEntry *e=ce_citation_cache_entry(cache,i); assert(e && e->visible && e->row_count);
        bool found=false;
        for(size_t j=0;j<e->row_count;j++) {
            CeCslOutputString s={0}; assert(ce_csl_output_plain(e->rows[j].body,&s,err,sizeof err));
            found|=strstr(s.data,"book")!=NULL; ce_csl_output_string_free(&s);
        }
        assert(found);
    }
    ce_citation_prepared_free(p); ce_citation_style_free(style);
}
static void test_profiles(void) {
    const CeZoteroItem *items[]={&item_a,&item_b};
    for(size_t i=0;i<3;i++) {
        CeDocument *doc=plain(); assert(ce_bibliography_select(doc,items,2,err,sizeof err));
        cJSON *root=tree(doc),*sources=selected_sources(root); assert(cJSON_GetArraySize(sources)==2);
        assert(!strcmp(str(cJSON_GetArrayItem(sources,1),"libraryId"),"9223372036854775807"));
        cJSON_Delete(sources); cJSON_Delete(root);
        prepared(doc,(CeCitationProfile)i,0,2,2,true);
        root=tree(doc); assert(!strcmp(str(get(section(root),"attrs"),"bibliographyMode"),"selected"));
        cJSON_Delete(root); ce_document_free(doc);
        doc=plain(); assert(ce_bibliography_select(doc,NULL,0,err,sizeof err)); prepared(doc,(CeCitationProfile)i,0,0,0,true); ce_document_free(doc);
    }
}
static void test_session(void) {
    const CeZoteroItem *items[]={&item_a}; CeDocument *live_doc=plain(),*candidate=clone(live_doc); CeCitationSession *session=NULL;
    assert(ce_citation_session_build(live_doc,NULL,NULL,&session,err,sizeof err));
    assert(ce_bibliography_select(candidate,items,1,err,sizeof err));
    assert(ce_citation_session_apply(&session,live_doc,candidate,NULL,NULL,true,err,sizeof err)); ce_document_free(candidate);
    assert(ce_citation_prepared_stats(ce_citation_session_prepared(session))->reference_count==1);
    assert(ce_document_undo(live_doc)); cJSON *root=tree(live_doc); assert(!section(root)); cJSON_Delete(root); assert(ce_document_redo(live_doc));
    candidate=clone(live_doc); CeCitationProfile ieee=CE_CITATION_PROFILE_IEEE;
    assert(ce_citation_session_apply(&session,live_doc,candidate,NULL,&ieee,true,err,sizeof err)); ce_document_free(candidate);
    assert(ce_document_undo(live_doc)); root=tree(live_doc); assert(!strcmp(str(get(root,"attrs"),"citationProfile"),"apa")); cJSON_Delete(root);
    assert(ce_citation_session_build(live_doc,NULL,NULL,&session,err,sizeof err));
    candidate=clone(live_doc); root=tree(candidate);
    cJSON *paragraph=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root,"content"),0);
    cJSON *text_node=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(paragraph,"content"),0);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(text_node,"text",cJSON_CreateString("Changed prose")));
    replace_tree(candidate,root); cJSON_Delete(root); const CeCitationPrepared *old=ce_citation_session_prepared(session);
    assert(ce_citation_session_apply(&session,live_doc,candidate,NULL,NULL,false,err,sizeof err));
    assert(ce_citation_session_prepared(session)==old); ce_document_free(candidate);
    candidate=clone(live_doc); assert(ce_citation_insert(candidate,0,&item_b,"12","quote",NULL,err,sizeof err));
    assert(ce_citation_session_apply(&session,live_doc,candidate,NULL,NULL,true,err,sizeof err)); ce_document_free(candidate);
    const CeCitationProcessStats *stats=ce_citation_prepared_stats(ce_citation_session_prepared(session)); assert(stats->citation_count==1 && stats->reference_count==2);
    candidate=clone(live_doc); root=tree(candidate); cJSON *content=cJSON_GetObjectItemCaseSensitive(root,"content");
    for(int i=cJSON_GetArraySize(content)-1;i>=0;i--) if(!strcmp(str(cJSON_GetArrayItem(content,i),"type"),"zoteroCitationBlock")) cJSON_DeleteItemFromArray(content,i);
    replace_tree(candidate,root); cJSON_Delete(root);
    assert(ce_citation_session_apply(&session,live_doc,candidate,NULL,NULL,true,err,sizeof err)); ce_document_free(candidate);
    stats=ce_citation_prepared_stats(ce_citation_session_prepared(session)); assert(!stats->citation_count && stats->reference_count==1 && stats->selected_count==1);
    ce_citation_session_free(session); ce_document_free(live_doc);
}
static void test_metadata_and_copy(void) {
    const CeZoteroItem *items[]={&item_a,&item_b}; CeDocument *doc=plain(); assert(ce_bibliography_select(doc,items,2,err,sizeof err));
    prepared(doc,CE_CITATION_PROFILE_APA,0,2,2,true);
    cJSON *root=tree(doc),*sources=selected_sources(root),*attrs=cJSON_GetObjectItemCaseSensitive(section(root),"attrs");
    assert(cJSON_AddNumberToObject(cJSON_GetArrayItem(sources,0),"custom",4503599627370501.0));
    char *json=ce_csl_json_print_exact(sources); assert(json && cJSON_ReplaceItemInObjectCaseSensitive(attrs,"selectedItemsJson",cJSON_CreateString(json)));
    cJSON_free(json); cJSON_Delete(sources); assert(cJSON_AddStringToObject(attrs,"customSection","keep")); replace_tree(doc,root); cJSON_Delete(root);
    assert(ce_bibliography_select(doc,items,1,err,sizeof err)); prepared(doc,CE_CITATION_PROFILE_APA,0,1,1,true);
    root=tree(doc); sources=selected_sources(root); assert(cJSON_GetArraySize(sources)==1);
    assert(get(cJSON_GetArrayItem(sources,0),"custom")->valuedouble==4503599627370501.0);
    assert(!strcmp(str(get(section(root),"attrs"),"customSection"),"keep")); cJSON_Delete(sources);
    assert(cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root,"content"),cJSON_Duplicate(section(root),true)));
    replace_tree(doc,root); cJSON_Delete(root); prepared(doc,CE_CITATION_PROFILE_APA,0,1,1,true);
    root=tree(doc); assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root,"content"))==2); cJSON_Delete(root);
    ce_document_free(doc);
}
static void test_reject_and_empty(void) {
    CeDocument *doc=plain(); const CeZoteroItem *items[]={&item_a}; char *before=ce_document_export_json(doc);
    assert(!ce_bibliography_select(doc,items,CE_CSL_INPUT_MAX_ITEMS+1,err,sizeof err));
    CeZoteroItem missing=item_a; missing.csl_json=""; const CeZoteroItem *bad[]={&missing};
    assert(!ce_bibliography_select(doc,bad,1,err,sizeof err)); char *after=ce_document_export_json(doc); assert(!strcmp(before,after)); free(before); free(after);
    assert(ce_citation_insert(doc,0,&item_b,NULL,NULL,NULL,err,sizeof err));
    assert(ce_bibliography_select(doc,items,1,err,sizeof err)); prepared(doc,CE_CITATION_PROFILE_APA,1,2,1,true);
    assert(ce_bibliography_select(doc,NULL,0,err,sizeof err)); prepared(doc,CE_CITATION_PROFILE_APA,1,1,0,true); ce_document_free(doc);
    doc=plain(); cJSON *root=tree(doc),*node=cJSON_CreateObject(),*attrs=cJSON_CreateObject();
    assert(cJSON_AddStringToObject(node,"type","zoteroEndnoteSection") && cJSON_AddItemToObject(node,"attrs",attrs));
    assert(cJSON_AddStringToObject(attrs,"entriesJson","[{\"entryHtml\":\"Legacy standalone\"}]"));
    assert(cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root,"content"),node)); replace_tree(doc,root); cJSON_Delete(root);
    before=ce_document_export_json(doc); assert(!ce_bibliography_select(doc,items,1,err,sizeof err));
    after=ce_document_export_json(doc); assert(!strcmp(before,after)); free(before); free(after); ce_document_free(doc);
}
static void test_oom(bool existing) {
    const CeZoteroItem *items[]={&item_a,&item_b}; CeDocument *base=plain();
    if(existing) {
        assert(ce_bibliography_select(base,items,2,err,sizeof err)); prepared(base,CE_CITATION_PROFILE_IEEE,0,2,2,true);
        cJSON *root=tree(base),*sources=selected_sources(root),*attrs=cJSON_GetObjectItemCaseSensitive(section(root),"attrs");
        assert(cJSON_AddStringToObject(sources->child,"customSource","preserve selected source"));
        char *json=ce_csl_json_print_exact(sources); assert(json && cJSON_ReplaceItemInObjectCaseSensitive(attrs,"selectedItemsJson",cJSON_CreateString(json)));
        cJSON_free(json); cJSON_Delete(sources);
        assert(cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root,"content"),cJSON_Duplicate(section(root),true)));
        replace_tree(base,root); cJSON_Delete(root);
    }
    CeDocument *doc=clone(base);
    cJSON_Hooks hooks={fault_malloc,fault_free}; cJSON_InitHooks(&hooks); calls=0;
    assert(ce_bibliography_select(doc,items,2,err,sizeof err)); size_t points=calls;
    ce_document_free(doc); assert(!live); cJSON_InitHooks(NULL);
    for(size_t point=1;point<=points;point++) {
        doc=clone(base); char *before=ce_document_export_json(doc); bool can_undo=ce_document_can_undo(doc);
        cJSON_InitHooks(&hooks); calls=0; fail_at=point;
        bool ok=ce_bibliography_select(doc,items,2,err,sizeof err); fail_at=0;
        assert(!ok && !live); cJSON_InitHooks(NULL);
        char *after=ce_document_export_json(doc); assert(!strcmp(before,after) && ce_document_can_undo(doc)==can_undo);
        free(before); free(after); ce_document_free(doc);
    }
    ce_document_free(base); printf("bibliography selection helper%s: %zu sticky OOM points preserve content/history\n",existing?" (existing metadata/copies)":"",points);
}
int main(void) {
    test_profiles(); test_session(); test_metadata_and_copy(); test_reject_and_empty(); test_oom(false); test_oom(true);
    puts("selected bibliography: three profiles, no body, include/clear, one undo, source metadata, copy, atomicity PASS"); return 0;
}
