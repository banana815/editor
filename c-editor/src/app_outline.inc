/* Folding is presentation state; only a successful section move edits JSON. */
typedef struct { uint64_t key; size_t ordinal; unsigned char folded; size_t used; } OutlineFold;
static int outline_fold_compare(const void *left,const void *right) {
    const OutlineFold *a=left,*b=right;
    return a->key<b->key?-1:a->key>b->key?1:a->ordinal<b->ordinal?-1:a->ordinal>b->ordinal;
}
static void outline_clear(App *a) {
    if(a->outline_view.pointer) SDL_CaptureMouse(false);
    ce_outline_free(&a->outline_view.index); free(a->outline_view.folded);
    free(a->outline_view.visible); free(a->outline_view.note);
    a->outline_view=(OutlineView){0};
}
static void outline_visible(App *a) {
    OutlineView *v=&a->outline_view; v->count=0;
    unsigned hidden=0;
    for(size_t i=0;i<v->index.count;i++) {
        const CeOutlineEntry *e=&v->index.entries[i];
        if(hidden && e->level>hidden) continue;
        hidden=0; v->visible[v->count++]=i;
        if(v->folded[i] && e->children) hidden=e->level;
    }
}
static bool outline_refresh(App *a) {
    OutlineView *v=&a->outline_view;
    uint64_t revision=ce_document_revision(a->doc);
    const char *name=a->active?a->active:"";
    if(v->valid && v->model==a->doc && v->revision==revision && v->note && !strcmp(v->note,name)) return true;
    CeOutline next={0};
    if(!ce_outline_build(a->doc,&next)) return false;
    unsigned char *folded=next.count?calloc(next.count,1):NULL;
    size_t *visible=next.count?malloc(next.count*sizeof *visible):NULL;
    char *note=copy_string(name);
    bool same=v->model==a->doc && v->note && !strcmp(v->note,name);
    OutlineFold *old=same && v->index.count?malloc(v->index.count*sizeof *old):NULL;
    if(!note || (next.count && (!folded || !visible)) || (same && v->index.count && !old)) {
        ce_outline_free(&next); free(folded); free(visible); free(note); free(old); return false;
    }
    if(old) {
        for(size_t i=0;i<v->index.count;i++) old[i]=(OutlineFold){v->index.entries[i].key,i,v->folded[i],0};
        qsort(old,v->index.count,sizeof *old,outline_fold_compare);
        for(size_t i=0;i<next.count;i++) {
            size_t lo=0,hi=v->index.count;
            while(lo<hi) { size_t mid=lo+(hi-lo)/2; if(old[mid].key<next.entries[i].key) lo=mid+1; else hi=mid; }
            if(lo<v->index.count && old[lo].key==next.entries[i].key) {
                /* Equal headings are matched in their previous stable order. */
                size_t pick=lo+old[lo].used++;
                if(pick<v->index.count && old[pick].key==old[lo].key) folded[i]=old[pick].folded;
            }
        }
    }
    free(old);
    if(v->pointer) SDL_CaptureMouse(false);
    outline_clear(a); v=&a->outline_view;
    v->index=next; v->folded=folded; v->visible=visible; v->note=note;
    v->model=a->doc; v->revision=revision; v->valid=true;
    outline_visible(a); return true;
}
static size_t outline_count(App *a) { return outline_refresh(a)?a->outline_view.count:0; }
static void outline_jump(App *a,size_t row) {
    if(!outline_refresh(a) || row>=a->outline_view.count) return;
    const CeOutlineEntry *entry=&a->outline_view.index.entries[a->outline_view.visible[row]];
    a->edit->cursor=(int)entry->start; a->edit->highlight1=a->edit->highlight2=-1;
    a->typing_mask=0; EditBox_SetFocus(a->edit,true);
    if(a->width<700) { a->sidebar=false; layout(a); }
    reveal_cursor(a); platform_web_input_request_focus();
}
static bool outline_reorder(App *a,size_t source,size_t target,bool after) {
    OutlineView *v=&a->outline_view;
    if(source>=v->index.count || target>=v->index.count) return false;
    CeDocument *candidate=edit_candidate(a); if(!candidate) return false;
    size_t offset=0; uint64_t before=ce_document_revision(candidate);
    bool ok=ce_outline_move(candidate,v->index.entries[source].block_index,v->index.entries[target].block_index,
        after,&offset,a->edit_error,sizeof a->edit_error);
    if(!ok) { ce_document_free(candidate); message(a,a->edit_error); return false; }
    if(before==ce_document_revision(candidate)) { ce_document_free(candidate); return true; }
    if(!edit_commit(a,candidate,true,NULL,false)) { message(a,a->edit_error); return false; }
    sync_view(a); a->edit->cursor=(int)SDL_min(offset,ce_document_length(a->doc));
    a->edit->highlight1=a->edit->highlight2=-1; a->typing_mask=0;
    mark_changed(a); reveal_cursor(a);
    message(a,"章节及其正文已移动 · 撤销可完整还原"); return true;
}
static void outline_draw(App *a,SDL_Rect side,SDL_Color text) {
    if(!outline_refresh(a)) { clipped_label(a,"无法更新大纲",(SDL_FRect){16,side.y+12,side.w-24,30},text); return; }
    OutlineView *v=&a->outline_view;
    for(size_t row=0;row<v->count;row++) {
        size_t index=v->visible[row]; const CeOutlineEntry *e=&v->index.entries[index];
        float y=a->sidebar_area.y+44+(float)row*40-a->sidebar_scroll;
        if(y+36<side.y || y>side.y+side.h) continue;
        float indent=12+(e->level-1)*12;
        if((size_t)SDL_max(0,a->edit->cursor)>=e->start && (size_t)SDL_max(0,a->edit->cursor)<e->section_end)
            fill(a,(SDL_FRect){6,y,a->sidebar_area.w-12,36},a->dark?(SDL_Color){43,66,62,255}:(SDL_Color){222,235,226,255});
        if(e->children) label(a,v->folded[index]?"+":"-",indent,y+6,text);
        char title[256]; size_t n=SDL_min(e->end-e->start,sizeof title-1);
        const char *source=ce_document_text(a->doc)+e->start;
        while(n && ((unsigned char)source[n]&0xc0)==0x80) n--;
        memcpy(title,source,n); title[n]=0;
        clipped_label(a,*title?title:"无标题",(SDL_FRect){indent+21,y+6,a->sidebar_area.w-indent-53,30},text);
        label(a,"≡",a->sidebar_area.w-25,y+6,text);
        if(v->pointer && v->handle && v->moved && index==v->target)
            fill(a,(SDL_FRect){8,y+(v->after?37:0),a->sidebar_area.w-16,3},(SDL_Color){66,149,116,255});
    }
    if(!v->count) {
        clipped_label(a,"用标题组织文章",(SDL_FRect){16,side.y+12,side.w-24,30},text);
        clipped_label(a,"输入 # 空格创建标题",(SDL_FRect){16,side.y+48,side.w-24,30},text);
    }
}
static bool outline_event(App *a,SDL_Event *event) {
    OutlineView *v=&a->outline_view;
    if(event->type==SDL_EVENT_WINDOW_FOCUS_LOST && v->pointer) { v->pointer=false; SDL_CaptureMouse(false); return false; }
    if(v->pointer && (event->type==SDL_EVENT_MOUSE_MOTION || event->type==SDL_EVENT_MOUSE_BUTTON_UP)) {
        if(v->model!=a->doc || !v->note || !a->active || strcmp(v->note,a->active) ||
           v->drag_revision!=ce_document_revision(a->doc) || !a->outline || !a->sidebar || a->focus) {
            v->pointer=false; SDL_CaptureMouse(false); return true;
        }
        float x=event->type==SDL_EVENT_MOUSE_MOTION?event->motion.x:event->button.x;
        float y=event->type==SDL_EVENT_MOUSE_MOTION?event->motion.y:event->button.y;
        if(event->type==SDL_EVENT_MOUSE_BUTTON_UP && event->button.button!=SDL_BUTTON_LEFT) return true;
        if(SDL_fabsf(x-v->down_x)>6 || SDL_fabsf(y-v->down_y)>6) v->moved=true;
        if(event->type==SDL_EVENT_MOUSE_MOTION) {
            if(v->moved && !v->handle) a->sidebar_scroll=SDL_clamp(a->sidebar_scroll+v->last_y-y,0,
                SDL_max(0,(float)v->count*40-a->sidebar_area.h+44));
            if(v->moved && v->handle) {
                if(y<a->sidebar_area.y+60) a->sidebar_scroll=SDL_max(0,a->sidebar_scroll-12);
                if(y>a->sidebar_area.y+a->sidebar_area.h-24) a->sidebar_scroll=SDL_min(
                    SDL_max(0,(float)v->count*40-a->sidebar_area.h+44),a->sidebar_scroll+12);
                float position=SDL_clamp(y-a->sidebar_area.y-44+a->sidebar_scroll,0,(float)v->count*40-1);
                size_t row=(size_t)(position/40); v->target=v->visible[row]; v->after=position-row*40>=20;
            }
            v->last_y=y; return true;
        }
        size_t pressed=v->pressed,target=v->target; bool move=v->moved && v->handle,after=v->after,click=!v->moved;
        v->pointer=false; SDL_CaptureMouse(false);
        if(move && contains(a->sidebar_area,x,y)) outline_reorder(a,pressed,target,after);
        else if(click) for(size_t row=0;row<v->count;row++) if(v->visible[row]==pressed) { outline_jump(a,row); break; }
        return true;
    }
    if(event->type!=SDL_EVENT_MOUSE_BUTTON_DOWN || event->button.button!=SDL_BUTTON_LEFT ||
       !a->outline || !a->sidebar || a->focus || !contains(a->sidebar_area,event->button.x,event->button.y)) return false;
    if(!outline_refresh(a)) return true;
    float position=event->button.y-a->sidebar_area.y-44+a->sidebar_scroll;
    if(event->button.y<a->sidebar_area.y+40 || position<0 || (size_t)(position/40)>=v->count) return true;
    size_t index=v->visible[(size_t)(position/40)]; const CeOutlineEntry *entry=&v->index.entries[index];
    float indent=12+(entry->level-1)*12;
    if(entry->children && event->button.x>=indent && event->button.x<indent+22) {
        v->folded[index]=!v->folded[index]; outline_visible(a);
        a->sidebar_scroll=SDL_clamp(a->sidebar_scroll,0,SDL_max(0,(float)v->count*40-a->sidebar_area.h+44)); return true;
    }
    v->pointer=true; v->moved=false; v->handle=event->button.x>=a->sidebar_area.w-32;
    v->pressed=v->target=index; v->after=false;
    v->down_x=event->button.x; v->down_y=v->last_y=event->button.y;
    v->drag_revision=ce_document_revision(a->doc); SDL_CaptureMouse(true); return true;
}
