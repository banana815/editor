#include "bundle_archive.h"
typedef struct {
    App *app;char *root,*active,*old_relative;
    NoteStore notes,library_store;CeWritingStore writing;CeImageStore images;
    CeLibraryPanel library;NoteList files;size_t *visible;
    CeDocument *document;CeCitationSession *session;EditBox *edit;
    CeArticleInfo article;char citation_warning[512];bool normalized;
    char **validated_images;size_t validated_count;
} RecoveryActivation;
static void recovery_activation_free(RecoveryActivation *p) {
    if(!p)return;free(p->root);free(p->active);free(p->old_relative);free(p->visible);
    for(size_t i=0;i<p->validated_count;i++)free(p->validated_images[i]);free(p->validated_images);
    note_store_destroy(&p->notes);note_store_destroy(&p->library_store);ce_writing_store_close(&p->writing);ce_image_store_close(&p->images);
    ce_library_panel_destroy(&p->library);note_store_list_free(&p->files);ce_document_free(p->document);ce_citation_session_free(p->session);EditBox_Destroy(p->edit);free(p);
}
static bool recovery_validate_image(RecoveryActivation *p,const char *uri) {
    size_t low=0,high=p->validated_count;
    while(low<high){size_t mid=low+(high-low)/2;int order=strcmp(p->validated_images[mid],uri);if(order<0)low=mid+1;else high=mid;}
    if(low<p->validated_count&&!strcmp(p->validated_images[low],uri))return true;
    unsigned char *bytes=NULL;size_t size=0;CeImageInfo info;
    bool ok=ce_image_store_read(&p->images,uri,&bytes,&size)&&ce_image_decode(bytes,size,&info,NULL,p->app->edit_error,sizeof p->app->edit_error);free(bytes);if(!ok)return false;
    if(p->validated_count>=CE_BUNDLE_MAX_ENTRIES)return false;
    char *name=copy_string(uri);char **next=name?realloc(p->validated_images,(p->validated_count+1)*sizeof *next):NULL;
    if(!next){free(name);return false;}p->validated_images=next;memmove(next+low+1,next+low,(p->validated_count-low)*sizeof *next);next[low]=name;p->validated_count++;return true;
}
static bool recovery_validate_images(RecoveryActivation *p,const CeDocument *doc) {
    for(size_t i=0;i<ce_document_block_count(doc);i++){CeBlockInfo block;if(!ce_document_block_info(doc,i,&block))return false;
        if(block.kind!=CE_BLOCK_IMAGE)continue;char *json=ce_document_block_json(doc,i);cJSON *node=json?cJSON_Parse(json):NULL;free(json);
        const cJSON *src=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(node,"attrs"),"src");
        /* Legacy external/unknown image sources are retained as inert nodes;
         * recovery must neither fetch them nor discard otherwise valid notes. */
        bool managed=cJSON_IsString(src)&&ce_image_asset_name(src->valuestring)!=NULL;
        bool ok=!managed||recovery_validate_image(p,src->valuestring);cJSON_Delete(node);if(!ok)return false;
    }return true;
}
static bool recovery_validate_document(RecoveryActivation *p,const char *data) {
    CeDocument *doc=ce_document_new();bool ok=doc&&ce_document_import(doc,data,CE_FORMAT_AUTO,p->app->edit_error,sizeof p->app->edit_error)&&recovery_validate_images(p,doc);ce_document_free(doc);return ok;
}
static bool recovery_prepare(RecoveryActivation *p,const char *root) {
    App *a=p->app;p->root=copy_string(root);p->document=ce_document_new();ce_library_panel_init(&p->library);
    if(!p->root||!p->document||note_store_init(&p->notes,root)!=NOTE_OK||!ce_writing_store_open(&p->writing,root)||!ce_image_store_open(&p->images,root)||note_store_list(&p->notes,&p->files)!=NOTE_OK)return false;
    for(size_t i=0;i<p->files.count;i++){char *data=NULL;size_t size=0;bool ok=note_store_read(&p->notes,p->files.names[i],&data,&size)==NOTE_OK&&size<=INT_MAX&&!memchr(data,0,size)&&recovery_validate_document(p,data);free(data);if(!ok)return false;}
    for(int trash=0;trash<2;trash++){CeArchiveList list={0};bool ok=ce_writing_archive_list(&p->writing,trash!=0,NULL,&list);
        for(size_t i=0;ok&&i<list.count;i++){char *name=NULL,*data=NULL;ok=ce_writing_archive_read(&p->writing,trash!=0,list.entries[i].id,&name,&data)&&recovery_validate_document(p,data);free(name);free(data);}ce_writing_archive_list_free(&list);if(!ok)return false;}
    char *path=NULL;if(SDL_asprintf(&path,"%s/.notelab-library",root)<0)return false;bool opened=note_store_init(&p->library_store,path)==NOTE_OK;SDL_free(path);if(!opened)return false;
    char *json=NULL;size_t size=0;NoteResult read=note_store_read(&p->library_store,"library.json",&json,&size);
    if(read!=NOTE_NOT_FOUND){CeZoteroLibrary library={0};bool ok=read==NOTE_OK&&!memchr(json,0,size)&&ce_zotero_cache_import(json,&library,a->edit_error,sizeof a->edit_error)&&ce_library_panel_replace(&p->library,&library);free(json);ce_zotero_library_free(&library);if(!ok)return false;}
    p->visible=calloc(p->files.count?p->files.count:1,sizeof *p->visible);if(!p->visible)return false;size_t count=0;
    for(int pass=0;pass<2;pass++)for(size_t i=0;i<p->files.count;i++){CeArticleInfo info;if(!ce_writing_article_get(&p->writing,p->files.names[i],&info))return false;if(info.pinned==(pass==0))p->visible[count++]=i;}
    if(p->files.count){p->active=copy_string(p->files.names[p->visible[0]]);char *data=NULL;if(!p->active||note_store_read(&p->notes,p->active,&data,&size)!=NOTE_OK)return false;
        bool ok=ce_document_import(p->document,data,CE_FORMAT_AUTO,a->edit_error,sizeof a->edit_error);free(data);if(!ok)return false;
        char *original=ce_document_export_json(p->document);if(!original)return false;
        bool processed=ce_citation_session_build(p->document,&p->library.library,NULL,&p->session,p->citation_warning,sizeof p->citation_warning);
        if(processed){const char *replacement=ce_citation_prepared_json(ce_citation_session_prepared(p->session));p->normalized=strcmp(original,replacement)!=0;ok=ce_document_import(p->document,replacement,CE_FORMAT_JSON,a->edit_error,sizeof a->edit_error);}free(original);if(!ok)return false;
        if(!ce_writing_article_get(&p->writing,p->active,&p->article))return false;
    }
    ce_document_mark_saved(p->document);p->edit=EditBox_Create(a->window,a->renderer,a->engine,a->font,&a->edit->rect);
    if(!p->edit||!EditBox_SetText(p->edit,ce_document_text(p->document)))return false;EditBox_SetLayout(p->edit,&rich_hooks,a);
    size_t base_len=strlen(a->base_notes_root);const char *old="";
    if(strcmp(a->notes_root,a->base_notes_root)){if(strncmp(a->notes_root,a->base_notes_root,base_len)||a->notes_root[base_len]!='/')return false;old=a->notes_root+base_len+1;}
    p->old_relative=copy_string(old);return p->old_relative!=NULL;
}
static void recovery_rollback_saved(bool success,const char *error,void *context){
    RecoveryActivation *p=context;App *a=p->app;a->recovery.busy=false;a->persistence_error=!success;
    message(a,success?"恢复未完成；已保留并重新保存原资料库选择":error);recovery_activation_free(p);
}
static void recovery_activation_saved(bool success,const char *error,void *context) {
    RecoveryActivation *p=context;App *a=p->app;
    if(!success){char problem[256];
        if(!ce_workspace_activate(a->base_notes_root,p->old_relative,problem,sizeof problem)){a->recovery.busy=false;a->persistence_error=true;message(a,"恢复持久化失败，且无法回写原资料库选择；当前文章仍保留，请保持应用打开并重新备份");recovery_activation_free(p);return;}
        message(a,error);platform_persist(recovery_rollback_saved,p);return;
    }
    preview_reset(a);editing_break(a);manager_preview_clear(a);ce_writing_archive_list_free(&a->manager_archives);search_results_clear(a);outline_clear(a);citation_context_clear(a);
    clipboard_request_free(a->clipboard_event);a->clipboard_event=NULL;++a->prompt_generation;
    /* Async clipboard requests own their allocations until completion; the
     * citation revision increment below invalidates them before any commit. */
    for(size_t i=0;i<16;i++){SDL_DestroySurface(a->images.cache[i].surface);a->images.cache[i].surface=NULL;}a->images.cache_bytes=a->images.cache_tick=0;
    a->manager_open=a->search_open=a->library_open=false;a->prompt_mode=PROMPT_NONE;EditBox_SetFocus(a->prompt,false);EditBox_SetFocus(a->edit,false);
    note_store_destroy(&a->store);a->store=p->notes;memset(&p->notes,0,sizeof p->notes);
    note_store_destroy(&a->library_store);a->library_store=p->library_store;memset(&p->library_store,0,sizeof p->library_store);a->library_store_ready=true;
    ce_writing_store_close(&a->writing_store);a->writing_store=p->writing;memset(&p->writing,0,sizeof p->writing);
    ce_image_store_close(&a->images.store);a->images.store=p->images;memset(&p->images,0,sizeof p->images);a->images.ready=true;
    ce_library_panel_destroy(&a->library);a->library=p->library;memset(&p->library,0,sizeof p->library);
    note_store_list_free(&a->files);a->files=p->files;memset(&p->files,0,sizeof p->files);free(a->visible_files);a->visible_files=p->visible;p->visible=NULL;a->visible_count=a->files.count;
    ce_document_free(a->doc);a->doc=p->document;p->document=NULL;ce_citation_session_free(a->citation_session);a->citation_session=p->session;p->session=NULL;
    a->citation_style=ce_citation_session_profile(a->citation_session);a->citation_normalized_pending=p->normalized;SDL_strlcpy(a->citation_warning,p->citation_warning,sizeof a->citation_warning);++a->citation_revision;
    EditBox_Destroy(a->edit);a->edit=p->edit;p->edit=NULL;free(a->active);a->active=p->active;p->active=NULL;free(a->notes_root);a->notes_root=p->root;p->root=NULL;
    a->article_info=p->article;*a->folder_filter=*a->tag_filter=0;a->status_filter=0;a->history_at=0;a->scroll=a->sidebar_scroll=0;
    a->library_unsaved=a->library_restore_failed=a->persistence_error=false;*a->library_warning=0;a->typing_mask=0;a->rich_dirty=true;a->rich_valid=false;a->stats_valid=a->goal_valid=false;
    a->touch_menu_visible=false;a->edited_at=SDL_GetTicks();platform_web_input_cancel();images_attach(a,a->rich);EditBox_SetFocus(a->edit,a->active!=NULL);update_title(a);a->recovery.busy=false;
    message(a,"已切换到恢复的资料库副本，原资料库完整保留");recovery_activation_free(p);
}
static bool recovery_activate(App *a,const char *relative,const char *root) {
    RecoveryActivation *p=calloc(1,sizeof *p);if(!p){message(a,"内存不足，原资料库保持不变");return false;}p->app=a;a->edit_error[0]=0;
    if(!recovery_prepare(p,root)){message(a,*a->edit_error?a->edit_error:"恢复副本中的文章、图片、历史记录或文献缓存无效；原资料库保持不变");recovery_activation_free(p);return false;}
    if(!ce_workspace_activate(a->base_notes_root,relative,a->edit_error,sizeof a->edit_error)){message(a,a->edit_error);recovery_activation_free(p);return false;}
    a->recovery.busy=true;platform_persist(recovery_activation_saved,p);return true;
}
