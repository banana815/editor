/* Uses the app's explicit base root; all restored files remain in a separate
 * workspace until recovery_activate validates and commits the app state. */
static bool recovery_idle(App *a) {
    if(a->recovery.busy || a->images.pending || a->images.job || a->import_job || a->export_job || a->source_job || a->pending_note_json ||
       a->saving || a->library_cache_saving || a->edit->composition_length || a->edit->composition_snapshot) {
        message(a,"请先完成输入、导入导出与保存，再备份或恢复资料库");return false;
    }
    return true;
}
static void recovery_backup_saved(bool success,const char *error,void *context) {
    App *a=context;a->recovery.busy=false;
    if(!success){message(a,error);return;}
    unsigned char *bytes=NULL;size_t size=0;
    if(!ce_workspace_backup(a->notes_root,&bytes,&size,a->edit_error,sizeof a->edit_error)){message(a,a->edit_error);return;}
    a->export_job=platform_export_bytes(a->window,"NoteLab-workspace.zip",bytes,size,PLATFORM_EXPORT_ZIP);free(bytes);
    message(a,a->export_job?"资料库已打包：文章、图片、属性、历史、回收站与文献缓存":"无法打开备份保存位置");
}
static void recovery_backup(App *a) {
    if(!recovery_idle(a))return;
    if(!save_document(a)&&!a->saving)return;
    a->recovery.busy=true;message(a,"正在保存并打包整个资料库…");platform_persist(recovery_backup_saved,a);
}
static void recovery_import(App *a) {
    if(!recovery_idle(a))return;
    a->recovery.job=platform_import_open(a->window,PLATFORM_IMPORT_BACKUP);
    a->recovery.busy=a->recovery.job!=NULL;
    message(a,a->recovery.job?"选择 Note Lab 资料库备份；恢复为独立副本，原资料库会保留":"无法打开备份文件选择器");
}
static void recovery_restore_saved(bool success,const char *error,void *context) {
    App *a=context;char *relative=NULL,*root=NULL;bool ok=false;
    if(!success)message(a,error);
    else if(!ce_workspace_restore_stage(a->base_notes_root,a->recovery.bytes,a->recovery.size,&relative,&root,a->edit_error,sizeof a->edit_error))message(a,a->edit_error);
    else ok=recovery_activate(a,relative,root);
    free(relative);free(root);free(a->recovery.bytes);a->recovery.bytes=NULL;a->recovery.size=0;
    if(!ok)a->recovery.busy=false; /* Activation owns busy until durable commit. */
}
static void recovery_poll(App *a) {
    if(!a->recovery.job)return;const char *status=NULL;
    PlatformImportStatus result=platform_import_status(a->recovery.job,&status);if(result==PLATFORM_IMPORT_PENDING)return;
    PlatformImportFile file={0};bool ok=result==PLATFORM_IMPORT_DONE&&platform_import_take_result(a->recovery.job,&file);
    if(!ok)message(a,status?status:"备份读取失败");
    platform_import_free(a->recovery.job);a->recovery.job=NULL;
    if(!ok){a->recovery.busy=false;return;}
    a->recovery.bytes=malloc(file.size?file.size:1);a->recovery.size=file.size;
    if(a->recovery.bytes&&file.size)memcpy(a->recovery.bytes,file.data,file.size);
    platform_import_file_free(&file);
    bool saved=a->recovery.bytes && save_document(a);
    if(!a->recovery.bytes||(!saved&&!a->saving)){free(a->recovery.bytes);a->recovery.bytes=NULL;a->recovery.size=0;a->recovery.busy=false;message(a,"无法保存当前文章；资料库保持不变");return;}
    message(a,"正在校验备份并准备恢复副本…");platform_persist(recovery_restore_saved,a);
}
static void recovery_shutdown(App *a){platform_import_free(a->recovery.job);free(a->recovery.bytes);memset(&a->recovery,0,sizeof a->recovery);}
