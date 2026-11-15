#include "citation_session.h"
#include "csl_json.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct CeCitationSession {
    CeCitationProfile profile;
    char locale[6]; /* Empty means the style's own default. */
    CeCitationStyle *style;
    CeCitationPrepared *prepared;
};
static bool problem(char *error,size_t capacity,const char *message) {
    if(error && capacity) snprintf(error,capacity,"%s",message);
    return false;
}
static cJSON *document_tree(const CeDocument *document,char *error,size_t capacity) {
    char *json=ce_document_export_json(document);
    cJSON *tree=NULL;
    if(!json) { problem(error,capacity,"Cannot copy document for citation processing"); return NULL; }
    bool ok=ce_csl_json_parse_strict(json,strlen(json),&tree,error,capacity);
    free(json);
    return ok?tree:NULL;
}
static bool profile_name(const char *name,CeCitationProfile *profile) {
    if(!strcmp(name,"apa")) *profile=CE_CITATION_PROFILE_APA;
    else if(!strcmp(name,"ieee")) *profile=CE_CITATION_PROFILE_IEEE;
    else if(!strcmp(name,"gbt-7714-2015-numeric") || !strcmp(name,"gb-t-7714")) *profile=CE_CITATION_PROFILE_GBT;
    else return false;
    return true;
}
static bool selection(const CeDocument *document,const CeCitationProfile *override,
    CeCitationProfile *profile,char locale[6],char *error,size_t capacity) {
    cJSON *tree=document_tree(document,error,capacity);
    if(!tree) return false;
    cJSON *attrs=cJSON_GetObjectItemCaseSensitive(tree,"attrs");
    cJSON *setting=cJSON_GetObjectItemCaseSensitive(attrs,"citationProfile");
    cJSON *language=cJSON_GetObjectItemCaseSensitive(attrs,"citationLocale");
    *profile=CE_CITATION_PROFILE_APA; locale[0]='\0';
    bool ok=true;
    if(override) {
        if(!ce_citation_style_info(*override)) ok=problem(error,capacity,"Unknown citation style");
        else *profile=*override;
    } else if(setting) {
        if(!cJSON_IsString(setting) || !profile_name(setting->valuestring,profile))
            ok=problem(error,capacity,"Unsupported document citation profile");
    } else {
        /* Legacy documents stored their choice on the bibliography section. */
        for(size_t i=0;ok && i<ce_document_block_count(document);i++) {
            CeBlockInfo block;
            if(!ce_document_block_info(document,i,&block) || block.kind!=CE_BLOCK_ENDNOTES) continue;
            char *json=ce_document_block_json(document,i);
            cJSON *node=json?cJSON_Parse(json):NULL;
            cJSON *a=cJSON_GetObjectItemCaseSensitive(node,"attrs");
            cJSON *value=cJSON_GetObjectItemCaseSensitive(a,"style");
            if(!node) ok=problem(error,capacity,"Cannot read legacy citation settings");
            else if(value && (!cJSON_IsString(value) || !profile_name(value->valuestring,profile)))
                ok=problem(error,capacity,"Unsupported legacy citation style");
            cJSON_Delete(node); free(json); break;
        }
    }
    if(ok && language) {
        if(!cJSON_IsString(language) ||
            (strcmp(language->valuestring,"en-US") && strcmp(language->valuestring,"zh-CN")))
            ok=problem(error,capacity,"Unsupported document citation locale");
        else memcpy(locale,language->valuestring,6);
    }
    cJSON_Delete(tree);
    return ok;
}
void ce_citation_session_free(CeCitationSession *session) {
    if(!session) return;
    ce_citation_prepared_free(session->prepared);
    ce_citation_style_free(session->style);
    free(session);
}
CeCitationProfile ce_citation_session_profile(const CeCitationSession *session) {
    return session?session->profile:CE_CITATION_PROFILE_APA;
}
const CeCitationPrepared *ce_citation_session_prepared(const CeCitationSession *session) {
    return session?session->prepared:NULL;
}
bool ce_citation_session_build(const CeDocument *document,const CeZoteroLibrary *library,
    const CeCitationProfile *override,CeCitationSession **out,char *error,size_t capacity) {
    if(!document || !out) return problem(error,capacity,"Invalid citation session input");
    CeCitationSession *next=calloc(1,sizeof *next);
    if(!next) return problem(error,capacity,"Out of memory preparing citation session");
    CeCitationProcessOptions options={override!=NULL};
    bool ok=selection(document,override,&next->profile,next->locale,error,capacity) &&
        ce_citation_style_load(next->profile,*next->locale?next->locale:NULL,NULL,&next->style,error,capacity) &&
        ce_citations_prepare(document,next->style,library,&options,&next->prepared,error,capacity);
    if(!ok) { ce_citation_session_free(next); return false; }
    CeCitationSession *previous=*out; *out=next; ce_citation_session_free(previous);
    return true;
}
static bool atom_kind(CeBlockKind kind) {
    return kind==CE_BLOCK_CITATION || kind==CE_BLOCK_ENDNOTES;
}
static size_t next_atom(const CeDocument *document,size_t i) {
    for(;i<ce_document_block_count(document);i++) {
        CeBlockInfo block;
        if(ce_document_block_info(document,i,&block) && atom_kind(block.kind)) break;
    }
    return i;
}
/* Conservative fallback only: do not let a processing failure hide a changed
 * source, deleted section, profile switch or unrecognized citation attribute. */
