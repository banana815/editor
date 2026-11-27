#include "writing_store.h"
#include "cJSON.h"
#include "csl_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define ARTICLE_LIMIT (16u*1024u*1024u)
#define CATALOG_LIMIT (16u*1024u*1024u)
/* A 16 MiB article may need six bytes per escaped control character. Metadata
 * is an object, not another escaped string. Readers and writers share this cap. */
#define ARCHIVE_LIMIT (6u*ARTICLE_LIMIT+CATALOG_LIMIT+1024u*1024u)
#define MAX_TIMESTAMP INT64_C(9007199254740991)
static char *duplicate(const char *s) { size_t n=strlen(s)+1; char *p=malloc(n); if(p) memcpy(p,s,n); return p; }
static bool fail(CeWritingStore *s,const char *error) { snprintf(s->error,sizeof s->error,"%s",error&&*error?error:"Writing storage failed"); return false; }
static const char *str(const cJSON *o,const char *key) { cJSON *n=cJSON_GetObjectItemCaseSensitive(o,key); return cJSON_IsString(n)?n->valuestring:NULL; }
static bool valid_label(const char *value,size_t maximum,bool folder) {
    if(!value || strlen(value)>=maximum) return false;
    for(const unsigned char *p=(const unsigned char *)value;*p;p++) if(*p<32 || *p==127 || (folder && (*p=='\\' || *p=='/'))) return false;
    return !folder || (strcmp(value,".") && strcmp(value,".."));
}
static bool integer(const cJSON *value,double maximum) {
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) && value->valuedouble>=0 &&
        value->valuedouble<=maximum && floor(value->valuedouble)==value->valuedouble;
}
static bool article_decode(const cJSON *item,CeArticleInfo *out) {
    if(!cJSON_IsObject(item)) return false;
    CeArticleInfo info={0};
    cJSON *folder=cJSON_GetObjectItemCaseSensitive(item,"folder"),*tags=cJSON_GetObjectItemCaseSensitive(item,"tags"),
        *status=cJSON_GetObjectItemCaseSensitive(item,"status"),*goal=cJSON_GetObjectItemCaseSensitive(item,"goal"),
        *pinned=cJSON_GetObjectItemCaseSensitive(item,"pinned");
    if((folder && (!cJSON_IsString(folder) || !valid_label(folder->valuestring,sizeof info.folder,true))) ||
       (tags && (!cJSON_IsString(tags) || !valid_label(tags->valuestring,sizeof info.tags,false))) ||
       (status && !integer(status,2)) || (goal && !integer(goal,10000000)) ||
       (pinned && !cJSON_IsBool(pinned))) return false;
    if(folder) snprintf(info.folder,sizeof info.folder,"%s",folder->valuestring);
    if(tags) snprintf(info.tags,sizeof info.tags,"%s",tags->valuestring);
    if(status) info.status=(unsigned)status->valuedouble;
    if(goal) info.goal=(size_t)goal->valuedouble;
    info.pinned=cJSON_IsTrue(pinned); if(out) *out=info; return true;
}
static bool catalog_valid(const cJSON *catalog) {
    cJSON *version=cJSON_GetObjectItemCaseSensitive(catalog,"version"),
        *articles=cJSON_GetObjectItemCaseSensitive(catalog,"articles"),
        *folders=cJSON_GetObjectItemCaseSensitive(catalog,"folders"),*entry;
    if(!cJSON_IsObject(catalog) || !integer(version,1) || version->valuedouble!=1 ||
       !cJSON_IsObject(articles) || !cJSON_IsArray(folders)) return false;
    cJSON_ArrayForEach(entry,articles) if(!note_store_valid_name(entry->string) || !article_decode(entry,NULL)) return false;
    cJSON_ArrayForEach(entry,folders) if(!cJSON_IsString(entry) || !*entry->valuestring || !valid_label(entry->valuestring,256,true)) return false;
    return true;
}
static char *prepare_json(CeWritingStore *s,const cJSON *value,size_t limit) {
    char *bytes=ce_csl_json_print_exact_with_limit(value,limit); cJSON *check=NULL;
    if(!bytes || !ce_csl_json_parse_strict_with_limit(bytes,strlen(bytes),limit,&check,s->error,sizeof s->error)) {
        cJSON_free(bytes); fail(s,"Cannot prepare bounded valid recovery JSON; original preserved"); return NULL;
    }
    cJSON_Delete(check); return bytes;
}
static bool open_child(NoteStore *out,const char *root,const char *child) {
    size_t n=strlen(root)+strlen(child)+2; char *path=malloc(n); if(!path) return false;
    snprintf(path,n,"%s/%s",root,child); bool ok=note_store_init(out,path)==NOTE_OK; free(path); return ok;
}
static bool commit_catalog(CeWritingStore *s,cJSON *candidate,char *bytes) {
    NoteResult r=note_store_save(&s->catalog_store,"catalog.json",bytes,strlen(bytes));
    if(r==NOTE_NOT_FOUND) r=note_store_create(&s->catalog_store,"catalog.json",bytes,strlen(bytes));
    cJSON_free(bytes);
    if(r!=NOTE_OK) { cJSON_Delete(candidate); return fail(s,s->catalog_store.error); }
    cJSON_Delete(s->catalog); s->catalog=candidate; s->error[0]=0; return true;
}
static bool save_catalog(CeWritingStore *s,cJSON *candidate) {
    char *bytes=catalog_valid(candidate)?prepare_json(s,candidate,CATALOG_LIMIT):NULL;
    if(!bytes) { cJSON_Delete(candidate); return fail(s,"Invalid or unallocatable article catalog; original preserved"); }
    return commit_catalog(s,candidate,bytes);
}
bool ce_writing_store_open(CeWritingStore *s,const char *root) {
    if(!s || !root) return false;
    if(s->ready) return true;
    ce_writing_store_close(s);
    if(!open_child(&s->catalog_store,root,".notelab-writing") ||
       !open_child(&s->versions,root,".notelab-writing/versions") ||
       !open_child(&s->trash,root,".notelab-writing/trash")) {
        ce_writing_store_close(s); return fail(s,"Cannot open article history directory");
    }
    char *bytes=NULL; size_t length=0;
    NoteResult r=note_store_read(&s->catalog_store,"catalog.json",&bytes,&length);
    cJSON *catalog=NULL;
    if(r==NOTE_NOT_FOUND) {
        catalog=cJSON_CreateObject();
        if(!catalog || !cJSON_AddNumberToObject(catalog,"version",1) || !cJSON_AddObjectToObject(catalog,"articles") || !cJSON_AddArrayToObject(catalog,"folders")) { cJSON_Delete(catalog); catalog=NULL; }
    } else if(r==NOTE_OK) ce_csl_json_parse_strict(bytes,length,&catalog,s->error,sizeof s->error);
    note_store_free(bytes);
    if(!catalog_valid(catalog)) {
        cJSON_Delete(catalog); ce_writing_store_close(s); return fail(s,"Article catalog is unreadable; original files preserved");
    }
    s->catalog=catalog; s->ready=true; return true;
}
void ce_writing_store_close(CeWritingStore *s) {
    if(!s) return;
    cJSON_Delete(s->catalog); s->catalog=NULL;
    note_store_destroy(&s->catalog_store); note_store_destroy(&s->versions); note_store_destroy(&s->trash);
    s->ready=false;
}
bool ce_writing_article_get(CeWritingStore *s,const char *name,CeArticleInfo *out) {
    if(!s || !s->ready || !out || !note_store_valid_name(name)) return false;
    cJSON *item=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(s->catalog,"articles"),name);
    if(!item) { memset(out,0,sizeof *out); return true; }
    return article_decode(item,out)?true:fail(s,"Invalid article properties; preserved unchanged");
}
static bool set_field(cJSON *item,const char *key,cJSON *value) {
    if(!value) return false;
    bool ok=cJSON_HasObjectItem(item,key)?cJSON_ReplaceItemInObjectCaseSensitive(item,key,value):cJSON_AddItemToObject(item,key,value);
    if(!ok) cJSON_Delete(value); return ok;
}
static bool add_folder(cJSON *catalog,const char *folder) {
    cJSON *folders=cJSON_GetObjectItemCaseSensitive(catalog,"folders"),*entry;
    cJSON_ArrayForEach(entry,folders) if(cJSON_IsString(entry) && !strcmp(entry->valuestring,folder)) return true;
    if(!*folder) return true;
    cJSON *value=cJSON_CreateString(folder); if(!value) return false;
    if(!cJSON_AddItemToArray(folders,value)) { cJSON_Delete(value); return false; } return true;
}
bool ce_writing_article_set(CeWritingStore *s,const char *name,const CeArticleInfo *info) {
    if(!s || !s->ready) return false;
    if(!note_store_valid_name(name) || !info || !valid_label(info->folder,sizeof info->folder,true) || !valid_label(info->tags,sizeof info->tags,false) || info->status>2 || info->goal>10000000) return fail(s,"Invalid article properties");
    cJSON *next=cJSON_Duplicate(s->catalog,true),*articles=next?cJSON_GetObjectItemCaseSensitive(next,"articles"):NULL;
    cJSON *item=articles?cJSON_GetObjectItemCaseSensitive(articles,name):NULL;
    if(!item && articles) item=cJSON_AddObjectToObject(articles,name);
    if(!cJSON_IsObject(item) || !set_field(item,"folder",cJSON_CreateString(info->folder)) ||
       !set_field(item,"tags",cJSON_CreateString(info->tags)) || !set_field(item,"status",cJSON_CreateNumber(info->status)) ||
       !set_field(item,"goal",cJSON_CreateNumber((double)info->goal)) || !set_field(item,"pinned",cJSON_CreateBool(info->pinned)) || !add_folder(next,info->folder)) {
        cJSON_Delete(next); return fail(s,"Cannot prepare article properties; original preserved");
    }
    return save_catalog(s,next);
}
bool ce_writing_folder_add(CeWritingStore *s,const char *folder) {
    if(!s || !s->ready) return false;
    if(!valid_label(folder,256,true) || !*folder) return fail(s,"Enter a folder name without path separators");
    cJSON *next=cJSON_Duplicate(s->catalog,true);
    if(!next || !add_folder(next,folder)) { cJSON_Delete(next); return fail(s,"Cannot create folder"); }
    return save_catalog(s,next);
}
size_t ce_writing_folder_count(const CeWritingStore *s) { return s&&s->ready?(size_t)cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(s->catalog,"folders")):0; }
const char *ce_writing_folder_at(const CeWritingStore *s,size_t i) {
    cJSON *item=s&&s->ready?cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(s->catalog,"folders"),(int)i):NULL;
    return cJSON_IsString(item)?item->valuestring:"";
}
static cJSON *archive_load(CeWritingStore *s,bool trash,const char *id) {
    NoteStore *files=trash?&s->trash:&s->versions; char *bytes=NULL; size_t length=0;
    if(note_store_read(files,id,&bytes,&length)!=NOTE_OK) { fail(s,files->error); return NULL; }
    cJSON *record=NULL;
    ce_csl_json_parse_strict_with_limit(bytes,length,ARCHIVE_LIMIT,&record,s->error,sizeof s->error); note_store_free(bytes);
    const char *name=record?str(record,"name"):NULL,*data=record?str(record,"data"):NULL;
    cJSON *stamp=record?cJSON_GetObjectItemCaseSensitive(record,"time"):NULL;
    cJSON *article=record?cJSON_GetObjectItemCaseSensitive(record,"article"):NULL;
    if(!cJSON_IsObject(record) || !name || !data || strlen(data)>ARTICLE_LIMIT || !note_store_valid_name(name) ||
       !integer(stamp,(double)MAX_TIMESTAMP) || (article && !article_decode(article,NULL))) {
        cJSON_Delete(record); fail(s,"Recovery record is invalid; preserved unchanged"); return NULL;
    }
    return record;
}
void ce_writing_archive_list_free(CeArchiveList *list) {
    if(!list) return;
    for(size_t i=0;i<list->count;i++) { free(list->entries[i].id); free(list->entries[i].name); }
    free(list->entries); memset(list,0,sizeof *list);
}
static int newer(const void *a,const void *b) {
    const CeArchiveEntry *x=a,*y=b;
    if(x->timestamp!=y->timestamp) return x->timestamp<y->timestamp?1:-1;
    return strcmp(y->id,x->id);
}
bool ce_writing_archive_list(CeWritingStore *s,bool trash,const char *name,CeArchiveList *out) {
    if(!s || !s->ready || !out) return false;
    NoteStore *files=trash?&s->trash:&s->versions; NoteList names={0}; CeArchiveList next={0};
    if(note_store_list(files,&names)!=NOTE_OK) return fail(s,files->error);
    next.entries=calloc(names.count?names.count:1,sizeof *next.entries);
    bool ok=next.entries!=NULL;
    for(size_t i=0;ok && i<names.count;i++) {
        cJSON *record=archive_load(s,trash,names.names[i]);
        if(!record) { ok=false; break; }
        const char *owner=str(record,"name");
        if(!name || !strcmp(name,owner)) {
            CeArchiveEntry *entry=&next.entries[next.count++];
            entry->id=duplicate(names.names[i]); entry->name=duplicate(owner);
            entry->timestamp=(int64_t)cJSON_GetObjectItemCaseSensitive(record,"time")->valuedouble;
            ok=entry->id && entry->name;
        }
        cJSON_Delete(record);
    }
    note_store_list_free(&names);
    if(!ok) { ce_writing_archive_list_free(&next); return fail(s,"Cannot list recovery records; original files preserved"); }
    qsort(next.entries,next.count,sizeof *next.entries,newer);
    ce_writing_archive_list_free(out); *out=next; return true;
}
bool ce_writing_archive_read(CeWritingStore *s,bool trash,const char *id,char **name,char **data) {
    if(!s || !s->ready || !name || !data) return false;
    cJSON *record=archive_load(s,trash,id); if(!record) return false;
    char *n=duplicate(str(record,"name")),*d=duplicate(str(record,"data")); cJSON_Delete(record);
    if(!n || !d) { free(n); free(d); return fail(s,"Out of memory; recovery unchanged"); }
    *name=n; *data=d; return true;
}
static bool archive_write(CeWritingStore *s,bool trash,const char *name,const char *data,int64_t now) {
    if(!note_store_valid_name(name) || !data || strlen(data)>ARTICLE_LIMIT || now<0 || now>MAX_TIMESTAMP) return fail(s,"Invalid recovery data");
    cJSON *record=cJSON_CreateObject();
    if(!record || !cJSON_AddStringToObject(record,"name",name) || !cJSON_AddStringToObject(record,"data",data) || !cJSON_AddNumberToObject(record,"time",(double)now)) { cJSON_Delete(record); return fail(s,"Out of memory; original preserved"); }
    cJSON *meta=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(s->catalog,"articles"),name);
    if(meta && (!article_decode(meta,NULL) || !set_field(record,"article",cJSON_Duplicate(meta,true)))) { cJSON_Delete(record); return fail(s,"Cannot preserve article properties"); }
    char *bytes=prepare_json(s,record,ARCHIVE_LIMIT); cJSON_Delete(record);
    if(!bytes) return fail(s,"Out of memory; original preserved");
    NoteStore *files=trash?&s->trash:&s->versions; NoteResult result=NOTE_EXISTS;
    for(unsigned attempt=0;attempt<10000 && result==NOTE_EXISTS;attempt++) {
        char id[80]; snprintf(id,sizeof id,"%s-%020lld-%010u.json",trash?"trash":"version",(long long)now,++s->sequence);
        result=note_store_create(files,id,bytes,strlen(bytes));
    }
    cJSON_free(bytes); return result==NOTE_OK?true:fail(s,files->error);
}
bool ce_writing_snapshot(CeWritingStore *s,const char *name,const char *data,int64_t now) {
    if(!s || !s->ready) return false;
    if(!note_store_valid_name(name) || !data || strlen(data)>ARTICLE_LIMIT || now<0 || now>MAX_TIMESTAMP) return fail(s,"Invalid recovery data");
    CeArchiveList list={0}; if(!ce_writing_archive_list(s,false,name,&list)) return false;
    if(list.count) {
        char *owner=NULL,*previous=NULL;
        bool ok=ce_writing_archive_read(s,false,list.entries[0].id,&owner,&previous);
        if(!ok) { ce_writing_archive_list_free(&list); return false; }
        bool same=!strcmp(previous,data); free(owner); free(previous);
        if(same) { ce_writing_archive_list_free(&list); return true; }
    }
    ce_writing_archive_list_free(&list); return archive_write(s,false,name,data,now);
}
bool ce_writing_trash_note(CeWritingStore *s,NoteStore *notes,const char *name,int64_t now) {
    if(!s || !s->ready) return false;
    char *data=NULL; size_t length=0;
    if(note_store_read(notes,name,&data,&length)!=NOTE_OK) return fail(s,notes->error);
    bool ok=memchr(data,0,length)?fail(s,"Binary NUL data cannot be archived; original preserved"):archive_write(s,true,name,data,now); note_store_free(data);
    if(!ok) return false;
    if(note_store_delete(notes,name)!=NOTE_OK) return fail(s,notes->error);
    return true;
}
bool ce_writing_trash_restore(CeWritingStore *s,NoteStore *notes,const char *id,const char *new_name) {
    if(!s || !s->ready) return false;
    cJSON *record=archive_load(s,true,id); if(!record) return false;
    const char *name=new_name?new_name:str(record,"name"),*data=str(record,"data");
    if(!note_store_valid_name(name)) { cJSON_Delete(record); return fail(s,"Invalid restore name; recovery retained"); }
    /* Prepare and validate all metadata before creating a new note. A later
     * catalog I/O failure rolls back that newly created note, never the archive. */
    cJSON *meta=cJSON_GetObjectItemCaseSensitive(record,"article");
    cJSON *next=cJSON_Duplicate(s->catalog,true),*articles=next?cJSON_GetObjectItemCaseSensitive(next,"articles"):NULL;
    bool prepared=articles!=NULL;
    if(prepared && meta) prepared=set_field(articles,name,cJSON_Duplicate(meta,true));
    else if(prepared) cJSON_DeleteItemFromObjectCaseSensitive(articles,name);
    if(prepared && meta) { const char *folder=str(meta,"folder"); if(folder) prepared=add_folder(next,folder); }
    char *bytes=prepared && catalog_valid(next)?prepare_json(s,next,CATALOG_LIMIT):NULL;
    if(!bytes) { cJSON_Delete(next); cJSON_Delete(record); return fail(s,"Cannot prepare restored properties; original and recovery unchanged"); }
    NoteResult r=note_store_create(notes,name,data,strlen(data));
    if(r!=NOTE_OK) { cJSON_free(bytes); cJSON_Delete(next); cJSON_Delete(record); return fail(s,notes->error); }
    if(!commit_catalog(s,next,bytes)) {
        char problem[256]; snprintf(problem,sizeof problem,"%s",s->error);
        bool rolled_back=note_store_delete(notes,name)==NOTE_OK;
        cJSON_Delete(record);
        if(!rolled_back) return fail(s,"Article restored but properties failed and rollback failed; recovered article and recovery copy retained");
        char message[256]; snprintf(message,sizeof message,"Restore rolled back; recovery retained: %.190s",problem);
        return fail(s,message);
    }
    /* Keep the recovery record after restore, including browser flush failure. */
    cJSON_Delete(record); return true;
}
