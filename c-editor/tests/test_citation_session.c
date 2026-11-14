#include "citation_session.h"
#include "citations.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[512];
static const char *item_a="{\"id\":\"A\",\"type\":\"book\",\"title\":\"First book\",\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],\"issued\":{\"date-parts\":[[2020]]},\"publisher\":\"Press\"}";
static const char *item_b="{\"id\":\"B\",\"type\":\"book\",\"title\":\"Second book\",\"author\":[{\"family\":\"Roe\",\"given\":\"Richard\"}],\"issued\":{\"date-parts\":[[2021]]},\"publisher\":\"Press\"}";
static CeDocument *clone(const CeDocument *source) {
    char *json=ce_document_export_json(source); assert(json);
    CeDocument *copy=ce_document_new(); assert(copy);
    assert(ce_document_import(copy,json,CE_FORMAT_JSON,error,sizeof error)); free(json); return copy;
}
static char *snapshot(const CeDocument *doc) { char *s=ce_document_export_json(doc); assert(s); return s; }
static void unchanged(const CeDocument *doc,const char *before) {
    char *after=snapshot(doc); assert(!strcmp(before,after)); free(after);
}
static char *insert(CeDocument *doc,const char *id,const char *json) {
    CeZoteroItem item={0}; item.id=(char *)id; item.item_key=(char *)id;
    item.title=(char *)"Source title"; item.csl_json=(char *)json;
    char *citation_id=NULL;
    assert(ce_citation_insert(doc,0,&item,"","independent quotation",&citation_id,error,sizeof error));
    assert(citation_id); return citation_id;
}
static void assert_citation(const CeCitationSession *session,const char *id,const char *expected) {
    const CeCitationCache *cache=ce_citation_prepared_citation(ce_citation_session_prepared(session),id);
    assert(cache); CeCslOutputString plain={0};
    assert(ce_csl_output_plain(ce_citation_cache_citation_output(cache),&plain,error,sizeof error));
    assert(!strcmp(plain.data,expected)); ce_csl_output_string_free(&plain);
}
static size_t citation_index(CeDocument *doc,const char *id,CeBlockInfo *block) {
    for(size_t i=0;i<ce_document_block_count(doc);i++) {
        assert(ce_document_block_info(doc,i,block)); if(block->kind!=CE_BLOCK_CITATION) continue;
        char *json=ce_document_block_json(doc,i); cJSON *node=cJSON_Parse(json); assert(node);
        cJSON *a=cJSON_GetObjectItemCaseSensitive(node,"attrs"),*value=cJSON_GetObjectItemCaseSensitive(a,"citationId");
        bool match=cJSON_IsString(value) && !strcmp(value->valuestring,id);
        free(json); cJSON_Delete(node); if(match) return i;
    }
    assert(false); return 0;
}
int main(void) {
    CeDocument *doc=ce_document_new(); assert(doc);
    assert(ce_document_import(doc,"A paragraph",CE_FORMAT_TEXT,error,sizeof error));
    ce_document_mark_saved(doc); char *original=snapshot(doc);
    CeCitationSession *session=NULL;
    assert(ce_citation_session_build(doc,NULL,NULL,&session,error,sizeof error));
    unchanged(doc,original);
    assert(!strcmp(original,ce_citation_prepared_json(ce_citation_session_prepared(session))));
    assert(!ce_document_can_undo(doc));
    CeDocument *candidate=clone(doc); char *a=insert(candidate,"A",item_a);
    assert(ce_citation_session_apply(&session,doc,candidate,NULL,NULL,true,error,sizeof error));
    ce_document_free(candidate); assert_citation(session,a,"(Doe, 2020)");
    char *with_a=snapshot(doc); assert(strstr(with_a,"cslRenderJson") && strstr(with_a,"zoteroEndnoteSection"));
    assert(ce_document_undo(doc)); unchanged(doc,original); assert(!ce_document_can_undo(doc));
    assert(!ce_document_is_modified(doc)); assert(ce_document_redo(doc)); unchanged(doc,with_a);
    assert(ce_citation_session_build(doc,NULL,NULL,&session,error,sizeof error));
    /* Prose changes retain the evaluated view but snapshots use current prose. */
    candidate=clone(doc); assert(ce_document_insert(candidate,0,"New prose"));
    CeCitationSession *same=session;
    assert(ce_citation_session_apply(&session,doc,candidate,NULL,NULL,false,error,sizeof error));
    ce_document_free(candidate); assert(session==same);
    CeCitationPrepared *saved=NULL;
    assert(ce_citation_session_snapshot(session,doc,NULL,&saved,error,sizeof error));
    assert(strstr(ce_citation_prepared_json(saved),"New prose"));
    ce_citation_prepared_free(saved);
    /* Changing style is one undo step and persists its selection. */
    char *before_style=snapshot(doc); candidate=clone(doc); CeCitationProfile ieee=CE_CITATION_PROFILE_IEEE;
    assert(ce_citation_session_apply(&session,doc,candidate,NULL,&ieee,true,error,sizeof error));
    ce_document_free(candidate); assert(ce_citation_session_profile(session)==ieee); assert_citation(session,a,"[1]");
    assert(ce_document_undo(doc)); unchanged(doc,before_style);
    assert(ce_citation_session_build(doc,NULL,NULL,&session,error,sizeof error));
    assert(ce_citation_session_profile(session)==CE_CITATION_PROFILE_APA);
    assert(ce_document_redo(doc)); assert(ce_citation_session_build(doc,NULL,NULL,&session,error,sizeof error));
    candidate=clone(doc); char *b=insert(candidate,"B",item_b);
    assert(ce_citation_session_apply(&session,doc,candidate,NULL,NULL,true,error,sizeof error));
    ce_document_free(candidate); assert_citation(session,b,"[1]"); assert_citation(session,a,"[2]");
    char *with_b=snapshot(doc); CeBlockInfo block; citation_index(doc,b,&block);
    candidate=clone(doc); assert(ce_document_delete(candidate,block.start,block.end));
    assert(ce_citation_session_apply(&session,doc,candidate,NULL,NULL,false,error,sizeof error));
    ce_document_free(candidate); assert_citation(session,a,"[1]");
    assert(ce_citation_prepared_stats(ce_citation_session_prepared(session))->reference_count==1);
    assert(ce_document_undo(doc)); unchanged(doc,with_b);
    assert(ce_citation_session_build(doc,NULL,NULL,&session,error,sizeof error));
    /* Incomplete or conflicting sources never reach the live revision. */
    char *before_failure=snapshot(doc); same=session;
    candidate=clone(doc); char *missing=insert(candidate,"Missing",NULL);
    assert(!ce_citation_session_apply(&session,doc,candidate,NULL,NULL,true,error,sizeof error));
    assert(*error && session==same); unchanged(doc,before_failure); ce_document_free(candidate); free(missing);
    candidate=clone(doc); char *conflict=insert(candidate,"A",item_b);
    assert(!ce_citation_session_apply(&session,doc,candidate,NULL,NULL,true,error,sizeof error));
    assert(session==same); unchanged(doc,before_failure); ce_document_free(candidate); free(conflict);
    /* A missing derived section must be rebuilt even with unchanged sources. */
    candidate=clone(doc); size_t count=ce_document_block_count(candidate);
    assert(ce_document_block_info(candidate,count-1,&block) && block.kind==CE_BLOCK_ENDNOTES);
    assert(ce_document_delete(candidate,block.start,block.end));
    assert(ce_citation_session_apply(&session,doc,candidate,NULL,NULL,false,error,sizeof error));
    ce_document_free(candidate); assert(ce_document_block_info(doc,ce_document_block_count(doc)-1,&block));
    assert(block.kind==CE_BLOCK_ENDNOTES);
    /* An unprocessable legacy note remains editable, with no trusted display. */
    CeDocument *legacy=ce_document_new(); assert(legacy);
    assert(ce_document_import(legacy,"{\"type\":\"doc\",\"attrs\":{\"unknown\":4503599627370496},\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"legacy\"}]}]}",CE_FORMAT_JSON,error,sizeof error));
    missing=insert(legacy,"Missing",NULL); CeCitationSession *invalid=NULL;
    assert(!ce_citation_session_build(legacy,NULL,NULL,&invalid,error,sizeof error)); assert(!invalid);
    candidate=clone(legacy); assert(ce_document_insert(candidate,0,"ordinary text"));
    assert(ce_citation_session_apply(&invalid,legacy,candidate,NULL,NULL,false,error,sizeof error));
    assert(!invalid); ce_document_free(candidate);
    /* Adjacent large integers differ even inside otherwise unchanged root
     * metadata; a failed processor must not bypass that edit. */
    char *numeric_before=snapshot(legacy),*numeric_change=snapshot(legacy);
    char *number=strstr(numeric_change,"4503599627370496"); assert(number); number[15]='7';
    candidate=clone(legacy); assert(ce_document_apply_json(candidate,numeric_change)); free(numeric_change);
    assert(!ce_citation_session_apply(&invalid,legacy,candidate,NULL,NULL,false,error,sizeof error));
    unchanged(legacy,numeric_before); free(numeric_before); ce_document_free(candidate);
    candidate=clone(legacy); assert(ce_citation_update(candidate,missing,"99",NULL,error,sizeof error));
    char *legacy_before=snapshot(legacy);
    assert(!ce_citation_session_apply(&invalid,legacy,candidate,NULL,NULL,true,error,sizeof error));
    unchanged(legacy,legacy_before); ce_document_free(candidate); ce_document_free(legacy);
    free(legacy_before); free(missing); free(original); free(with_a); free(before_style); free(with_b); free(before_failure);
    free(a); free(b); ce_citation_session_free(session); ce_document_free(doc);
    puts("citation sessions: atomic edits, undo/profile restore, prose reuse, live snapshots, renumbering and explicit legacy failures passed");
    return 0;
}
