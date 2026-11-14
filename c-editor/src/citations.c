#include "citations.h"
#include "cJSON.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CITATION "zoteroCitationBlock"
#define ENDNOTES "zoteroEndnoteSection"
#define DEPTH_LIMIT 128

typedef struct { cJSON **items; size_t count, capacity; } Nodes;
typedef struct { char **items; size_t count, capacity; } Strings;
static bool failure(char *error,size_t capacity,const char *message) {
    if(error && capacity) snprintf(error,capacity,"%s",message?message:"Out of memory");
    return false;
}
static void clear_error(char *error,size_t capacity) { if(error && capacity) *error=0; }
static char *copy(const char *value) {
    const char *s=value?value:""; size_t size=strlen(s)+1;
    char *result=malloc(size); if(result) memcpy(result,s,size); return result;
}
static char *print_owned(const cJSON *value) {
    char *printed=cJSON_PrintUnformatted(value);
    char *owned=printed?copy(printed):NULL;
    cJSON_free(printed); return owned;
}
static cJSON *field(const cJSON *node,const char *key) { return cJSON_GetObjectItemCaseSensitive(node,key); }
static const char *text(const cJSON *node,const char *key) {
    cJSON *value=field(node,key); return cJSON_IsString(value)?value->valuestring:"";
}
static bool type_is(const cJSON *node,const char *type) { return !strcmp(text(node,"type"),type); }
static bool put(cJSON *object,const char *key,cJSON *value) {
    if(!value || !object) { cJSON_Delete(value); return false; }
    bool ok=field(object,key)?cJSON_ReplaceItemInObjectCaseSensitive(object,key,value):cJSON_AddItemToObject(object,key,value);
    if(!ok) cJSON_Delete(value);
    return ok;
}
static bool put_text(cJSON *object,const char *key,const char *value) { return put(object,key,cJSON_CreateString(value?value:"")); }
static cJSON *attributes(cJSON *node) {
    cJSON *a=field(node,"attrs");
    if(!a) { if(!put(node,"attrs",cJSON_CreateObject())) return NULL; a=field(node,"attrs"); }
    return cJSON_IsObject(a)?a:NULL;
}
static bool grow(void **data,size_t *capacity,size_t count,size_t size) {
    if(count<=*capacity) return true;
    if(*capacity>SIZE_MAX/2) return false;
    size_t next=*capacity?*capacity*2:16;
    if(next<count) next=count;
    if(next>SIZE_MAX/size) return false;
    void *p=realloc(*data,next*size); if(!p) return false;
    *data=p; *capacity=next; return true;
}
static bool node_add(Nodes *nodes,cJSON *node) {
    if(!grow((void**)&nodes->items,&nodes->capacity,nodes->count+1,sizeof(cJSON*))) return false;
    nodes->items[nodes->count++]=node; return true;
}
static void strings_free(Strings *strings) {
    for(size_t i=0;i<strings->count;i++) free(strings->items[i]);
    free(strings->items); memset(strings,0,sizeof *strings);
}
static size_t string_index(const Strings *strings,const char *value) {
    for(size_t i=0;i<strings->count;i++) if(!strcmp(strings->items[i],value)) return i;
    return SIZE_MAX;
}
static bool string_add(Strings *strings,const char *value) {
    if(!grow((void**)&strings->items,&strings->capacity,strings->count+1,sizeof(char*))) return false;
    char *owned=copy(value); if(!owned) return false;
    strings->items[strings->count++]=owned; return true;
}
static bool collect(cJSON *node,Nodes *citations,Nodes *sections,unsigned depth) {
    if(depth>DEPTH_LIMIT) return false;
    if(type_is(node,CITATION)) return node_add(citations,node);
    if(type_is(node,ENDNOTES)) return node_add(sections,node);
    /* These nodes are leaves in CeDocument's visible block traversal. */
    if(type_is(node,"paragraph") || type_is(node,"heading")) return true;
    cJSON *child;
    cJSON_ArrayForEach(child,field(node,"content")) if(!collect(child,citations,sections,depth+1)) return false;
    return true;
}
static cJSON *snapshot(CeDocument *doc,Nodes *citations,Nodes *sections) {
    char *json=ce_document_export_json(doc);
    cJSON *root=json?cJSON_ParseWithOpts(json,NULL,true):NULL;
    free(json);
    if(root && !collect(root,citations,sections,0)) { cJSON_Delete(root); root=NULL; }
    return root;
}
static bool reserve_ids(const Nodes *nodes,Strings *reserved) {
    for(size_t i=0;i<nodes->count;i++) {
        const char *id=text(field(nodes->items[i],"attrs"),"citationId");
        if(*id && string_index(reserved,id)==SIZE_MAX && !string_add(reserved,id)) return false;
    }
    return true;
}
static char *unique_id(Strings *reserved,size_t *sequence) {
    char value[64];
    for(;;) {
        if(*sequence==SIZE_MAX) return NULL;
        snprintf(value,sizeof value,"cite-c-%zu",++*sequence);
        if(string_index(reserved,value)==SIZE_MAX) {
            char *id=copy(value);
            if(!id || !string_add(reserved,id)) { free(id); return NULL; }
            return id;
        }
    }
}
static bool apply(CeDocument *doc,cJSON *root,char *error,size_t capacity) {
    char *json=print_owned(root);
    if(!json) return failure(error,capacity,"Out of memory");
    bool ok=ce_document_apply_json(doc,json); free(json);
    return ok?true:failure(error,capacity,ce_document_error(doc));
}
bool ce_citation_insert(CeDocument *doc,size_t offset,const CeZoteroItem *item,
                         const char *page,const char *note,char **new_id,char *error,size_t capacity) {
    clear_error(error,capacity);
    if(!doc || !item) return failure(error,capacity,"A document and library item are required");
    if(offset>ce_document_length(doc) ||
       (((unsigned char)ce_document_text(doc)[offset]&0xc0)==0x80))
        return failure(error,capacity,"Citation insertion requires a UTF-8 byte boundary within the document");
    Nodes citations={0},sections={0}; Strings reserved={0}; size_t sequence=0;
    cJSON *root=snapshot(doc,&citations,&sections),*node=NULL,*attrs=NULL;
    char *id=NULL,*json=NULL,*node_json=NULL,*result_json=NULL;
    CeDocument *candidate=NULL; bool ok=false;
    if(!root || !reserve_ids(&citations,&reserved) || !(id=unique_id(&reserved,&sequence))) goto oom;
    node=cJSON_CreateObject(); attrs=cJSON_CreateObject();
    if(!node || !attrs) goto oom;
    if(!put(node,"attrs",attrs)) { attrs=NULL; goto oom; }
    if(!put_text(node,"type",CITATION) || !put_text(attrs,"citationId",id) ||
       !put(attrs,"endnoteNumber",cJSON_CreateNumber(0)) ||
       !put_text(attrs,"itemKey",item->item_key) || !put_text(attrs,"itemUri",item->item_uri) ||
       !put_text(attrs,"pdfUri",item->pdf_uri) || !put_text(attrs,"title",item->title) ||
       !put_text(attrs,"author",item->author) || !put_text(attrs,"year",item->year) ||
       !put_text(attrs,"citeKey",item->cite_key && *item->cite_key?item->cite_key:item->item_key) ||
       !put_text(attrs,"page",page) || !put_text(attrs,"note",note)) goto oom;
    if(item->id && *item->id && !put_text(attrs,"sourceId",item->id)) goto oom;
    if(item->csl_json && *item->csl_json && !put_text(attrs,"cslJson",item->csl_json)) goto oom;
    if(item->library_id>0) {
        char library[32]; snprintf(library,sizeof library,"%" PRId64,item->library_id);
        if(!put_text(attrs,"libraryId",library)) goto oom;
    }
    json=print_owned(root); node_json=print_owned(node); candidate=ce_document_new();
    if(!json || !node_json || !candidate) goto oom;
    if(!ce_document_import(candidate,json,CE_FORMAT_JSON,error,capacity)) goto done;
    if(!ce_document_insert_node(candidate,offset,node_json)) { failure(error,capacity,ce_document_error(candidate)); goto done; }
    result_json=ce_document_export_json(candidate);
    if(!result_json) goto oom;
    if(!ce_document_apply_json(doc,result_json)) { failure(error,capacity,ce_document_error(doc)); goto done; }
    if(new_id) { *new_id=id; id=NULL; }
    ok=true; goto done;
oom: failure(error,capacity,"Out of memory while inserting citation");
done:
    /* attrs is owned by node after the successful attach above. */
    if(!node) cJSON_Delete(attrs);
    cJSON_Delete(node); cJSON_Delete(root); free(id); free(json); free(node_json); free(result_json);
    ce_document_free(candidate); free(citations.items); free(sections.items); strings_free(&reserved);
    return ok;
}
bool ce_citation_update(CeDocument *doc,const char *id,const char *page,const char *note,char *error,size_t capacity) {
    clear_error(error,capacity);
    if(!doc || !id || !*id) return failure(error,capacity,"A nonempty citationId is required");
    Nodes citations={0},sections={0}; cJSON *root=snapshot(doc,&citations,&sections),*target=NULL; bool ok=false;
    if(!root) { failure(error,capacity,"Unable to copy document"); goto done; }
    for(size_t i=0;i<citations.count;i++) if(!strcmp(text(field(citations.items[i],"attrs"),"citationId"),id)) {
        if(target) { failure(error,capacity,"Duplicate citationId; synchronize references to repair IDs first"); goto done; }
        target=citations.items[i];
    }
    if(!target) { failure(error,capacity,"Citation was not found"); goto done; }
    cJSON *attrs=attributes(target);
    if(!attrs || (page && !put_text(attrs,"page",page)) || (note && !put_text(attrs,"note",note))) {
        failure(error,capacity,"Out of memory while editing citation"); goto done;
    }
    ok=apply(doc,root,error,capacity);
done: cJSON_Delete(root); free(citations.items); free(sections.items); return ok;
}
static char *tagged(const char *tag,const char *value) {
    size_t a=strlen(tag),b=strlen(value);
    if(a>SIZE_MAX-b-1) return NULL;
    char *out=malloc(a+b+1); if(out) { memcpy(out,tag,a); memcpy(out+a,value,b+1); } return out;
}
static bool has_scalar(const cJSON *value) {
    return (cJSON_IsString(value) && *value->valuestring) || cJSON_IsNumber(value);
}
static char *scalar(const cJSON *value) {
    if(cJSON_IsString(value)) return copy(value->valuestring);
    if(cJSON_IsNumber(value)) return print_owned(value);
    return NULL;
}
static char *qualified(const char *tag,const char *library,const char *value) {
    size_t a=strlen(tag),b=strlen(library),c=strlen(value);
    if(a>SIZE_MAX-b || a+b>SIZE_MAX-c || a+b+c>SIZE_MAX-64) return NULL;
    size_t capacity=a+b+c+64;
    char *out=malloc(capacity);
    if(out) snprintf(out,capacity,"%s%zu:%s:%s",tag,b,library,value);
    return out;
}
static char *identity(cJSON *attrs) {
    const char *uri=text(attrs,"itemUri");
    if(*uri) return tagged("uri:",uri);
    cJSON *value=field(attrs,"sourceId");
    if(has_scalar(value)) {
        char *source=scalar(value),*out=NULL;
        if(!source) return NULL;
        cJSON *library_value=field(attrs,"libraryId");
        if(has_scalar(library_value)) {
            char *library=scalar(library_value);
            if(library) out=qualified("library-source:",library,source);
            free(library);
        } else out=tagged("source:",source);
        free(source); return out;
    }
    const char *key=text(attrs,"itemKey");
    value=field(attrs,"libraryId");
    if(has_scalar(value) && *key) {
        char *library=scalar(value);
        if(!library) return NULL;
        char *out=qualified("library:",library,key);
        free(library); return out;
    }
    if(*key) return tagged("key:",key);
    key=text(attrs,"citeKey");
    return *key?tagged("cite:",key):tagged("id:",text(attrs,"citationId"));
}
static char *escape_html(const char *value) {
    size_t size=0;
    for(const unsigned char *p=(const unsigned char*)value;*p;p++) {
        size_t add=*p=='&'?5:*p=='<' || *p=='>'?4:*p=='"'?6:*p=='\''?5:1;
        if(size>SIZE_MAX-add-1) return NULL;
        size+=add;
    }
    char *out=malloc(size+1),*at=out; if(!out) return NULL;
    for(const unsigned char *p=(const unsigned char*)value;*p;p++) {
        const char *escaped=*p=='&'?"&amp;":*p=='<'?"&lt;":*p=='>'?"&gt;":*p=='"'?"&quot;":*p=='\''?"&#39;":NULL;
        if(escaped) { size_t n=strlen(escaped); memcpy(at,escaped,n); at+=n; } else *at++=(char)*p;
    }
    *at=0; return out;
}
static bool simple_section(cJSON *node) {
    cJSON *value;
    cJSON_ArrayForEach(value,node) if(strcmp(value->string,"type") && strcmp(value->string,"attrs")) return false;
    cJSON_ArrayForEach(value,field(node,"attrs"))
        if(strcmp(value->string,"style") && strcmp(value->string,"entriesJson")) return false;
    cJSON *stored=field(field(node,"attrs"),"entriesJson");
    if(stored && !cJSON_IsString(stored)) return false;
    const char *serialized=text(field(node,"attrs"),"entriesJson");
    cJSON *entries=*serialized?cJSON_ParseWithOpts(serialized,NULL,true):cJSON_CreateArray();
    bool simple=cJSON_IsArray(entries);
    cJSON *entry;
    cJSON_ArrayForEach(entry,entries) {
        if(!cJSON_IsObject(entry)) { simple=false; break; }
        cJSON_ArrayForEach(value,entry) {
            const char *key=value->string;
            if(strcmp(key,"citationId") && strcmp(key,"number") && strcmp(key,"entryHtml") &&
               strcmp(key,"key") && strcmp(key,"itemUri") && strcmp(key,"pdfUri") &&
               strcmp(key,"formatting")) { simple=false; break; }
        }
    }
    cJSON_Delete(entries); return simple;
}
static void strip_sections(cJSON *node) {
    if(type_is(node,CITATION) || type_is(node,"paragraph") || type_is(node,"heading")) return;
    cJSON *content=field(node,"content");
    for(int i=cJSON_GetArraySize(content)-1;i>=0;i--) {
        cJSON *child=cJSON_GetArrayItem(content,i);
        if(type_is(child,ENDNOTES)) cJSON_DeleteItemFromArray(content,i);
        else strip_sections(child);
    }
}
static cJSON *old_entry(cJSON *entries,cJSON *attrs) {
    const char *id=text(attrs,"citationId"),*uri=text(attrs,"itemUri"); cJSON *entry;
    cJSON_ArrayForEach(entry,entries) if(cJSON_IsObject(entry) && *id && !strcmp(text(entry,"citationId"),id)) {
        const char *previous_uri=text(entry,"itemUri");
        if(!*previous_uri || (*uri && !strcmp(previous_uri,uri))) return entry;
    }
    if(*uri) cJSON_ArrayForEach(entry,entries) if(cJSON_IsObject(entry) && !strcmp(text(entry,"itemUri"),uri)) return entry;
    return NULL;
}
static cJSON *make_entry(cJSON *attrs,size_t number,CeZoteroApproximateStyle style,cJSON *previous) {
    CeZoteroItem item={0};
    item.item_key=(char*)text(attrs,"itemKey"); item.title=(char*)text(attrs,"title");
    item.author=(char*)text(attrs,"author"); item.year=(char*)text(attrs,"year");
    item.item_uri=(char*)text(attrs,"itemUri"); item.pdf_uri=(char*)text(attrs,"pdfUri");
    item.cite_key=(char*)text(attrs,"citeKey");
    char *plain=ce_zotero_format_approximate(&item,number,style),*html=plain?escape_html(plain):NULL;
    free(plain);
    cJSON *entry=previous?cJSON_Duplicate(previous,true):cJSON_CreateObject();
    if(!entry || !html || !put_text(entry,"citationId",text(attrs,"citationId")) ||
       !put(entry,"number",cJSON_CreateNumber((double)number)) || !put_text(entry,"entryHtml",html) ||
       !put_text(entry,"key",*item.cite_key?item.cite_key:item.item_key) ||
       !put_text(entry,"itemUri",item.item_uri) || !put_text(entry,"pdfUri",item.pdf_uri) ||
       !put_text(entry,"formatting","approximate")) { cJSON_Delete(entry); entry=NULL; }
    free(html); return entry;
}
bool ce_citations_sync_approximate(CeDocument *doc,CeZoteroApproximateStyle style,
                                   CeCitationSyncResult *result,char *error,size_t capacity) {
    clear_error(error,capacity);
    const char *style_name=style==CE_ZOTERO_APPROXIMATE_APA?"apa":style==CE_ZOTERO_APPROXIMATE_IEEE?"ieee":style==CE_ZOTERO_APPROXIMATE_GBT_7714?"gb-t-7714":NULL;
    if(!doc || !style_name) return failure(error,capacity,"Unknown approximate citation style");
    Nodes citations={0},sections={0}; Strings reserved={0},seen={0},identities={0};
    cJSON *root=snapshot(doc,&citations,&sections),*section=NULL,*entries=NULL,*old_entries=NULL;
    CeCitationSyncResult stats={0}; size_t sequence=0; bool ok=false;
    if(!root) goto oom;
    stats.citation_count=citations.count;
    if(!citations.count) {
        stats.removed_sections=sections.count; strip_sections(root);
        ok=apply(doc,root,error,capacity); goto done;
    }
    for(size_t i=1;i<sections.count;i++) if(!simple_section(sections.items[i])) {
        failure(error,capacity,"Extra bibliography section contains custom metadata; merge it explicitly before synchronization"); goto done;
    }
    if(sections.count) {
        section=cJSON_Duplicate(sections.items[0],true);
        cJSON *previous=field(field(sections.items[0],"attrs"),"entriesJson");
        if(previous && !cJSON_IsString(previous)) { failure(error,capacity,"Existing entriesJson must be a JSON array string; original bibliography retained"); goto done; }
        const char *old=text(field(sections.items[0],"attrs"),"entriesJson");
        old_entries=*old?cJSON_ParseWithOpts(old,NULL,true):cJSON_CreateArray();
        if(!old_entries || !cJSON_IsArray(old_entries)) { failure(error,capacity,"Existing entriesJson is malformed; original bibliography retained"); goto done; }
        cJSON *entry;
        cJSON_ArrayForEach(entry,old_entries) if(!cJSON_IsObject(entry)) {
            failure(error,capacity,"Existing bibliography has a malformed entry; original bibliography retained"); goto done;
        }
    } else {
        section=cJSON_CreateObject();
        if(!put_text(section,"type",ENDNOTES)) goto oom;
    }
    entries=cJSON_CreateArray();
    if(!section || !entries || !reserve_ids(&citations,&reserved)) goto oom;
    for(size_t i=0;i<citations.count;i++) {
        cJSON *attrs=attributes(citations.items[i]); if(!attrs) goto oom;
        const char *id=text(attrs,"citationId");
        if(!*id || string_index(&seen,id)!=SIZE_MAX) {
            char *generated=unique_id(&reserved,&sequence);
            if(!generated) goto oom;
            bool assigned=put_text(attrs,"citationId",generated); free(generated);
            if(!assigned) goto oom;
            id=text(attrs,"citationId"); stats.repaired_ids++;
        }
        if(!string_add(&seen,id)) goto oom;
        char *key=identity(attrs); if(!key) goto oom;
        size_t number=string_index(&identities,key);
        if(number==SIZE_MAX) {
            if(!string_add(&identities,key)) { free(key); goto oom; }
            number=identities.count-1;
            cJSON *entry=make_entry(attrs,number+1,style,old_entry(old_entries,attrs));
            if(!entry || !cJSON_AddItemToArray(entries,entry)) { cJSON_Delete(entry); free(key); goto oom; }
        }
        free(key);
        if(!put(attrs,"endnoteNumber",cJSON_CreateNumber((double)(number+1)))) goto oom;
    }
    stats.reference_count=identities.count;
    char *serialized=print_owned(entries);
    cJSON *attrs=attributes(section);
    if(!serialized || !attrs) { free(serialized); goto oom; }
    bool updated=put_text(attrs,"style",style_name) && put_text(attrs,"entriesJson",serialized);
    free(serialized); if(!updated) goto oom;
    strip_sections(root);
    cJSON *content=field(root,"content");
    if(!content) { if(!put(root,"content",cJSON_CreateArray())) goto oom; content=field(root,"content"); }
    if(!cJSON_AddItemToArray(content,section)) goto oom;
    section=NULL;
    stats.removed_sections=sections.count>1?sections.count-1:0;
    ok=apply(doc,root,error,capacity); goto done;
oom: failure(error,capacity,"Unable to construct approximate bibliography");
done:
    if(ok && result) *result=stats;
    cJSON_Delete(section); cJSON_Delete(entries); cJSON_Delete(old_entries); cJSON_Delete(root);
    free(citations.items); free(sections.items); strings_free(&reserved); strings_free(&seen); strings_free(&identities);
    return ok;
}
