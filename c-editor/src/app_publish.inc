/* Included after citation_export/message helpers. Publication is read-only and
 * uses a freshly evaluated private session, even when the open note has no
 * usable display session. Imported render caches never authorize output. */
static bool publish_append(char **text,size_t *size,const char *part) {
    size_t n=strlen(part);
    if(n>16u*1024u*1024u-*size) return false;
    char *next=realloc(*text,*size+n+1); if(!next) return false;
    memcpy(next+*size,part,n+1); *text=next; *size+=n; return true;
}
static bool publish_escape(char **text,size_t *size,const char *part) {
    for(;*part;part++) {
        char one[]={*part,0};
        if(!publish_append(text,size,*part=='&'?"&amp;":*part=='<'?"&lt;":
            *part=='>'?"&gt;":*part=='\"'?"&quot;":*part=='\n'?"<br>":one)) return false;
    }
    return true;
}
static char *publish_atom_html(void *context,const cJSON *atom,char *error,size_t capacity) {
    const CeCitationPrepared *snapshot=context;
    const cJSON *attrs=cJSON_GetObjectItemCaseSensitive(atom,"attrs");
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(atom,"type");
    char *result=NULL; size_t size=0; bool ok=false;
    if(cJSON_IsString(type) && !strcmp(type->valuestring,"zoteroCitationBlock")) {
        const cJSON *id=cJSON_GetObjectItemCaseSensitive(attrs,"citationId");
        const CeCitationCache *cache=cJSON_IsString(id)?ce_citation_prepared_citation(snapshot,id->valuestring):NULL;
        CeCslOutputString html={0};
        ok=cache && ce_csl_output_html(ce_citation_cache_citation_output(cache),&html,error,capacity);
        if(ok) ok=publish_append(&result,&size,"<div class=\"citation\">") &&
            publish_append(&result,&size,html.data) && publish_append(&result,&size,"</div>\n");
        ce_csl_output_string_free(&html);
        const cJSON *note=cJSON_GetObjectItemCaseSensitive(attrs,"note");
        if(ok && cJSON_IsString(note) && *note->valuestring)
            ok=publish_append(&result,&size,"<blockquote>") && publish_escape(&result,&size,note->valuestring) &&
                publish_append(&result,&size,"</blockquote>\n");
    } else {
        /* entriesJson here belongs to the newly generated snapshot, not the
         * imported node. Each entryHtml was emitted by ce_csl_bibliography_html
         * during evaluation, preserving its controlled CSL formatting. */
        const CeCitationCache *cache=ce_citation_prepared_bibliography(snapshot);
        const cJSON *json=cJSON_GetObjectItemCaseSensitive(attrs,"entriesJson");
        cJSON *entries=cJSON_IsString(json)?cJSON_Parse(json->valuestring):NULL;
        ok=cache && cJSON_IsArray(entries) && (size_t)cJSON_GetArraySize(entries)==ce_citation_cache_entry_count(cache) &&
            publish_append(&result,&size,"<section class=\"references\">\n");
        for(int i=0;ok && i<cJSON_GetArraySize(entries);i++) {
            const CeCitationCacheEntry *entry=ce_citation_cache_entry(cache,(size_t)i);
            const cJSON *html=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(entries,i),"entryHtml");
            if(!entry || !cJSON_IsString(html)) { ok=false; break; }
            if(entry->visible) ok=publish_append(&result,&size,html->valuestring) && publish_append(&result,&size,"\n");
        }
        if(ok) ok=publish_append(&result,&size,"</section>\n");
        cJSON_Delete(entries);
    }
    if(!ok) {
        free(result);
        if(error && capacity && !*error) SDL_strlcpy(error,"无法完整发布引用内容，请检查文献来源",capacity);
        return NULL;
    }
    return result;
}
static char *publish_prepare(App *a,CePublishFormat format) {
    a->edit_error[0]=0;
    if(!a->active || a->edit->composition_length || a->edit->composition_snapshot) {
        SDL_strlcpy(a->edit_error,"请先打开文章并完成输入法输入，再复制或导出",sizeof a->edit_error); return NULL;
    }
    CeCitationSession *session=NULL; CeCitationPrepared *snapshot=NULL; CeDocument *document=NULL;
    char *result=NULL;
    if(!ce_citation_session_build(a->doc,&a->library.library,NULL,&session,a->edit_error,sizeof a->edit_error) ||
       !ce_citation_session_snapshot(session,a->doc,&a->library.library,&snapshot,a->edit_error,sizeof a->edit_error)) goto done;
    document=ce_document_new();
    if(!document || !ce_document_import(document,ce_citation_prepared_json(snapshot),CE_FORMAT_JSON,a->edit_error,sizeof a->edit_error)) goto done;
    result=ce_document_publish(document,format,publish_atom_html,snapshot,a->edit_error,sizeof a->edit_error);
done:
    ce_document_free(document); ce_citation_prepared_free(snapshot); ce_citation_session_free(session);
    if(!result && !*a->edit_error) SDL_strlcpy(a->edit_error,"内存不足，未复制或导出",sizeof a->edit_error);
    return result;
}
#ifdef __EMSCRIPTEN__
static void publish_copy_complete(bool success,const char *plain,const char *html,const char *typed,
                                  const char *error,void *context) {
    (void)plain; (void)html; (void)typed;
    message(context,success?"文章源码已复制，可粘贴到博客源码编辑器":error?error:"浏览器拒绝复制，请使用文件导出");
}
#endif
static void publish_action(App *a,CePublishFormat format,bool copy) {
    if(!copy && a->export_job) { message(a,"正在导出，请先完成文件选择"); return; }
    char *source=publish_prepare(a,format);
    if(!source) { message(a,a->edit_error); return; }
    if(copy) {
        /* Source editors need literal HTML/Markdown, not an implicit rich paste.
         * The browser transport copies bytes synchronously before returning and
         * owns its pending callback until completion/cancellation at shutdown. */
#ifdef __EMSCRIPTEN__
        if(!platform_web_clipboard_write(source,NULL,NULL,publish_copy_complete,a))
            message(a,"浏览器未允许复制，请点击重试或使用文件导出");
#else
        message(a,SDL_SetClipboardText(source)?"文章源码已复制，可粘贴到博客源码编辑器":"无法写入剪贴板，请使用文件导出");
#endif
    } else {
        const char *extension=format==CE_PUBLISH_MARKDOWN?".md":".html";
        const char *dot=strrchr(a->active,'.'); size_t stem=dot?(size_t)(dot-a->active):strlen(a->active);
        char *filename=malloc(stem+strlen(extension)+1),*file=source;
        if(filename) { memcpy(filename,a->active,stem); strcpy(filename+stem,extension); }
        if(format==CE_PUBLISH_HTML) {
            file=NULL; size_t size=0;
            if(!publish_append(&file,&size,"<!doctype html>\n<html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"></head><body>\n") ||
               !publish_append(&file,&size,source) || !publish_append(&file,&size,"</body></html>\n")) { free(file); file=NULL; }
        }
        if(filename && file) a->export_job=platform_export_text(a->window,filename,file,
            format==CE_PUBLISH_MARKDOWN?PLATFORM_EXPORT_MARKDOWN:PLATFORM_EXPORT_HTML);
        message(a,a->export_job?"请选择文章导出位置":"无法准备导出文件，原文保持不变");
        free(filename); if(file!=source) free(file);
    }
    free(source);
}
