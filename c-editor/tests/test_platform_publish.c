/* The save dialog is a fixture; the production C writer uses a temporary
 * isolated directory. No actual UI or user files/clipboard are accessed. */
#include "platform_export.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char *selected_path,*expected_filter;
static bool canceled;
static void SDLCALL test_save_dialog(SDL_DialogFileCallback callback,void *userdata,SDL_Window *window,
                                    const SDL_DialogFileFilter *filters,int count,const char *filename) {
    (void)window; assert(filename && count==1 && !strcmp(filters[0].pattern,expected_filter));
    const char *paths[]={canceled?NULL:selected_path,NULL}; callback(userdata,paths,0);
}
#define SDL_ShowSaveFileDialog test_save_dialog
#include "../src/platform_export.c"
#undef SDL_ShowSaveFileDialog

static void contents(const char *path,const char *expected) {
    size_t size=0; char *text=SDL_LoadFile(path,&size);
    assert(text && size==strlen(expected) && !strcmp(text,expected)); SDL_free(text);
}
int main(int argc,char **argv) {
    assert(argc==2); NoteStore store={0}; assert(note_store_init(&store,argv[1])==NOTE_OK); note_store_destroy(&store);
    char path[2048];
    const char *extensions[]={"json","md","html"};
    const char *data[]={"{\"type\":\"doc\"}","# 中文\n\nA draft\n","<!doctype html><meta charset=\"utf-8\"><h1>中文</h1>"};
    for(int kind=PLATFORM_EXPORT_JSON;kind<=PLATFORM_EXPORT_HTML;kind++) {
        snprintf(path,sizeof path,"%s/published.%s",argv[1],extensions[kind]);
        selected_path=path; expected_filter=extensions[kind];
        PlatformExport *job=platform_export_text(NULL,"article",data[kind],(PlatformExportKind)kind);
        assert(job && platform_export_status(job,NULL)==PLATFORM_EXPORT_DONE); platform_export_free(job); contents(path,data[kind]);
        job=platform_export_text(NULL,"article","updated",(PlatformExportKind)kind);
        assert(job && platform_export_status(job,NULL)==PLATFORM_EXPORT_DONE); platform_export_free(job); contents(path,"updated");
    }
    expected_filter="md"; /* Choosing .html for Markdown must preserve the file. */
    PlatformExport *job=platform_export_text(NULL,"article.md","do not overwrite",PLATFORM_EXPORT_MARKDOWN);
    assert(job && platform_export_status(job,NULL)==PLATFORM_EXPORT_FAILED); platform_export_free(job); contents(path,"updated");
    canceled=true; job=platform_export_text(NULL,"article.md","canceled",PLATFORM_EXPORT_MARKDOWN);
    assert(job && platform_export_status(job,NULL)==PLATFORM_EXPORT_CANCELED); platform_export_free(job); contents(path,"updated");
    assert(!note_store_valid_name("published.html") && note_store_valid_export_name("published.HTML"));
    assert(note_store_init(&store,argv[1])==NOTE_OK);
    assert(note_store_create(&store,"ordinary.html","x",1)==NOTE_INVALID_NAME);
    assert(note_store_save(&store,"published.html","x",1)==NOTE_INVALID_NAME);
    assert(note_store_export(&store,"../outside.html","x",1)==NOTE_INVALID_NAME);
    assert(note_store_export(&store,"unsafe.exe","x",1)==NOTE_INVALID_NAME);
    assert(note_store_export(&store,"CON.html","x",1)==NOTE_INVALID_NAME);
    assert(note_store_export(&store,"published.html",NULL,1)==NOTE_IO_ERROR); contents(path,"updated");
    NoteList listed={0}; assert(note_store_list(&store,&listed)==NOTE_OK);
    for(size_t i=0;i<listed.count;i++) assert(!strstr(listed.names[i],".html"));
    note_store_list_free(&listed); note_store_destroy(&store);
    canceled=false; expected_filter="zip"; snprintf(path,sizeof path,"%s/bundle.zip",argv[1]); selected_path=path;
    const unsigned char binary[]={0x50,0x4b,0,255,128,0,13,10};
    job=platform_export_bytes(NULL,"bundle.zip",binary,sizeof binary,PLATFORM_EXPORT_ZIP);
    assert(job && platform_export_status(job,NULL)==PLATFORM_EXPORT_DONE); platform_export_free(job);
    size_t binary_size=0; void *binary_read=SDL_LoadFile(path,&binary_size);
    assert(binary_read && binary_size==sizeof binary && !memcmp(binary_read,binary,sizeof binary)); SDL_free(binary_read);
    assert(!note_store_valid_name("bundle.zip") && note_store_valid_export_name("bundle.zip"));
    return 0;
}
