static void show_prompt(App *a,Prompt kind);
static void editing_break(App *a) { a->undo_typing=false; a->link_click_pending=false; }

static void editing_event(App *a,const SDL_Event *event) {
    if(event->type==SDL_EVENT_KEY_DOWN || event->type==SDL_EVENT_TEXT_INPUT)
        a->link_click_pending=false;
    if(event->type==SDL_EVENT_TEXT_EDITING || event->type==SDL_EVENT_MOUSE_BUTTON_DOWN ||
       event->type==SDL_EVENT_FINGER_DOWN || event->type==SDL_EVENT_WINDOW_FOCUS_LOST)
        editing_break(a);
    if(event->type==SDL_EVENT_KEY_DOWN) {
        SDL_Keycode key=event->key.key;
        if((event->key.mod&(SDL_KMOD_CTRL|SDL_KMOD_GUI|SDL_KMOD_ALT)) ||
           key==SDLK_LEFT || key==SDLK_RIGHT || key==SDLK_UP || key==SDLK_DOWN ||
           key==SDLK_HOME || key==SDLK_END || key==SDLK_PAGEUP || key==SDLK_PAGEDOWN ||
           key==SDLK_BACKSPACE || key==SDLK_DELETE || key==SDLK_RETURN || key==SDLK_TAB || key==SDLK_ESCAPE)
            editing_break(a);
    }
}
static void editing_record_input(App *a,uint64_t before,size_t start,bool eligible) {
    uint64_t current=ce_document_revision(a->doc); Uint64 now=SDL_GetTicks();
    if(!eligible || current==before) { editing_break(a); return; }
    if(a->undo_typing && a->undo_revision==before && a->undo_caret==start &&
       now-a->undo_typed_at<=500) ce_document_join_undo(a->doc,before);
    a->undo_typing=true; a->undo_revision=current; a->undo_typed_at=now;
    a->undo_caret=(size_t)SDL_max(0,a->edit->cursor);
}

/* Links can cross formatting spans but not paragraph boundaries. Use the
 * authoritative marks, preserving every non-link mark when editing/removing. */
static const char *editing_link_range(App *a,size_t *start,size_t *end) {
    CeStyleSpan span; size_t count=ce_document_span_count(a->doc),index=count;
    if(*start!=*end) return ce_document_style_at(a->doc,*start).link;
    size_t caret=*start;
    for(size_t i=0;i<count;i++) {
        if(!ce_document_span(a->doc,i,&span) || !span.style.link || !*span.style.link) continue;
        if(caret>=span.start && caret<span.end) { index=i; break; }
        if(caret==span.end) index=i;
    }
    if(index==count || !ce_document_span(a->doc,index,&span)) return NULL;
    const char *url=span.style.link; *start=span.start; *end=span.end;
    for(size_t i=index;i>0;) {
        CeStyleSpan previous; --i;
        if(!ce_document_span(a->doc,i,&previous) || previous.end!=*start ||
           !previous.style.link || strcmp(previous.style.link,url)) break;
        *start=previous.start;
    }
    for(size_t i=index+1;i<count;i++) {
        CeStyleSpan next;
        if(!ce_document_span(a->doc,i,&next) || next.start!=*end ||
           !next.style.link || strcmp(next.style.link,url)) break;
        *end=next.end;
    }
    return url;
}

/* Defer a single-click link inspector until the double-click window expires.
 * Opening immediately on mouse-up would steal the second/third click from
 * word/paragraph selection. Dragging and keyboard activity cancel the intent. */
static void editing_after_event(App *a,const SDL_Event *event) {
    if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button==SDL_BUTTON_LEFT &&
       event->button.clicks==1 && !a->edit->composition_snapshot && !a->edit->composition_length) {
        size_t start,end; get_selection(a,&start,&end);
        if(start==end && editing_link_range(a,&start,&end)) {
            a->link_click_pending=true; a->link_click_ready_at=0;
            a->link_click_x=event->button.x; a->link_click_y=event->button.y;
            a->link_click_offset=a->edit->cursor; a->link_click_revision=a->citation_revision;
        }
    }
    if(!a->link_click_pending) return;
    if(event->type==SDL_EVENT_MOUSE_MOTION) {
        size_t start,end; get_selection(a,&start,&end);
        if(start!=end || SDL_fabsf(event->motion.x-a->link_click_x)>3 || SDL_fabsf(event->motion.y-a->link_click_y)>3)
            a->link_click_pending=false;
    }
    if(event->type==SDL_EVENT_MOUSE_BUTTON_UP && event->button.button==SDL_BUTTON_LEFT) {
        size_t start,end; get_selection(a,&start,&end);
        if(start!=end || SDL_fabsf(event->button.x-a->link_click_x)>3 || SDL_fabsf(event->button.y-a->link_click_y)>3) {
            a->link_click_pending=false; return;
        }
        const char *hint=SDL_GetHint(SDL_HINT_MOUSE_DOUBLE_CLICK_TIME);
        Uint64 delay=hint?(Uint64)SDL_max(1,SDL_atoi(hint)):500;
        a->link_click_ready_at=SDL_GetTicks()+delay;
    }
}
static void editing_tick(App *a) {
    if(!a->link_click_pending || !a->link_click_ready_at || SDL_GetTicks()<a->link_click_ready_at) return;
    a->link_click_pending=false;
    if(a->prompt_mode!=PROMPT_NONE || a->library_open || !a->active || !a->edit->has_focus ||
       a->link_click_revision!=a->citation_revision || a->edit->cursor!=a->link_click_offset) return;
    size_t start,end; get_selection(a,&start,&end);
    if(start==end && editing_link_range(a,&start,&end)) show_prompt(a,PROMPT_LINK);
}
