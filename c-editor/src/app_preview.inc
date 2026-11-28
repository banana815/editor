static bool preview_finite(float value) { return !SDL_isnanf(value) && !SDL_isinff(value); }
static void preview_dispose(BlogPreview *p) {
    ce_rich_layout_destroy(p->layout); ce_document_free(p->document);
    ce_citation_prepared_free(p->snapshot); SDL_DestroyTexture(p->texture);
    p->layout=NULL; p->document=NULL; p->snapshot=NULL; p->texture=NULL;
    free(p->notes_root); p->notes_root=NULL;
    p->texture_width=p->texture_height=0; p->valid=false; p->model=NULL;
}
static void preview_reset(App *a) {
    preview_dispose(&a->preview); a->preview.open=false; a->preview.pointer=false;
}
static CeRichCitationStatus preview_citation(void *context,const cJSON *atom,
    const CeCitationCache **cache,char *error,size_t capacity) {
    (void)error; (void)capacity;
    const CeCitationPrepared *snapshot=context;
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(atom,"type");
    const cJSON *id=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(atom,"attrs"),"citationId");
    *cache=cJSON_IsString(type) && !strcmp(type->valuestring,"zoteroEndnoteSection")?
        ce_citation_prepared_bibliography(snapshot):cJSON_IsString(id)?ce_citation_prepared_citation(snapshot,id->valuestring):NULL;
    return *cache?CE_RICH_CITATION_VALID:CE_RICH_CITATION_UNSUPPORTED;
}
/* Validation uses a bounded asset URL, not base64. Otherwise a valid 13 MiB
 * image alone exceeds the publication serializer's 16 MiB text budget. */
