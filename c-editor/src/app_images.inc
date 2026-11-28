#include "citation_digest.h"
/* Included after editing/prompt helpers. Import work binds to the current note
 * and revision; asynchronous clipboard/picker results never target another note. */
static bool images_fingerprint(App *a,char hash[65]) {
    char *json=ce_document_export_json(a->doc); if(!json) return false;
    CeCitationDigest digest; ce_citation_digest_init(&digest);
    bool ok=ce_citation_digest_update(&digest,json,strlen(json),NULL,0) && ce_citation_digest_final_hex(&digest,hash,NULL,0);
    free(json); return ok;
}
static bool images_ready(App *a) {
    if(a->images.ready) return true;
    if(!ce_image_store_open(&a->images.store,a->notes_root)) { message(a,a->images.store.error); return false; }
    a->images.ready=true; return true;
}
static SDL_Surface *images_resolve(void *context,const cJSON *node,char *error,size_t capacity) {
    App *a=context; const cJSON *attrs=cJSON_GetObjectItemCaseSensitive(node,"attrs");
    const cJSON *src=cJSON_GetObjectItemCaseSensitive(attrs,"src"); unsigned char *bytes=NULL,*pixels=NULL; size_t size=0; CeImageInfo info;
    if(cJSON_IsString(src)) for(size_t i=0;i<16;i++) if(a->images.cache[i].surface && !strcmp(src->valuestring,a->images.cache[i].uri)) {
        SDL_Surface *cached=a->images.cache[i].surface; a->images.cache[i].used=++a->images.cache_tick;
        return SDL_CreateSurfaceFrom(cached->w,cached->h,cached->format,cached->pixels,cached->pitch);
    }
    if(!images_ready(a) || !cJSON_IsString(src) || !ce_image_store_read(&a->images.store,src->valuestring,&bytes,&size)) {
        snprintf(error,capacity,"%s",*a->images.store.error?a->images.store.error:"图片资源无效"); return NULL;
    }
    bool ok=ce_image_decode(bytes,size,&info,&pixels,error,capacity); free(bytes); if(!ok) return NULL;
    SDL_Surface *surface=SDL_CreateSurface((int)info.width,(int)info.height,SDL_PIXELFORMAT_RGBA32);
    if(surface) for(unsigned y=0;y<info.height;y++) memcpy((char*)surface->pixels+(size_t)y*surface->pitch,pixels+(size_t)y*info.width*4,(size_t)info.width*4);
    free(pixels); if(!surface) return NULL;
    size_t cost=(size_t)surface->pitch*(size_t)surface->h,slot=0;
    for(;;) {
        slot=0; for(size_t i=0;i<16;i++) if(!a->images.cache[i].surface || a->images.cache[i].used<a->images.cache[slot].used) slot=i;
        if(!a->images.cache[slot].surface && a->images.cache_bytes+cost<=96u*1024u*1024u) break;
        if(!a->images.cache[slot].surface) { for(size_t i=0;i<16;i++) if(a->images.cache[i].surface && (!a->images.cache[slot].surface || a->images.cache[i].used<a->images.cache[slot].used)) slot=i; }
        SDL_Surface *old=a->images.cache[slot].surface;
        if(old) { a->images.cache_bytes-=(size_t)old->pitch*(size_t)old->h; SDL_DestroySurface(old); a->images.cache[slot].surface=NULL; }
    }
    SDL_strlcpy(a->images.cache[slot].uri,src->valuestring,sizeof a->images.cache[slot].uri);
    a->images.cache[slot].surface=surface; a->images.cache[slot].used=++a->images.cache_tick; a->images.cache_bytes+=cost;
    return SDL_CreateSurfaceFrom(surface->w,surface->h,surface->format,surface->pixels,surface->pitch);
}
static void images_attach(App *a,CeRichLayout *layout) {
    if(!layout) return;
    CeRichImageSource source={a->images.store.revision,a,images_resolve}; ce_rich_layout_set_images(layout,&source);
}
static void images_pending_clear(App *a) { free(a->images.pending_note); free(a->images.pending_root); a->images.pending_note=a->images.pending_root=NULL; a->images.pending=false; }
static bool images_begin(App *a) {
    if(a->recovery.busy || !a->active || a->prompt_mode!=PROMPT_NONE || a->manager_open || a->search_open || a->library_open ||
       a->edit->composition_length || a->edit->composition_snapshot || a->images.pending) return false;
    size_t start,end; get_selection(a,&start,&end);
    if(start!=end) { message(a,"请先将光标放到图片插入位置"); return false; }
    if(!images_ready(a)) return false;
    char *name=copy_string(a->active),*root=copy_string(a->notes_root);
    if(!name || !root || !images_fingerprint(a,a->images.pending_hash)) { free(name); free(root);return false; }
    a->images.pending_note=name; a->images.pending_root=root; a->images.pending_revision=ce_document_revision(a->doc);
    a->images.pending_caret=start; a->images.pending=true; editing_break(a); return true;
}
static bool images_receive(App *a,const void *bytes,size_t size) {
    if(!a->images.pending) return false;
    char current_hash[65];
    bool current=a->active && !strcmp(a->notes_root,a->images.pending_root) && !strcmp(a->active,a->images.pending_note) &&
        images_fingerprint(a,current_hash) && !strcmp(current_hash,a->images.pending_hash) &&
        !a->edit->composition_length && !a->edit->composition_snapshot;
    size_t offset=a->images.pending_caret; images_pending_clear(a);
    if(!current) { message(a,"文章已改变，已取消图片插入，请重试"); return false; }
    char uri[83]; if(!ce_image_store_add(&a->images.store,bytes,size,uri,NULL)) { message(a,a->images.store.error); return false; }
    char *json=ce_image_node_json(uri,"","",0); CeDocument *candidate=edit_candidate(a);
    bool ok=candidate && json && ce_document_insert_node(candidate,offset,json); free(json);
    if(!edit_commit(a,candidate,ok,NULL,false)) { message(a,a->edit_error); return false; }
    sync_view(a); a->edit->cursor=(int)SDL_min(offset+3,ce_document_length(a->doc)); a->typing_mask=0;
    images_attach(a,a->rich); mark_changed(a); reveal_cursor(a); platform_persist(structure_persisted,a);
    message(a,"图片已插入 · 可设置替代文本、说明和宽度"); return true;
}
static void images_insert_picker(App *a) {
    if(!images_begin(a)) return;
    a->images.job=platform_import_open(a->window,PLATFORM_IMPORT_IMAGE);
    if(!a->images.job) { images_pending_clear(a); message(a,"无法打开图片选择器"); }
}
static void images_poll(App *a) {
    if(!a->images.job) return;
    const char *status=NULL; PlatformImportStatus state=platform_import_status(a->images.job,&status);
    if(state==PLATFORM_IMPORT_PENDING) return;
    PlatformImportFile file={0};
    if(state==PLATFORM_IMPORT_DONE && platform_import_take_result(a->images.job,&file)) images_receive(a,file.data,file.size);
    else { images_pending_clear(a); message(a,status?status:"图片导入已取消"); }
    platform_import_file_free(&file); platform_import_free(a->images.job); a->images.job=NULL;
}
static bool images_drop(App *a,const char *path) {
    if(!path || !images_begin(a)) return false;
    SDL_IOStream *stream=SDL_IOFromFile(path,"rb"); Sint64 size=stream?SDL_GetIOSize(stream):-1;
    if(size<=0 || (uint64_t)size>CE_IMAGE_MAX_BYTES) { if(stream) SDL_CloseIO(stream); images_pending_clear(a); message(a,"图片不能超过 16 MiB"); return false; }
    unsigned char *bytes=malloc((size_t)size); bool read=bytes && SDL_ReadIO(stream,bytes,(size_t)size)==(size_t)size;
    SDL_CloseIO(stream); bool ok=false;
    if(read) ok=images_receive(a,bytes,(size_t)size); else { images_pending_clear(a); message(a,"读取图片失败"); }
    free(bytes); return ok;
}
static bool images_paste_native(App *a) {
    void *bytes=NULL; size_t size=0; int result=platform_image_clipboard(&bytes,&size);
    if(!result) return false;
    if(result<0) { message(a,"无法读取剪贴板图片"); return true; }
    if(images_begin(a)) images_receive(a,bytes,size);
    SDL_free(bytes); return true;
}
static void images_property_prompt(App *a,unsigned field) {
    if(!a->active || field>2 || a->edit->composition_length) return;
    size_t offset=(size_t)a->edit->cursor,index=0; CeBlockInfo block; bool found=false;
    for(;index<ce_document_block_count(a->doc);index++) if(ce_document_block_info(a->doc,index,&block) && block.kind==CE_BLOCK_IMAGE && offset>=block.start && offset<=block.end) { found=true; break; }
    if(!found) { message(a,"请先点击正文中的图片"); return; }
    char *json=ce_document_block_json(a->doc,index); cJSON *node=json?cJSON_Parse(json):NULL; free(json);
    cJSON *attrs=cJSON_GetObjectItemCaseSensitive(node,"attrs"),*v=cJSON_GetObjectItemCaseSensitive(attrs,field==0?"alt":field==1?"caption":"width");
    char number[32]; snprintf(number,sizeof number,"%d",cJSON_IsNumber(v)?v->valueint:0);
    char *note=copy_string(a->active),*root=copy_string(a->notes_root);
    if(!note || !root || !images_fingerprint(a,a->images.property_hash)) {free(note);free(root);cJSON_Delete(node);return;}
    free(a->images.property_note);free(a->images.property_root);a->images.property_note=note;a->images.property_root=root;
    show_prompt(a,field==0?PROMPT_IMAGE_ALT:field==1?PROMPT_IMAGE_CAPTION:PROMPT_IMAGE_WIDTH);
    const char *value=field==2?number:cJSON_IsString(v)?v->valuestring:"";
    EditBox_SetText(a->prompt,value); a->prompt->cursor=(int)strlen(value);
    a->images.property_block=index; a->images.property_revision=ce_document_revision(a->doc); cJSON_Delete(node);
}
static bool images_prompt_accept(App *a,const char *value) {
    Prompt mode=a->prompt_mode; if(mode!=PROMPT_IMAGE_ALT && mode!=PROMPT_IMAGE_CAPTION && mode!=PROMPT_IMAGE_WIDTH) return false;
    char hash[65];
    if(!a->active || !a->images.property_note || !a->images.property_root || strcmp(a->active,a->images.property_note) ||
       strcmp(a->notes_root,a->images.property_root) || !images_fingerprint(a,hash) || strcmp(hash,a->images.property_hash)) { message(a,"文章已改变，请重新选择图片"); return true; }
    if(strlen(value)>4096) { message(a,"图片说明过长"); return true; }
    unsigned width=0;
    if(mode==PROMPT_IMAGE_WIDTH) { if(!*value) { message(a,"请输入 0 到 2400 的宽度"); return true; } for(const char *p=value;*p;p++) { if(*p<'0'||*p>'9'||width>2400) { message(a,"请输入 0 到 2400 的宽度"); return true; } width=width*10+(unsigned)(*p-'0'); } if(width>2400) { message(a,"宽度最多 2400 像素"); return true; } }
    char *json=ce_document_block_json(a->doc,a->images.property_block); cJSON *node=json?cJSON_Parse(json):NULL; free(json);
    cJSON *attrs=cJSON_GetObjectItemCaseSensitive(node,"attrs");
    const char *key=mode==PROMPT_IMAGE_ALT?"alt":mode==PROMPT_IMAGE_CAPTION?"caption":"width";
    cJSON *replacement=mode==PROMPT_IMAGE_WIDTH?cJSON_CreateNumber(width):cJSON_CreateString(value);
    bool ok=node && cJSON_IsObject(attrs) && replacement;
    if(ok) { if(cJSON_GetObjectItemCaseSensitive(attrs,key)) ok=cJSON_ReplaceItemInObjectCaseSensitive(attrs,key,replacement); else ok=cJSON_AddItemToObject(attrs,key,replacement); }
    if(ok) replacement=NULL; cJSON_Delete(replacement); json=ok?ce_csl_json_print_exact_with_limit(node,16u*1024u*1024u):NULL; cJSON_Delete(node);
    CeDocument *candidate=edit_candidate(a); ok=candidate && json && ce_document_replace_block_json(candidate,a->images.property_block,json); cJSON_free(json);
    if(edit_commit(a,candidate,ok,NULL,false)) { sync_view(a); mark_changed(a); message(a,"图片属性已更新"); } else message(a,a->edit_error);
    a->prompt_mode=PROMPT_NONE; EditBox_SetFocus(a->prompt,false); EditBox_SetFocus(a->edit,true); return true;
}
#ifdef __EMSCRIPTEN__
static App *images_web_app;
EMSCRIPTEN_KEEPALIVE int images_web_begin(void) { return images_web_app && images_begin(images_web_app); }
EMSCRIPTEN_KEEPALIVE void *images_web_alloc(size_t size) { return size<=CE_IMAGE_MAX_BYTES?malloc(size):NULL; }
EMSCRIPTEN_KEEPALIVE void images_web_finish(void *bytes,size_t size) {
    if(images_web_app) { if(bytes) images_receive(images_web_app,bytes,size); else { images_pending_clear(images_web_app); message(images_web_app,"无法读取图片文件"); } }
    free(bytes);
}
EM_JS(void,images_web_install,(),{
    if(Module.noteLabImagePaste) return;
    function load(file) {
        if(!file || !['image/png','image/jpeg','image/bmp','image/x-ms-bmp'].includes(file.type)) return false;
        _platform_web_input_pointer_finish();
        if(!_images_web_begin()) return false;
        if(file.size>16777216) { _images_web_finish(0,0); return true; }
        file.arrayBuffer().then(function(buffer) {
            var bytes=new Uint8Array(buffer),ptr=_images_web_alloc(bytes.length);
            if(ptr) HEAPU8.set(bytes,ptr); _images_web_finish(ptr,bytes.length);
        }).catch(function(){_images_web_finish(0,0);}); return true;
    }
    Module.noteLabImagePaste=function(event) {
        if(!event.clipboardData) return false;
        for(var item of event.clipboardData.items) if(item.kind==='file' && item.type.startsWith('image/')) return load(item.getAsFile());
        return false;
    };
    var canvas=Module.canvas;
    canvas.addEventListener('dragover',function(event){if(event.dataTransfer && Array.from(event.dataTransfer.types).includes('Files')) event.preventDefault();});
    canvas.addEventListener('drop',function(event){
        if(event.dataTransfer && event.dataTransfer.files.length && load(event.dataTransfer.files[0])) { event.preventDefault();event.stopPropagation(); }
    },true);
});
static void images_web_init(App *a) { images_web_app=a; images_web_install(); }
#else
static void images_web_init(App *a) { (void)a; }
#endif
static void images_destroy(App *a) {
#ifdef __EMSCRIPTEN__
    if(images_web_app==a) images_web_app=NULL;
#endif
    images_pending_clear(a); platform_import_free(a->images.job); a->images.job=NULL; ce_image_store_close(&a->images.store); a->images.ready=false;
    for(size_t i=0;i<16;i++) { SDL_DestroySurface(a->images.cache[i].surface); a->images.cache[i].surface=NULL; }
    a->images.cache_bytes=0; free(a->images.property_note); free(a->images.property_root);a->images.property_note=a->images.property_root=NULL;
}
