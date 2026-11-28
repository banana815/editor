#define SDL_MAIN_HANDLED 1
#include "../src/app.c"
#undef main
#include "image_fixtures.h"
#include <assert.h>
#ifdef _WIN32
#include <crtdbg.h>
#endif
int main(int argc,char **argv){
#ifdef _WIN32
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    assert(argc>=3);char root[2048];SDL_snprintf(root,sizeof root,"%s/run-%llu",argv[1],(unsigned long long)SDL_GetPerformanceCounter());
    void *state=NULL;char *args[]={"recovery-test","--notes",root,"--font",argv[2]};assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE);App *a=state;
    const char *old="<h1>备份文章</h1><p>完整的正文</p>";assert(note_store_create(&a->store,"backup.json",old,strlen(old))==NOTE_OK);assert(refresh_files(a));open_note(a,"backup.json");assert(a->active&&!strcmp(a->active,"backup.json"));
    strcpy(a->article_info.folder,"博客");strcpy(a->article_info.tags,"历史,恢复");a->article_info.goal=1800;a->article_info.pinned=true;assert(manager_save_info(a));
    assert(ce_writing_snapshot(&a->writing_store,a->active,"<p>之前的段落</p>",(int64_t)time(NULL)));assert(note_store_create(&a->store,"deleted.json","<p>deleted</p>",14)==NOTE_OK);assert(ce_writing_trash_note(&a->writing_store,&a->store,"deleted.json",(int64_t)time(NULL)));
    manager_show(a,1);manager_select_archive(a,0);char *unchanged=ce_document_export_json(a->doc);manager_action(a,MAN_DIFF);assert(a->manager_diff&&a->manager_preview_text&&strstr(a->manager_preview_text,"修改"));char *still=ce_document_export_json(a->doc);assert(!strcmp(unchanged,still));free(still);free(unchanged);manager_close(a);
    a->edit->cursor=(int)ce_document_length(a->doc);a->edit->highlight1=a->edit->highlight2=-1;assert(images_begin(a));assert(images_receive(a,image_png,sizeof image_png));assert(save_document(a));char image_uri[83];assert(ce_image_store_add(&a->images.store,image_png,sizeof image_png,image_uri,NULL));
    unsigned char *zip=NULL;size_t size=0;char error[256];assert(ce_workspace_backup(a->notes_root,&zip,&size,error,sizeof error));
    assert(edit_replace(a,0,ce_document_length(a->doc),"after backup"));sync_view(a);mark_changed(a);assert(save_document(a));char *current_root=copy_string(a->notes_root);char *relative=NULL,*restored=NULL;assert(ce_workspace_restore_stage(a->base_notes_root,zip,size,&relative,&restored,error,sizeof error));
    /* A valid ZIP containing an invalid document cannot activate. */
    NoteStore staged={0};assert(note_store_init(&staged,restored)==NOTE_OK);assert(note_store_save(&staged,"backup.json","{invalid",8)==NOTE_OK);note_store_destroy(&staged);assert(!recovery_activate(a,relative,restored));assert(!strcmp(a->notes_root,current_root)&&!strcmp(ce_document_text(a->doc),"after backup"));char *resolved=NULL;assert(ce_workspace_resolve(a->base_notes_root,&resolved,error,sizeof error)&&!strcmp(resolved,current_root));free(resolved);free(relative);free(restored);
    assert(ce_workspace_restore_stage(a->base_notes_root,zip,size,&relative,&restored,error,sizeof error));
    char asset_path[2400];SDL_snprintf(asset_path,sizeof asset_path,"%s/.notelab-writing/assets/%s",restored,ce_image_asset_name(image_uri));assert(SDL_SaveFile(asset_path,"corrupt",7));assert(!recovery_activate(a,relative,restored));assert(!strcmp(a->notes_root,current_root));free(relative);free(restored);
    assert(ce_workspace_restore_stage(a->base_notes_root,zip,size,&relative,&restored,error,sizeof error));
    /* Exercise the browser persistence failure callback with the same fully
     * prepared candidate. The old App and active-root marker must both survive. */
    RecoveryActivation *pending=calloc(1,sizeof *pending);assert(pending);pending->app=a;assert(recovery_prepare(pending,restored));assert(ce_workspace_activate(a->base_notes_root,relative,error,sizeof error));a->recovery.busy=true;recovery_activation_saved(false,"injected persistence failure",pending);
    assert(!a->recovery.busy&&!strcmp(a->notes_root,current_root)&&!strcmp(ce_document_text(a->doc),"after backup"));assert(ce_workspace_resolve(a->base_notes_root,&resolved,error,sizeof error)&&!strcmp(resolved,current_root));free(resolved);
    assert(recovery_activate(a,relative,restored));assert(!a->recovery.busy&&!strcmp(a->notes_root,restored)&&strcmp(a->notes_root,current_root));assert(strstr(ce_document_text(a->doc),"备份文章\n完整的正文")&&a->article_info.goal==1800&&a->article_info.pinned&&!strcmp(a->article_info.folder,"博客"));unsigned char *pixels=NULL;size_t pixel_size=0;assert(ce_image_store_read(&a->images.store,image_uri,&pixels,&pixel_size)&&pixel_size==sizeof image_png&&!memcmp(pixels,image_png,pixel_size));free(pixels);
    CeArchiveList records={0};assert(ce_writing_archive_list(&a->writing_store,true,NULL,&records)&&records.count==1);ce_writing_archive_list_free(&records);assert(ce_workspace_resolve(a->base_notes_root,&resolved,error,sizeof error)&&!strcmp(resolved,restored));free(resolved);
    char *original_data=NULL;size_t original_size=0;NoteStore original={0};assert(note_store_init(&original,current_root)==NOTE_OK);assert(note_store_read(&original,"backup.json",&original_data,&original_size)==NOTE_OK&&strstr(original_data,"after backup"));free(original_data);note_store_destroy(&original);
    SDL_AppQuit(a,SDL_APP_SUCCESS);state=NULL;assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE);a=state;assert(!strcmp(a->notes_root,restored));open_note(a,"backup.json");assert(strstr(ce_document_text(a->doc),"完整的正文")&&a->article_info.goal==1800);SDL_AppQuit(a,SDL_APP_SUCCESS);
    free(current_root);free(relative);free(restored);free(zip);puts("History diff, rejected invalid recovery, durable workspace activation and restart passed");return 0;
}