static bool same_unprocessed_sources(const CeDocument *a,const CeDocument *b) {
    char error[128];
    cJSON *x=document_tree(a,error,sizeof error),*y=document_tree(b,error,sizeof error);
    cJSON *xa=cJSON_GetObjectItemCaseSensitive(x,"attrs"),*ya=cJSON_GetObjectItemCaseSensitive(y,"attrs");
    char *ax=xa?ce_csl_json_print_exact(xa):NULL,*ay=ya?ce_csl_json_print_exact(ya):NULL;
    bool same=x && y && ((!xa && !ya) || (ax && ay && !strcmp(ax,ay)));
    cJSON_free(ax); cJSON_free(ay);
    cJSON_Delete(x); cJSON_Delete(y);
    for(size_t i=0,j=0;same;) {
        i=next_atom(a,i); j=next_atom(b,j);
        if(i==ce_document_block_count(a) || j==ce_document_block_count(b))
            return i==ce_document_block_count(a) && j==ce_document_block_count(b);
        char *left=ce_document_block_json(a,i++),*right=ce_document_block_json(b,j++);
        same=left && right && !strcmp(left,right);
        free(left); free(right);
    }
    return false;
}
static bool apply_json(CeDocument *live,const char *json,char *error,size_t capacity) {
    if(!json) return problem(error,capacity,"Cannot copy edited document");
    if(!ce_document_apply_json(live,json)) return problem(error,capacity,ce_document_error(live));
    return true;
}
static bool same_canonical_section(const CeDocument *a,const CeDocument *b,bool bibliography) {
    size_t indices[2]={0},counts[2]={0};
    const CeDocument *documents[2]={a,b};
    for(size_t d=0;d<2;d++) {
        for(size_t i=0;i<ce_document_block_count(documents[d]);i++) {
            CeBlockInfo block;
            if(ce_document_block_info(documents[d],i,&block) && block.kind==CE_BLOCK_ENDNOTES) {
                indices[d]=i; ++counts[d];
            }
        }
        if(counts[d]!=(bibliography?1u:0u)) return false;
        if(counts[d] && indices[d]+1!=ce_document_block_count(documents[d])) return false;
    }
    if(!bibliography) return true;
    char *x=ce_document_block_json(a,indices[0]),*y=ce_document_block_json(b,indices[1]);
    bool same=x && y && !strcmp(x,y);
    free(x); free(y); return same;
}
bool ce_citation_session_apply(CeCitationSession **session,CeDocument *live,
    const CeDocument *candidate,const CeZoteroLibrary *library,
    const CeCitationProfile *override,bool force,char *error,size_t capacity) {
    if(!session || !live || !candidate || live==candidate)
        return problem(error,capacity,"Citation edits require a private candidate document");
    bool reuse=false;
    if(*session && !force && !override) {
        CeCitationProfile profile; char locale[6],digest[65];
        reuse=selection(candidate,NULL,&profile,locale,error,capacity) &&
            profile==(*session)->profile && !strcmp(locale,(*session)->locale) &&
            ce_citations_source_digest(candidate,(*session)->style,library,digest,error,capacity) &&
            !strcmp(digest,ce_citation_prepared_digest((*session)->prepared)) &&
            same_canonical_section(live,candidate,ce_citation_prepared_bibliography((*session)->prepared)!=NULL);
    }
    if(reuse || (!*session && !force && !override && same_unprocessed_sources(live,candidate))) {
        char *json=ce_document_export_json(candidate);
        bool ok=apply_json(live,json,error,capacity); free(json);
        return ok;
    }
    CeCitationSession *next=NULL;
    if(!ce_citation_session_build(candidate,library,override,&next,error,capacity)) return false;
    if(!apply_json(live,ce_citation_prepared_json(next->prepared),error,capacity)) {
        ce_citation_session_free(next); return false;
    }
    CeCitationSession *previous=*session; *session=next; ce_citation_session_free(previous);
    return true;
}
bool ce_citation_session_snapshot(const CeCitationSession *session,const CeDocument *document,
    const CeZoteroLibrary *library,CeCitationPrepared **out,char *error,size_t capacity) {
    if(!session || !document || !out) return problem(error,capacity,"No valid citation session");
    CeCitationProfile profile; char locale[6];
    if(!selection(document,NULL,&profile,locale,error,capacity)) return false;
    if(profile!=session->profile || strcmp(locale,session->locale))
        return problem(error,capacity,"Citation session does not match the document settings");
    return ce_citations_prepare(document,session->style,library,NULL,out,error,capacity);
}
