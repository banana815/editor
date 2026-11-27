#include "writing_store.h"
#include "cJSON.h"
#include "csl_json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <crtdbg.h>
#endif
static void clear(NoteStore *s) { NoteList list={0}; assert(note_store_list(s,&list)==NOTE_OK); for(size_t i=0;i<list.count;i++) assert(note_store_delete(s,list.names[i])==NOTE_OK); note_store_list_free(&list); }
static size_t allocation_calls,allocation_fail;
static void *test_allocate(size_t size) { ++allocation_calls; return allocation_fail && allocation_calls>=allocation_fail?NULL:malloc(size); }
static void test_restore_failures(CeWritingStore *store,NoteStore *notes,const char *id,const char *root) {
    assert(ce_writing_trash_restore(store,notes,id,"fault-restore.json"));
    assert(note_store_delete(notes,"fault-restore.json")==NOTE_OK);
    cJSON_Hooks hooks={test_allocate,free}; allocation_calls=allocation_fail=0; cJSON_InitHooks(&hooks);
    assert(ce_writing_trash_restore(store,notes,id,"fault-restore.json"));
    size_t calls=allocation_calls; cJSON_InitHooks(NULL); assert(calls>20);
    assert(note_store_delete(notes,"fault-restore.json")==NOTE_OK);
    for(size_t point=1;point<=calls;point++) {
        allocation_calls=0; allocation_fail=point; cJSON_InitHooks(&hooks);
        bool restored=ce_writing_trash_restore(store,notes,id,"fault-restore.json");
        cJSON_InitHooks(NULL); allocation_fail=0;
        assert(!restored); char *data=NULL; size_t size=0;
        assert(note_store_read(notes,"fault-restore.json",&data,&size)==NOTE_NOT_FOUND);
    }
    /* Post-create disk failure rolls back the new article, not the archive. */
    note_store_destroy(&store->catalog_store);
    assert(!ce_writing_trash_restore(store,notes,id,"rollback.json"));
    char *data=NULL; size_t size=0;
    assert(note_store_read(notes,"rollback.json",&data,&size)==NOTE_NOT_FOUND);
    char path[1024]; snprintf(path,sizeof path,"%s/.notelab-writing",root);
    assert(note_store_init(&store->catalog_store,path)==NOTE_OK);
    char *name=NULL; assert(ce_writing_archive_read(store,true,id,&name,&data)); free(name); free(data);
}
static void test_archive_bounds(CeWritingStore *store,NoteStore *notes) {
    assert(!ce_writing_snapshot(store,"bounds.json",NULL,10));
    assert(!ce_writing_snapshot(store,NULL,"text",10));
    assert(!ce_writing_snapshot(store,"bounds.json","text",-1));
    assert(!ce_writing_snapshot(store,"bounds.json","text",INT64_C(9007199254740992)));
    size_t length=16u*1024u*1024u;
    char *large=malloc(length+2); assert(large); memset(large,1,length+1); large[length]=0;
    assert(note_store_create(notes,"bounds.json",large,length)==NOTE_OK);
    assert(ce_writing_trash_note(store,notes,"bounds.json",INT64_C(9007199254740991)));
    CeArchiveList list={0}; assert(ce_writing_archive_list(store,true,"bounds.json",&list) && list.count==1);
    char *owner=NULL,*data=NULL;
    assert(ce_writing_archive_read(store,true,list.entries[0].id,&owner,&data));
    assert(strlen(data)==length && !memcmp(data,large,length)); free(owner); free(data);
    assert(ce_writing_trash_restore(store,notes,list.entries[0].id,NULL));
    size_t size=0; assert(note_store_read(notes,"bounds.json",&data,&size)==NOTE_OK && size==length && !memcmp(data,large,length)); free(data);
    large[length]=1; large[length+1]=0;
    assert(note_store_save(notes,"bounds.json",large,length+1)==NOTE_OK);
    assert(!ce_writing_trash_note(store,notes,"bounds.json",100));
    assert(note_store_read(notes,"bounds.json",&data,&size)==NOTE_OK && size==length+1); free(data);
    const char invalid[]={(char)0xff}; assert(note_store_save(notes,"bounds.json",invalid,sizeof invalid)==NOTE_OK);
    assert(!ce_writing_trash_note(store,notes,"bounds.json",100));
    assert(note_store_read(notes,"bounds.json",&data,&size)==NOTE_OK && size==1 && (unsigned char)data[0]==0xff); free(data);
    assert(note_store_delete(notes,"bounds.json")==NOTE_OK);
    assert(note_store_delete(&store->trash,list.entries[0].id)==NOTE_OK); ce_writing_archive_list_free(&list); free(large);
    const char *bad[]={"{\"name\":\"bad.json\",\"data\":\"x\",\"time\":1.5}",
        "{\"name\":\"bad.json\",\"data\":\"x\\u0000hidden\",\"time\":1}",
        "{\"name\":\"bad.json\",\"data\":\"x\",\"time\":9007199254740992}"};
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++) {
        assert(note_store_create(&store->trash,"invalid.json",bad[i],strlen(bad[i]))==NOTE_OK);
        owner=data=NULL; assert(!ce_writing_archive_read(store,true,"invalid.json",&owner,&data) && !owner && !data);
        assert(note_store_delete(&store->trash,"invalid.json")==NOTE_OK);
    }
}
static void test_catalog_validation(CeWritingStore *store,const char *root) {
    char *valid=NULL; size_t size=0; assert(note_store_read(&store->catalog_store,"catalog.json",&valid,&size)==NOTE_OK);
    const char *fields[]={"\"folder\":3","\"tags\":null","\"pinned\":1","\"goal\":1.5","\"status\":\"draft\""};
    for(size_t i=0;i<sizeof fields/sizeof *fields;i++) {
        char bad[256]; snprintf(bad,sizeof bad,"{\"version\":1,\"articles\":{\"bad.json\":{%s}},\"folders\":[]}",fields[i]);
        assert(note_store_save(&store->catalog_store,"catalog.json",bad,strlen(bad))==NOTE_OK); ce_writing_store_close(store);
        assert(!ce_writing_store_open(store,root));
        char path[1024]; snprintf(path,sizeof path,"%s/.notelab-writing",root); NoteStore repair={0};
        assert(note_store_init(&repair,path)==NOTE_OK); char *unchanged=NULL; size_t n=0;
        assert(note_store_read(&repair,"catalog.json",&unchanged,&n)==NOTE_OK && !strcmp(unchanged,bad)); free(unchanged);
        assert(note_store_save(&repair,"catalog.json",valid,size)==NOTE_OK); note_store_destroy(&repair);
        assert(ce_writing_store_open(store,root));
    }
    free(valid);
}
int main(int argc,char **argv) {
#ifdef _WIN32
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    assert(argc==2); NoteStore notes={0}; assert(note_store_init(&notes,argv[1])==NOTE_OK);
    CeWritingStore store={0}; assert(ce_writing_store_open(&store,argv[1]));
    clear(&notes); clear(&store.versions); clear(&store.trash);
    CeArticleInfo info={0},read={0}; strcpy(info.folder,"博客文章"); strcpy(info.tags,"写作, C语言"); info.pinned=true; info.status=1; info.goal=1200;
    assert(ce_writing_article_set(&store,"文章.json",&info)); assert(ce_writing_article_get(&store,"文章.json",&read));
    assert(!strcmp(info.folder,read.folder)&&read.pinned&&read.status==1&&read.goal==1200);
    assert(ce_writing_folder_add(&store,"空文件夹")); assert(ce_writing_folder_count(&store)>=2);
    char *catalog=NULL;size_t bytes=0;assert(note_store_read(&store.catalog_store,"catalog.json",&catalog,&bytes)==NOTE_OK);
    cJSON *json=cJSON_Parse(catalog);free(catalog);assert(json);
    cJSON *item=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(json,"articles"),"文章.json");
    assert(cJSON_AddStringToObject(item,"future","保留"));
    assert(cJSON_AddNumberToObject(item,"futureNumber",4503599627370501.0));
    catalog=ce_csl_json_print_exact(json);cJSON_Delete(json);assert(catalog);
    assert(note_store_save(&store.catalog_store,"catalog.json",catalog,strlen(catalog))==NOTE_OK);cJSON_free(catalog);
    ce_writing_store_close(&store);assert(ce_writing_store_open(&store,argv[1]));info.goal=2400;assert(ce_writing_article_set(&store,"文章.json",&info));
    assert(note_store_read(&store.catalog_store,"catalog.json",&catalog,&bytes)==NOTE_OK);assert(strstr(catalog,"future")&&strstr(catalog,"保留")&&strstr(catalog,"4503599627370501"));free(catalog);
    test_catalog_validation(&store,argv[1]);
    strcpy(info.folder,"../bad");assert(!ce_writing_article_set(&store,"文章.json",&info));assert(ce_writing_article_get(&store,"文章.json",&read)&&read.goal==2400);
    const char *first="<p>original 中文🙂</p>",*second="<p>revised</p>";
    assert(note_store_create(&notes,"文章.json",first,strlen(first))==NOTE_OK);
    assert(ce_writing_snapshot(&store,"文章.json",first,10));assert(ce_writing_snapshot(&store,"文章.json",first,11));assert(ce_writing_snapshot(&store,"文章.json",second,20));
    CeArchiveList versions={0};assert(ce_writing_archive_list(&store,false,"文章.json",&versions)&&versions.count==2&&versions.entries[0].timestamp==20);
    char *name=NULL,*data=NULL;assert(ce_writing_archive_read(&store,false,versions.entries[1].id,&name,&data));assert(!strcmp(name,"文章.json")&&!strcmp(data,first));free(name);free(data);
    assert(ce_writing_trash_note(&store,&notes,"文章.json",30));assert(note_store_read(&notes,"文章.json",&data,&bytes)==NOTE_NOT_FOUND);
    CeArchiveList trash={0};assert(ce_writing_archive_list(&store,true,NULL,&trash)&&trash.count==1);
    assert(ce_writing_trash_restore(&store,&notes,trash.entries[0].id,NULL));assert(note_store_read(&notes,"文章.json",&data,&bytes)==NOTE_OK&&!strcmp(data,first));free(data);
    assert(!ce_writing_trash_restore(&store,&notes,trash.entries[0].id,NULL));assert(note_store_read(&notes,"文章.json",&data,&bytes)==NOTE_OK&&!strcmp(data,first));free(data);
    assert(ce_writing_trash_restore(&store,&notes,trash.entries[0].id,"恢复副本.json"));assert(ce_writing_article_get(&store,"恢复副本.json",&read)&&read.goal==2400&&read.pinned);
    test_restore_failures(&store,&notes,trash.entries[0].id,argv[1]);
    test_archive_bounds(&store,&notes);
    note_store_destroy(&store.trash);assert(!ce_writing_trash_note(&store,&notes,"文章.json",40));
    assert(note_store_read(&notes,"文章.json",&data,&bytes)==NOTE_OK&&!strcmp(data,first));free(data);
    ce_writing_archive_list_free(&versions);ce_writing_archive_list_free(&trash);
    assert(note_store_save(&store.catalog_store,"catalog.json","{broken",7)==NOTE_OK);ce_writing_store_close(&store);
    assert(!ce_writing_store_open(&store,argv[1]));
    /* Restore only the intentionally corrupted fixture for repeatable runs. */
    char path[1024];snprintf(path,sizeof path,"%s/.notelab-writing",argv[1]);NoteStore cleanup={0};assert(note_store_init(&cleanup,path)==NOTE_OK);assert(note_store_delete(&cleanup,"catalog.json")==NOTE_OK);note_store_destroy(&cleanup);
    ce_writing_store_close(&store);note_store_destroy(&notes);puts("Writing catalog, snapshots, trash and failure preservation passed");return 0;
}
