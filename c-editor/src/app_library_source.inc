/* Private C controller: transport supplies bytes/path only; source choices,
 * parsing, replacement and persistence belong to this application. */
static char *library_source_config(const char *kind,const char *location) {
    if(!kind || !location || strlen(location)>PLATFORM_LIBRARY_SOURCE_MAX_URL) return NULL;
    cJSON *tree=cJSON_CreateObject(); char *printed=NULL,*owned=NULL;
    if(tree && cJSON_AddNumberToObject(tree,"version",1) &&
       cJSON_AddStringToObject(tree,"kind",kind) && cJSON_AddStringToObject(tree,"location",location))
        printed=ce_csl_json_print_exact_with_limit(tree,16384);
    if(printed) owned=copy_string(printed);
    cJSON_free(printed); cJSON_Delete(tree); return owned;
}
static cJSON *library_source_read(App *a) {
    const char *json=a->library.library.client_state_json; cJSON *tree=NULL;
    if(!json || strlen(json)>16384 || !ce_csl_json_parse_strict(json,strlen(json),&tree,NULL,0)) return NULL;
    const cJSON *version=cJSON_GetObjectItemCaseSensitive(tree,"version");
    const cJSON *location=cJSON_GetObjectItemCaseSensitive(tree,"location");
    const char *kind=json_string(tree,"kind");
    if(!cJSON_IsObject(tree) || !cJSON_IsNumber(version) || version->valuedouble!=1 ||
       !cJSON_IsString(location) || strlen(location->valuestring)>PLATFORM_LIBRARY_SOURCE_MAX_URL ||
       (strcmp(kind,"http") && strcmp(kind,"sqlite") && strcmp(kind,"file"))) {
        cJSON_Delete(tree); return NULL;
    }
    return tree;
}
static bool library_source_begin(App *a,const char *kind,const char *location) {
    if(a->source_job || a->import_job || a->pending_note_json || a->library_cache_saving) {
        message(a,"正在读取或保存，请稍候"); return false;
    }
    cJSON *old=library_source_read(a);
    bool same=old && !strcmp(json_string(old,"kind"),kind) && !strcmp(json_string(old,"location"),location);
    char *config=same?copy_string(a->library.library.client_state_json):library_source_config(kind,location);
    cJSON_Delete(old); char error[256];
    if(!config) { message(a,"来源地址过长或内存不足，原文献库保持不变"); return false; }
    PlatformLibrarySource *job=NULL;
    if(!strcmp(kind,"http")) job=platform_library_source_fetch(location,NULL,error,sizeof error);
    else if(!strcmp(kind,"sqlite")) job=platform_library_source_reopen_sqlite(location,error,sizeof error);
    else SDL_strlcpy(error,"请重新选择导出文件",sizeof error);
    if(!job) { free(config); message(a,error); return false; }
    a->source_job=job; a->source_pending_config=config;
    message(a,"正在读取来源，原文献库保留至读取成功"); return true;
}
static void library_source_refresh(App *a) {
    cJSON *config=library_source_read(a);
    if(!config) message(a,"尚无可刷新的来源，请连接来源或重新导入文件");
    else if(!strcmp(json_string(config,"kind"),"file")) library_action(a,LIB_IMPORT);
    else library_source_begin(a,json_string(config,"kind"),json_string(config,"location"));
    cJSON_Delete(config);
}
static void library_source_poll(App *a) {
    if(!a->source_job) return;
    const char *status=NULL;
    PlatformLibrarySourceStatus state=platform_library_source_status(a->source_job,&status);
    if(state==PLATFORM_LIBRARY_SOURCE_PENDING) return;
    /* An in-flight prompt belongs to the current citation/draft. Replacing
     * the panel must not redirect that prompt to another editing context. */
    if(state==PLATFORM_LIBRARY_SOURCE_DONE && (a->library_cache_saving || a->prompt_mode==PROMPT_CITATION_PAGE ||
       a->prompt_mode==PROMPT_CITATION_NOTE)) return;
    if(state!=PLATFORM_LIBRARY_SOURCE_DONE) message(a,status && *status?status:"来源读取已取消，原文献库保留");
    else {
        PlatformLibrarySourceResult result={0}; CeZoteroLibrary imported={0};
        char error[256]="无法读取来源结果，原文献库保留";
        bool ok=platform_library_source_take_result(a->source_job,&result);
        if(ok && result.native_path) ok=ce_zotero_load_sqlite(result.native_path,&imported,error,sizeof error);
        else if(ok) {
            if(!result.data || result.size>PLATFORM_LIBRARY_SOURCE_MAX_BYTES || memchr(result.data,0,result.size)) {
                SDL_strlcpy(error,"来源包含二进制内容或超过大小限制，未导入",sizeof error); ok=false;
            } else ok=ce_zotero_import_csl((const char *)result.data,&imported,error,sizeof error);
        }
        if(ok) {
            imported.client_state_json=a->source_pending_config; a->source_pending_config=NULL;
            if(!ce_library_panel_replace(&a->library,&imported)) {
                SDL_strlcpy(error,"内存不足，原文献库保持不变",sizeof error); ok=false;
            }
        }
        if(ok) {
            a->library_citation_mode=false; a->library_dragging=false;
            a->library_unsaved=true; a->library_restore_failed=false; a->library_warning[0]=0;
            update_title(a); library_cache_save(a);
        } else message(a,error);
        ce_zotero_library_free(&imported); platform_library_source_result_free(&result);
    }
    platform_library_source_free(a->source_job); a->source_job=NULL;
    free(a->source_pending_config); a->source_pending_config=NULL;
}
