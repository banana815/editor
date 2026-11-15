#include "bibliography_selection.h"
#include "citation_process.h"
#include "csl_json.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SectionList { cJSON **nodes; size_t count,capacity; } SectionList;
typedef struct SavedSource { char *identity; cJSON *attrs; } SavedSource;
static bool problem(char *error,size_t capacity,const char *message) {
    if(error && capacity) snprintf(error,capacity,"%s",message);
    return false;
}
static const cJSON *get(const cJSON *node,const char *key) { return cJSON_GetObjectItemCaseSensitive(node,key); }
static const char *str(const cJSON *node,const char *key) {
    const cJSON *value=get(node,key); return cJSON_IsString(value)?value->valuestring:"";
}
static bool set(cJSON *node,const char *key,cJSON *value) {
    if(!value) return false;
    bool ok=get(node,key)?cJSON_ReplaceItemInObjectCaseSensitive(node,key,value):cJSON_AddItemToObject(node,key,value);
    if(!ok) cJSON_Delete(value);
    return ok;
}
static bool set_text(cJSON *node,const char *key,const char *value) { return set(node,key,cJSON_CreateString(value?value:"")); }
static bool collect(cJSON *node,SectionList *list,unsigned depth) {
    if(depth>128) return false;
    const char *type=str(node,"type");
    if(!strcmp(type,"zoteroEndnoteSection")) {
        if(list->count==CE_CSL_INPUT_MAX_CLUSTERS) return false;
        if(list->count==list->capacity) {
            size_t capacity=list->capacity?list->capacity*2:8;
            cJSON **nodes=realloc(list->nodes,capacity*sizeof *nodes); if(!nodes) return false;
            list->nodes=nodes; list->capacity=capacity;
        }
        list->nodes[list->count++]=node; return true;
    }
    if(!strcmp(type,"zoteroCitationBlock") || !strcmp(type,"paragraph") || !strcmp(type,"heading")) return true;
    cJSON *content=cJSON_GetObjectItemCaseSensitive(node,"content");
    for(cJSON *child=content?content->child:NULL;child;child=child->next) if(!collect(child,list,depth+1)) return false;
    return true;
}
static const char *known[]={"itemUri","sourceId","libraryId","itemKey","citeKey","cslJson","title","author","year","pdfUri"};
static cJSON *unknown(const cJSON *attrs) {
    cJSON *copy=cJSON_Duplicate(attrs,true);
    if(copy) for(size_t i=0;i<sizeof known/sizeof *known;i++) cJSON_DeleteItemFromObjectCaseSensitive(copy,known[i]);
    return copy;
}
static int saved_compare(const void *a,const void *b) {
    return strcmp(((const SavedSource *)a)->identity,((const SavedSource *)b)->identity);
}
static const SavedSource *saved_find(const SavedSource *saved,size_t count,const char *identity) {
    SavedSource key={(char *)identity,NULL};
    return count?bsearch(&key,saved,count,sizeof *saved,saved_compare):NULL;
}
static bool load_saved(const SectionList *sections,cJSON *sources,char *error,size_t capacity) {
    size_t total=0;
    for(size_t i=0;i<sections->count;i++) {
        const cJSON *attrs=get(sections->nodes[i],"attrs"),*mode=get(attrs,"bibliographyMode");
        if(!cJSON_IsObject(attrs)) return problem(error,capacity,"Bibliography attributes must be an object");
        bool selected=mode && cJSON_IsString(mode) && !strcmp(mode->valuestring,"selected");
        if(mode && (!cJSON_IsString(mode) || (!selected && strcmp(mode->valuestring,"citations"))))
            return problem(error,capacity,"Cannot replace an unsupported bibliography mode");
        if(!mode) {
            const cJSON *old=get(attrs,"entriesJson");
            cJSON *entries=NULL;
            if(old && (!cJSON_IsString(old) || (*old->valuestring &&
               !ce_csl_json_parse_strict(old->valuestring,strlen(old->valuestring),&entries,error,capacity)))) return false;
            bool safe=!entries || (cJSON_IsArray(entries) && !entries->child);
            cJSON_Delete(entries);
            if(!safe) return problem(error,capacity,"Legacy bibliography entries must be preserved; update matching citations first or keep the archive separately");
        }
        const cJSON *value=get(attrs,"selectedItemsJson");
        if(!selected) {
            if(value) return problem(error,capacity,"selectedItemsJson requires selected bibliographyMode");
            continue;
        }
        if(!cJSON_IsString(value)) return problem(error,capacity,"Selected sources must be a JSON string");
        cJSON *array=NULL; bool exact=false;
        if(!ce_csl_json_parse_strict_with_numbers(value->valuestring,strlen(value->valuestring),&array,&exact,error,capacity)) return false;
        bool ok=exact && cJSON_IsArray(array);
        if(!ok) problem(error,capacity,"Selected sources contain invalid or non-lossless metadata");
        size_t count=ok?(size_t)cJSON_GetArraySize(array):0;
        if(ok && count>CE_CSL_INPUT_MAX_ITEMS-total) ok=problem(error,capacity,"Too many selected sources");
        while(ok && array->child) {
            if(!cJSON_IsObject(array->child)) { ok=problem(error,capacity,"Selected source is not an attribute object"); break; }
            cJSON *attrs_node=cJSON_DetachItemFromArray(array,0);
            ok=cJSON_AddItemToArray(sources,attrs_node); if(!ok) cJSON_Delete(attrs_node);
        }
        cJSON_Delete(array); if(!ok) return false;
        total+=count;
    }
    return true;
}
bool ce_bibliography_select(CeDocument *candidate,const CeZoteroItem *const *items,size_t count,char *error,size_t capacity) {
    if(error && capacity) *error=0;
    if(!candidate || (count && !items) || count>CE_CSL_INPUT_MAX_ITEMS)
        return problem(error,capacity,"Bibliography selection requires a candidate and at most 50000 items");
    char *source=ce_document_export_json(candidate),*serialized=NULL,*result=NULL;
    cJSON *root=NULL,*saved_nodes=cJSON_CreateArray(),*selected=cJSON_CreateArray();
    SectionList sections={0}; SavedSource *saved=NULL; size_t saved_count=0; bool ok=false;
    if(!source || !saved_nodes || !selected || !ce_csl_json_parse_strict(source,strlen(source),&root,error,capacity)) goto done;
    if(strcmp(str(root,"type"),"doc") || !collect(root,&sections,0) || !load_saved(&sections,saved_nodes,error,capacity)) goto done;
    saved_count=(size_t)cJSON_GetArraySize(saved_nodes);
    saved=saved_count?calloc(saved_count,sizeof *saved):NULL; if(saved_count && !saved) goto done;
    size_t index=0;
    for(cJSON *attrs=saved_nodes->child;attrs;attrs=attrs->next,index++) {
        saved[index].attrs=attrs;
        if(!ce_citations_source_identity(attrs,&saved[index].identity,error,capacity)) goto done;
    }
    if(saved_count>1) qsort(saved,saved_count,sizeof *saved,saved_compare);
    for(size_t i=1;i<saved_count;i++) if(!strcmp(saved[i-1].identity,saved[i].identity)) {
        cJSON *a=unknown(saved[i-1].attrs),*b=unknown(saved[i].attrs);
        char *left=a?ce_csl_json_print_exact(a):NULL,*right=b?ce_csl_json_print_exact(b):NULL;
        bool equal=left && right && !strcmp(left,right);
        cJSON_free(left); cJSON_free(right); cJSON_Delete(a); cJSON_Delete(b);
        if(!equal) { problem(error,capacity,"Repeated selected source has differing or uncomparable custom metadata"); goto done; }
    }
    for(size_t i=0;i<count;i++) {
        const CeZoteroItem *item=items[i]; cJSON *attrs=cJSON_CreateObject(),*csl=NULL; char *identity=NULL;
        if(!attrs || !item || !item->csl_json || !*item->csl_json ||
           !ce_csl_json_parse_strict(item->csl_json,strlen(item->csl_json),&csl,error,capacity) || !cJSON_IsObject(csl)) {
            cJSON_Delete(attrs); cJSON_Delete(csl); problem(error,capacity,"Selected library item lacks a complete CSL object"); goto done;
        }
        cJSON_Delete(csl);
        bool stored=set_text(attrs,"itemUri",item->item_uri) && set_text(attrs,"sourceId",item->id) &&
            set_text(attrs,"itemKey",item->item_key) && set_text(attrs,"citeKey",item->cite_key && *item->cite_key?item->cite_key:item->item_key) &&
            set_text(attrs,"cslJson",item->csl_json) && set_text(attrs,"title",item->title) &&
            set_text(attrs,"author",item->author) && set_text(attrs,"year",item->year) && set_text(attrs,"pdfUri",item->pdf_uri);
        if(stored && item->library_id>0) {
            char number[32]; snprintf(number,sizeof number,"%" PRId64,item->library_id); stored=set_text(attrs,"libraryId",number);
        }
        if(stored) stored=ce_citations_source_identity(attrs,&identity,error,capacity);
        const SavedSource *previous=stored?saved_find(saved,saved_count,identity):NULL;
        if(previous) {
            cJSON *custom=unknown(previous->attrs);
            if(!custom) stored=false;
            while(stored && custom->child) {
                cJSON *value=custom->child; const char *name=value->string;
                cJSON *copy=cJSON_Duplicate(value,true);
                stored=set(attrs,name,copy);
                cJSON_DeleteItemFromObjectCaseSensitive(custom,name);
            }
            cJSON_Delete(custom);
        }
        free(identity);
        if(!stored || !cJSON_AddItemToArray(selected,attrs)) { cJSON_Delete(attrs); goto done; }
    }
    serialized=ce_csl_json_print_exact(selected); if(!serialized) goto done;
    if(!sections.count) {
        cJSON *node=cJSON_CreateObject(),*attrs=cJSON_CreateObject();
        if(!node || !attrs) { cJSON_Delete(node); cJSON_Delete(attrs); goto done; }
        if(!set(node,"attrs",attrs) || !set_text(node,"type","zoteroEndnoteSection")) { cJSON_Delete(node); goto done; }
        cJSON *content=cJSON_GetObjectItemCaseSensitive(root,"content");
        if(!content) { if(!set(root,"content",cJSON_CreateArray())) { cJSON_Delete(node); goto done; } content=cJSON_GetObjectItemCaseSensitive(root,"content"); }
        if(!cJSON_IsArray(content) || !cJSON_AddItemToArray(content,node)) { cJSON_Delete(node); goto done; }
        if(!collect(node,&sections,0)) goto done;
    }
    for(size_t i=0;i<sections.count;i++) {
        cJSON *attrs=cJSON_GetObjectItemCaseSensitive(sections.nodes[i],"attrs");
        if(!i) {
            if(!set_text(attrs,"bibliographyMode","selected") || !set_text(attrs,"selectedItemsJson",serialized)) goto done;
        } else if(!strcmp(str(attrs,"bibliographyMode"),"selected")) {
            if(!set_text(attrs,"bibliographyMode","citations")) goto done;
            cJSON_DeleteItemFromObjectCaseSensitive(attrs,"selectedItemsJson");
        }
    }
    result=ce_csl_json_print_exact(root); if(!result) goto done;
    cJSON *validated=NULL;
    if(!ce_csl_json_parse_strict(result,strlen(result),&validated,error,capacity)) goto done;
    cJSON_Delete(validated);
    ok=ce_document_apply_json(candidate,result);
    if(!ok) problem(error,capacity,ce_document_error(candidate));
done:
    if(saved) for(size_t i=0;i<saved_count;i++) free(saved[i].identity);
    free(saved); free(sections.nodes); free(source); cJSON_free(serialized); cJSON_free(result);
    cJSON_Delete(root); cJSON_Delete(saved_nodes); cJSON_Delete(selected);
    if(!ok && error && capacity && !*error) problem(error,capacity,"Unable to replace bibliography selection within memory/JSON limits");
    return ok;
}
