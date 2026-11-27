/* Search is an explicit, read-only traversal of the article store. The active
 * in-memory article wins over its saved copy, including unsaved changes. */
static void search_results_clear(App *a) {
    for(size_t i=0;i<a->search_count;i++) { free(a->search_hits[i].name); free(a->search_hits[i].snippet); }
    free(a->search_hits); a->search_hits=NULL; a->search_count=0; a->search_scroll=0;
}
static void search_close(App *a) {
    a->search_open=false; a->search_dragging=false; SDL_CaptureMouse(false);
    EditBox_SetFocus(a->edit,a->active!=NULL && !a->library_open && !a->manager_open);
}
static char *search_snippet(const CeDocument *doc,size_t start,size_t end) {
    const char *text=ce_document_text(doc); size_t length=ce_document_length(doc);
    size_t left=start>36?start-36:0,right=SDL_min(length,end+90);
    while(left<start && ((unsigned char)text[left]&0xc0)==0x80) ++left;
    while(right>left && ((unsigned char)text[right]&0xc0)==0x80) --right;
    char *out=malloc(right-left+1); if(!out) return NULL;
    memcpy(out,text+left,right-left); out[right-left]=0;
    for(char *p=out;*p;p++) if(*p=='\n' || *p=='\r' || *p=='\t') *p=' ';
    return out;
}
static bool search_add(SearchHit **hits,size_t *count,const char *name,const CeDocument *doc,const char *query) {
    size_t start,end;
    if(!ce_document_search_next(doc,query,0,false,&start,&end)) return true;
    SearchHit hit={copy_string(name),search_snippet(doc,start,end),start,end};
    if(!hit.name || !hit.snippet) { free(hit.name); free(hit.snippet); return false; }
    if(*count>=SIZE_MAX/sizeof **hits) { free(hit.name); free(hit.snippet); return false; }
    SearchHit *next=realloc(*hits,(*count+1)*sizeof **hits);
    if(!next) { free(hit.name); free(hit.snippet); return false; }
    next[(*count)++]=hit; *hits=next; return true;
}
static bool search_collect(App *a,const char *query) {
    if(!ce_document_search_query_valid(query)) { message(a,"请输入非空的单行查找文字"); return false; }
    NoteList files={0};
    if(note_store_list(&a->store,&files)!=NOTE_OK) { message(a,a->store.error); return false; }
    SearchHit *hits=NULL; size_t count=0,skipped=0; bool ok=true,active_seen=false;
    for(size_t i=0;ok && i<files.count;i++) {
        if(a->active && !strcmp(a->active,files.names[i])) {
            active_seen=true; ok=search_add(&hits,&count,files.names[i],a->doc,query); continue;
        }
        char *data=NULL; size_t length=0; char error[256];
        if(note_store_read(&a->store,files.names[i],&data,&length)!=NOTE_OK) { ++skipped; continue; }
        CeDocument *doc=ce_document_new();
        if(!doc) { note_store_free(data); ok=false; break; }
        bool loaded=length<=INT_MAX && !memchr(data,0,length) &&
            ce_document_import(doc,data,CE_FORMAT_AUTO,error,sizeof error);
        note_store_free(data);
        if(loaded) ok=search_add(&hits,&count,files.names[i],doc,query); else ++skipped;
        ce_document_free(doc);
    }
    if(ok && a->active && !active_seen) ok=search_add(&hits,&count,a->active,a->doc,query);
    note_store_list_free(&files);
    char *owned=ok?copy_string(query):NULL;
    if(!owned) {
        for(size_t i=0;i<count;i++) { free(hits[i].name); free(hits[i].snippet); }
        free(hits); message(a,"内存不足，搜索结果保持不变"); return false;
    }
    search_results_clear(a); free(a->all_search_query); a->all_search_query=owned;
    a->search_hits=hits; a->search_count=count; a->search_skipped=skipped;
    a->search_open=true; a->manager_open=false; a->touch_menu_visible=false;
    EditBox_SetFocus(a->edit,false);
    char status[256];
    if(skipped) SDL_snprintf(status,sizeof status,"找到 %zu 篇文章 · %zu 个文件无法读取 · 当前文章以编辑内容为准",count,skipped);
    else SDL_snprintf(status,sizeof status,"找到 %zu 篇文章 · 当前文章以编辑内容为准",count);
    message(a,status); return true;
}
static void search_begin_all(App *a) {
    a->manager_open=false;
    show_prompt(a,PROMPT_SEARCH_ALL);
    if(a->all_search_query) { EditBox_SetText(a->prompt,a->all_search_query); a->prompt->cursor=(int)strlen(a->all_search_query); }
}
static void search_begin_replace(App *a,bool all) {
    if(!a->active) { message(a,"请先打开文章"); return; }
    a->replace_all=all; show_prompt(a,PROMPT_REPLACE_QUERY);
    if(a->search_keyword) { EditBox_SetText(a->prompt,a->search_keyword); a->prompt->cursor=(int)strlen(a->search_keyword); }
}
/* Returns true for every search-owned prompt, including a validation error;
 * unrelated prompt types continue through the app's normal dispatcher. */
