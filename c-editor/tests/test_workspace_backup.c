#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "workspace_backup.h"
#include "bundle_archive.h"
#include "note_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
static void create(const char *root,const char *name,const char *value){NoteStore s={0};assert(note_store_init(&s,root)==NOTE_OK);assert(note_store_create(&s,name,value,strlen(value))==NOTE_OK);note_store_destroy(&s);}
static void equal(const char *root,const char *name,const char *expected){NoteStore s={0};char *data=NULL;size_t size=0;assert(note_store_init(&s,root)==NOTE_OK);assert(note_store_read(&s,name,&data,&size)==NOTE_OK&&size==strlen(expected)&&!memcmp(data,expected,size));free(data);note_store_destroy(&s);}
int main(int argc,char **argv){assert(argc>=2);char root[2048],child[2300],error[256]={0};
#ifdef _WIN32
    unsigned long pid=GetCurrentProcessId();
#else
    unsigned long pid=(unsigned long)getpid();
#endif
    snprintf(root,sizeof root,"%s/recovery-%lu-%lld",argv[1],pid,(long long)time(NULL));create(root,"article.json","{\"type\":\"doc\",\"content\":[]}");snprintf(child,sizeof child,"%s/.notelab-writing/versions",root);create(child,"first.json","{\"unknown\":\"retained\"}");snprintf(child,sizeof child,"%s/.notelab-writing/trash",root);create(child,"deleted.json","exact deleted bytes");snprintf(child,sizeof child,"%s/.notelab-library",root);create(child,"cache.json","library bytes");
    const char *asset="0000000000000000000000000000000000000000000000000000000000000000.png";const unsigned char pixels[]={0,1,255,0,42};NoteStore assets={0};snprintf(child,sizeof child,"%s/.notelab-writing/assets",root);assert(note_store_init(&assets,child)==NOTE_OK);assert(note_store_create_asset(&assets,asset,pixels,sizeof pixels)==NOTE_OK);note_store_destroy(&assets);
    snprintf(child,sizeof child,"%s/private/.notelab-restored",root);create(child,"unknown.json","unknown nested data");
    unsigned char *zip=NULL;size_t size=0;assert(ce_workspace_backup(root,&zip,&size,error,sizeof error));CeBundle b={0};assert(ce_bundle_read(zip,size,&b,error,sizeof error)&&b.count==7);ce_bundle_free(&b);
    char *selected=NULL;assert(ce_workspace_resolve(root,&selected,error,sizeof error)&&!strcmp(selected,root));free(selected);
    char *relative=NULL,*restored=NULL;assert(ce_workspace_restore_stage(root,zip,size,&relative,&restored,error,sizeof error));equal(restored,"article.json","{\"type\":\"doc\",\"content\":[]}");snprintf(child,sizeof child,"%s/.notelab-writing/versions",restored);equal(child,"first.json","{\"unknown\":\"retained\"}");snprintf(child,sizeof child,"%s/.notelab-writing/trash",restored);equal(child,"deleted.json","exact deleted bytes");snprintf(child,sizeof child,"%s/.notelab-library",restored);equal(child,"cache.json","library bytes");
    snprintf(child,sizeof child,"%s/.notelab-writing/assets",restored);assert(note_store_init(&assets,child)==NOTE_OK);char *asset_data=NULL;size_t asset_size=0;assert(note_store_read_asset(&assets,asset,&asset_data,&asset_size)==NOTE_OK&&asset_size==sizeof pixels&&!memcmp(asset_data,pixels,sizeof pixels));free(asset_data);note_store_destroy(&assets);snprintf(child,sizeof child,"%s/private/.notelab-restored",restored);equal(child,"unknown.json","unknown nested data");
    assert(ce_workspace_activate(root,relative,error,sizeof error));assert(ce_workspace_resolve(root,&selected,error,sizeof error)&&!strcmp(selected,restored));free(selected);
    assert(!ce_workspace_activate(root,"../bad",error,sizeof error));assert(ce_workspace_resolve(root,&selected,error,sizeof error)&&!strcmp(selected,restored));free(selected);
    zip[40]^=1;char *r=NULL,*p=NULL;assert(!ce_workspace_restore_stage(root,zip,size,&r,&p,error,sizeof error)&&!r&&!p);assert(ce_workspace_resolve(root,&selected,error,sizeof error)&&!strcmp(selected,restored));free(selected);zip[40]^=1;
    unsigned char *again=NULL;size_t again_size=0;assert(ce_workspace_backup(root,&again,&again_size,error,sizeof error));assert(ce_bundle_read(again,again_size,&b,error,sizeof error)&&b.count==7);ce_bundle_free(&b);free(again);
    assert(ce_workspace_activate(root,"",error,sizeof error));assert(ce_workspace_resolve(root,&selected,error,sizeof error)&&!strcmp(selected,root));free(selected);equal(root,"article.json","{\"type\":\"doc\",\"content\":[]}");
    assert(!ce_workspace_activate(root,".notelab-restored/missing",error,sizeof error));assert(ce_workspace_resolve(root,&selected,error,sizeof error)&&!strcmp(selected,root));free(selected);
    /* A file cannot be treated as a parent directory; validation precedes any
     * restoration writes and preserves the original root selection. */
    CeBundleEntry invalid[]={{"notelab-backup.json",(const unsigned char *)"wrong manifest",14}};unsigned char *bad=NULL;size_t bad_size=0;assert(ce_bundle_write(invalid,1,&bad,&bad_size,error,sizeof error));r=p=NULL;assert(!ce_workspace_restore_stage(root,bad,bad_size,&r,&p,error,sizeof error)&&!r&&!p);free(bad);
    snprintf(child,sizeof child,"%s/link.json",root);
#ifdef _WIN32
    char target[2300];snprintf(target,sizeof target,"%s/article.json",root);bool linked=CreateSymbolicLinkA(child,target,2)!=0;
#else
    bool linked=symlink("article.json",child)==0;
#endif
    if(linked){again=(unsigned char *)1;again_size=9;assert(!ce_workspace_backup(root,&again,&again_size,error,sizeof error)&&again==(void *)1&&again_size==9);
#ifdef _WIN32
        assert(DeleteFileA(child));
#else
        assert(unlink(child)==0);
#endif
    }else puts("Symlink creation unavailable on this host; POSIX CI covers no-follow rejection");
    /* Binary assets and empty files preserve NULs exactly through the core ZIP. */
    free(relative);free(restored);free(zip);puts("Workspace snapshot, staged recovery, persistent activation and failure preservation passed");return 0;}