static char *preview_image_url(void *context,const cJSON *node,char *error,size_t capacity) {
    CeImageStore *store=context;
    const cJSON *src=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(node,"attrs"),"src");
    const char *name=cJSON_IsString(src)?ce_image_asset_name(src->valuestring):NULL;
    unsigned char *bytes=NULL; size_t size=0;
    if(!name || !ce_image_store_read(store,src->valuestring,&bytes,&size)) {
        SDL_snprintf(error,capacity,"%s",name?store->error:"图片不是本地受管资源"); return NULL;
    }
    free(bytes);
    char path[80]; SDL_snprintf(path,sizeof path,"assets/%s",name);
    char *url=copy_string(path);
    if(!url) SDL_strlcpy(error,"内存不足，无法校验图片",capacity);
    return url;
}
static SDL_Surface *preview_image_surface(void *context,const cJSON *node,char *error,size_t capacity) {
    App *a=context; SDL_Surface *surface=images_resolve(a,node,error,capacity);
    if(!surface) SDL_strlcpy(a->preview.error,*error?error:"图片无法解码，不能预览发布效果",sizeof a->preview.error);
    return surface;
}
static bool preview_prepare(App *a) {
    BlogPreview *p=&a->preview;
    if(!p->open || !a->active) return false;
    float width=p->mobile?390:720;
    if(publish_has_images(a) && !images_ready(a)) { p->valid=false; p->model=NULL; SDL_strlcpy(p->error,a->images.store.error,sizeof p->error); return false; }
    float font_size=TTF_GetFontSize(a->font);
    uint64_t revision=ce_document_revision(a->doc);
    if(p->model==a->doc && p->revision==revision && p->citation_revision==a->citation_revision && p->width==width &&
       p->images_revision==a->images.store.revision && p->notes_root && !strcmp(p->notes_root,a->notes_root) && p->font_size==font_size)
        return p->valid;
    char *notes_root=copy_string(a->notes_root);
    if(!notes_root) { p->valid=false; p->model=NULL; SDL_strlcpy(p->error,"内存不足，未更新预览",sizeof p->error); return false; }
    free(p->notes_root); p->notes_root=notes_root; p->images_revision=a->images.store.revision; p->font_size=font_size;
    p->model=a->doc; p->revision=revision; p->citation_revision=a->citation_revision; p->width=width;
    p->valid=false; p->error[0]=0;
    CeCitationSession *session=NULL; CeCitationPrepared *snapshot=NULL; CeDocument *document=NULL;
    CeRichLayout *rich=NULL; char *html=NULL; bool ok=false;
    if(!ce_citation_session_build(a->doc,&a->library.library,NULL,&session,p->error,sizeof p->error) ||
       !ce_citation_session_snapshot(session,a->doc,&a->library.library,&snapshot,p->error,sizeof p->error)) goto done;
    document=ce_document_new();
    if(!document || !ce_document_import(document,ce_citation_prepared_json(snapshot),CE_FORMAT_JSON,p->error,sizeof p->error)) goto done;
    if(publish_has_images(a) && !images_ready(a)) { SDL_strlcpy(p->error,a->images.store.error,sizeof p->error); goto done; }
    /* The actual publication serializer must accept the whole snapshot before
     * it is presented as publishable. Unsupported data is never hidden. */
    html=ce_document_publish_images(document,CE_PUBLISH_HTML,publish_atom_html,snapshot,
        preview_image_url,&a->images.store,p->error,sizeof p->error);
    if(!html) goto done;
    rich=ce_rich_layout_create(a->renderer,a->font); if(!rich) goto done;
    ce_rich_layout_set_fallback(rich,a->emoji_font);
    CeRichImageSource images={a->images.store.revision,a,preview_image_surface}; ce_rich_layout_set_images(rich,&images);
    CeRichTheme theme=ce_rich_theme_default(false);
    CeRichCitationSource citations={a->citation_revision,snapshot,preview_citation};
    if(!ce_rich_layout_update_citations(rich,document,width-48,&theme,&citations)) {
        SDL_strlcpy(p->error,ce_rich_layout_error(rich),sizeof p->error); goto done;
    }
    if(*p->error) goto done; /* A decode failure must not become a publishable placeholder. */
    ce_rich_layout_destroy(p->layout); ce_document_free(p->document); ce_citation_prepared_free(p->snapshot);
    p->layout=rich; rich=NULL; p->document=document; document=NULL; p->snapshot=snapshot; snapshot=NULL;
    p->images=p->links=p->code=0;
    for(size_t i=0;i<ce_document_block_count(p->document);i++) {
        CeBlockInfo block; if(!ce_document_block_info(p->document,i,&block)) continue;
        p->images+=block.kind==CE_BLOCK_IMAGE; p->code+=block.kind==CE_BLOCK_CODE;
    }
    for(size_t i=0;i<ce_document_span_count(p->document);i++) {
        CeStyleSpan span; if(ce_document_span(p->document,i,&span) && (span.style.marks&CE_MARK_LINK)) p->links++;
    }
    p->valid=true; ok=true;
done:
    if(!ok && !*p->error) SDL_strlcpy(p->error,"无法准备发布预览，正文保持不变",sizeof p->error);
    free(html); ce_rich_layout_destroy(rich); ce_document_free(document);
    ce_citation_prepared_free(snapshot); ce_citation_session_free(session); return ok;
}
static void preview_toggle(App *a) {
    if(a->preview.open) { a->preview.open=false; a->preview.pointer=false; EditBox_SetFocus(a->edit,a->active!=NULL); }
    else {
        if(!a->active || a->edit->composition_length || a->edit->composition_snapshot) { message(a,"请先打开文章并完成输入，再预览发布效果"); return; }
        if(a->manager_open) manager_close(a); if(a->search_open) search_close(a); if(a->library_open) library_close(a);
        a->preview.open=true; a->preview.scroll=0; a->preview.model=NULL;
        a->touch_menu_visible=false; message(a,"发布预览 · 桌面 / 手机排版 · 点击链接查看地址");
    }
    layout(a); reveal_cursor(a);
}
static void preview_layout(App *a,float left,float top) {
    BlogPreview *p=&a->preview; p->area=p->viewport=(SDL_FRect){0};
    if(!p->open || !a->active) return;
    float available=a->width-left;
    if(available>=760) {
        float half=available*.5f;
        a->editor_area.x=left+24; a->editor_area.w=half-48;
        p->area=(SDL_FRect){left+half,top,half,SDL_max(40,a->height-top-58)};
    } else {
        p->area=(SDL_FRect){0,top,(float)a->width,SDL_max(40,a->height-top-58)};
        a->sidebar=false; a->sidebar_area.w=0;
        EditBox_SetFocus(a->edit,false);
    }
    float button=(p->area.w-20)/4;
    for(size_t i=0;i<4;i++) p->buttons[i]=(SDL_FRect){p->area.x+8+i*(button+1),top+8,button,34};
    float preview_width=SDL_min(p->mobile?390.0f:720.0f,SDL_max(1,p->area.w-20));
    p->viewport=(SDL_FRect){p->area.x+(p->area.w-preview_width)/2,top+77,preview_width,SDL_max(1,p->area.h-87)};
    p->scale=SDL_min(1.0f,p->viewport.w/(p->mobile?390.0f:720.0f));
}
static void preview_scroll(App *a,float delta) {
    BlogPreview *p=&a->preview;
    if(!(p->scale>0) || !preview_finite(p->scale) || !preview_finite(delta)) return;
    if(!preview_prepare(a)) return;
    p->scroll=SDL_clamp(p->scroll+delta,0,SDL_max(0,ce_rich_layout_height(p->layout)+48-p->viewport.h/p->scale));
}
static void preview_draw(App *a) {
    BlogPreview *p=&a->preview; if(!p->open || !a->active) return;
    SDL_Color border=a->dark?(SDL_Color){42,49,55,255}:(SDL_Color){235,239,235,255};
    fill(a,p->area,border);
    const char *buttons[]={"桌面","手机","导出HTML","关闭"};
    for(size_t i=0;i<4;i++) {
        fill(a,p->buttons[i],(i==0&&!p->mobile)||(i==1&&p->mobile)?(SDL_Color){177,211,192,255}:paper(a));
        clipped_label(a,buttons[i],(SDL_FRect){p->buttons[i].x+6,p->buttons[i].y+7,p->buttons[i].w-8,27},ink(a));
    }
    if(!preview_prepare(a)) {
        clipped_label(a,"无法发布这篇文章",(SDL_FRect){p->area.x+16,p->area.y+50,p->area.w-32,28},ink(a));
        clipped_label(a,p->error,(SDL_FRect){p->area.x+16,p->area.y+90,p->area.w-32,32},ink(a)); return;
    }
    char summary[160]; SDL_snprintf(summary,sizeof summary,"%s %dpx · 图片 %zu · 链接 %zu · 代码 %zu",p->mobile?"手机":"桌面",p->mobile?390:720,p->images,p->links,p->code);
    clipped_label(a,summary,(SDL_FRect){p->area.x+12,p->area.y+48,p->area.w-24,27},ink(a));
    preview_scroll(a,0);
    float logical_height=p->scale>0?SDL_ceilf(p->viewport.h/p->scale):0;
    if(!preview_finite(logical_height) || logical_height<1 || logical_height>8192) {
        clipped_label(a,"调整窗口大小以查看预览",p->viewport,ink(a)); return;
    }
    int width=(int)p->width,height=(int)logical_height;
    if(!p->texture || p->texture_width!=width || p->texture_height!=height) {
        SDL_Texture *texture=SDL_CreateTexture(a->renderer,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,width,height);
        if(!texture) { message(a,"无法创建预览画布，文章保持不变"); return; }
        SDL_DestroyTexture(p->texture); p->texture=texture; p->texture_width=width; p->texture_height=height;
        SDL_SetTextureScaleMode(texture,SDL_SCALEMODE_LINEAR);
    }
    SDL_Texture *previous=SDL_GetRenderTarget(a->renderer);
    if(!SDL_SetRenderTarget(a->renderer,p->texture)) return;
    SDL_SetRenderDrawColor(a->renderer,255,255,253,255); SDL_RenderClear(a->renderer);
    SDL_FRect clip={0,0,(float)width,(float)height};
    bool drawn=ce_rich_layout_draw(p->layout,24,24-p->scroll,&clip,0,0);
    SDL_SetRenderTarget(a->renderer,previous);
    if(drawn) SDL_RenderTexture(a->renderer,p->texture,NULL,&p->viewport);
}
typedef struct { float x,y; bool inside; } PreviewLinkHit;
static void preview_link_rect(const SDL_FRect *rect,void *context) {
    PreviewLinkHit *hit=context;
    if(hit->x>=rect->x && hit->x<rect->x+rect->w && hit->y>=rect->y && hit->y<rect->y+rect->h) hit->inside=true;
}
static bool preview_event(App *a,SDL_Event *event) {
    BlogPreview *p=&a->preview; if(!p->open || !a->active) return false;
    if(event->type==SDL_EVENT_KEY_DOWN && event->key.key==SDLK_ESCAPE && !a->edit->composition_length) { preview_toggle(a); return true; }
    if(event->type==SDL_EVENT_WINDOW_FOCUS_LOST) p->pointer=false;
    if(event->type==SDL_EVENT_MOUSE_WHEEL && contains(p->area,event->wheel.mouse_x,event->wheel.mouse_y)) { preview_scroll(a,-event->wheel.y*48/p->scale); return true; }
    if(p->pointer && (event->type==SDL_EVENT_MOUSE_MOTION || event->type==SDL_EVENT_MOUSE_BUTTON_UP)) {
        if(event->type==SDL_EVENT_MOUSE_BUTTON_UP && event->button.button!=SDL_BUTTON_LEFT) return true;
        float x=event->type==SDL_EVENT_MOUSE_MOTION?event->motion.x:event->button.x;
        float y=event->type==SDL_EVENT_MOUSE_MOTION?event->motion.y:event->button.y;
        if(SDL_fabsf(x-p->down_x)>6 || SDL_fabsf(y-p->down_y)>6) p->moved=true;
        if(event->type==SDL_EVENT_MOUSE_MOTION) { preview_scroll(a,(p->last_y-y)/p->scale); p->last_y=y; }
        else {
            p->pointer=false;
            if(!p->moved && p->valid && p->scale>0 && contains(p->viewport,x,y)) {
                PreviewLinkHit hit={(x-p->viewport.x)/p->scale-24,(y-p->viewport.y)/p->scale+p->scroll-24,false};
                for(size_t i=0;i<ce_document_span_count(p->document);i++) {
                    CeStyleSpan span; if(!ce_document_span(p->document,i,&span) || !(span.style.marks&CE_MARK_LINK) || !span.style.link || !*span.style.link) continue;
                    ce_rich_layout_selection_rects(p->layout,span.start,span.end,preview_link_rect,&hit);
                    if(hit.inside) { char text[512]; SDL_snprintf(text,sizeof text,"链接地址：%s",span.style.link); message(a,text); break; }
                }
            }
        }
        return true;
    }
    if(event->type!=SDL_EVENT_MOUSE_BUTTON_DOWN || !contains(p->area,event->button.x,event->button.y)) return false;
    if(event->button.button!=SDL_BUTTON_LEFT) return true;
    for(size_t i=0;i<4;i++) if(contains(p->buttons[i],event->button.x,event->button.y)) {
        if(i<2) { p->mobile=i==1; p->scroll=0; layout(a); }
        else if(i==2) publish_action(a,CE_PUBLISH_HTML,false);
        else preview_toggle(a);
        return true;
    }
    if(contains(p->viewport,event->button.x,event->button.y)) {
        p->pointer=true; p->moved=false; p->down_x=event->button.x; p->down_y=p->last_y=event->button.y;
    }
    return true;
}