static bool search_prompt_accept(App *a,const char *value) {
    if(a->prompt_mode==PROMPT_SEARCH_ALL) {
        if(search_collect(a,value)) { close_prompt(a); EditBox_SetFocus(a->edit,false); }
        return true;
    }
    if(a->prompt_mode==PROMPT_REPLACE_QUERY) {
        if(!ce_document_search_query_valid(value)) { message(a,"请输入非空的单行查找文字"); return true; }
        char *query=copy_string(value); if(!query) { message(a,"内存不足"); return true; }
        free(a->replace_query); a->replace_query=query;
        size_t from=a->selection_start;
        show_prompt(a,PROMPT_REPLACE_VALUE); a->selection_start=from;
        if(a->replace_text) { EditBox_SetText(a->prompt,a->replace_text); a->prompt->cursor=(int)strlen(a->replace_text); }
        return true;
    }
    if(a->prompt_mode!=PROMPT_REPLACE_VALUE) return false;
    char *replacement=copy_string(value);
    if(!replacement) { message(a,"内存不足，原文保持不变"); return true; }
    CeDocument *candidate=edit_candidate(a); size_t count=0,cursor=0;
    bool ok=candidate && ce_document_search_replace(candidate,a->replace_query,replacement,
        a->selection_start,a->replace_all,&count,&cursor,a->edit_error,sizeof a->edit_error);
    if(!ok) { ce_document_free(candidate); free(replacement); message(a,a->edit_error); return true; }
    if(!count) { ce_document_free(candidate); free(replacement); close_prompt(a); message(a,"没有找到匹配文字"); return true; }
    if(!edit_commit(a,candidate,true,NULL,false)) { free(replacement); message(a,a->edit_error); return true; }
    free(a->replace_text); a->replace_text=replacement;
    char *query=copy_string(a->replace_query); if(query) { free(a->search_keyword); a->search_keyword=query; }
    close_prompt(a); sync_view(a); a->typing_mask=0;
    a->edit->cursor=(int)SDL_min(cursor,ce_document_length(a->doc));
    mark_changed(a); reveal_cursor(a);
    char status[128]; SDL_snprintf(status,sizeof status,"已替换 %zu 处 · 可一次撤销",count); message(a,status);
    return true;
}
static void search_layout(App *a) {
    float width=SDL_min(720.0f,SDL_max(1.0f,a->width-24.0f)),height=SDL_max(60.0f,a->height-64.0f);
    a->search_area=(SDL_FRect){(a->width-width)/2,12,width,height};
    a->search_close=(SDL_FRect){a->search_area.x+width-80,22,68,36};
    a->search_again=(SDL_FRect){a->search_area.x+width-172,22,84,36};
    a->search_scroll=SDL_clamp(a->search_scroll,0,SDL_max(0,(float)a->search_count*70-height+102));
}
static void search_open_hit(App *a,size_t index) {
    if(index>=a->search_count) return;
    SearchHit *hit=&a->search_hits[index];
    if(!a->active || strcmp(a->active,hit->name)) {
        open_note(a,hit->name);
        if(!a->active || strcmp(a->active,hit->name)) {
            message(a,"文章未打开；请先完成当前文章保存，再重试"); return;
        }
    }
    size_t start,end;
    if(!ce_document_search_next(a->doc,a->all_search_query,hit->start,true,&start,&end)) {
        message(a,"文章内容已变化，未找到原匹配；请重新搜索"); return;
    }
    search_close(a); a->edit->cursor=(int)end; a->edit->cursor_trailing=false;
    a->edit->highlight1=(int)start; a->edit->highlight2=(int)end; a->typing_mask=0;
    char *query=copy_string(a->all_search_query); if(query) { free(a->search_keyword); a->search_keyword=query; }
    layout(a); reveal_cursor(a); platform_web_input_request_focus();
    message(a,"已打开搜索结果并选中匹配文字");
}
/* Parent calls after library/prompt routing and before normal editor input. */
static bool search_event(App *a,SDL_Event *event) {
    if(!a->search_open) return false;
    search_layout(a);
    if(event->type==SDL_EVENT_KEY_DOWN && event->key.key==SDLK_ESCAPE) search_close(a);
    else if(event->type==SDL_EVENT_WINDOW_FOCUS_LOST) {
        a->search_dragging=false; SDL_CaptureMouse(false);
    } else if(event->type==SDL_EVENT_MOUSE_WHEEL) {
        a->search_scroll-=event->wheel.y*45; search_layout(a);
    } else if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN) {
        float x=event->button.x,y=event->button.y;
        if(contains(a->search_close,x,y)) search_close(a);
        else if(contains(a->search_again,x,y)) search_begin_all(a);
        else if(contains(a->search_area,x,y) && y>=a->search_area.y+94) {
            float row=y-a->search_area.y-94+a->search_scroll;
            a->search_dragging=true; a->search_drag_moved=false;
            a->search_drag_x=x; a->search_drag_y=y; a->search_drag_scroll=a->search_scroll;
            a->search_pressed_row=row>=0?(size_t)(row/70):SIZE_MAX;
            SDL_CaptureMouse(true);
        }
    } else if(event->type==SDL_EVENT_MOUSE_MOTION && a->search_dragging) {
        float dx=event->motion.x-a->search_drag_x,dy=event->motion.y-a->search_drag_y;
        if(SDL_fabsf(dx)>=CE_TOUCH_SLOP || SDL_fabsf(dy)>=CE_TOUCH_SLOP) a->search_drag_moved=true;
        if(a->search_drag_moved) { a->search_scroll=a->search_drag_scroll-dy; search_layout(a); }
    } else if(event->type==SDL_EVENT_MOUSE_BUTTON_UP && a->search_dragging) {
        a->search_dragging=false; SDL_CaptureMouse(false);
        float x=event->button.x,y=event->button.y;
        float row=y-a->search_area.y-94+a->search_scroll;
        if(!a->search_drag_moved && contains(a->search_area,x,y) && y>=a->search_area.y+94 && row>=0 &&
           (size_t)(row/70)==a->search_pressed_row) search_open_hit(a,a->search_pressed_row);
    }
    return true;
}
static void search_draw(App *a) {
    if(!a->search_open) return;
    search_layout(a);
    SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_BLEND);
    fill(a,(SDL_FRect){0,0,(float)a->width,(float)a->height},(SDL_Color){0,0,0,100});
    SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_NONE);
    SDL_Color text=ink(a),surface=a->dark?(SDL_Color){44,51,58,255}:(SDL_Color){237,242,238,255};
    fill(a,a->search_area,paper(a)); fill(a,a->search_again,surface); fill(a,a->search_close,surface);
    label(a,"重新搜索",a->search_again.x+8,a->search_again.y+7,text);
    label(a,"关闭",a->search_close.x+16,a->search_close.y+7,text);
    clipped_label(a,"搜索文章",(SDL_FRect){a->search_area.x+14,29,a->search_area.w-196,28},text);
    char title[512]; SDL_snprintf(title,sizeof title,"%s · %zu 篇文章",a->all_search_query?a->all_search_query:"",a->search_count);
    clipped_label(a,title,(SDL_FRect){a->search_area.x+14,70,a->search_area.w-28,28},text);
    SDL_Rect clip={(int)a->search_area.x+8,(int)a->search_area.y+94,(int)a->search_area.w-16,(int)a->search_area.h-100};
    SDL_SetRenderClipRect(a->renderer,&clip);
    if(!a->search_count) label(a,"没有匹配文章，请换一个关键词",clip.x+6,clip.y+12,text);
    for(size_t i=0;i<a->search_count;i++) {
        float y=clip.y+(float)i*70-a->search_scroll;
        if(y+66<clip.y || y>clip.y+clip.h) continue;
        fill(a,(SDL_FRect){clip.x,y,clip.w,64},surface);
        clipped_label(a,a->search_hits[i].name,(SDL_FRect){clip.x+8,y+5,clip.w-16,28},text);
        clipped_label(a,a->search_hits[i].snippet,(SDL_FRect){clip.x+8,y+32,clip.w-16,28},text);
    }
    SDL_SetRenderClipRect(a->renderer,NULL);
}
